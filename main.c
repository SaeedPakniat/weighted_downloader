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
        "  %s <URL> [output_file]\n\n"
        "You will be prompted for partitions/weights and optional worker threads.\n",
        prog);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }

    const char *url = argv[1];
    const char *out = (argc >= 3) ? argv[2] : "output.bin";

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
