
#define _POSIX_C_SOURCE 200809L

#include "progress.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <inttypes.h>

#include <ncurses.h>

#include "util.h"

static void print_bar(double frac, int width) {
    int filled = (int)(frac * width);
    if (filled < 0) filled = 0;
    if (filled > width) filled = width;
    putchar('[');
    for (int i = 0; i < width; i++) putchar(i < filled ? '#' : ' ');
    putchar(']');
}

static void format_bytes(char *buf, size_t buflen, int64_t bytes) {
    const char *units[] = {"B", "KB", "MB", "GB", "TB"};
    double v = (double)bytes;
    int u = 0;
    while (v >= 1024.0 && u < 4) {
        v /= 1024.0;
        u++;
    }
    snprintf(buf, buflen, "%.2f%s", v, units[u]);
}

static void format_eta(char *buf, size_t buflen, double seconds) {
    if (seconds < 0 || seconds > 99 * 3600) {
        snprintf(buf, buflen, "--:--");
        return;
    }
    int sec = (int)(seconds + 0.5);
    int h = sec / 3600;
    int m = (sec % 3600) / 60;
    int s = sec % 60;
    if (h > 0) {
        snprintf(buf, buflen, "%02d:%02d:%02d", h, m, s);
    } else {
        snprintf(buf, buflen, "%02d:%02d", m, s);
    }
}

static void render_bar(char *buf, size_t buflen, double frac, int width) {
    if (width < 3) width = 3;
    if ((size_t)width + 1 > buflen) width = (int)buflen - 1;
    int filled = (int)(frac * (width - 2));
    if (filled < 0) filled = 0;
    if (filled > width - 2) filled = width - 2;
    int pos = 0;
    buf[pos++] = '[';
    for (int i = 0; i < width - 2; i++) buf[pos++] = (i < filled) ? '#' : ' ';
    buf[pos++] = ']';
    buf[pos] = '\0';
}

static void draw_tui(progress_ctx_t *p, const int64_t *per, const int *fin, int64_t total,
                     double smoothed_bps) {
    int rows = 0, cols = 0;
    getmaxyx(stdscr, rows, cols);
    erase();

    int sum_weights = 0;
    for (int i = 0; i < p->P; i++) sum_weights += p->sched->parts[i].weight;

    int paused = scheduler_is_paused(p->sched);
    int row = 0;
    mvprintw(row++, 0, "Weighted Parallel Downloader (DRR)%s",
             paused ? "  [PAUSED]" : "");

    double frac = (p->file_size > 0) ? (double)total / (double)p->file_size : 0.0;
    if (frac < 0.0) frac = 0.0;
    if (frac > 1.0) frac = 1.0;

    char total_buf[32], size_buf[32], speed_buf[32], eta_buf[32], bar[128];
    format_bytes(total_buf, sizeof(total_buf), total);
    format_bytes(size_buf, sizeof(size_buf), p->file_size);
    if (smoothed_bps > 0.0) {
        format_bytes(speed_buf, sizeof(speed_buf), (int64_t)smoothed_bps);
    } else {
        snprintf(speed_buf, sizeof(speed_buf), "--");
    }
    double eta_sec = (smoothed_bps > 1.0) ? ((double)(p->file_size - total) / smoothed_bps) : -1.0;
    format_eta(eta_buf, sizeof(eta_buf), eta_sec);

    int bar_width = cols - 40;
    if (bar_width < 10) bar_width = 10;
    if (bar_width > 60) bar_width = 60;
    render_bar(bar, sizeof(bar), frac, bar_width);

    mvprintw(row++, 0, "Overall: %s %6.2f%%  %s / %s", bar, 100.0 * frac, total_buf, size_buf);
    mvprintw(row++, 0, "Speed: %s/s  ETA: %s", speed_buf, eta_buf);
    row++;

    for (int i = 0; i < p->P && row < rows - 1; i++) {
        double pfrac = (p->sched->parts[i].total_bytes > 0)
            ? (double)per[i] / (double)p->sched->parts[i].total_bytes
            : 1.0;
        if (pfrac < 0.0) pfrac = 0.0;
        if (pfrac > 1.0) pfrac = 1.0;

        char pbar[64], cur_buf[24], tot_buf[24];
        render_bar(pbar, sizeof(pbar), pfrac, 24);
        format_bytes(cur_buf, sizeof(cur_buf), per[i]);
        format_bytes(tot_buf, sizeof(tot_buf), p->sched->parts[i].total_bytes);

        double achieved = (total > 0) ? (100.0 * (double)per[i] / (double)total) : 0.0;
        double expected = (sum_weights > 0)
            ? (100.0 * (double)p->sched->parts[i].weight / (double)sum_weights)
            : 0.0;

        char line[512];
        snprintf(line, sizeof(line),
                 "P%02d w=%d %s %6.2f%%  %s/%s  share %.1f/%.1f%s",
                 i, p->sched->parts[i].weight, pbar, 100.0 * pfrac,
                 cur_buf, tot_buf, achieved, expected, fin && fin[i] ? " DONE" : "");
        mvprintw(row++, 0, "%.*s", cols - 1, line);
    }

    mvprintw(rows - 1, 0, "Controls: p=pause  r=resume  q=quit");
    refresh();
}

static void *progress_thread_main(void *argp) {
    progress_ctx_t *p = (progress_ctx_t*)argp;

    int64_t *per = (int64_t*)calloc((size_t)p->P, sizeof(int64_t));
    int *fin = (int*)calloc((size_t)p->P, sizeof(int));
    if (!per || !fin) return NULL;

    int64_t last_total = 0;
    int64_t last_ms = now_millis();
    double smoothed_bps = 0.0;

    if (p->ui_enabled) {
        initscr();
        cbreak();
        noecho();
        curs_set(0);
        nodelay(stdscr, TRUE);
        keypad(stdscr, TRUE);
    }

    while (!p->stop_flag) {
        int64_t total = 0;
        scheduler_snapshot(p->sched, per, fin, &total);

        int64_t now = now_millis();
        double dt = (double)(now - last_ms) / 1000.0;
        if (dt > 0.0) {
            double inst = (double)(total - last_total) / dt;
            if (inst < 0.0) inst = 0.0;
            if (smoothed_bps <= 0.0) smoothed_bps = inst;
            else smoothed_bps = 0.2 * inst + 0.8 * smoothed_bps;
            last_total = total;
            last_ms = now;
        }

        if (p->ui_enabled) {
            int ch = getch();
            if (ch == 'p' || ch == 'P') {
                scheduler_set_paused(p->sched, 1);
            } else if (ch == 'r' || ch == 'R') {
                scheduler_set_paused(p->sched, 0);
            } else if (ch == 'q' || ch == 'Q') {
                p->stop_flag = 1;
                scheduler_signal_shutdown(p->sched);
            }
            draw_tui(p, per, fin, total, smoothed_bps);
        } else {
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
        }

        if (scheduler_all_done(p->sched)) break;
        msleep(p->progress_interval_ms);
    }

    if (p->ui_enabled) {
        endwin();
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
                  int progress_interval_ms, int csv_sample_interval_ms, int ui_enabled) {
    memset(p, 0, sizeof(*p));
    p->sched = sched;
    p->P = P;
    p->file_size = file_size;
    p->progress_interval_ms = progress_interval_ms;
    p->csv_sample_interval_ms = csv_sample_interval_ms;
    p->ui_enabled = ui_enabled;
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
