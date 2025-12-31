#pragma once

#include <stddef.h>

#include "downloader.h"
#include "job_manager.h"

typedef struct gui_removed_jobs {
    download_job_t **jobs;
    size_t count;
} gui_removed_jobs_t;

int gui_run(job_manager_t *jm, const downloader_config_t *cfg, gui_removed_jobs_t *out_removed);
