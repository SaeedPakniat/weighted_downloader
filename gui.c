#define _POSIX_C_SOURCE 200809L

#include "gui.h"

#include <gtk/gtk.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "util.h"

typedef struct gui_job_row {
    download_job_t *job;
    GtkWidget *row;
    GtkProgressBar *bar;
    GtkLabel *title_label;
    GtkLabel *url_label;
    GtkLabel *bytes_label;
    GtkLabel *speed_label;
    GtkLabel *eta_label;
    GtkLabel *state_label;
    GtkWidget *pause_btn;
    GtkWidget *resume_btn;
    GtkWidget *delete_btn;
    GtkWidget *parts_box;
    GPtrArray *part_rows;
    int P;
    int *weights;
    int64_t *part_sizes;
    int64_t *part_downloaded;
    int *part_finished;
    int64_t last_total;
    int64_t last_ms;
    double smoothed_bps;
    int deleting;
} gui_job_row_t;

typedef struct gui_part_row {
    GtkWidget *row;
    GtkProgressBar *bar;
    GtkLabel *label;
} gui_part_row_t;

typedef struct gui_ctx {
    job_manager_t *jm;
    const downloader_config_t *cfg;
    GtkApplication *app;
    GtkWindow *window;
    GtkListBox *list;
    GtkLabel *summary_label;
    GPtrArray *rows;
    GPtrArray *bg_threads;
    GPtrArray *removed_jobs;
    GMutex removed_mtx;
    guint timer_id;
    int quit_requested;
} gui_ctx_t;

typedef struct add_job_task {
    gui_ctx_t *ctx;
    GtkWindow *dialog;
    GtkEntry *url_entry;
    GtkEntry *path_entry;
    GtkEntry *parts_entry;
    GtkEntry *threads_entry;
    GtkEntry *weights_entry;
    GtkLabel *status_label;
    GtkWidget *add_btn;
    GtkWidget *cancel_btn;
    char *url;
    char *path;
    int parts;
    int threads;
    int has_parts;
    int has_threads;
    int *weights;
    int weights_count;
    char *error;
    download_job_t *job;
} add_job_task_t;

typedef struct delete_job_task {
    gui_ctx_t *ctx;
    gui_job_row_t *row;
} delete_job_task_t;

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
    if (h > 0) snprintf(buf, buflen, "%02d:%02d:%02d", h, m, s);
    else snprintf(buf, buflen, "%02d:%02d", m, s);
}

static void compute_partition_sizes(int64_t total, int P, int64_t *out_sizes) {
    if (!out_sizes || P <= 0) return;
    int64_t base = total / P;
    int64_t rem = total % P;
    for (int i = 0; i < P; i++) {
        out_sizes[i] = base + (i < rem ? 1 : 0);
    }
}

static const char *state_label(download_job_state_t st) {
    switch (st) {
        case JOB_IDLE: return "Queued";
        case JOB_RUNNING: return "Running";
        case JOB_PAUSED: return "Paused";
        case JOB_STOPPING: return "Stopping";
        case JOB_STOPPED: return "Stopped";
        case JOB_DONE: return "Done";
        case JOB_ERROR: return "Error";
        default: return "Unknown";
    }
}

static const char *path_basename(const char *path) {
    if (!path) return "";
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static char *make_state_path(const char *output_path) {
    if (!output_path) return NULL;
    const char *suffix = ".wdstate";
    size_t len = strlen(output_path) + strlen(suffix) + 1;
    char *path = (char*)calloc(len, 1);
    if (!path) return NULL;
    snprintf(path, len, "%s%s", output_path, suffix);
    return path;
}

static int parse_optional_int(const char *text, int *out) {
    if (!text || !*text) return 0;
    char *end = NULL;
    long v = strtol(text, &end, 10);
    if (!end || *end != '\0' || v <= 0 || v > 1024) return -1;
    *out = (int)v;
    return 1;
}

static int parse_weights_list(const char *text, int **out_weights, int *out_count) {
    if (!text || !*text) return 0;
    int *weights = NULL;
    int count = 0;
    const char *p = text;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == ',') p++;
        if (!*p) break;
        char *end = NULL;
        long v = strtol(p, &end, 10);
        if (!end || v <= 0 || v > 100000) {
            free(weights);
            return -1;
        }
        int *next = (int*)realloc(weights, (size_t)(count + 1) * sizeof(int));
        if (!next) {
            free(weights);
            return -1;
        }
        weights = next;
        weights[count++] = (int)v;
        p = end;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == ',') p++;
    }
    if (count == 0) {
        free(weights);
        return -1;
    }
    *out_weights = weights;
    *out_count = count;
    return 1;
}

static void gui_request_quit(gui_ctx_t *ctx) {
    if (ctx->quit_requested) return;
    ctx->quit_requested = 1;

    int count = job_manager_count(ctx->jm);
    for (int i = 0; i < count; i++) {
        download_job_t *job = job_manager_get(ctx->jm, i);
        if (job) download_job_stop(job);
    }
    if (ctx->app) g_application_quit(G_APPLICATION(ctx->app));
}

static void apply_css(void) {
    GtkCssProvider *provider = gtk_css_provider_new();
    const char *css =
        "window { background: #1a1c1f; color: #e6e9ee; }\n"
        "label { color: #e6e9ee; }\n"
        ".title { font-weight: 700; font-size: 18px; color: #f4f7fb; }\n"
        ".subtitle { color: #b1b7c0; }\n"
        ".job-card { background: #23262b; border: 1px solid #343840; border-radius: 8px; padding: 10px; }\n"
        ".job-bar { min-height: 12px; }\n"
        ".meta { color: #c8ced7; }\n"
        "entry { background: #1f2227; color: #f2f5f9; border: 1px solid #343840; }\n"
        "button { background: #2a2f36; color: #f2f5f9; border: 1px solid #3b414b; }\n"
        "button:hover { background: #353b45; }\n";
    gtk_css_provider_load_from_string(provider, css);
    gtk_style_context_add_provider_for_display(gdk_display_get_default(),
        GTK_STYLE_PROVIDER(provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);
}

static void gui_job_row_free(gui_job_row_t *row) {
    if (!row) return;
    if (row->part_rows) {
        for (guint i = 0; i < row->part_rows->len; i++) {
            gui_part_row_t *p = (gui_part_row_t*)g_ptr_array_index(row->part_rows, i);
            free(p);
        }
        g_ptr_array_free(row->part_rows, TRUE);
    }
    free(row->weights);
    free(row->part_sizes);
    free(row->part_downloaded);
    free(row->part_finished);
    free(row);
}

static gui_job_row_t *gui_job_row_new(download_job_t *job) {
    gui_job_row_t *row = (gui_job_row_t*)calloc(1, sizeof(*row));
    if (!row) return NULL;
    row->job = job;
    row->last_ms = now_millis();

    download_job_meta_t meta;
    memset(&meta, 0, sizeof(meta));
    download_job_get_meta(job, &meta);

    const char *title = meta.output_path ? path_basename(meta.output_path) : "Download";
    const char *url = meta.url ? meta.url : "";

    GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_add_css_class(card, "job-card");

    GtkWidget *title_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *title_label = gtk_label_new(title);
    gtk_widget_add_css_class(title_label, "title");
    gtk_widget_set_halign(title_label, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(title_row), title_label);
    gtk_box_append(GTK_BOX(card), title_row);

    GtkWidget *url_label = gtk_label_new(url);
    gtk_widget_add_css_class(url_label, "subtitle");
    gtk_label_set_wrap(GTK_LABEL(url_label), TRUE);
    gtk_label_set_wrap_mode(GTK_LABEL(url_label), PANGO_WRAP_WORD_CHAR);
    gtk_widget_set_halign(url_label, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(card), url_label);

    GtkWidget *bar = gtk_progress_bar_new();
    gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(bar), TRUE);
    gtk_widget_add_css_class(bar, "job-bar");
    gtk_box_append(GTK_BOX(card), bar);

    GtkWidget *parts_box = NULL;
    GPtrArray *part_rows = NULL;
    if (meta.P > 0 && meta.file_size > 0 && meta.range_supported) {
        parts_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
        part_rows = g_ptr_array_new();
        gtk_widget_set_margin_top(parts_box, 4);
        gtk_widget_set_margin_bottom(parts_box, 2);
        for (int i = 0; i < meta.P; i++) {
            GtkWidget *pbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
            GtkWidget *plabel = gtk_label_new("");
            gtk_widget_add_css_class(plabel, "meta");
            gtk_widget_set_halign(plabel, GTK_ALIGN_START);
            GtkWidget *pbar = gtk_progress_bar_new();
            gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(pbar), TRUE);
            gtk_widget_add_css_class(pbar, "job-bar");
            gtk_box_append(GTK_BOX(pbox), plabel);
            gtk_box_append(GTK_BOX(pbox), pbar);
            gtk_box_append(GTK_BOX(parts_box), pbox);

            gui_part_row_t *prow = (gui_part_row_t*)calloc(1, sizeof(*prow));
            if (prow) {
                prow->row = pbox;
                prow->bar = GTK_PROGRESS_BAR(pbar);
                prow->label = GTK_LABEL(plabel);
                g_ptr_array_add(part_rows, prow);
            }
        }
        gtk_box_append(GTK_BOX(card), parts_box);
    }

    GtkWidget *info_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget *bytes_label = gtk_label_new("Downloaded: --");
    GtkWidget *speed_label = gtk_label_new("Speed: --/s");
    GtkWidget *eta_label = gtk_label_new("ETA: --:--");
    GtkWidget *state_label = gtk_label_new("Status: Queued");
    gtk_widget_add_css_class(bytes_label, "meta");
    gtk_widget_add_css_class(speed_label, "meta");
    gtk_widget_add_css_class(eta_label, "meta");
    gtk_widget_add_css_class(state_label, "meta");
    gtk_widget_set_halign(bytes_label, GTK_ALIGN_START);
    gtk_widget_set_halign(speed_label, GTK_ALIGN_START);
    gtk_widget_set_halign(eta_label, GTK_ALIGN_START);
    gtk_widget_set_halign(state_label, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(info_row), bytes_label);
    gtk_box_append(GTK_BOX(info_row), speed_label);
    gtk_box_append(GTK_BOX(info_row), eta_label);
    gtk_box_append(GTK_BOX(info_row), state_label);
    gtk_box_append(GTK_BOX(card), info_row);

    GtkWidget *btn_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *pause_btn = gtk_button_new_with_label("Pause");
    GtkWidget *resume_btn = gtk_button_new_with_label("Resume");
    GtkWidget *delete_btn = gtk_button_new_with_label("Delete");
    gtk_box_append(GTK_BOX(btn_row), pause_btn);
    gtk_box_append(GTK_BOX(btn_row), resume_btn);
    gtk_box_append(GTK_BOX(btn_row), delete_btn);
    gtk_box_append(GTK_BOX(card), btn_row);

    GtkWidget *list_row = gtk_list_box_row_new();
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(list_row), card);

    row->row = list_row;
    row->bar = GTK_PROGRESS_BAR(bar);
    row->title_label = GTK_LABEL(title_label);
    row->url_label = GTK_LABEL(url_label);
    row->bytes_label = GTK_LABEL(bytes_label);
    row->speed_label = GTK_LABEL(speed_label);
    row->eta_label = GTK_LABEL(eta_label);
    row->state_label = GTK_LABEL(state_label);
    row->pause_btn = pause_btn;
    row->resume_btn = resume_btn;
    row->delete_btn = delete_btn;
    row->parts_box = parts_box;
    row->part_rows = part_rows;
    row->P = meta.P;
    if (meta.P > 0 && meta.weights) {
        row->weights = (int*)calloc((size_t)meta.P, sizeof(int));
        if (row->weights) {
            memcpy(row->weights, meta.weights, (size_t)meta.P * sizeof(int));
        }
    }
    if (meta.P > 0 && meta.file_size > 0 && meta.range_supported) {
        row->part_sizes = (int64_t*)calloc((size_t)meta.P, sizeof(int64_t));
        row->part_downloaded = (int64_t*)calloc((size_t)meta.P, sizeof(int64_t));
        row->part_finished = (int*)calloc((size_t)meta.P, sizeof(int));
        if (row->part_sizes) compute_partition_sizes(meta.file_size, meta.P, row->part_sizes);
    }

    download_job_meta_free(&meta);
    return row;
}

static gboolean gui_delete_complete_ui(gpointer data) {
    delete_job_task_t *task = (delete_job_task_t*)data;
    gui_ctx_t *ctx = task->ctx;
    gui_job_row_t *row = task->row;

    for (guint i = 0; i < ctx->rows->len; i++) {
        if (g_ptr_array_index(ctx->rows, i) == row) {
            g_ptr_array_remove_index(ctx->rows, i);
            break;
        }
    }
    gtk_list_box_remove(ctx->list, row->row);
    gui_job_row_free(row);
    free(task);
    return G_SOURCE_REMOVE;
}

static void *delete_job_thread(void *arg) {
    delete_job_task_t *task = (delete_job_task_t*)arg;
    gui_ctx_t *ctx = task->ctx;
    gui_job_row_t *row = task->row;

    download_job_stop(row->job);
    download_job_join(row->job);
    job_manager_remove(ctx->jm, row->job);

    download_job_meta_t meta;
    memset(&meta, 0, sizeof(meta));
    if (download_job_get_meta(row->job, &meta) == 0) {
        if (meta.output_path) {
            unlink(meta.output_path);
            char *state_path = make_state_path(meta.output_path);
            if (state_path) {
                unlink(state_path);
                free(state_path);
            }
        }
        download_job_meta_free(&meta);
    }

    g_mutex_lock(&ctx->removed_mtx);
    g_ptr_array_add(ctx->removed_jobs, row->job);
    g_mutex_unlock(&ctx->removed_mtx);

    if (ctx->app && !ctx->quit_requested) {
        g_idle_add(gui_delete_complete_ui, task);
    } else {
        free(task);
    }
    return NULL;
}

static void on_pause_clicked(GtkButton *btn, gpointer data) {
    (void)btn;
    gui_job_row_t *row = (gui_job_row_t*)data;
    if (row->deleting) return;
    download_job_pause(row->job);
}

static void on_resume_clicked(GtkButton *btn, gpointer data) {
    (void)btn;
    gui_job_row_t *row = (gui_job_row_t*)data;
    if (row->deleting) return;
    download_job_resume(row->job);
}

static void on_delete_clicked(GtkButton *btn, gpointer data) {
    (void)btn;
    gui_job_row_t *row = (gui_job_row_t*)data;
    if (row->deleting) return;
    row->deleting = 1;
    gtk_widget_set_sensitive(row->pause_btn, FALSE);
    gtk_widget_set_sensitive(row->resume_btn, FALSE);
    gtk_widget_set_sensitive(row->delete_btn, FALSE);
    gtk_label_set_text(row->state_label, "Status: Deleting");

    delete_job_task_t *task = (delete_job_task_t*)calloc(1, sizeof(*task));
    if (!task) return;
    task->ctx = (gui_ctx_t*)g_object_get_data(G_OBJECT(btn), "ctx");
    task->row = row;

    GThread *thr = g_thread_new("delete-job", delete_job_thread, task);
    g_ptr_array_add(task->ctx->bg_threads, thr);
}

static gboolean gui_add_job_ui(gpointer data) {
    add_job_task_t *task = (add_job_task_t*)data;
    gui_ctx_t *ctx = task->ctx;

    if (task->job) {
        gui_job_row_t *row = gui_job_row_new(task->job);
        if (row) {
            gtk_list_box_append(ctx->list, row->row);
            g_ptr_array_add(ctx->rows, row);
            g_signal_connect(row->pause_btn, "clicked", G_CALLBACK(on_pause_clicked), row);
            g_signal_connect(row->resume_btn, "clicked", G_CALLBACK(on_resume_clicked), row);
            g_signal_connect(row->delete_btn, "clicked", G_CALLBACK(on_delete_clicked), row);
            g_object_set_data(G_OBJECT(row->delete_btn), "ctx", ctx);
        }
        gtk_window_destroy(task->dialog);
        free(task->url);
        free(task->path);
        free(task->weights);
        free(task->error);
        free(task);
    } else {
        gtk_label_set_text(task->status_label,
                           task->error ? task->error : "Failed to add download.");
        gtk_widget_set_sensitive(task->add_btn, TRUE);
        gtk_widget_set_sensitive(task->cancel_btn, TRUE);
        free(task->url);
        free(task->path);
        free(task->weights);
        free(task->error);
        task->url = NULL;
        task->path = NULL;
        task->weights = NULL;
        task->error = NULL;
    }
    return G_SOURCE_REMOVE;
}

static void *add_job_thread(void *arg) {
    add_job_task_t *task = (add_job_task_t*)arg;
    gui_ctx_t *ctx = task->ctx;

    download_job_t *job = download_job_create(task->url, task->path, ctx->cfg);
    if (!job) {
        task->error = strdup("Failed to create download job.");
        g_idle_add(gui_add_job_ui, task);
        return NULL;
    }

    int64_t size = -1;
    int range_supported = 0;
    if (download_job_probe(job, &size, &range_supported) == 0) {
        download_job_set_probe(job, size, range_supported);
    }

    if (task->has_parts || task->has_threads || task->weights_count > 0) {
        if (!(size > 0 && range_supported)) {
            task->error = strdup("Server does not support range downloads.");
            download_job_destroy(job);
            g_idle_add(gui_add_job_ui, task);
            return NULL;
        }
    }

    if (size > 0 && range_supported) {
        int P = task->has_parts ? task->parts : ((task->weights_count > 0) ? task->weights_count : 4);
        int T = task->has_threads ? task->threads : ((P < 8) ? P : 8);
        if (P <= 0 || T <= 0) {
            task->error = strdup("Invalid partitions/threads values.");
            download_job_destroy(job);
            g_idle_add(gui_add_job_ui, task);
            return NULL;
        }
        if (task->weights_count > 0 && task->weights_count != P) {
            task->error = strdup("Weights count must match partitions.");
            download_job_destroy(job);
            g_idle_add(gui_add_job_ui, task);
            return NULL;
        }
        int *weights = task->weights;
        int allocated = 0;
        if (!weights) {
            weights = (int*)calloc((size_t)P, sizeof(int));
            if (!weights) {
                task->error = strdup("Failed to allocate weights.");
                download_job_destroy(job);
                g_idle_add(gui_add_job_ui, task);
                return NULL;
            }
            for (int i = 0; i < P; i++) weights[i] = 1;
            allocated = 1;
        }
        if (download_job_set_scheduling(job, P, weights, T) != 0) {
            if (allocated) free(weights);
            task->error = strdup("Failed to configure scheduling.");
            download_job_destroy(job);
            g_idle_add(gui_add_job_ui, task);
            return NULL;
        }
        if (allocated) free(weights);
    }

    if (job_manager_add(ctx->jm, job) < 0) {
        task->error = strdup("Failed to add job to manager.");
        download_job_destroy(job);
        g_idle_add(gui_add_job_ui, task);
        return NULL;
    }

    task->job = job;
    g_idle_add(gui_add_job_ui, task);
    return NULL;
}

static void on_add_confirm(GtkButton *btn, gpointer data) {
    (void)btn;
    add_job_task_t *task = (add_job_task_t*)data;

    free(task->weights);
    task->weights = NULL;
    task->weights_count = 0;
    task->has_parts = 0;
    task->has_threads = 0;

    const char *url = gtk_editable_get_text(GTK_EDITABLE(task->url_entry));
    const char *path = gtk_editable_get_text(GTK_EDITABLE(task->path_entry));
    const char *parts_text = gtk_editable_get_text(GTK_EDITABLE(task->parts_entry));
    const char *threads_text = gtk_editable_get_text(GTK_EDITABLE(task->threads_entry));
    const char *weights_text = gtk_editable_get_text(GTK_EDITABLE(task->weights_entry));

    if (!url || !*url || !path || !*path) {
        gtk_label_set_text(task->status_label, "URL and output path required.");
        return;
    }

    int parts = 0;
    int threads = 0;
    int parse = parse_optional_int(parts_text, &parts);
    if (parse < 0) {
        gtk_label_set_text(task->status_label, "Partitions must be a positive integer.");
        return;
    }
    task->has_parts = parse;
    task->parts = parts;

    parse = parse_optional_int(threads_text, &threads);
    if (parse < 0) {
        gtk_label_set_text(task->status_label, "Threads must be a positive integer.");
        return;
    }
    task->has_threads = parse;
    task->threads = threads;

    int *weights = NULL;
    int weights_count = 0;
    parse = parse_weights_list(weights_text, &weights, &weights_count);
    if (parse < 0) {
        gtk_label_set_text(task->status_label, "Weights must be comma-separated positive integers.");
        return;
    }
    task->weights = weights;
    task->weights_count = (parse == 0) ? 0 : weights_count;

    gtk_widget_set_sensitive(task->add_btn, FALSE);
    gtk_widget_set_sensitive(task->cancel_btn, FALSE);
    gtk_label_set_text(task->status_label, "Probing...");

    task->url = strdup(url);
    task->path = strdup(path);
    if (!task->url || !task->path) {
        gtk_label_set_text(task->status_label, "Out of memory.");
        gtk_widget_set_sensitive(task->add_btn, TRUE);
        gtk_widget_set_sensitive(task->cancel_btn, TRUE);
        free(task->url);
        free(task->path);
        task->url = NULL;
        task->path = NULL;
        return;
    }

    GThread *thr = g_thread_new("add-job", add_job_thread, task);
    g_ptr_array_add(task->ctx->bg_threads, thr);
}

static void on_add_cancel(GtkButton *btn, gpointer data) {
    (void)btn;
    add_job_task_t *task = (add_job_task_t*)data;
    gtk_window_destroy(task->dialog);
    free(task->url);
    free(task->path);
    free(task->weights);
    free(task->error);
    free(task);
}

static gboolean on_add_close_request(GtkWindow *window, gpointer data) {
    (void)window;
    add_job_task_t *task = (add_job_task_t*)data;
    if (!gtk_widget_is_sensitive(task->add_btn)) {
        return TRUE;
    }
    on_add_cancel(NULL, task);
    return TRUE;
}

static void on_add_clicked(GtkButton *btn, gpointer data) {
    (void)btn;
    gui_ctx_t *ctx = (gui_ctx_t*)data;

    GtkWindow *dialog = GTK_WINDOW(gtk_window_new());
    gtk_window_set_title(dialog, "Add Download");
    gtk_window_set_transient_for(dialog, ctx->window);
    gtk_window_set_modal(dialog, TRUE);
    gtk_window_set_default_size(dialog, 520, 360);

    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_top(root, 12);
    gtk_widget_set_margin_bottom(root, 12);
    gtk_widget_set_margin_start(root, 12);
    gtk_widget_set_margin_end(root, 12);
    gtk_window_set_child(dialog, root);

    GtkWidget *url_label = gtk_label_new("URL:");
    gtk_widget_set_halign(url_label, GTK_ALIGN_START);
    GtkWidget *url_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(url_entry), "https://example.com/file.bin");

    GtkWidget *path_label = gtk_label_new("Output path:");
    gtk_widget_set_halign(path_label, GTK_ALIGN_START);
    GtkWidget *path_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(path_entry), "/path/to/file.bin");

    GtkWidget *parts_label = gtk_label_new("Partitions (P):");
    gtk_widget_set_halign(parts_label, GTK_ALIGN_START);
    GtkWidget *parts_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(parts_entry), "optional, e.g. 4");

    GtkWidget *threads_label = gtk_label_new("Worker threads (T):");
    gtk_widget_set_halign(threads_label, GTK_ALIGN_START);
    GtkWidget *threads_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(threads_entry), "optional, e.g. 4");

    GtkWidget *weights_label = gtk_label_new("Weights (comma-separated):");
    gtk_widget_set_halign(weights_label, GTK_ALIGN_START);
    GtkWidget *weights_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(weights_entry), "optional, e.g. 1,1,2,1");

    GtkWidget *status_label = gtk_label_new("");
    gtk_widget_set_halign(status_label, GTK_ALIGN_START);
    gtk_widget_add_css_class(status_label, "subtitle");

    gtk_box_append(GTK_BOX(root), url_label);
    gtk_box_append(GTK_BOX(root), url_entry);
    gtk_box_append(GTK_BOX(root), path_label);
    gtk_box_append(GTK_BOX(root), path_entry);
    gtk_box_append(GTK_BOX(root), parts_label);
    gtk_box_append(GTK_BOX(root), parts_entry);
    gtk_box_append(GTK_BOX(root), threads_label);
    gtk_box_append(GTK_BOX(root), threads_entry);
    gtk_box_append(GTK_BOX(root), weights_label);
    gtk_box_append(GTK_BOX(root), weights_entry);
    gtk_box_append(GTK_BOX(root), status_label);

    GtkWidget *btn_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *add_btn = gtk_button_new_with_label("Add");
    GtkWidget *cancel_btn = gtk_button_new_with_label("Cancel");
    gtk_box_append(GTK_BOX(btn_row), add_btn);
    gtk_box_append(GTK_BOX(btn_row), cancel_btn);
    gtk_box_append(GTK_BOX(root), btn_row);

    add_job_task_t *task = (add_job_task_t*)calloc(1, sizeof(*task));
    if (!task) return;
    task->ctx = ctx;
    task->dialog = dialog;
    task->url_entry = GTK_ENTRY(url_entry);
    task->path_entry = GTK_ENTRY(path_entry);
    task->parts_entry = GTK_ENTRY(parts_entry);
    task->threads_entry = GTK_ENTRY(threads_entry);
    task->weights_entry = GTK_ENTRY(weights_entry);
    task->status_label = GTK_LABEL(status_label);
    task->add_btn = add_btn;
    task->cancel_btn = cancel_btn;

    g_signal_connect(add_btn, "clicked", G_CALLBACK(on_add_confirm), task);
    g_signal_connect(cancel_btn, "clicked", G_CALLBACK(on_add_cancel), task);
    g_signal_connect(dialog, "close-request", G_CALLBACK(on_add_close_request), task);

    gtk_window_present(dialog);
}

static gboolean gui_tick(gpointer data) {
    gui_ctx_t *ctx = (gui_ctx_t*)data;
    int64_t total_speed = 0;
    int active = 0;

    for (guint i = 0; i < ctx->rows->len; i++) {
        gui_job_row_t *row = (gui_job_row_t*)g_ptr_array_index(ctx->rows, i);
        if (!row || row->deleting) continue;

        int64_t total = 0;
        int64_t size = 0;
        download_job_state_t st = JOB_ERROR;
        download_job_snapshot(row->job, &total, &size, &st);

        int64_t now = now_millis();
        double dt = (double)(now - row->last_ms) / 1000.0;
        if (dt > 0.0) {
            double inst = (double)(total - row->last_total) / dt;
            if (inst < 0.0) inst = 0.0;
            if (row->smoothed_bps <= 0.0) row->smoothed_bps = inst;
            else row->smoothed_bps = 0.2 * inst + 0.8 * row->smoothed_bps;
            row->last_total = total;
            row->last_ms = now;
        }

        double frac = (size > 0) ? (double)total / (double)size : 0.0;
        if (frac < 0.0) frac = 0.0;
        if (frac > 1.0) frac = 1.0;

        char pct_text[32];
        if (size > 0) snprintf(pct_text, sizeof(pct_text), "%.2f%%", 100.0 * frac);
        else snprintf(pct_text, sizeof(pct_text), "--");
        gtk_progress_bar_set_fraction(row->bar, size > 0 ? frac : 0.0);
        gtk_progress_bar_set_text(row->bar, pct_text);

        char total_buf[32];
        char size_buf[32];
        format_bytes(total_buf, sizeof(total_buf), total);
        if (size > 0) format_bytes(size_buf, sizeof(size_buf), size);
        else snprintf(size_buf, sizeof(size_buf), "?");

        char bytes_label[96];
        snprintf(bytes_label, sizeof(bytes_label), "Downloaded: %s / %s", total_buf, size_buf);
        gtk_label_set_text(row->bytes_label, bytes_label);

        char speed_buf[32];
        if (row->smoothed_bps > 0.0) {
            format_bytes(speed_buf, sizeof(speed_buf), (int64_t)row->smoothed_bps);
        } else {
            snprintf(speed_buf, sizeof(speed_buf), "--");
        }
        char speed_label[64];
        snprintf(speed_label, sizeof(speed_label), "Speed: %s/s", speed_buf);
        gtk_label_set_text(row->speed_label, speed_label);

        double eta_sec = (row->smoothed_bps > 1.0 && size > 0)
            ? ((double)(size - total) / row->smoothed_bps)
            : -1.0;
        char eta_buf[32];
        format_eta(eta_buf, sizeof(eta_buf), eta_sec);
        char eta_label[64];
        snprintf(eta_label, sizeof(eta_label), "ETA: %s", eta_buf);
        gtk_label_set_text(row->eta_label, eta_label);

        char state_buf[64];
        snprintf(state_buf, sizeof(state_buf), "Status: %s", state_label(st));
        gtk_label_set_text(row->state_label, state_buf);

        gtk_widget_set_sensitive(row->pause_btn, st == JOB_RUNNING);
        gtk_widget_set_sensitive(row->resume_btn, st == JOB_PAUSED);
        gtk_widget_set_sensitive(row->delete_btn, st != JOB_STOPPING);

        if (row->P > 0 && row->part_rows && row->part_downloaded && row->part_sizes) {
            if (download_job_snapshot_parts(row->job, row->part_downloaded, row->part_finished, row->P) == 0) {
                for (int p = 0; p < row->P && p < (int)row->part_rows->len; p++) {
                    gui_part_row_t *prow = (gui_part_row_t*)g_ptr_array_index(row->part_rows, p);
                    if (!prow) continue;
                    int64_t psize = row->part_sizes[p];
                    int64_t pdl = row->part_downloaded[p];
                    if (pdl < 0) pdl = 0;
                    if (psize > 0 && pdl > psize) pdl = psize;
                    double pfrac = (psize > 0) ? (double)pdl / (double)psize : 0.0;
                    if (pfrac < 0.0) pfrac = 0.0;
                    if (pfrac > 1.0) pfrac = 1.0;

                    char ptotal_buf[32];
                    char psize_buf[32];
                    format_bytes(ptotal_buf, sizeof(ptotal_buf), pdl);
                    if (psize > 0) format_bytes(psize_buf, sizeof(psize_buf), psize);
                    else snprintf(psize_buf, sizeof(psize_buf), "?");

                    int w = row->weights ? row->weights[p] : 1;
                    char plabel[160];
                    snprintf(plabel, sizeof(plabel), "Part %d (w=%d): %s / %s%s",
                             p, w, ptotal_buf, psize_buf,
                             (row->part_finished && row->part_finished[p]) ? " DONE" : "");
                    gtk_label_set_text(prow->label, plabel);

                    char ppct[32];
                    if (psize > 0) snprintf(ppct, sizeof(ppct), "%.2f%%", 100.0 * pfrac);
                    else snprintf(ppct, sizeof(ppct), "--");
                    gtk_progress_bar_set_fraction(prow->bar, psize > 0 ? pfrac : 0.0);
                    gtk_progress_bar_set_text(prow->bar, ppct);
                }
            }
        }

        if (st == JOB_RUNNING) {
            total_speed += (int64_t)row->smoothed_bps;
            active++;
        }
    }

    char summary[128];
    if (active > 0) {
        char speed_buf[32];
        format_bytes(speed_buf, sizeof(speed_buf), total_speed);
        snprintf(summary, sizeof(summary), "%d active downloads | total speed %s/s", active, speed_buf);
    } else {
        snprintf(summary, sizeof(summary), "No active downloads");
    }
    gtk_label_set_text(ctx->summary_label, summary);

    return G_SOURCE_CONTINUE;
}

static gboolean on_close_request(GtkWindow *window, gpointer data) {
    (void)window;
    gui_request_quit((gui_ctx_t*)data);
    return FALSE;
}

static void on_activate(GtkApplication *app, gpointer data) {
    gui_ctx_t *ctx = (gui_ctx_t*)data;
    ctx->app = app;

    apply_css();

    GtkWidget *window = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(window), "Weighted Downloader");
    gtk_window_set_default_size(GTK_WINDOW(window), 920, 680);
    g_signal_connect(window, "close-request", G_CALLBACK(on_close_request), ctx);

    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_top(root, 12);
    gtk_widget_set_margin_bottom(root, 12);
    gtk_widget_set_margin_start(root, 12);
    gtk_widget_set_margin_end(root, 12);
    gtk_window_set_child(GTK_WINDOW(window), root);

    GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *title = gtk_label_new("Weighted Download Manager");
    gtk_widget_add_css_class(title, "title");
    gtk_widget_set_halign(title, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(header), title);

    GtkWidget *add_btn = gtk_button_new_with_label("Add Download");
    gtk_widget_set_halign(add_btn, GTK_ALIGN_END);
    gtk_widget_set_hexpand(add_btn, TRUE);
    gtk_box_append(GTK_BOX(header), add_btn);
    gtk_box_append(GTK_BOX(root), header);

    GtkWidget *summary = gtk_label_new("No active downloads");
    gtk_widget_add_css_class(summary, "subtitle");
    gtk_widget_set_halign(summary, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(root), summary);

    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(scroll, TRUE);

    GtkWidget *list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(list), GTK_SELECTION_NONE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), list);
    gtk_box_append(GTK_BOX(root), scroll);

    ctx->window = GTK_WINDOW(window);
    ctx->list = GTK_LIST_BOX(list);
    ctx->summary_label = GTK_LABEL(summary);

    g_signal_connect(add_btn, "clicked", G_CALLBACK(on_add_clicked), ctx);

    int count = job_manager_count(ctx->jm);
    for (int i = 0; i < count; i++) {
        download_job_t *job = job_manager_get(ctx->jm, i);
        gui_job_row_t *row = gui_job_row_new(job);
        if (!row) continue;
        gtk_list_box_append(ctx->list, row->row);
        g_ptr_array_add(ctx->rows, row);
        g_signal_connect(row->pause_btn, "clicked", G_CALLBACK(on_pause_clicked), row);
        g_signal_connect(row->resume_btn, "clicked", G_CALLBACK(on_resume_clicked), row);
        g_signal_connect(row->delete_btn, "clicked", G_CALLBACK(on_delete_clicked), row);
        g_object_set_data(G_OBJECT(row->delete_btn), "ctx", ctx);
    }

    ctx->timer_id = g_timeout_add(200, gui_tick, ctx);
    gtk_window_present(GTK_WINDOW(window));
}

int gui_run(job_manager_t *jm, const downloader_config_t *cfg, gui_removed_jobs_t *out_removed) {
    if (!jm || !cfg) return -1;

    gui_ctx_t *ctx = (gui_ctx_t*)calloc(1, sizeof(*ctx));
    if (!ctx) return -1;
    ctx->jm = jm;
    ctx->cfg = cfg;
    ctx->rows = g_ptr_array_new();
    ctx->bg_threads = g_ptr_array_new();
    ctx->removed_jobs = g_ptr_array_new();
    g_mutex_init(&ctx->removed_mtx);

    GtkApplication *app = gtk_application_new("com.pandatron.weighted_downloader",
                                              G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), ctx);
    int status = g_application_run(G_APPLICATION(app), 0, NULL);
    g_object_unref(app);

    if (ctx->timer_id) g_source_remove(ctx->timer_id);

    for (guint i = 0; i < ctx->bg_threads->len; i++) {
        GThread *thr = (GThread*)g_ptr_array_index(ctx->bg_threads, i);
        g_thread_join(thr);
    }

    if (out_removed) {
        g_mutex_lock(&ctx->removed_mtx);
        out_removed->count = ctx->removed_jobs->len;
        if (out_removed->count > 0) {
            out_removed->jobs = (download_job_t**)calloc(out_removed->count, sizeof(download_job_t*));
            if (out_removed->jobs) {
                for (guint i = 0; i < ctx->removed_jobs->len; i++) {
                    out_removed->jobs[i] = (download_job_t*)g_ptr_array_index(ctx->removed_jobs, i);
                }
            } else {
                out_removed->count = 0;
            }
        } else {
            out_removed->jobs = NULL;
        }
        g_mutex_unlock(&ctx->removed_mtx);
    }

    for (guint i = 0; i < ctx->rows->len; i++) {
        gui_job_row_t *row = (gui_job_row_t*)g_ptr_array_index(ctx->rows, i);
        gui_job_row_free(row);
    }
    g_ptr_array_free(ctx->rows, TRUE);
    g_ptr_array_free(ctx->bg_threads, TRUE);
    g_ptr_array_free(ctx->removed_jobs, TRUE);
    g_mutex_clear(&ctx->removed_mtx);
    free(ctx);

    return status == 0 ? 0 : -1;
}
