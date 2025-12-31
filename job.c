#define _POSIX_C_SOURCE 200809L

#include "job.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include <curl/curl.h>

#include "gui.h"
#include "progress.h"
#include "scheduler.h"
#include "util.h"

typedef struct head_info {
    long response_code;
    int accept_ranges_bytes;
    int content_length_known;
    int64_t content_length;
} head_info_t;

struct download_job {
    pthread_mutex_t mtx;

    char *url;
    char *output_path;
    downloader_config_t cfg;

    int P;
    int T;
    int *weights;

    int64_t file_size;
    int range_supported; // -1 unknown, 0 no, 1 yes

    int64_t *part_sizes;

    int out_fd;

    scheduler_t sched;
    int sched_inited;

    progress_ctx_t pctx;
    int progress_inited;
    int progress_started;

    worker_pool_t pool;
    int pool_started;

    pthread_t thread;
    int thread_started;
    int thread_joined;

    int stop_requested;
    int result;
    download_job_state_t state;

    int start_paused;

    int resume_loaded;
    int resume_P;
    int64_t resume_file_size;
    int64_t *resume_next_off;
    int64_t *resume_downloaded;
};

static size_t header_cb(char *buffer, size_t size, size_t nitems, void *userdata) {
    size_t len = size * nitems;
    head_info_t *hi = (head_info_t *)userdata;

    if (len > 0) {
        char tmp[512];
        size_t cpy = (len < sizeof(tmp) - 1) ? len : sizeof(tmp) - 1;
        memcpy(tmp, buffer, cpy);
        tmp[cpy] = '\0';

        if (strncasecmp(tmp, "Accept-Ranges:", 14) == 0) {
            if (strstr(tmp, "bytes") != NULL || strstr(tmp, "Bytes") != NULL) {
                hi->accept_ranges_bytes = 1;
            }
        } else if (strncasecmp(tmp, "Content-Length:", 15) == 0) {
            const char *p = tmp + 15;
            while (*p == ' ' || *p == '\t') p++;
            char *endp = NULL;
            long long v = strtoll(p, &endp, 10);
            if (endp != p && v > 0) {
                hi->content_length_known = 1;
                hi->content_length = (int64_t)v;
            }
        }
    }
    return len;
}

static int http_head_probe(const char *url, const downloader_config_t *cfg, head_info_t *out) {
    memset(out, 0, sizeof(*out));

    CURL *curl = curl_easy_init();
    if (!curl) return -1;

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_cb);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, out);

    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, cfg->connect_timeout_sec);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, cfg->low_speed_time_sec);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, cfg->low_speed_limit_bytes);

    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        curl_easy_cleanup(curl);
        return -1;
    }

    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &out->response_code);
    curl_easy_cleanup(curl);

    return 0;
}

typedef struct sink_ctx {
    size_t bytes;
} sink_ctx_t;

static size_t sink_write_cb(char *ptr, size_t size, size_t nmemb, void *userdata) {
    (void)ptr;
    sink_ctx_t *s = (sink_ctx_t*)userdata;
    size_t n = size * nmemb;
    s->bytes += n;
    return n;
}

static int http_range_probe_0_0(const char *url, const downloader_config_t *cfg, int64_t *out_size_if_known) {
    *out_size_if_known = -1;

    CURL *curl = curl_easy_init();
    if (!curl) return 0;

    sink_ctx_t sink = {0};
    long code = 0;

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, sink_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &sink);

    curl_easy_setopt(curl, CURLOPT_RANGE, "0-0");

    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, cfg->connect_timeout_sec);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, cfg->low_speed_time_sec);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, cfg->low_speed_limit_bytes);

    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        curl_easy_cleanup(curl);
        return 0;
    }
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);

#if LIBCURL_VERSION_NUM >= 0x073700
    curl_off_t cl = -1;
    if (curl_easy_getinfo(curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &cl) == CURLE_OK) {
        if (cl > 0) *out_size_if_known = (int64_t)cl;
    }
#else
    double cl = -1.0;
    if (curl_easy_getinfo(curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD, &cl) == CURLE_OK) {
        if (cl > 0) *out_size_if_known = (int64_t)cl;
    }
#endif

    curl_easy_cleanup(curl);

    return (code == 206);
}

typedef struct pwrite_ctx {
    int fd;
    int64_t cur_off;
    int error;
} pwrite_ctx_t;

static size_t pwrite_cb(char *ptr, size_t size, size_t nmemb, void *userdata) {
    pwrite_ctx_t *ctx = (pwrite_ctx_t*)userdata;
    size_t total = size * nmemb;
    if (total == 0) return 0;

    const char *p = ptr;
    size_t left = total;

    while (left > 0) {
        ssize_t w = pwrite(ctx->fd, p, left, (off_t)ctx->cur_off);
        if (w < 0) {
            if (errno == EINTR) continue;
            ctx->error = errno;
            return 0;
        }
        ctx->cur_off += (int64_t)w;
        p += (size_t)w;
        left -= (size_t)w;
    }
    return total;
}

static int job_should_stop(const download_job_t *job) {
    int stop = 0;
    pthread_mutex_lock((pthread_mutex_t*)&job->mtx);
    stop = job->stop_requested;
    pthread_mutex_unlock((pthread_mutex_t*)&job->mtx);
    return stop;
}

static void job_clear_resume(download_job_t *job) {
    free(job->resume_next_off);
    free(job->resume_downloaded);
    job->resume_next_off = NULL;
    job->resume_downloaded = NULL;
    job->resume_loaded = 0;
    job->resume_P = 0;
    job->resume_file_size = 0;
}

static int job_snapshot_state(download_job_t *job, int64_t **out_next,
                              int64_t **out_downloaded, int *out_P, int64_t *out_size) {
    if (!job || !out_next || !out_downloaded || !out_P || !out_size) return -1;

    pthread_mutex_lock((pthread_mutex_t*)&job->mtx);
    int sched_inited = job->sched_inited;
    int P = job->P;
    int64_t size = job->file_size;
    pthread_mutex_unlock((pthread_mutex_t*)&job->mtx);

    if (sched_inited) {
        int64_t *next = (int64_t*)calloc((size_t)P, sizeof(int64_t));
        int64_t *dl = (int64_t*)calloc((size_t)P, sizeof(int64_t));
        if (!next || !dl) {
            free(next);
            free(dl);
            return -1;
        }

        pthread_mutex_lock((pthread_mutex_t*)&job->mtx);
        pthread_mutex_lock(&job->sched.mtx);
        for (int i = 0; i < job->sched.num_partitions; i++) {
            next[i] = job->sched.parts[i].next_off;
            dl[i] = job->sched.parts[i].downloaded_bytes;
        }
        size = job->sched.file_size;
        pthread_mutex_unlock(&job->sched.mtx);
        pthread_mutex_unlock((pthread_mutex_t*)&job->mtx);

        *out_next = next;
        *out_downloaded = dl;
        *out_P = P;
        *out_size = size;
        return 0;
    }

    pthread_mutex_lock((pthread_mutex_t*)&job->mtx);
    if (job->resume_loaded) {
        int Pcopy = job->resume_P;
        int64_t *next = (int64_t*)calloc((size_t)Pcopy, sizeof(int64_t));
        int64_t *dl = (int64_t*)calloc((size_t)Pcopy, sizeof(int64_t));
        if (!next || !dl) {
            pthread_mutex_unlock((pthread_mutex_t*)&job->mtx);
            free(next);
            free(dl);
            return -1;
        }
        memcpy(next, job->resume_next_off, (size_t)Pcopy * sizeof(int64_t));
        memcpy(dl, job->resume_downloaded, (size_t)Pcopy * sizeof(int64_t));
        *out_next = next;
        *out_downloaded = dl;
        *out_P = Pcopy;
        *out_size = job->resume_file_size;
        pthread_mutex_unlock((pthread_mutex_t*)&job->mtx);
        return 0;
    }
    pthread_mutex_unlock((pthread_mutex_t*)&job->mtx);
    return -1;
}

static void job_apply_resume(download_job_t *job) {
    if (!job->resume_loaded || !job->sched_inited) return;

    pthread_mutex_lock(&job->mtx);
    int P = job->resume_P;
    int64_t *next = job->resume_next_off;
    int64_t *dl = job->resume_downloaded;
    job->resume_next_off = NULL;
    job->resume_downloaded = NULL;
    job->resume_loaded = 0;
    job->resume_P = 0;
    job->resume_file_size = 0;
    pthread_mutex_unlock(&job->mtx);

    if (!next || !dl || P <= 0) {
        free(next);
        free(dl);
        return;
    }

    pthread_mutex_lock(&job->sched.mtx);
    for (int i = 0; i < job->sched.num_partitions && i < P; i++) {
        partition_desc_t *p = &job->sched.parts[i];
        int64_t next_off = next[i];
        int64_t dl_bytes = dl[i];

        if (dl_bytes < 0) dl_bytes = 0;
        if (dl_bytes > p->total_bytes) dl_bytes = p->total_bytes;

        if (next_off <= p->start && dl_bytes > 0) {
            next_off = p->start + dl_bytes;
        }
        if (next_off < p->start) next_off = p->start;
        if (next_off > p->end + 1) next_off = p->end + 1;

        p->downloaded_bytes = dl_bytes;
        p->next_off = next_off;
        if (p->downloaded_bytes >= p->total_bytes || p->next_off >= p->end + 1) {
            p->finished = 1;
        }
    }
    pthread_mutex_unlock(&job->sched.mtx);

    free(next);
    free(dl);
}

#if LIBCURL_VERSION_NUM >= 0x072000
static int single_thread_xferinfo(void *clientp, curl_off_t dltotal, curl_off_t dlnow,
                                  curl_off_t ultotal, curl_off_t ulnow) {
    (void)dltotal;
    (void)dlnow;
    (void)ultotal;
    (void)ulnow;
    download_job_t *job = (download_job_t*)clientp;
    return job_should_stop(job) ? 1 : 0;
}
#else
static int single_thread_progress(void *clientp, double dltotal, double dlnow,
                                  double ultotal, double ulnow) {
    (void)dltotal;
    (void)dlnow;
    (void)ultotal;
    (void)ulnow;
    download_job_t *job = (download_job_t*)clientp;
    return job_should_stop(job) ? 1 : 0;
}
#endif

static int single_thread_download(download_job_t *job) {
    printf("[Fallback] Server does not support Range requests. Using single-thread download.\n");

    int fd = open(job->output_path, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (fd < 0) {
        fprintf(stderr, "open(%s) failed: %s\n", job->output_path, strerror(errno));
        return -1;
    }

    CURL *curl = curl_easy_init();
    if (!curl) {
        close(fd);
        return -1;
    }

    pwrite_ctx_t wctx = {.fd = fd, .cur_off = 0, .error = 0};
    long code = 0;

    curl_easy_setopt(curl, CURLOPT_URL, job->url);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, pwrite_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &wctx);

    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, job->cfg.connect_timeout_sec);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, job->cfg.low_speed_time_sec);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, job->cfg.low_speed_limit_bytes);

    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
#if LIBCURL_VERSION_NUM >= 0x072000
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, single_thread_xferinfo);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, job);
#else
    curl_easy_setopt(curl, CURLOPT_PROGRESSFUNCTION, single_thread_progress);
    curl_easy_setopt(curl, CURLOPT_PROGRESSDATA, job);
#endif

    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        if (res == CURLE_ABORTED_BY_CALLBACK && job_should_stop(job)) {
            curl_easy_cleanup(curl);
            close(fd);
            return 1;
        }
        fprintf(stderr, "curl_easy_perform failed: %s\n", curl_easy_strerror(res));
        curl_easy_cleanup(curl);
        close(fd);
        return -1;
    }
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    if (code < 200 || code >= 300) {
        fprintf(stderr, "HTTP response code %ld\n", code);
        curl_easy_cleanup(curl);
        close(fd);
        return -1;
    }

    curl_easy_cleanup(curl);
    if (fsync(fd) != 0) {
        fprintf(stderr, "fsync failed: %s\n", strerror(errno));
    }
    close(fd);
    return 0;
}

static void download_job_set_state(download_job_t *job, download_job_state_t state) {
    pthread_mutex_lock(&job->mtx);
    job->state = state;
    pthread_mutex_unlock(&job->mtx);
}

static int download_job_run(download_job_t *job) {
    int64_t size = -1;
    int range_supported = -1;

    pthread_mutex_lock(&job->mtx);
    size = job->file_size;
    range_supported = job->range_supported;
    pthread_mutex_unlock(&job->mtx);

    if (size <= 0 || range_supported < 0) {
        int64_t probed_size = -1;
        int probed_range = 0;
        download_job_probe(job, &probed_size, &probed_range);
        download_job_set_probe(job, probed_size, probed_range);
        size = probed_size;
        range_supported = probed_range;
    }

    if (size <= 0 || !range_supported) {
        int rc = single_thread_download(job);
        if (rc == 1) return 1;
        return rc;
    }

    if (job->P <= 0 || !job->weights || job->T <= 0) {
        fprintf(stderr, "Invalid scheduling settings; partitions and worker threads required.\n");
        return -1;
    }

    printf("Server supports Range requests. Content-Length = %" PRId64 " bytes\n", size);

    if (job->cfg.gui_enabled) {
        job->part_sizes = (int64_t*)xcalloc((size_t)job->P, sizeof(int64_t));
        int64_t base = size / job->P;
        int64_t rem = size % job->P;
        for (int i = 0; i < job->P; i++) {
            job->part_sizes[i] = base + (i < rem ? 1 : 0);
        }
    }

    int resume_available = 0;
    pthread_mutex_lock(&job->mtx);
    resume_available = job->resume_loaded;
    pthread_mutex_unlock(&job->mtx);

    int open_flags = O_CREAT | O_WRONLY;
    if (!resume_available) {
        open_flags |= O_TRUNC;
    }

    int fd = open(job->output_path, open_flags, 0644);
    if (fd < 0) {
        fprintf(stderr, "open(%s) failed: %s\n", job->output_path, strerror(errno));
        return -1;
    }
    if (size > 0) {
        if (resume_available) {
            struct stat st;
            if (fstat(fd, &st) == 0 && st.st_size != (off_t)size) {
                if (ftruncate(fd, (off_t)size) != 0) {
                    fprintf(stderr, "ftruncate failed: %s\n", strerror(errno));
                    close(fd);
                    return -1;
                }
            }
        } else {
            if (ftruncate(fd, (off_t)size) != 0) {
                fprintf(stderr, "ftruncate failed: %s\n", strerror(errno));
                close(fd);
                return -1;
            }
        }
    }
    job->out_fd = fd;

    if (scheduler_init(&job->sched, job->P, size, (size_t)job->cfg.block_size,
                       job->weights, job->cfg.max_retries_per_block) != 0) {
        fprintf(stderr, "scheduler_init failed\n");
        close(fd);
        return -1;
    }
    pthread_mutex_lock(&job->mtx);
    job->sched_inited = 1;
    pthread_mutex_unlock(&job->mtx);

    if (resume_available) {
        pthread_mutex_lock(&job->mtx);
        int resume_ok = (job->resume_P == job->P) &&
                        (job->resume_file_size <= 0 || job->resume_file_size == size);
        pthread_mutex_unlock(&job->mtx);
        if (resume_ok) {
            job_apply_resume(job);
        }
    }

    if (progress_init(&job->pctx, &job->sched, job->P, size, job->cfg.progress_interval_ms,
                      job->cfg.csv_sample_interval_ms, job->cfg.ui_enabled,
                      job->cfg.output_enabled) != 0) {
        fprintf(stderr, "progress_init failed\n");
        scheduler_destroy(&job->sched);
        close(fd);
        return -1;
    }
    job->progress_inited = 1;

    char csv_name[256];
    snprintf(csv_name, sizeof(csv_name), "weights_log_%lld.csv", (long long)now_millis());

    if (progress_start(&job->pctx, csv_name) != 0) {
        fprintf(stderr, "progress_start failed\n");
        progress_destroy(&job->pctx);
        scheduler_destroy(&job->sched);
        close(fd);
        return -1;
    }
    job->progress_started = 1;

    if (job->start_paused) {
        scheduler_set_paused(&job->sched, 1);
        download_job_set_state(job, JOB_PAUSED);
    }

    if (worker_pool_start(&job->pool, &job->sched, job->url, fd, job->T, &job->cfg) != 0) {
        fprintf(stderr, "worker_pool_start failed\n");
        scheduler_signal_shutdown(&job->sched);
        progress_stop(&job->pctx);
        progress_destroy(&job->pctx);
        scheduler_destroy(&job->sched);
        close(fd);
        return -1;
    }
    job->pool_started = 1;

    int gui_rc = 0;
    if (job->cfg.gui_enabled) {
        gui_rc = gui_run(&job->sched, job->P, size, job->weights, job->part_sizes);
        if (gui_rc != 0) {
            fprintf(stderr, "gui_run failed\n");
            scheduler_signal_shutdown(&job->sched);
        }
    }

    worker_pool_join(&job->pool);
    job->pool_started = 0;

    progress_stop(&job->pctx);
    progress_destroy(&job->pctx);
    job->progress_started = 0;
    job->progress_inited = 0;

    int ok = scheduler_all_done(&job->sched);
    scheduler_destroy(&job->sched);
    pthread_mutex_lock(&job->mtx);
    job->sched_inited = 0;
    pthread_mutex_unlock(&job->mtx);

    if (fsync(fd) != 0) {
        fprintf(stderr, "fsync failed: %s\n", strerror(errno));
    }
    close(fd);
    job->out_fd = -1;

    free(job->part_sizes);
    job->part_sizes = NULL;

    if (gui_rc != 0 || !ok) {
        fprintf(stderr, "Not all partitions completed.\n");
        return -1;
    }

    printf("CSV log written: %s\n", csv_name);
    printf("Tip: verify correctness with: sha256sum %s\n", job->output_path);
    return 0;
}

static void *download_job_thread_main(void *arg) {
    download_job_t *job = (download_job_t*)arg;

    download_job_set_state(job, JOB_RUNNING);
    int rc = download_job_run(job);

    int stop_requested = job_should_stop(job);
    if (stop_requested && rc != 0) {
        download_job_set_state(job, JOB_STOPPED);
        pthread_mutex_lock(&job->mtx);
        job->result = -1;
        pthread_mutex_unlock(&job->mtx);
        return NULL;
    }

    if (rc == 0) {
        download_job_set_state(job, JOB_DONE);
        pthread_mutex_lock(&job->mtx);
        job->result = 0;
        pthread_mutex_unlock(&job->mtx);
    } else {
        download_job_set_state(job, JOB_ERROR);
        pthread_mutex_lock(&job->mtx);
        job->result = -1;
        pthread_mutex_unlock(&job->mtx);
    }
    return NULL;
}

download_job_t *download_job_create(const char *url, const char *output_path,
                                    const downloader_config_t *cfg) {
    if (!url || !output_path || !cfg) return NULL;

    download_job_t *job = (download_job_t*)calloc(1, sizeof(*job));
    if (!job) return NULL;

    if (pthread_mutex_init(&job->mtx, NULL) != 0) {
        free(job);
        return NULL;
    }

    job->url = strdup(url);
    job->output_path = strdup(output_path);
    if (!job->url || !job->output_path) {
        free(job->url);
        free(job->output_path);
        pthread_mutex_destroy(&job->mtx);
        free(job);
        return NULL;
    }

    job->cfg = *cfg;
    job->file_size = -1;
    job->range_supported = -1;
    job->out_fd = -1;
    job->state = JOB_IDLE;
    job->result = 0;
    job->start_paused = 0;
    job->resume_loaded = 0;
    job->resume_P = 0;
    job->resume_file_size = 0;
    job->resume_next_off = NULL;
    job->resume_downloaded = NULL;

    return job;
}

void download_job_destroy(download_job_t *job) {
    if (!job) return;

    if (job->thread_started && !job->thread_joined) {
        pthread_join(job->thread, NULL);
    }

    free(job->weights);
    free(job->part_sizes);
    job_clear_resume(job);
    free(job->url);
    free(job->output_path);

    pthread_mutex_destroy(&job->mtx);
    free(job);
}

int download_job_set_scheduling(download_job_t *job, int P, const int *weights, int T) {
    if (!job || P <= 0 || T <= 0 || !weights) return -1;

    int *copy = (int*)calloc((size_t)P, sizeof(int));
    if (!copy) return -1;
    memcpy(copy, weights, (size_t)P * sizeof(int));

    pthread_mutex_lock(&job->mtx);
    free(job->weights);
    job->weights = copy;
    job->P = P;
    job->T = T;
    if (job->resume_loaded && job->resume_P != P) {
        job_clear_resume(job);
    }
    pthread_mutex_unlock(&job->mtx);

    return 0;
}

void download_job_set_probe(download_job_t *job, int64_t size, int range_supported) {
    if (!job) return;
    pthread_mutex_lock(&job->mtx);
    job->file_size = size;
    job->range_supported = range_supported;
    pthread_mutex_unlock(&job->mtx);
}

int download_job_set_start_paused(download_job_t *job, int start_paused) {
    if (!job) return -1;
    pthread_mutex_lock(&job->mtx);
    job->start_paused = start_paused ? 1 : 0;
    pthread_mutex_unlock(&job->mtx);
    return 0;
}

int download_job_probe(const download_job_t *job, int64_t *out_size, int *out_range_supported) {
    if (!job || !out_size || !out_range_supported) return -1;

    head_info_t hi;
    int head_ok = (http_head_probe(job->url, &job->cfg, &hi) == 0);

    int64_t size = -1;
    int range_supported = 0;

    if (head_ok && hi.content_length_known) size = hi.content_length;
    if (head_ok && hi.accept_ranges_bytes) range_supported = 1;

    if (!range_supported) {
        int64_t probe_size = -1;
        int ok = http_range_probe_0_0(job->url, &job->cfg, &probe_size);
        if (ok) range_supported = 1;
        if (size < 0 && probe_size > 0) size = probe_size;
    }

    *out_size = size;
    *out_range_supported = range_supported;
    return 0;
}

int download_job_start(download_job_t *job) {
    if (!job) return -1;

    pthread_mutex_lock(&job->mtx);
    if (job->state != JOB_IDLE) {
        pthread_mutex_unlock(&job->mtx);
        return -1;
    }
    job->state = JOB_RUNNING;
    job->thread_started = 1;
    pthread_mutex_unlock(&job->mtx);

    int rc = pthread_create(&job->thread, NULL, download_job_thread_main, job);
    if (rc != 0) {
        pthread_mutex_lock(&job->mtx);
        job->state = JOB_ERROR;
        job->thread_started = 0;
        pthread_mutex_unlock(&job->mtx);
        return -1;
    }
    return 0;
}

int download_job_pause(download_job_t *job) {
    if (!job) return -1;

    pthread_mutex_lock(&job->mtx);
    if (!job->sched_inited || (job->state != JOB_RUNNING && job->state != JOB_PAUSED)) {
        pthread_mutex_unlock(&job->mtx);
        return -1;
    }
    if (job->state == JOB_PAUSED) {
        pthread_mutex_unlock(&job->mtx);
        return 0;
    }
    job->state = JOB_PAUSED;
    pthread_mutex_unlock(&job->mtx);

    scheduler_set_paused(&job->sched, 1);
    return 0;
}

int download_job_resume(download_job_t *job) {
    if (!job) return -1;

    pthread_mutex_lock(&job->mtx);
    if (!job->sched_inited || job->state != JOB_PAUSED) {
        pthread_mutex_unlock(&job->mtx);
        return -1;
    }
    job->state = JOB_RUNNING;
    pthread_mutex_unlock(&job->mtx);

    scheduler_set_paused(&job->sched, 0);
    return 0;
}

int download_job_stop(download_job_t *job) {
    if (!job) return -1;

    pthread_mutex_lock(&job->mtx);
    if (job->state != JOB_RUNNING && job->state != JOB_PAUSED && job->state != JOB_STOPPING) {
        pthread_mutex_unlock(&job->mtx);
        return -1;
    }
    job->stop_requested = 1;
    job->state = JOB_STOPPING;
    int sched_inited = job->sched_inited;
    pthread_mutex_unlock(&job->mtx);

    if (sched_inited) {
        scheduler_set_paused(&job->sched, 0);
        scheduler_signal_shutdown(&job->sched);
    }
    return 0;
}

int download_job_join(download_job_t *job) {
    if (!job) return -1;

    pthread_mutex_lock(&job->mtx);
    if (!job->thread_started || job->thread_joined) {
        pthread_mutex_unlock(&job->mtx);
        return 0;
    }
    pthread_mutex_unlock(&job->mtx);

    pthread_join(job->thread, NULL);

    pthread_mutex_lock(&job->mtx);
    job->thread_joined = 1;
    pthread_mutex_unlock(&job->mtx);

    return 0;
}

const char *download_job_url(const download_job_t *job) {
    return job ? job->url : NULL;
}

const char *download_job_output_path(const download_job_t *job) {
    return job ? job->output_path : NULL;
}

download_job_state_t download_job_get_state(const download_job_t *job) {
    if (!job) return JOB_ERROR;
    pthread_mutex_lock((pthread_mutex_t*)&job->mtx);
    download_job_state_t st = job->state;
    pthread_mutex_unlock((pthread_mutex_t*)&job->mtx);
    return st;
}

int download_job_result(const download_job_t *job) {
    if (!job) return -1;
    pthread_mutex_lock((pthread_mutex_t*)&job->mtx);
    int res = job->result;
    pthread_mutex_unlock((pthread_mutex_t*)&job->mtx);
    return res;
}

int download_job_get_meta(const download_job_t *job, download_job_meta_t *out) {
    if (!job || !out) return -1;
    memset(out, 0, sizeof(*out));

    pthread_mutex_lock((pthread_mutex_t*)&job->mtx);
    out->url = job->url ? strdup(job->url) : NULL;
    out->output_path = job->output_path ? strdup(job->output_path) : NULL;
    out->P = job->P;
    out->T = job->T;
    if (job->weights && job->P > 0) {
        out->weights = (int*)calloc((size_t)job->P, sizeof(int));
        if (out->weights) {
            memcpy(out->weights, job->weights, (size_t)job->P * sizeof(int));
        }
    }
    out->file_size = job->file_size;
    out->range_supported = job->range_supported;
    out->block_size = job->cfg.block_size;
    out->max_retries_per_block = job->cfg.max_retries_per_block;
    out->state = job->state;
    pthread_mutex_unlock((pthread_mutex_t*)&job->mtx);

    if (!out->url || !out->output_path) {
        download_job_meta_free(out);
        return -1;
    }
    return 0;
}

void download_job_meta_free(download_job_meta_t *meta) {
    if (!meta) return;
    free(meta->url);
    free(meta->output_path);
    free(meta->weights);
    memset(meta, 0, sizeof(*meta));
}

int download_job_save_state(const download_job_t *job, const char *state_path) {
    if (!job || !state_path) return -1;

    int64_t *next = NULL;
    int64_t *dl = NULL;
    int P = 0;
    int64_t size = 0;
    if (job_snapshot_state((download_job_t*)job, &next, &dl, &P, &size) != 0) {
        return -1;
    }

    FILE *f = fopen(state_path, "w");
    if (!f) {
        free(next);
        free(dl);
        return -1;
    }

    fprintf(f, "version=1\n");
    fprintf(f, "file_size=%" PRId64 "\n", size);
    fprintf(f, "P=%d\n", P);
    for (int i = 0; i < P; i++) {
        fprintf(f, "part%d_next=%" PRId64 "\n", i, next[i]);
        fprintf(f, "part%d_downloaded=%" PRId64 "\n", i, dl[i]);
    }
    fflush(f);
    fsync(fileno(f));
    fclose(f);

    free(next);
    free(dl);
    return 0;
}

int download_job_load_state(download_job_t *job, const char *state_path) {
    if (!job || !state_path) return -1;

    FILE *f = fopen(state_path, "r");
    if (!f) return -1;

    char line[256];
    int P = 0;
    int64_t size = 0;
    int64_t *next = NULL;
    int64_t *dl = NULL;

    while (fgets(line, sizeof(line), f)) {
        trim_newline(line);
        if (strncmp(line, "P=", 2) == 0) {
            P = atoi(line + 2);
            if (P > 0) {
                next = (int64_t*)calloc((size_t)P, sizeof(int64_t));
                dl = (int64_t*)calloc((size_t)P, sizeof(int64_t));
                if (!next || !dl) {
                    free(next);
                    free(dl);
                    fclose(f);
                    return -1;
                }
            }
        } else if (strncmp(line, "file_size=", 10) == 0) {
            size = strtoll(line + 10, NULL, 10);
        } else if (strncmp(line, "part", 4) == 0) {
            if (!next || !dl || P <= 0) continue;
            int idx = -1;
            char *eq = strchr(line, '=');
            if (!eq) continue;
            if (sscanf(line, "part%d_next", &idx) == 1 && idx >= 0 && idx < P) {
                next[idx] = strtoll(eq + 1, NULL, 10);
            } else if (sscanf(line, "part%d_downloaded", &idx) == 1 && idx >= 0 && idx < P) {
                dl[idx] = strtoll(eq + 1, NULL, 10);
            }
        }
    }
    fclose(f);

    if (P <= 0 || !next || !dl) {
        free(next);
        free(dl);
        return -1;
    }

    pthread_mutex_lock(&job->mtx);
    job_clear_resume(job);
    job->resume_loaded = 1;
    job->resume_P = P;
    job->resume_file_size = size;
    job->resume_next_off = next;
    job->resume_downloaded = dl;
    if (job->file_size <= 0 && size > 0) {
        job->file_size = size;
    }
    pthread_mutex_unlock(&job->mtx);

    return 0;
}
