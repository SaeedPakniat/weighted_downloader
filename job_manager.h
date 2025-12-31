#pragma once

#include <pthread.h>
#include <stdint.h>

#include "job.h"

typedef struct job_manager {
    pthread_mutex_t mtx;
    pthread_cond_t cv;

    download_job_t **jobs;
    size_t count;
    size_t cap;

    int max_active;
    int stop_flag;
    int dirty;
    int persist_dirty;

    char *persist_path;
    int persist_interval_ms;
    int persist_enabled;
    int64_t last_persist_ms;

    pthread_t thread;
    int thread_started;
} job_manager_t;

int job_manager_init(job_manager_t *jm, int max_active);
void job_manager_destroy(job_manager_t *jm);

int job_manager_start(job_manager_t *jm);
void job_manager_stop(job_manager_t *jm);

int job_manager_set_max_active(job_manager_t *jm, int max_active);
int job_manager_add(job_manager_t *jm, download_job_t *job);

download_job_t *job_manager_get(job_manager_t *jm, int job_id);
int job_manager_count(job_manager_t *jm);
int job_manager_wait_all(job_manager_t *jm);
int job_manager_remove(job_manager_t *jm, download_job_t *job);

int job_manager_enable_persistence(job_manager_t *jm, const char *json_path, int interval_ms);
int job_manager_load_persisted(job_manager_t *jm, const char *json_path,
                               const downloader_config_t *cfg, int *out_loaded);

int job_manager_pause(job_manager_t *jm, int job_id);
int job_manager_resume(job_manager_t *jm, int job_id);
int job_manager_stop_job(job_manager_t *jm, int job_id);
