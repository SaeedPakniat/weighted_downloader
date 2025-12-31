#pragma once

#include <stdint.h>
#include <stddef.h>

#include "downloader.h"

typedef struct download_job download_job_t;

typedef enum download_job_state {
    JOB_IDLE = 0,
    JOB_RUNNING,
    JOB_PAUSED,
    JOB_STOPPING,
    JOB_STOPPED,
    JOB_DONE,
    JOB_ERROR
} download_job_state_t;

typedef struct download_job_meta {
    char *url;
    char *output_path;
    int P;
    int T;
    int *weights;
    int64_t file_size;
    int range_supported;
    size_t block_size;
    int max_retries_per_block;
    download_job_state_t state;
} download_job_meta_t;

download_job_t *download_job_create(const char *url, const char *output_path,
                                    const downloader_config_t *cfg);
void download_job_destroy(download_job_t *job);

int download_job_set_scheduling(download_job_t *job, int P, const int *weights, int T);
void download_job_set_probe(download_job_t *job, int64_t size, int range_supported);
int download_job_probe(const download_job_t *job, int64_t *out_size, int *out_range_supported);

int download_job_start(download_job_t *job);
int download_job_pause(download_job_t *job);
int download_job_resume(download_job_t *job);
int download_job_stop(download_job_t *job);
int download_job_join(download_job_t *job);
int download_job_set_start_paused(download_job_t *job, int start_paused);

const char *download_job_url(const download_job_t *job);
const char *download_job_output_path(const download_job_t *job);

download_job_state_t download_job_get_state(const download_job_t *job);
int download_job_result(const download_job_t *job);

int download_job_get_meta(const download_job_t *job, download_job_meta_t *out);
void download_job_meta_free(download_job_meta_t *meta);

int download_job_save_state(const download_job_t *job, const char *state_path);
int download_job_load_state(download_job_t *job, const char *state_path);
