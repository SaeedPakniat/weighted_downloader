
#pragma once

#include <stddef.h>
#include <stdint.h>

#include <curl/curl.h>

#define DEFAULT_BLOCK_SIZE (128 * 1024)
#define DEFAULT_MAX_RETRIES 5

typedef struct downloader_config {
    size_t block_size;
    int max_retries_per_block;

    long connect_timeout_sec;
    long low_speed_time_sec;
    long low_speed_limit_bytes;

    int progress_interval_ms;
    int csv_sample_interval_ms;
} downloader_config_t;

int downloader_run_interactive(const char *url, const char *output_path,
                               const downloader_config_t *cfg);
