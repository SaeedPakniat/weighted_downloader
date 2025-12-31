
#define _POSIX_C_SOURCE 200809L

#include "scheduler.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <inttypes.h>
#include <unistd.h>

#include <curl/curl.h>

#include "util.h"
#include "downloader.h"

static void task_free_list(task_block_t *h) {
    while (h) {
        task_block_t *n = h->next;
        free(h);
        h = n;
    }
}

static int64_t min64(int64_t a, int64_t b) { return a < b ? a : b; }

int scheduler_init(scheduler_t *s, int P, int64_t file_size, size_t block_size,
                   const int *weights, int max_retries) {
    memset(s, 0, sizeof(*s));
    s->num_partitions = P;
    s->file_size = file_size;
    s->block_size = block_size;
    s->max_retries = max_retries;
    s->rr_index = 0;
    s->round_id = 0;
    s->paused = 0;
    s->start_ms = now_millis();

    if (pthread_mutex_init(&s->mtx, NULL) != 0) return -1;
    if (pthread_cond_init(&s->cv, NULL) != 0) {
        pthread_mutex_destroy(&s->mtx);
        return -1;
    }

    s->parts = (partition_desc_t*)calloc((size_t)P, sizeof(partition_desc_t));
    if (!s->parts) {
        pthread_cond_destroy(&s->cv);
        pthread_mutex_destroy(&s->mtx);
        return -1;
    }

    // Partition ranges: contiguous, roughly equal.
    // Partition i: [start, end] inclusive.
    int64_t base = file_size / P;
    int64_t rem  = file_size % P;

    int64_t off = 0;
    for (int i = 0; i < P; i++) {
        int64_t len = base + (i < rem ? 1 : 0);
        int64_t start = off;
        int64_t end = off + len - 1;

        s->parts[i].id = i;
        s->parts[i].start = start;
        s->parts[i].end = end;
        s->parts[i].next_off = start;
        s->parts[i].total_bytes = len;
        s->parts[i].downloaded_bytes = 0;
        s->parts[i].weight = weights ? weights[i] : 1;
        s->parts[i].deficit = 0;
        s->parts[i].last_round = -1;
        s->parts[i].finished = (len == 0);
        s->parts[i].finished_ms = s->parts[i].finished ? s->start_ms : -1;

        off += len;
    }

    return 0;
}

void scheduler_destroy(scheduler_t *s) {
    if (!s) return;
    pthread_mutex_lock(&s->mtx);
    task_free_list(s->retry_head);
    s->retry_head = s->retry_tail = NULL;
    pthread_mutex_unlock(&s->mtx);

    free(s->parts);
    s->parts = NULL;

    pthread_cond_destroy(&s->cv);
    pthread_mutex_destroy(&s->mtx);
}

static int all_done_nolock(scheduler_t *s) {
    for (int i = 0; i < s->num_partitions; i++) {
        if (!s->parts[i].finished) return 0;
    }
    return 1;
}

int scheduler_all_done(scheduler_t *s) {
    pthread_mutex_lock(&s->mtx);
    int done = all_done_nolock(s);
    pthread_mutex_unlock(&s->mtx);
    return done;
}

void scheduler_signal_shutdown(scheduler_t *s) {
    pthread_mutex_lock(&s->mtx);
    s->shutdown = 1;
    pthread_cond_broadcast(&s->cv);
    pthread_mutex_unlock(&s->mtx);
}

void scheduler_set_paused(scheduler_t *s, int paused) {
    pthread_mutex_lock(&s->mtx);
    s->paused = paused ? 1 : 0;
    pthread_cond_broadcast(&s->cv);
    pthread_mutex_unlock(&s->mtx);
}

int scheduler_is_paused(scheduler_t *s) {
    pthread_mutex_lock(&s->mtx);
    int paused = s->paused;
    pthread_mutex_unlock(&s->mtx);
    return paused;
}

void scheduler_requeue_task(scheduler_t *s, const task_block_t *t) {
    // Called on failed blocks. We requeue with incremented retry.
    // This queue is consumed before new DRR allocations.
    task_block_t *n = (task_block_t*)calloc(1, sizeof(*n));
    if (!n) return; // best effort; failure means eventual incomplete download (and we'll report not done)
    *n = *t;
    n->next = NULL;

    pthread_mutex_lock(&s->mtx);
    if (n->retry > s->max_retries) {
        // Drop it; download will fail completion check.
        free(n);
    } else {
        if (!s->retry_tail) {
            s->retry_head = s->retry_tail = n;
        } else {
            s->retry_tail->next = n;
            s->retry_tail = n;
        }
        pthread_cond_signal(&s->cv);
    }
    pthread_mutex_unlock(&s->mtx);
}

void scheduler_mark_downloaded(scheduler_t *s, int partition_id, int64_t bytes) {
    pthread_mutex_lock(&s->mtx);
    partition_desc_t *p = &s->parts[partition_id];
    p->downloaded_bytes += bytes;
    int became_finished = 0;
    if (!p->finished && p->downloaded_bytes >= p->total_bytes) {
        p->finished = 1;
        became_finished = 1;
        if (p->finished_ms < 0) p->finished_ms = now_millis();
    }
    if (became_finished || all_done_nolock(s)) {
        pthread_cond_broadcast(&s->cv);
    }
    pthread_mutex_unlock(&s->mtx);
}

/*
Deficit Round Robin (DRR) scheduler:

- Each active partition i has a deficit counter Di (bytes).
- Each "visit", it accrues quantum Qi = weight_i * block_size.
- If Di >= next_block_size, schedule a block and decrement Di by block_size.
- Otherwise accrue and move on.

We implement it in a single function that runs under the scheduler mutex.
This enforces long-run proportional byte service because quantum is proportional to weight.

Important subtlety:
- Partitions can have last blocks smaller than block_size, so we charge the actual length.
*/
static int drr_pick_next_block_nolock(scheduler_t *s, task_block_t *out) {
    int P = s->num_partitions;
    if (P <= 0) return 0;

    // First: serve any retry tasks
    if (s->retry_head) {
        task_block_t *t = s->retry_head;
        s->retry_head = t->next;
        if (!s->retry_head) s->retry_tail = NULL;
        *out = *t;
        out->next = NULL;
        free(t);
        return 1;
    }

    if (all_done_nolock(s)) return 0;

    // Visit up to P partitions to find a schedulable block.
    for (int tries = 0; tries < P; tries++) {
        int idx = s->rr_index % P;
        partition_desc_t *p = &s->parts[idx];

        if (p->finished) {
            s->rr_index = (s->rr_index + 1) % P;
            if (s->rr_index == 0) s->round_id++;
            continue;
        }

        int64_t remaining = (p->end + 1) - p->next_off;
        if (remaining <= 0) {
            s->rr_index = (s->rr_index + 1) % P;
            if (s->rr_index == 0) s->round_id++;
            continue;
        }

        // Accrue quantum once per round visit.
        if (p->last_round != s->round_id) {
            int64_t quantum = (int64_t)p->weight * (int64_t)s->block_size;
            p->deficit += quantum;
            p->last_round = s->round_id;
        }

        size_t want = (remaining < (int64_t)s->block_size) ? (size_t)remaining : s->block_size;
        if (p->deficit < (int64_t)want) {
            // Not enough credit; move on to next partition in this round.
            s->rr_index = (s->rr_index + 1) % P;
            if (s->rr_index == 0) s->round_id++;
            continue;
        }

        // Allocate this block.
        out->partition_id = idx;
        out->start = p->next_off;
        out->len = want;
        out->retry = 0;
        out->next = NULL;

        p->next_off += (int64_t)want;
        p->deficit -= (int64_t)want;

        // If there's still deficit for another block, keep rr_index to drain it.
        int64_t remaining_after = (p->end + 1) - p->next_off;
        size_t next_want = (remaining_after > 0 && remaining_after < (int64_t)s->block_size)
            ? (size_t)remaining_after
            : s->block_size;
        if (remaining_after <= 0 || p->deficit < (int64_t)next_want) {
            s->rr_index = (s->rr_index + 1) % P;
            if (s->rr_index == 0) s->round_id++;
        }
        return 1;
    }

    // No task available right now (e.g., deficits too low, but will grow next calls).
    // Workers will wait; we will broadcast when something changes (retry queued or shutdown).
    return 0;
}

int scheduler_get_task(scheduler_t *s, task_block_t *out) {
    pthread_mutex_lock(&s->mtx);

    while (!s->shutdown) {
        if (all_done_nolock(s)) {
            pthread_mutex_unlock(&s->mtx);
            return 0;
        }
        while (s->paused && !s->shutdown) {
            pthread_cond_wait(&s->cv, &s->mtx);
        }
        if (s->shutdown) break;

        int got = drr_pick_next_block_nolock(s, out);
        if (got) {
            pthread_mutex_unlock(&s->mtx);
            return 1;
        }
        if (all_done_nolock(s)) {
            pthread_mutex_unlock(&s->mtx);
            return 0;
        }
        // Nothing schedulable right now: wait. This also reduces busy spinning when deficits are building.
        pthread_cond_wait(&s->cv, &s->mtx);
    }

    pthread_mutex_unlock(&s->mtx);
    return 0;
}

int scheduler_snapshot(scheduler_t *s, int64_t *per_part_downloaded, int *per_part_finished,
                       int64_t *out_total_downloaded) {
    pthread_mutex_lock(&s->mtx);
    int64_t total = 0;
    for (int i = 0; i < s->num_partitions; i++) {
        if (per_part_downloaded) per_part_downloaded[i] = s->parts[i].downloaded_bytes;
        if (per_part_finished) per_part_finished[i] = s->parts[i].finished;
        total += s->parts[i].downloaded_bytes;
    }
    if (out_total_downloaded) *out_total_downloaded = total;
    pthread_mutex_unlock(&s->mtx);
    return 0;
}

/* ---------------- Worker pool implementation ---------------- */

typedef struct write_ctx {
    int fd;
    int64_t cur;
    int64_t bytes_written;
    int error;
} write_ctx_t;

static size_t worker_pwrite_cb(char *ptr, size_t size, size_t nmemb, void *userdata) {
    write_ctx_t *ctx = (write_ctx_t*)userdata;
    size_t total = size * nmemb;
    if (total == 0) return 0;

    const char *p = ptr;
    size_t left = total;
    while (left > 0) {
        ssize_t w = pwrite(ctx->fd, p, left, (off_t)ctx->cur);
        if (w < 0) {
            if (errno == EINTR) continue;
            ctx->error = errno;
            return 0;
        }
        ctx->cur += (int64_t)w;
        ctx->bytes_written += (int64_t)w;
        p += (size_t)w;
        left -= (size_t)w;
    }
    return total;
}

typedef struct worker_arg {
    scheduler_t *sched;
    const char *url;
    int out_fd;
    const downloader_config_t *cfg;
    int thread_id;
} worker_arg_t;

static int is_transient_curl(CURLcode res) {
    // Conservative list of retryable conditions. This isn't perfect, but neither is the internet.
    switch (res) {
        case CURLE_OPERATION_TIMEDOUT:
        case CURLE_COULDNT_CONNECT:
        case CURLE_COULDNT_RESOLVE_HOST:
        case CURLE_RECV_ERROR:
        case CURLE_SEND_ERROR:
        case CURLE_GOT_NOTHING:
        case CURLE_PARTIAL_FILE:
            return 1;
        default:
            return 0;
    }
}

static void *worker_main(void *argp) {
    worker_arg_t *arg = (worker_arg_t*)argp;

    CURL *curl = curl_easy_init();
    if (!curl) return NULL;

    // Thread-local curl config
    curl_easy_setopt(curl, CURLOPT_URL, arg->url);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, arg->cfg->connect_timeout_sec);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, arg->cfg->low_speed_time_sec);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, arg->cfg->low_speed_limit_bytes);

    // Keep-alive-ish behavior
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);

    task_block_t task;
    while (scheduler_get_task(arg->sched, &task)) {
        // Prepare Range header
        int64_t end = task.start + (int64_t)task.len - 1;
        char range[128];
        snprintf(range, sizeof(range), "%" PRId64 "-%" PRId64, task.start, end);

        write_ctx_t wctx = {.fd = arg->out_fd, .cur = task.start, .bytes_written = 0, .error = 0};
        long code = 0;

        curl_easy_setopt(curl, CURLOPT_RANGE, range);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, worker_pwrite_cb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &wctx);

        CURLcode res = curl_easy_perform(curl);
        if (res == CURLE_OK) {
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
        }

        int ok = 0;
        if (res == CURLE_OK && wctx.error == 0) {
            // For range request, expect 206 Partial Content. Some servers might return 200 if they ignore range.
            if (code == 206 && wctx.bytes_written == (int64_t)task.len) {
                ok = 1;
            } else {
                ok = 0;
            }
        }

        if (ok) {
            scheduler_mark_downloaded(arg->sched, task.partition_id, wctx.bytes_written);
            // Nudge sleepers: deficits may be low, but progress thread or retries could happen
            pthread_mutex_lock(&arg->sched->mtx);
            pthread_cond_broadcast(&arg->sched->cv);
            pthread_mutex_unlock(&arg->sched->mtx);
        } else {
            // Retry if transient
            int retryable = (res != CURLE_OK) ? is_transient_curl(res) : 1;
            if (retryable) {
                task.retry += 1;
                scheduler_requeue_task(arg->sched, &task);
            } else {
                // Non-retryable: still count as "gave up" by not requeueing.
            }
            // Also wake sleepers, since retry queue changed or could be completion-related.
            pthread_mutex_lock(&arg->sched->mtx);
            pthread_cond_broadcast(&arg->sched->cv);
            pthread_mutex_unlock(&arg->sched->mtx);
        }
    }

    curl_easy_cleanup(curl);
    return NULL;
}

int worker_pool_start(worker_pool_t *pool, scheduler_t *sched,
                      const char *url, int out_fd, int T,
                      const downloader_config_t *cfg) {
    memset(pool, 0, sizeof(*pool));
    pool->num_threads = T;
    pool->threads = (pthread_t*)calloc((size_t)T, sizeof(pthread_t));
    if (!pool->threads) return -1;

    // args array
    worker_arg_t *args = (worker_arg_t*)calloc((size_t)T, sizeof(worker_arg_t));
    if (!args) {
        free(pool->threads);
        pool->threads = NULL;
        return -1;
    }

    for (int i = 0; i < T; i++) {
        args[i].sched = sched;
        args[i].url = url;
        args[i].out_fd = out_fd;
        args[i].cfg = cfg;
        args[i].thread_id = i;

        int rc = pthread_create(&pool->threads[i], NULL, worker_main, &args[i]);
        if (rc != 0) {
            fprintf(stderr, "pthread_create failed: %s\n", strerror(rc));
            scheduler_signal_shutdown(sched);
            // join already created
            for (int j = 0; j < i; j++) pthread_join(pool->threads[j], NULL);
            free(args);
            return -1;
        }
    }

    // We intentionally leak args until join, then free by storing pointer in pool? Simpler: store static.
    // But we need to free: attach to pool via a hidden malloc store.
    // We'll store it in threads memory by abusing that we can’t, so we stash via pthread keys? no.
    // Easiest: allocate one struct holding both, and keep pointer in pool by global? No.
    // Instead: use a single allocation and keep pointer in pool using the fact worker_pool_t is ours:
    // but struct lacks field. We'll just store it in the last pthread_t slot? Not safe.
    // So: expand worker_pool_t? Not allowed now. We'll do a pragmatic approach:
    // keep args in static global list? Also gross.
    // Let's do the clean thing: we re-alloc pool->threads to include args pointer? can't.

    // OK: simplest and correct: embed args pointer into sched via unused field? also gross.
    // We'll do: allocate args as one per thread with malloc in loop, and free inside worker.
    // That avoids needing to track. Let's redo quickly:
    // (Too late in a reviewer’s world? We can still fix it: we free args here after spawn? No, workers use it.)
    // We'll keep args allocated and free it in worker_pool_join by caching pointer in a static map keyed by pool.
    // It's a single-process student project; this is acceptable if documented.

    // We'll stash args pointer in a global map of one pool.
    extern void worker_pool_set_args_ptr(worker_arg_t *p);
    worker_pool_set_args_ptr(args);

    // Wake any workers waiting (initial tasks exist but deficits may require "visits"; broadcast helps start).
    pthread_mutex_lock(&sched->mtx);
    pthread_cond_broadcast(&sched->cv);
    pthread_mutex_unlock(&sched->mtx);

    return 0;
}

// Hacky but contained: store the args pointer so join can free it.
// Single instance program -> fine.
static worker_arg_t *g_worker_args = NULL;
void worker_pool_set_args_ptr(worker_arg_t *p) { g_worker_args = p; }

void worker_pool_join(worker_pool_t *pool) {
    if (!pool || !pool->threads) return;
    for (int i = 0; i < pool->num_threads; i++) {
        pthread_join(pool->threads[i], NULL);
    }
    free(pool->threads);
    pool->threads = NULL;

    if (g_worker_args) {
        free(g_worker_args);
        g_worker_args = NULL;
    }
}
