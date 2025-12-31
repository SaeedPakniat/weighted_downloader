#define _POSIX_C_SOURCE 200809L

#include "gui.h"

#include <gtk/gtk.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"

typedef struct gui_part_row {
    GtkProgressBar *bar;
    GtkLabel *meta_label;
    GtkLabel *bytes_label;
    GtkLabel *share_label;
} gui_part_row_t;

typedef struct gui_ctx {
    scheduler_t *sched;
    int P;
    int64_t file_size;
    int64_t *part_sizes;
    int *weights;
    int sum_weights;

    GtkApplication *app;
    GtkWindow *window;
    GtkProgressBar *overall_bar;
    GtkLabel *overall_label;
    GtkLabel *speed_label;
    GtkLabel *eta_label;
    GtkLabel *status_label;
    GtkDrawingArea *chart;

    gui_part_row_t *rows;

    int64_t *per;
    int *fin;

    int64_t last_total;
    int64_t last_ms;
    double smoothed_bps;

    double *share_history;
    int history_len;
    int history_pos;
    int history_count;

    guint timer_id;
    int quit_requested;
} gui_ctx_t;

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

static void gui_request_quit(gui_ctx_t *ctx) {
    if (ctx->quit_requested) return;
    ctx->quit_requested = 1;
    scheduler_signal_shutdown(ctx->sched);
    if (ctx->app) g_application_quit(G_APPLICATION(ctx->app));
}

static void on_pause_clicked(GtkButton *btn, gpointer data) {
    (void)btn;
    gui_ctx_t *ctx = (gui_ctx_t*)data;
    scheduler_set_paused(ctx->sched, 1);
}

static void on_resume_clicked(GtkButton *btn, gpointer data) {
    (void)btn;
    gui_ctx_t *ctx = (gui_ctx_t*)data;
    scheduler_set_paused(ctx->sched, 0);
}

static void on_quit_clicked(GtkButton *btn, gpointer data) {
    (void)btn;
    gui_ctx_t *ctx = (gui_ctx_t*)data;
    gui_request_quit(ctx);
}

static gboolean on_close_request(GtkWindow *window, gpointer data) {
    (void)window;
    gui_ctx_t *ctx = (gui_ctx_t*)data;
    gui_request_quit(ctx);
    return FALSE;
}

static void chart_draw(GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer data) {
    (void)area;
    gui_ctx_t *ctx = (gui_ctx_t*)data;
    if (ctx->history_count < 2 || ctx->P <= 0) return;

    double pad = 8.0;
    double w = (double)width - 2.0 * pad;
    double h = (double)height - 2.0 * pad;
    if (w <= 1.0 || h <= 1.0) return;

    cairo_set_source_rgba(cr, 0.5, 0.5, 0.5, 0.2);
    for (int i = 1; i < 4; i++) {
        double y = pad + h * (1.0 - (double)i / 4.0);
        cairo_move_to(cr, pad, y);
        cairo_line_to(cr, pad + w, y);
    }
    cairo_set_line_width(cr, 1.0);
    cairo_stroke(cr);

    static const double palette[][3] = {
        {0.23, 0.49, 0.97},
        {0.96, 0.55, 0.19},
        {0.25, 0.76, 0.52},
        {0.84, 0.37, 0.67},
        {0.94, 0.76, 0.20},
        {0.36, 0.70, 0.91},
        {0.82, 0.31, 0.28},
    };
    int palette_len = (int)(sizeof(palette) / sizeof(palette[0]));

    int start = ctx->history_pos - ctx->history_count;
    if (start < 0) start += ctx->history_len;

    for (int p = 0; p < ctx->P; p++) {
        const double *col = palette[p % palette_len];
        cairo_set_source_rgba(cr, col[0], col[1], col[2], 0.9);
        cairo_set_line_width(cr, 2.0);

        for (int i = 0; i < ctx->history_count; i++) {
            int idx = (start + i) % ctx->history_len;
            double share = ctx->share_history[idx * ctx->P + p];
            if (share < 0.0) share = 0.0;
            if (share > 1.0) share = 1.0;

            double x = pad + w * ((double)i / (double)(ctx->history_count - 1));
            double y = pad + h * (1.0 - share);
            if (i == 0) cairo_move_to(cr, x, y);
            else cairo_line_to(cr, x, y);
        }
        cairo_stroke(cr);
    }
}

static gboolean gui_tick(gpointer data) {
    gui_ctx_t *ctx = (gui_ctx_t*)data;
    int64_t total = 0;
    scheduler_snapshot(ctx->sched, ctx->per, ctx->fin, &total);

    int64_t now = now_millis();
    double dt = (double)(now - ctx->last_ms) / 1000.0;
    if (dt > 0.0) {
        double inst = (double)(total - ctx->last_total) / dt;
        if (inst < 0.0) inst = 0.0;
        if (ctx->smoothed_bps <= 0.0) ctx->smoothed_bps = inst;
        else ctx->smoothed_bps = 0.2 * inst + 0.8 * ctx->smoothed_bps;
        ctx->last_total = total;
        ctx->last_ms = now;
    }

    double frac = (ctx->file_size > 0) ? (double)total / (double)ctx->file_size : 0.0;
    if (frac < 0.0) frac = 0.0;
    if (frac > 1.0) frac = 1.0;

    char total_buf[32], size_buf[32], speed_buf[32], eta_buf[32], overall_text[64];
    format_bytes(total_buf, sizeof(total_buf), total);
    format_bytes(size_buf, sizeof(size_buf), ctx->file_size);
    if (ctx->smoothed_bps > 0.0) {
        format_bytes(speed_buf, sizeof(speed_buf), (int64_t)ctx->smoothed_bps);
    } else {
        snprintf(speed_buf, sizeof(speed_buf), "--");
    }
    double eta_sec = (ctx->smoothed_bps > 1.0)
        ? ((double)(ctx->file_size - total) / ctx->smoothed_bps)
        : -1.0;
    format_eta(eta_buf, sizeof(eta_buf), eta_sec);

    snprintf(overall_text, sizeof(overall_text), "%.2f%%", 100.0 * frac);
    gtk_progress_bar_set_fraction(ctx->overall_bar, frac);
    gtk_progress_bar_set_text(ctx->overall_bar, overall_text);

    char overall_label[128];
    snprintf(overall_label, sizeof(overall_label), "Downloaded: %s / %s", total_buf, size_buf);
    gtk_label_set_text(ctx->overall_label, overall_label);

    char speed_label[64];
    snprintf(speed_label, sizeof(speed_label), "Speed: %s/s", speed_buf);
    gtk_label_set_text(ctx->speed_label, speed_label);
    char eta_label[64];
    snprintf(eta_label, sizeof(eta_label), "ETA: %s", eta_buf);
    gtk_label_set_text(ctx->eta_label, eta_label);

    int paused = scheduler_is_paused(ctx->sched);
    gtk_label_set_text(ctx->status_label, paused ? "Status: Paused" : "Status: Running");

    for (int i = 0; i < ctx->P; i++) {
        double pfrac = (ctx->part_sizes[i] > 0)
            ? (double)ctx->per[i] / (double)ctx->part_sizes[i]
            : 1.0;
        if (pfrac < 0.0) pfrac = 0.0;
        if (pfrac > 1.0) pfrac = 1.0;

        char pct_text[32];
        snprintf(pct_text, sizeof(pct_text), "%.2f%%", 100.0 * pfrac);
        gtk_progress_bar_set_fraction(ctx->rows[i].bar, pfrac);
        gtk_progress_bar_set_text(ctx->rows[i].bar, pct_text);

        char cur_buf[24], tot_buf[24], bytes_text[64];
        format_bytes(cur_buf, sizeof(cur_buf), ctx->per[i]);
        format_bytes(tot_buf, sizeof(tot_buf), ctx->part_sizes[i]);
        snprintf(bytes_text, sizeof(bytes_text), "%s / %s", cur_buf, tot_buf);
        gtk_label_set_text(ctx->rows[i].bytes_label, bytes_text);

        double achieved = (total > 0) ? (100.0 * (double)ctx->per[i] / (double)total) : 0.0;
        double expected = (ctx->sum_weights > 0)
            ? (100.0 * (double)ctx->weights[i] / (double)ctx->sum_weights)
            : 0.0;
        char share_text[64];
        snprintf(share_text, sizeof(share_text), "share %.1f/%.1f%%", achieved, expected);
        gtk_label_set_text(ctx->rows[i].share_label, share_text);

        char meta_text[64];
        snprintf(meta_text, sizeof(meta_text), "P%02d  w=%d%s",
                 i, ctx->weights[i], ctx->fin[i] ? "  DONE" : "");
        gtk_label_set_text(ctx->rows[i].meta_label, meta_text);
    }

    if (ctx->history_len > 0 && ctx->P > 0) {
        double total_d = (double)total;
        for (int i = 0; i < ctx->P; i++) {
            double share = (total_d > 0.0) ? (double)ctx->per[i] / total_d : 0.0;
            ctx->share_history[ctx->history_pos * ctx->P + i] = share;
        }
        ctx->history_pos = (ctx->history_pos + 1) % ctx->history_len;
        if (ctx->history_count < ctx->history_len) ctx->history_count++;
        gtk_widget_queue_draw(GTK_WIDGET(ctx->chart));
    }

    if (scheduler_all_done(ctx->sched)) {
        if (!ctx->quit_requested) g_application_quit(G_APPLICATION(ctx->app));
        return G_SOURCE_REMOVE;
    }

    return G_SOURCE_CONTINUE;
}

static void apply_css(void) {
    GtkCssProvider *provider = gtk_css_provider_new();
    const char *css =
        "* { color-scheme: light dark; }\n"
        ".overall-bar { min-height: 12px; }\n"
        ".part-bar { min-height: 10px; }\n"
        ".share-chart { min-height: 140px; }\n";
    gtk_css_provider_load_from_string(provider, css);
    gtk_style_context_add_provider_for_display(gdk_display_get_default(),
        GTK_STYLE_PROVIDER(provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);
}

static void on_activate(GtkApplication *app, gpointer data) {
    gui_ctx_t *ctx = (gui_ctx_t*)data;
    ctx->app = app;

    apply_css();

    GtkWidget *window = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(window), "Weighted Downloader");
    gtk_window_set_default_size(GTK_WINDOW(window), 860, 640);
    g_signal_connect(window, "close-request", G_CALLBACK(on_close_request), ctx);

    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_top(root, 12);
    gtk_widget_set_margin_bottom(root, 12);
    gtk_widget_set_margin_start(root, 12);
    gtk_widget_set_margin_end(root, 12);
    gtk_window_set_child(GTK_WINDOW(window), root);

    GtkWidget *title = gtk_label_new("Weighted Parallel Downloader (DRR)");
    gtk_widget_set_halign(title, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(root), title);

    GtkWidget *overall_bar = gtk_progress_bar_new();
    gtk_widget_add_css_class(overall_bar, "overall-bar");
    gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(overall_bar), TRUE);
    gtk_box_append(GTK_BOX(root), overall_bar);

    GtkWidget *overall_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget *overall_label = gtk_label_new("Downloaded: 0 / 0");
    GtkWidget *speed_label = gtk_label_new("Speed: --/s");
    GtkWidget *eta_label = gtk_label_new("ETA: --:--");
    GtkWidget *status_label = gtk_label_new("Status: Running");
    gtk_widget_set_halign(overall_label, GTK_ALIGN_START);
    gtk_widget_set_halign(speed_label, GTK_ALIGN_START);
    gtk_widget_set_halign(eta_label, GTK_ALIGN_START);
    gtk_widget_set_halign(status_label, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(overall_row), overall_label);
    gtk_box_append(GTK_BOX(overall_row), speed_label);
    gtk_box_append(GTK_BOX(overall_row), eta_label);
    gtk_box_append(GTK_BOX(overall_row), status_label);
    gtk_box_append(GTK_BOX(root), overall_row);

    GtkWidget *btn_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *pause_btn = gtk_button_new_with_label("Pause");
    GtkWidget *resume_btn = gtk_button_new_with_label("Resume");
    GtkWidget *quit_btn = gtk_button_new_with_label("Quit");
    g_signal_connect(pause_btn, "clicked", G_CALLBACK(on_pause_clicked), ctx);
    g_signal_connect(resume_btn, "clicked", G_CALLBACK(on_resume_clicked), ctx);
    g_signal_connect(quit_btn, "clicked", G_CALLBACK(on_quit_clicked), ctx);
    gtk_box_append(GTK_BOX(btn_row), pause_btn);
    gtk_box_append(GTK_BOX(btn_row), resume_btn);
    gtk_box_append(GTK_BOX(btn_row), quit_btn);
    gtk_box_append(GTK_BOX(root), btn_row);

    GtkWidget *part_label = gtk_label_new("Partitions");
    gtk_widget_set_halign(part_label, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(root), part_label);

    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(scroll, TRUE);

    GtkWidget *list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(list), GTK_SELECTION_NONE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), list);
    gtk_box_append(GTK_BOX(root), scroll);

    ctx->rows = (gui_part_row_t*)calloc((size_t)ctx->P, sizeof(gui_part_row_t));
    for (int i = 0; i < ctx->P; i++) {
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
        GtkWidget *top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);

        GtkWidget *meta = gtk_label_new("");
        gtk_widget_set_halign(meta, GTK_ALIGN_START);
        gtk_box_append(GTK_BOX(top), meta);

        GtkWidget *share = gtk_label_new("");
        gtk_widget_set_halign(share, GTK_ALIGN_END);
        gtk_widget_set_hexpand(share, TRUE);
        gtk_box_append(GTK_BOX(top), share);

        GtkWidget *bar_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        GtkWidget *bar = gtk_progress_bar_new();
        gtk_widget_add_css_class(bar, "part-bar");
        gtk_widget_set_hexpand(bar, TRUE);
        gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(bar), TRUE);

        GtkWidget *bytes = gtk_label_new("0 / 0");
        gtk_widget_set_halign(bytes, GTK_ALIGN_END);

        gtk_box_append(GTK_BOX(bar_row), bar);
        gtk_box_append(GTK_BOX(bar_row), bytes);

        gtk_box_append(GTK_BOX(row), top);
        gtk_box_append(GTK_BOX(row), bar_row);

        gtk_list_box_append(GTK_LIST_BOX(list), row);

        ctx->rows[i].bar = GTK_PROGRESS_BAR(bar);
        ctx->rows[i].meta_label = GTK_LABEL(meta);
        ctx->rows[i].bytes_label = GTK_LABEL(bytes);
        ctx->rows[i].share_label = GTK_LABEL(share);
    }

    GtkWidget *chart_label = gtk_label_new("Share History (Achieved %)");
    gtk_widget_set_halign(chart_label, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(root), chart_label);

    GtkWidget *chart = gtk_drawing_area_new();
    gtk_widget_add_css_class(chart, "share-chart");
    gtk_widget_set_vexpand(chart, FALSE);
    gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(chart), 800);
    gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(chart), 160);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(chart), chart_draw, ctx, NULL);
    gtk_box_append(GTK_BOX(root), chart);

    ctx->window = GTK_WINDOW(window);
    ctx->overall_bar = GTK_PROGRESS_BAR(overall_bar);
    ctx->overall_label = GTK_LABEL(overall_label);
    ctx->speed_label = GTK_LABEL(speed_label);
    ctx->eta_label = GTK_LABEL(eta_label);
    ctx->status_label = GTK_LABEL(status_label);
    ctx->chart = GTK_DRAWING_AREA(chart);

    ctx->last_total = 0;
    ctx->last_ms = now_millis();
    ctx->smoothed_bps = 0.0;
    ctx->timer_id = g_timeout_add(100, gui_tick, ctx);

    gtk_window_present(GTK_WINDOW(window));
}

int gui_run(scheduler_t *sched, int P, int64_t file_size,
            const int *weights, const int64_t *part_sizes) {
    gui_ctx_t *ctx = (gui_ctx_t*)calloc(1, sizeof(gui_ctx_t));
    if (!ctx) return -1;

    ctx->sched = sched;
    ctx->P = P;
    ctx->file_size = file_size;
    ctx->history_len = 200;

    ctx->weights = (int*)calloc((size_t)P, sizeof(int));
    ctx->part_sizes = (int64_t*)calloc((size_t)P, sizeof(int64_t));
    ctx->per = (int64_t*)calloc((size_t)P, sizeof(int64_t));
    ctx->fin = (int*)calloc((size_t)P, sizeof(int));
    ctx->share_history = (double*)calloc((size_t)P * (size_t)ctx->history_len, sizeof(double));

    if (!ctx->weights || !ctx->part_sizes || !ctx->per || !ctx->fin || !ctx->share_history) {
        free(ctx->weights);
        free(ctx->part_sizes);
        free(ctx->per);
        free(ctx->fin);
        free(ctx->share_history);
        free(ctx);
        return -1;
    }

    memcpy(ctx->weights, weights, (size_t)P * sizeof(int));
    memcpy(ctx->part_sizes, part_sizes, (size_t)P * sizeof(int64_t));
    for (int i = 0; i < P; i++) ctx->sum_weights += ctx->weights[i];

    GtkApplication *app = gtk_application_new("com.pandatron.weighted_downloader",
                                              G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), ctx);
    int status = g_application_run(G_APPLICATION(app), 0, NULL);
    g_object_unref(app);

    free(ctx->rows);
    free(ctx->weights);
    free(ctx->part_sizes);
    free(ctx->per);
    free(ctx->fin);
    free(ctx->share_history);
    free(ctx);

    return status == 0 ? 0 : -1;
}
