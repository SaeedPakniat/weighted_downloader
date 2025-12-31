#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "downloader.h"
#include "gui.h"
#include "job_manager.h"
#include "util.h"

static void usage(const char *prog) {
    fprintf(stderr,
        "Usage:\n"
        "  %s [--no-ui] [--gui] <URL> [output_file]\n"
        "  %s [--no-ui] [--gui]\n\n"
        "When no URL is provided, the app resumes jobs from downloads.json.\n"
        "You will be prompted for partitions/weights and optional worker threads.\n",
        prog, prog);
}

int main(int argc, char **argv) {
    int argi = 1;
    int ui_enabled = 1;
    int output_enabled = 1;
    int gui_enabled = 0;
    int no_ui = 0;
    while (argi < argc && strncmp(argv[argi], "--", 2) == 0) {
        if (strcmp(argv[argi], "--no-ui") == 0) {
            no_ui = 1;
            argi++;
        } else if (strcmp(argv[argi], "--gui") == 0) {
            gui_enabled = 1;
            argi++;
        } else {
            usage(argv[0]);
            return 1;
        }
    }
    if (no_ui && gui_enabled) {
        fprintf(stderr, "Error: --no-ui and --gui are mutually exclusive.\n");
        return 1;
    }
    if (no_ui) {
        ui_enabled = 0;
        output_enabled = 1;
    }
    if (gui_enabled) {
        ui_enabled = 0;
        output_enabled = 0;
    }
    int have_url = (argc - argi >= 1);
    const char *url = have_url ? argv[argi] : NULL;
    const char *out = have_url ? ((argc - argi >= 2) ? argv[argi + 1] : "output.bin") : NULL;

    if (curl_global_init(CURL_GLOBAL_DEFAULT) != 0) {
        fprintf(stderr, "curl_global_init failed\n");
        return 1;
    }

    downloader_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.block_size = DEFAULT_BLOCK_SIZE;
    cfg.max_retries_per_block = DEFAULT_MAX_RETRIES;
    cfg.connect_timeout_sec = 10;
    cfg.low_speed_time_sec = 15;
    cfg.low_speed_limit_bytes = 1024; // 1KB/s for 15s triggers timeout
    cfg.progress_interval_ms = 200;
    cfg.csv_sample_interval_ms = 200;
    cfg.ui_enabled = ui_enabled;
    cfg.output_enabled = output_enabled;
    cfg.gui_enabled = 0;

    job_manager_t jm;
    if (job_manager_init(&jm, 1) != 0) {
        fprintf(stderr, "Failed to initialize job manager.\n");
        curl_global_cleanup();
        return 1;
    }
    job_manager_enable_persistence(&jm, "downloads.json", 1000);

    int loaded = 0;
    job_manager_load_persisted(&jm, "downloads.json", &cfg, &loaded);

    if (have_url) {
        printf("URL: %s\nOutput: %s\n", url, out);
        download_job_t *job = downloader_prepare_job_interactive(url, out, &cfg);
        if (!job) {
            fprintf(stderr, "Failed to configure download job.\n");
            if (loaded == 0) {
                job_manager_destroy(&jm);
                curl_global_cleanup();
                return 1;
            }
        } else {
            job_manager_add(&jm, job);
        }
    }

    if (!gui_enabled && !have_url && loaded == 0) {
        fprintf(stderr, "No persisted downloads to resume.\n");
        job_manager_destroy(&jm);
        curl_global_cleanup();
        return 1;
    }

    int overall_ok = 1;
    if (gui_enabled) {
        gui_removed_jobs_t removed = {0};
        job_manager_start(&jm);
        if (gui_run(&jm, &cfg, &removed) != 0) {
            fprintf(stderr, "GUI exited with errors.\n");
            overall_ok = 0;
        }
        job_manager_stop(&jm);
        job_manager_wait_all(&jm);

        int count = job_manager_count(&jm);
        for (int i = 0; i < count; i++) {
            download_job_t *job = job_manager_get(&jm, i);
            if (download_job_result(job) != 0) overall_ok = 0;
            download_job_destroy(job);
        }
        for (size_t i = 0; i < removed.count; i++) {
            if (download_job_result(removed.jobs[i]) != 0) overall_ok = 0;
            download_job_destroy(removed.jobs[i]);
        }
        free(removed.jobs);
    } else {
        job_manager_start(&jm);
        job_manager_wait_all(&jm);
        job_manager_stop(&jm);

        int count = job_manager_count(&jm);
        for (int i = 0; i < count; i++) {
            download_job_t *job = job_manager_get(&jm, i);
            if (download_job_result(job) != 0) overall_ok = 0;
            download_job_destroy(job);
        }
    }
    job_manager_destroy(&jm);

    curl_global_cleanup();

    if (!overall_ok) {
        fprintf(stderr, "One or more downloads failed.\n");
        return 1;
    }

    printf("Download manager finished.\n");
    return 0;
}
