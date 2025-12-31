
#define _POSIX_C_SOURCE 200809L

#include "progress.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <inttypes.h>

#include "util.h"

static void print_bar(double frac, int width) {
    int filled = (int)(frac * width);
    if (filled < 0) filled = 0;
    if (filled > width) filled = width;
    putchar('[');
    for (int i = 0; i < width; i++) putchar(i < filled ? '#' : ' ');
    putchar(']');
}

static void *progress_thread_main(void *argp) {
    progress_ctx_t *p = (progress_ctx_t*)argp;

    int64_t *per = (int64_t*)calloc((size_t)p->P, sizeof(int64_t));
    int *fin = (int*)calloc((size_t)p->P, sizeof(int));
    if (!per || !fin) return NULL;

    while (!p->stop_flag) {
        int64_t total = 0;
        scheduler_snapshot(p->sched, per, fin, &total);

        // Clear-ish output
        printf("\033[2J\033[H"); // ANSI clear screen + home
        printf("Weighted Parallel Downloader (DRR)\n");
        printf("Total: %" PRId64 " / %" PRId64 " bytes (%.2f%%)\n\n",
               total, p->file_size, 100.0 * (double)total / (double)p->file_size);

        for (int i = 0; i < p->P; i++) {
            double frac = (p->sched->parts[i].total_bytes > 0)
                ? (double)per[i] / (double)p->sched->parts[i].total_bytes
                : 1.0;
            printf("Part %2d (w=%d): ", i, p->sched->parts[i].weight);
            print_bar(frac, 30);
            printf("  %" PRId64 "/%" PRId64 " (%.2f%%)%s\n",
                   per[i], p->sched->parts[i].total_bytes, 100.0 * frac,
                   fin[i] ? " DONE" : "");
        }

        fflush(stdout);

        if (scheduler_all_done(p->sched)) break;
        msleep(p->progress_interval_ms);
    }

    free(per);
    free(fin);
    return NULL;
}

static void *csv_thread_main(void *argp) {
    progress_ctx_t *p = (progress_ctx_t*)argp;

    int64_t *per = (int64_t*)calloc((size_t)p->P, sizeof(int64_t));
    if (!per) return NULL;

    while (!p->stop_flag) {
        scheduler_snapshot(p->sched, per, NULL, NULL);
        int64_t ts = now_millis() - p->start_ms;

        // Write one row per partition each sample.
        // CSV format: timestamp_ms, partition_id, bytes_downloaded_total
        for (int i = 0; i < p->P; i++) {
            fprintf(p->csv, "%" PRId64 ",%d,%" PRId64 "\n", ts, i, per[i]);
        }
        fflush(p->csv);

        if (scheduler_all_done(p->sched)) break;
        msleep(p->csv_sample_interval_ms);
    }

    free(per);
    return NULL;
}

int progress_init(progress_ctx_t *p, scheduler_t *sched, int P, int64_t file_size,
                  int progress_interval_ms, int csv_sample_interval_ms) {
    memset(p, 0, sizeof(*p));
    p->sched = sched;
    p->P = P;
    p->file_size = file_size;
    p->progress_interval_ms = progress_interval_ms;
    p->csv_sample_interval_ms = csv_sample_interval_ms;
    p->stop_flag = 0;
    p->csv = NULL;
    p->start_ms = now_millis();
    return 0;
}

int progress_start(progress_ctx_t *p, const char *csv_path) {
    strncpy(p->csv_path, csv_path, sizeof(p->csv_path) - 1);
    p->csv = fopen(p->csv_path, "w");
    if (!p->csv) {
        perror("fopen csv");
        return -1;
    }
    fprintf(p->csv, "timestamp_ms,partition_id,bytes_downloaded_total\n");
    fflush(p->csv);

    p->start_ms = now_millis();

    if (pthread_create(&p->thread, NULL, progress_thread_main, p) != 0) {
        fclose(p->csv);
        p->csv = NULL;
        return -1;
    }
    if (pthread_create(&p->csv_thread, NULL, csv_thread_main, p) != 0) {
        p->stop_flag = 1;
        pthread_join(p->thread, NULL);
        fclose(p->csv);
        p->csv = NULL;
        return -1;
    }
    return 0;
}

void progress_stop(progress_ctx_t *p) {
    if (!p) return;
    p->stop_flag = 1;
    // Wake workers potentially sleeping in scheduler
    pthread_mutex_lock(&p->sched->mtx);
    pthread_cond_broadcast(&p->sched->cv);
    pthread_mutex_unlock(&p->sched->mtx);

    pthread_join(p->thread, NULL);
    pthread_join(p->csv_thread, NULL);
}

void progress_destroy(progress_ctx_t *p) {
    if (!p) return;
    if (p->csv) {
        fclose(p->csv);
        p->csv = NULL;
    }
}
