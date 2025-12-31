#define _POSIX_C_SOURCE 200809L

#include "downloader.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "job.h"
#include "util.h"

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

download_job_t *downloader_prepare_job_interactive(const char *url, const char *output_path,
                                                   const downloader_config_t *cfg) {
    int64_t size = -1;
    int range_supported = 0;

    download_job_t *job = download_job_create(url, output_path, cfg);
    if (!job) return NULL;

    if (download_job_probe(job, &size, &range_supported) != 0) {
        size = -1;
        range_supported = 0;
    }
    download_job_set_probe(job, size, range_supported);

    if (size <= 0) {
        fprintf(stderr, "Could not determine Content-Length. Cannot partition safely.\n");
    }

    if (size > 0 && range_supported) {
        int P = prompt_int("Enter number of partitions P", 1, 64, 4, 1);
        if (P == 1) {
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

        if (download_job_set_scheduling(job, P, weights, T) != 0) {
            fprintf(stderr, "Failed to configure scheduling.\n");
            free(weights);
            download_job_destroy(job);
            return NULL;
        }
        free(weights);
    }

    return job;
}

int downloader_run_interactive(const char *url, const char *output_path,
                               const downloader_config_t *cfg) {
    download_job_t *job = downloader_prepare_job_interactive(url, output_path, cfg);
    if (!job) return -1;

    if (download_job_start(job) != 0) {
        fprintf(stderr, "Failed to start download job.\n");
        download_job_destroy(job);
        return -1;
    }

    download_job_join(job);
    int rc = download_job_result(job);
    download_job_destroy(job);
    return rc;
}
