
#define _POSIX_C_SOURCE 200809L

#include "downloader.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <inttypes.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

#include "scheduler.h"
#include "progress.h"
#include "gui.h"
#include "util.h"

typedef struct head_info {
    long response_code;
    int accept_ranges_bytes;
    int content_length_known;
    int64_t content_length;
} head_info_t;

static size_t header_cb(char *buffer, size_t size, size_t nitems, void *userdata) {
    size_t len = size * nitems;
    head_info_t *hi = (head_info_t *)userdata;

    // header lines are not null-terminated
    // We scan for:
    //   Accept-Ranges: bytes
    //   Content-Length: N
    if (len > 0) {
        // make a temporary null-terminated copy (bounded)
        char tmp[512];
        size_t cpy = (len < sizeof(tmp) - 1) ? len : sizeof(tmp) - 1;
        memcpy(tmp, buffer, cpy);
        tmp[cpy] = '\0';

        // case-insensitive-ish checks (simple)
        if (strncasecmp(tmp, "Accept-Ranges:", 14) == 0) {
            if (strstr(tmp, "bytes") != NULL || strstr(tmp, "Bytes") != NULL) {
                hi->accept_ranges_bytes = 1;
            }
        } else if (strncasecmp(tmp, "Content-Length:", 15) == 0) {
            // parse integer after colon
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

    // Some servers may not allow HEAD properly; we still might proceed with GET probe.
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
    // Probe by requesting bytes=0-0. If server returns 206, range is supported.
    // Also attempt to obtain size via CURLINFO_CONTENT_LENGTH_DOWNLOAD_T.
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
            return 0; // abort transfer
        }
        ctx->cur_off += (int64_t)w;
        p += (size_t)w;
        left -= (size_t)w;
    }
    return total;
}

static int single_thread_download(const char *url, const char *output_path, const downloader_config_t *cfg) {
    printf("[Fallback] Server does not support Range requests. Using single-thread download.\n");

    int fd = open(output_path, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (fd < 0) {
        fprintf(stderr, "open(%s) failed: %s\n", output_path, strerror(errno));
        return -1;
    }

    CURL *curl = curl_easy_init();
    if (!curl) {
        close(fd);
        return -1;
    }

    pwrite_ctx_t wctx = {.fd = fd, .cur_off = 0, .error = 0};
    long code = 0;

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, pwrite_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &wctx);

    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, cfg->connect_timeout_sec);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, cfg->low_speed_time_sec);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, cfg->low_speed_limit_bytes);

    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
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

static int prompt_int(const char *label, int minv, int maxv, int defv, int allow_default) {
    char buf[128];
    while (1) {
        if (allow_default) {
            printf("%s [%d..%d] (default %d): ", label, minv, maxv, defv);
        } else {
            printf("%s [%d..%d]: ", label, minv, maxv);
        }
        fflush(stdout);

        if (!fgets(buf, sizeof(buf), stdin)) return defv;
        trim_newline(buf);

        if (allow_default && buf[0] == '\0') return defv;

        char *endp = NULL;
        long v = strtol(buf, &endp, 10);
        if (endp != buf && *endp == '\0' && v >= minv && v <= maxv) return (int)v;

        printf("Invalid input.\n");
    }
}

int downloader_run_interactive(const char *url, const char *output_path,
                               const downloader_config_t *cfg) {
    head_info_t hi;
    int head_ok = (http_head_probe(url, cfg, &hi) == 0);

    int64_t size = -1;
    int range_supported = 0;

    if (head_ok && hi.content_length_known) size = hi.content_length;
    if (head_ok && hi.accept_ranges_bytes) range_supported = 1;

    // If not clearly supported, probe with 0-0 range request.
    if (!range_supported) {
        int64_t probe_size = -1;
        int ok = http_range_probe_0_0(url, cfg, &probe_size);
        if (ok) {
            range_supported = 1;
        }
        if (size < 0 && probe_size > 0) size = probe_size;
    }

    if (size <= 0) {
        fprintf(stderr, "Could not determine Content-Length. Cannot partition safely.\n");
        // Still can download single-thread (chunked transfer).
        return single_thread_download(url, output_path, cfg);
    }

    if (!range_supported) {
        return single_thread_download(url, output_path, cfg);
    }

    printf("Server supports Range requests. Content-Length = %" PRId64 " bytes\n", size);

    int P = prompt_int("Enter number of partitions P", 1, 64, 4, 1);
    if (P == 1) {
        // You asked for partitions; but if user chooses 1, just do single partition threaded.
        printf("Using a single partition (still uses worker pool if T>1, but scheduling is trivial).\n");
    }

    int *weights = (int*)xcalloc((size_t)P, sizeof(int));
    for (int i = 0; i < P; i++) {
        char label[128];
        snprintf(label, sizeof(label), "Weight for partition %d", i);
        weights[i] = prompt_int(label, 1, 10, 1, 1);
    }

    int default_T = (P < 8) ? P : 8;
    int T = prompt_int("Enter number of worker threads T", 1, 128, default_T, 1);

    int64_t *part_sizes = NULL;
    if (cfg->gui_enabled) {
        part_sizes = (int64_t*)xcalloc((size_t)P, sizeof(int64_t));
        int64_t base = size / P;
        int64_t rem = size % P;
        for (int i = 0; i < P; i++) {
            part_sizes[i] = base + (i < rem ? 1 : 0);
        }
    }

    // Pre-create output file and allocate size to avoid surprises.
    int fd = open(output_path, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (fd < 0) {
        fprintf(stderr, "open(%s) failed: %s\n", output_path, strerror(errno));
        free(weights);
        free(part_sizes);
        return -1;
    }
    if (ftruncate(fd, (off_t)size) != 0) {
        fprintf(stderr, "ftruncate failed: %s\n", strerror(errno));
        close(fd);
        free(weights);
        free(part_sizes);
        return -1;
    }

    // Build scheduler and partition table
    scheduler_t sched;
    if (scheduler_init(&sched, P, size, (size_t)cfg->block_size, weights, cfg->max_retries_per_block) != 0) {
        fprintf(stderr, "scheduler_init failed\n");
        close(fd);
        free(weights);
        free(part_sizes);
        return -1;
    }

    // Create progress/logging threads
    progress_ctx_t pctx;
    if (progress_init(&pctx, &sched, P, size, cfg->progress_interval_ms,
                      cfg->csv_sample_interval_ms, cfg->ui_enabled, cfg->output_enabled) != 0) {
        fprintf(stderr, "progress_init failed\n");
        scheduler_destroy(&sched);
        close(fd);
        free(weights);
        free(part_sizes);
        return -1;
    }

    char csv_name[256];
    snprintf(csv_name, sizeof(csv_name), "weights_log_%lld.csv", (long long)now_millis());

    if (progress_start(&pctx, csv_name) != 0) {
        fprintf(stderr, "progress_start failed\n");
        progress_destroy(&pctx);
        scheduler_destroy(&sched);
        close(fd);
        free(weights);
        free(part_sizes);
        return -1;
    }

    // Spawn workers
    worker_pool_t pool;
    if (worker_pool_start(&pool, &sched, url, fd, T, cfg) != 0) {
        fprintf(stderr, "worker_pool_start failed\n");
        scheduler_signal_shutdown(&sched);
        progress_stop(&pctx);
        progress_destroy(&pctx);
        scheduler_destroy(&sched);
        close(fd);
        free(weights);
        free(part_sizes);
        return -1;
    }

    int gui_rc = 0;
    if (cfg->gui_enabled) {
        gui_rc = gui_run(&sched, P, size, weights, part_sizes);
        if (gui_rc != 0) {
            fprintf(stderr, "gui_run failed\n");
            scheduler_signal_shutdown(&sched);
        }
    }

    // Wait for workers to finish
    worker_pool_join(&pool);

    // Stop progress threads
    progress_stop(&pctx);
    progress_destroy(&pctx);

    int ok = scheduler_all_done(&sched);
    scheduler_destroy(&sched);

    if (fsync(fd) != 0) {
        fprintf(stderr, "fsync failed: %s\n", strerror(errno));
    }
    close(fd);
    free(weights);
    free(part_sizes);

    if (gui_rc != 0 || !ok) {
        fprintf(stderr, "Not all partitions completed.\n");
        return -1;
    }

    printf("CSV log written: %s\n", csv_name);
    printf("Tip: verify correctness with: sha256sum %s\n", output_path);
    return 0;
}
