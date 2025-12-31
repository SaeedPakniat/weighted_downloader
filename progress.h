
#pragma once

#include <stdint.h>
#include <pthread.h>
#include <stdio.h>

#include "scheduler.h"

typedef struct progress_ctx {
    scheduler_t *sched;
    int P;
    int64_t file_size;

    int progress_interval_ms;
    int csv_sample_interval_ms;

    pthread_t thread;
    pthread_t csv_thread;

    int stop_flag;

    char csv_path[256];
    FILE *csv;

    int64_t start_ms;
} progress_ctx_t;

int progress_init(progress_ctx_t *p, scheduler_t *sched, int P, int64_t file_size,
                  int progress_interval_ms, int csv_sample_interval_ms);

int progress_start(progress_ctx_t *p, const char *csv_path);
void progress_stop(progress_ctx_t *p);
void progress_destroy(progress_ctx_t *p);
