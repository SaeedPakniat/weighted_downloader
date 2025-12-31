#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "downloader.h"
#include "util.h"

static void usage(const char *prog) {
    fprintf(stderr,
        "Usage:\n"
        "  %s [--no-ui] [--gui] <URL> [output_file]\n\n"
        "You will be prompted for partitions/weights and optional worker threads.\n",
        prog);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }

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
    if (argc - argi < 1) {
        usage(argv[0]);
        return 1;
    }

    const char *url = argv[argi];
    const char *out = (argc - argi >= 2) ? argv[argi + 1] : "output.bin";

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
    cfg.gui_enabled = gui_enabled;

    printf("URL: %s\nOutput: %s\n", url, out);

    int rc = downloader_run_interactive(url, out, &cfg);

    curl_global_cleanup();

    if (rc != 0) {
        fprintf(stderr, "Download failed.\n");
        return 1;
    }

    printf("Download completed successfully.\n");
    return 0;
}
