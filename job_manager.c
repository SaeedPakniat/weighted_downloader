#define _POSIX_C_SOURCE 200809L

#include "job_manager.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "util.h"

static void timespec_add_ms(struct timespec *ts, int ms) {
    ts->tv_sec += ms / 1000;
    ts->tv_nsec += (long)(ms % 1000) * 1000000L;
    if (ts->tv_nsec >= 1000000000L) {
        ts->tv_sec += 1;
        ts->tv_nsec -= 1000000000L;
    }
}

static int is_active_state(download_job_state_t st) {
    return st == JOB_RUNNING || st == JOB_PAUSED || st == JOB_STOPPING;
}

static int is_done_state(download_job_state_t st) {
    return st == JOB_DONE || st == JOB_ERROR || st == JOB_STOPPED;
}

static const char *job_state_string(download_job_state_t st) {
    switch (st) {
        case JOB_IDLE: return "idle";
        case JOB_RUNNING: return "running";
        case JOB_PAUSED: return "paused";
        case JOB_STOPPING: return "stopping";
        case JOB_STOPPED: return "stopped";
        case JOB_DONE: return "done";
        case JOB_ERROR: return "error";
        default: return "unknown";
    }
}

static download_job_state_t job_state_from_string(const char *s) {
    if (!s) return JOB_ERROR;
    if (strcmp(s, "idle") == 0) return JOB_IDLE;
    if (strcmp(s, "running") == 0) return JOB_RUNNING;
    if (strcmp(s, "paused") == 0) return JOB_PAUSED;
    if (strcmp(s, "stopping") == 0) return JOB_STOPPING;
    if (strcmp(s, "stopped") == 0) return JOB_STOPPED;
    if (strcmp(s, "done") == 0) return JOB_DONE;
    if (strcmp(s, "error") == 0) return JOB_ERROR;
    return JOB_ERROR;
}

static void json_write_escaped(FILE *f, const char *s) {
    fputc('"', f);
    for (const unsigned char *p = (const unsigned char*)s; *p; p++) {
        unsigned char c = *p;
        if (c == '"' || c == '\\') {
            fputc('\\', f);
            fputc(c, f);
        } else if (c < 0x20) {
            fprintf(f, "\\u%04x", (unsigned int)c);
        } else {
            fputc(c, f);
        }
    }
    fputc('"', f);
}

static char *make_state_path(const char *output_path) {
    const char *suffix = ".wdstate";
    size_t len = strlen(output_path) + strlen(suffix) + 1;
    char *path = (char*)calloc(len, 1);
    if (!path) return NULL;
    snprintf(path, len, "%s%s", output_path, suffix);
    return path;
}

typedef struct job_json {
    char *url;
    char *output;
    char *state;
    char *state_path;
    int64_t file_size;
    int range_supported;
    int P;
    int T;
    size_t block_size;
    int max_retries;
    int *weights;
    int weights_count;
} job_json_t;

static void job_json_free(job_json_t *j) {
    if (!j) return;
    free(j->url);
    free(j->output);
    free(j->state);
    free(j->state_path);
    free(j->weights);
    memset(j, 0, sizeof(*j));
}

static const char *json_skip_ws(const char *p) {
    while (*p && isspace((unsigned char)*p)) p++;
    return p;
}

static int json_expect(const char **p, char c) {
    const char *s = json_skip_ws(*p);
    if (*s != c) return -1;
    *p = s + 1;
    return 0;
}

static int json_parse_string(const char **p, char **out) {
    const char *s = json_skip_ws(*p);
    if (*s != '"') return -1;
    s++;
    size_t cap = 64;
    size_t len = 0;
    char *buf = (char*)malloc(cap);
    if (!buf) return -1;

    while (*s && *s != '"') {
        unsigned char c = (unsigned char)*s++;
        if (c == '\\') {
            c = (unsigned char)*s++;
            switch (c) {
                case '"': c = '"'; break;
                case '\\': c = '\\'; break;
                case '/': c = '/'; break;
                case 'b': c = '\b'; break;
                case 'f': c = '\f'; break;
                case 'n': c = '\n'; break;
                case 'r': c = '\r'; break;
                case 't': c = '\t'; break;
                case 'u': {
                    unsigned int val = 0;
                    for (int i = 0; i < 4; i++) {
                        char h = *s++;
                        if (!isxdigit((unsigned char)h)) {
                            free(buf);
                            return -1;
                        }
                        val <<= 4;
                        if (h >= '0' && h <= '9') val |= (unsigned int)(h - '0');
                        else if (h >= 'a' && h <= 'f') val |= (unsigned int)(h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F') val |= (unsigned int)(h - 'A' + 10);
                    }
                    if (val <= 0x7F) c = (unsigned char)val;
                    else c = '?';
                    break;
                }
                default:
                    free(buf);
                    return -1;
            }
        }
        if (len + 1 >= cap) {
            cap *= 2;
            char *nbuf = (char*)realloc(buf, cap);
            if (!nbuf) {
                free(buf);
                return -1;
            }
            buf = nbuf;
        }
        buf[len++] = (char)c;
    }
    if (*s != '"') {
        free(buf);
        return -1;
    }
    buf[len] = '\0';
    *out = buf;
    *p = s + 1;
    return 0;
}

static int json_parse_int64(const char **p, int64_t *out) {
    const char *s = json_skip_ws(*p);
    if (!s || (!isdigit((unsigned char)*s) && *s != '-')) return -1;
    char *end = NULL;
    long long v = strtoll(s, &end, 10);
    if (end == s) return -1;
    *out = (int64_t)v;
    *p = end;
    return 0;
}

static int json_parse_int(const char **p, int *out) {
    int64_t v = 0;
    if (json_parse_int64(p, &v) != 0) return -1;
    *out = (int)v;
    return 0;
}

static int json_parse_array_int(const char **p, int **out, int *out_count) {
    if (json_expect(p, '[') != 0) return -1;
    int *vals = NULL;
    int count = 0;
    int cap = 0;
    const char *s = json_skip_ws(*p);
    if (*s == ']') {
        *p = s + 1;
        *out = NULL;
        *out_count = 0;
        return 0;
    }
    while (1) {
        int v = 0;
        if (json_parse_int(&s, &v) != 0) {
            free(vals);
            return -1;
        }
        if (count == cap) {
            int new_cap = cap ? cap * 2 : 8;
            int *tmp = (int*)realloc(vals, (size_t)new_cap * sizeof(int));
            if (!tmp) {
                free(vals);
                return -1;
            }
            vals = tmp;
            cap = new_cap;
        }
        vals[count++] = v;

        s = json_skip_ws(s);
        if (*s == ',') {
            s++;
            continue;
        }
        if (*s == ']') {
            s++;
            break;
        }
        free(vals);
        return -1;
    }
    *p = s;
    *out = vals;
    *out_count = count;
    return 0;
}

static int json_skip_value(const char **p) {
    const char *s = json_skip_ws(*p);
    if (*s == '"') {
        char *tmp = NULL;
        int rc = json_parse_string(&s, &tmp);
        free(tmp);
        if (rc != 0) return -1;
    } else if (*s == '{') {
        int depth = 0;
        do {
            if (*s == '"') {
                char *tmp = NULL;
                if (json_parse_string(&s, &tmp) != 0) return -1;
                free(tmp);
                continue;
            }
            if (*s == '{') depth++;
            if (*s == '}') depth--;
            s++;
        } while (*s && depth > 0);
    } else if (*s == '[') {
        int depth = 0;
        do {
            if (*s == '"') {
                char *tmp = NULL;
                if (json_parse_string(&s, &tmp) != 0) return -1;
                free(tmp);
                continue;
            }
            if (*s == '[') depth++;
            if (*s == ']') depth--;
            s++;
        } while (*s && depth > 0);
    } else if (isdigit((unsigned char)*s) || *s == '-') {
        int64_t v = 0;
        if (json_parse_int64(&s, &v) != 0) return -1;
    } else if (strncmp(s, "true", 4) == 0) {
        s += 4;
    } else if (strncmp(s, "false", 5) == 0) {
        s += 5;
    } else if (strncmp(s, "null", 4) == 0) {
        s += 4;
    } else {
        return -1;
    }
    *p = s;
    return 0;
}

static int json_parse_job_object(const char **p, job_json_t *out) {
    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    out->file_size = -1;
    out->range_supported = -1;
    out->P = -1;
    out->T = -1;
    out->block_size = 0;
    out->max_retries = -1;

    if (json_expect(p, '{') != 0) return -1;
    while (1) {
        const char *s = json_skip_ws(*p);
        if (*s == '}') {
            *p = s + 1;
            break;
        }

        char *key = NULL;
        if (json_parse_string(&s, &key) != 0) {
            job_json_free(out);
            return -1;
        }
        if (json_expect(&s, ':') != 0) {
            free(key);
            job_json_free(out);
            return -1;
        }

        if (strcmp(key, "url") == 0) {
            free(out->url);
            if (json_parse_string(&s, &out->url) != 0) {
                free(key);
                job_json_free(out);
                return -1;
            }
        } else if (strcmp(key, "output") == 0) {
            free(out->output);
            if (json_parse_string(&s, &out->output) != 0) {
                free(key);
                job_json_free(out);
                return -1;
            }
        } else if (strcmp(key, "state") == 0) {
            free(out->state);
            if (json_parse_string(&s, &out->state) != 0) {
                free(key);
                job_json_free(out);
                return -1;
            }
        } else if (strcmp(key, "state_path") == 0) {
            free(out->state_path);
            if (json_parse_string(&s, &out->state_path) != 0) {
                free(key);
                job_json_free(out);
                return -1;
            }
        } else if (strcmp(key, "file_size") == 0) {
            if (json_parse_int64(&s, &out->file_size) != 0) {
                free(key);
                job_json_free(out);
                return -1;
            }
        } else if (strcmp(key, "range_supported") == 0) {
            if (json_parse_int(&s, &out->range_supported) != 0) {
                free(key);
                job_json_free(out);
                return -1;
            }
        } else if (strcmp(key, "P") == 0) {
            if (json_parse_int(&s, &out->P) != 0) {
                free(key);
                job_json_free(out);
                return -1;
            }
        } else if (strcmp(key, "T") == 0) {
            if (json_parse_int(&s, &out->T) != 0) {
                free(key);
                job_json_free(out);
                return -1;
            }
        } else if (strcmp(key, "block_size") == 0) {
            int64_t v = 0;
            if (json_parse_int64(&s, &v) != 0) {
                free(key);
                job_json_free(out);
                return -1;
            }
            out->block_size = (size_t)v;
        } else if (strcmp(key, "max_retries") == 0) {
            if (json_parse_int(&s, &out->max_retries) != 0) {
                free(key);
                job_json_free(out);
                return -1;
            }
        } else if (strcmp(key, "weights") == 0) {
            free(out->weights);
            out->weights = NULL;
            out->weights_count = 0;
            if (json_parse_array_int(&s, &out->weights, &out->weights_count) != 0) {
                free(key);
                job_json_free(out);
                return -1;
            }
        } else {
            if (json_skip_value(&s) != 0) {
                free(key);
                job_json_free(out);
                return -1;
            }
        }
        free(key);

        s = json_skip_ws(s);
        if (*s == ',') {
            s++;
            *p = s;
            continue;
        }
        if (*s == '}') {
            *p = s + 1;
            break;
        }
        job_json_free(out);
        return -1;
    }
    return 0;
}

static int job_manager_write_json(const char *path, download_job_t **jobs, size_t count) {
    if (!path) return -1;

    char tmp_path[512];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);
    FILE *f = fopen(tmp_path, "w");
    if (!f) return -1;

    fprintf(f, "{\n  \"version\": 1,\n  \"jobs\": [\n");
    size_t written = 0;
    for (size_t i = 0; i < count; i++) {
        download_job_meta_t meta;
        if (download_job_get_meta(jobs[i], &meta) != 0) continue;

        char *state_path = make_state_path(meta.output_path);
        if (state_path) {
            if (meta.state == JOB_DONE) {
                unlink(state_path);
            } else {
                download_job_save_state(jobs[i], state_path);
            }
        }

        if (written > 0) fprintf(f, ",\n");
        fprintf(f, "    {\n");
        fprintf(f, "      \"url\": ");
        json_write_escaped(f, meta.url);
        fprintf(f, ",\n      \"output\": ");
        json_write_escaped(f, meta.output_path);
        fprintf(f, ",\n      \"state\": ");
        json_write_escaped(f, job_state_string(meta.state));
        fprintf(f, ",\n      \"file_size\": %" PRId64 ",\n", meta.file_size);
        fprintf(f, "      \"range_supported\": %d,\n", meta.range_supported);
        fprintf(f, "      \"P\": %d,\n", meta.P);
        fprintf(f, "      \"T\": %d,\n", meta.T);
        fprintf(f, "      \"block_size\": %zu,\n", meta.block_size);
        fprintf(f, "      \"max_retries\": %d,\n", meta.max_retries_per_block);
        fprintf(f, "      \"weights\": [");
        for (int w = 0; w < meta.P; w++) {
            fprintf(f, "%d%s", meta.weights ? meta.weights[w] : 1, (w + 1 == meta.P) ? "" : ", ");
        }
        fprintf(f, "]");
        if (state_path) {
            fprintf(f, ",\n      \"state_path\": ");
            json_write_escaped(f, state_path);
            fprintf(f, "\n");
        } else {
            fprintf(f, "\n");
        }
        fprintf(f, "    }\n");

        free(state_path);
        download_job_meta_free(&meta);
        written++;
    }
    fprintf(f, "  ]\n}\n");
    fflush(f);
    fsync(fileno(f));
    fclose(f);

    if (rename(tmp_path, path) != 0) {
        unlink(tmp_path);
        return -1;
    }
    return 0;
}

static void *job_manager_main(void *arg) {
    job_manager_t *jm = (job_manager_t*)arg;

    while (1) {
        pthread_mutex_lock(&jm->mtx);
        while (!jm->stop_flag && !jm->dirty) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            timespec_add_ms(&ts, 200);
            pthread_cond_timedwait(&jm->cv, &jm->mtx, &ts);
            if (jm->dirty) break;
        }

        if (jm->stop_flag) {
            pthread_mutex_unlock(&jm->mtx);
            break;
        }
        jm->dirty = 0;

        size_t count = jm->count;
        int max_active = jm->max_active;
        int persist_dirty = jm->persist_dirty;
        int persist_enabled = jm->persist_enabled;
        int persist_interval_ms = jm->persist_interval_ms;
        int64_t last_persist_ms = jm->last_persist_ms;
        char *persist_path = jm->persist_path ? strdup(jm->persist_path) : NULL;
        download_job_t **jobs = NULL;
        if (count > 0) {
            jobs = (download_job_t**)calloc(count, sizeof(download_job_t*));
            if (jobs) memcpy(jobs, jm->jobs, count * sizeof(download_job_t*));
        }
        pthread_mutex_unlock(&jm->mtx);

        if (count > 0 && !jobs) {
            free(persist_path);
            continue;
        }

        int active = 0;
        for (size_t i = 0; i < count; i++) {
            download_job_state_t st = download_job_get_state(jobs[i]);
            if (is_active_state(st)) active++;
            if (is_done_state(st)) download_job_join(jobs[i]);
        }

        for (size_t i = 0; i < count && active < max_active; i++) {
            if (download_job_get_state(jobs[i]) == JOB_IDLE) {
                if (download_job_start(jobs[i]) == 0) active++;
            }
        }
        if (persist_enabled && persist_path) {
            int64_t now = now_millis();
            if (persist_dirty || (persist_interval_ms > 0 &&
                                  now - last_persist_ms >= persist_interval_ms)) {
                if (job_manager_write_json(persist_path, jobs, count) == 0) {
                    pthread_mutex_lock(&jm->mtx);
                    jm->last_persist_ms = now;
                    jm->persist_dirty = 0;
                    pthread_mutex_unlock(&jm->mtx);
                }
            }
        }

        free(persist_path);
        free(jobs);
    }
    return NULL;
}

int job_manager_init(job_manager_t *jm, int max_active) {
    if (!jm) return -1;
    memset(jm, 0, sizeof(*jm));

    if (pthread_mutex_init(&jm->mtx, NULL) != 0) return -1;
    if (pthread_cond_init(&jm->cv, NULL) != 0) {
        pthread_mutex_destroy(&jm->mtx);
        return -1;
    }

    jm->max_active = (max_active > 0) ? max_active : 1;
    jm->dirty = 1;
    jm->persist_dirty = 1;
    jm->persist_enabled = 0;
    jm->persist_interval_ms = 0;
    jm->last_persist_ms = 0;
    jm->persist_path = NULL;
    return 0;
}

void job_manager_destroy(job_manager_t *jm) {
    if (!jm) return;

    if (jm->thread_started) job_manager_stop(jm);

    free(jm->jobs);
    free(jm->persist_path);
    pthread_cond_destroy(&jm->cv);
    pthread_mutex_destroy(&jm->mtx);
}

int job_manager_start(job_manager_t *jm) {
    if (!jm) return -1;

    pthread_mutex_lock(&jm->mtx);
    if (jm->thread_started) {
        pthread_mutex_unlock(&jm->mtx);
        return 0;
    }
    jm->thread_started = 1;
    pthread_mutex_unlock(&jm->mtx);

    int rc = pthread_create(&jm->thread, NULL, job_manager_main, jm);
    if (rc != 0) {
        pthread_mutex_lock(&jm->mtx);
        jm->thread_started = 0;
        pthread_mutex_unlock(&jm->mtx);
        return -1;
    }
    return 0;
}

void job_manager_stop(job_manager_t *jm) {
    if (!jm) return;

    pthread_mutex_lock(&jm->mtx);
    jm->stop_flag = 1;
    jm->dirty = 1;
    pthread_cond_signal(&jm->cv);
    pthread_mutex_unlock(&jm->mtx);

    if (jm->thread_started) {
        pthread_join(jm->thread, NULL);
        jm->thread_started = 0;
    }

    if (jm->persist_enabled && jm->persist_path) {
        pthread_mutex_lock(&jm->mtx);
        size_t count = jm->count;
        download_job_t **jobs = NULL;
        if (count > 0) {
            jobs = (download_job_t**)calloc(count, sizeof(download_job_t*));
            if (jobs) memcpy(jobs, jm->jobs, count * sizeof(download_job_t*));
        }
        pthread_mutex_unlock(&jm->mtx);
        if (count == 0 || jobs) {
            job_manager_write_json(jm->persist_path, jobs, count);
        }
        free(jobs);
    }
}

int job_manager_set_max_active(job_manager_t *jm, int max_active) {
    if (!jm || max_active <= 0) return -1;
    pthread_mutex_lock(&jm->mtx);
    jm->max_active = max_active;
    jm->dirty = 1;
    jm->persist_dirty = 1;
    pthread_cond_signal(&jm->cv);
    pthread_mutex_unlock(&jm->mtx);
    return 0;
}

int job_manager_add(job_manager_t *jm, download_job_t *job) {
    if (!jm || !job) return -1;

    pthread_mutex_lock(&jm->mtx);
    if (jm->count == jm->cap) {
        size_t new_cap = jm->cap ? jm->cap * 2 : 8;
        download_job_t **new_jobs = (download_job_t**)realloc(jm->jobs, new_cap * sizeof(*new_jobs));
        if (!new_jobs) {
            pthread_mutex_unlock(&jm->mtx);
            return -1;
        }
        jm->jobs = new_jobs;
        jm->cap = new_cap;
    }

    int id = (int)jm->count;
    jm->jobs[jm->count++] = job;
    jm->dirty = 1;
    jm->persist_dirty = 1;
    pthread_cond_signal(&jm->cv);
    pthread_mutex_unlock(&jm->mtx);
    return id;
}

download_job_t *job_manager_get(job_manager_t *jm, int job_id) {
    if (!jm || job_id < 0) return NULL;

    pthread_mutex_lock(&jm->mtx);
    download_job_t *job = (job_id < (int)jm->count) ? jm->jobs[job_id] : NULL;
    pthread_mutex_unlock(&jm->mtx);
    return job;
}

int job_manager_count(job_manager_t *jm) {
    if (!jm) return 0;
    pthread_mutex_lock(&jm->mtx);
    int count = (int)jm->count;
    pthread_mutex_unlock(&jm->mtx);
    return count;
}

int job_manager_remove(job_manager_t *jm, download_job_t *job) {
    if (!jm || !job) return -1;
    pthread_mutex_lock(&jm->mtx);
    size_t idx = jm->count;
    for (size_t i = 0; i < jm->count; i++) {
        if (jm->jobs[i] == job) {
            idx = i;
            break;
        }
    }
    if (idx == jm->count) {
        pthread_mutex_unlock(&jm->mtx);
        return -1;
    }
    for (size_t i = idx + 1; i < jm->count; i++) {
        jm->jobs[i - 1] = jm->jobs[i];
    }
    jm->count--;
    jm->dirty = 1;
    jm->persist_dirty = 1;
    pthread_cond_signal(&jm->cv);
    pthread_mutex_unlock(&jm->mtx);
    return 0;
}

int job_manager_wait_all(job_manager_t *jm) {
    if (!jm) return -1;
    while (1) {
        pthread_mutex_lock(&jm->mtx);
        size_t count = jm->count;
        download_job_t **jobs = NULL;
        if (count > 0) {
            jobs = (download_job_t**)calloc(count, sizeof(download_job_t*));
            if (jobs) memcpy(jobs, jm->jobs, count * sizeof(download_job_t*));
        }
        pthread_mutex_unlock(&jm->mtx);

        if (count > 0 && !jobs) return -1;

        int all_done = 1;
        for (size_t i = 0; i < count; i++) {
            download_job_state_t st = download_job_get_state(jobs[i]);
            if (!is_done_state(st)) {
                all_done = 0;
                break;
            }
        }
        free(jobs);
        if (all_done) break;
        msleep(200);
    }
    return 0;
}

int job_manager_enable_persistence(job_manager_t *jm, const char *json_path, int interval_ms) {
    if (!jm || !json_path || interval_ms <= 0) return -1;
    pthread_mutex_lock(&jm->mtx);
    free(jm->persist_path);
    jm->persist_path = strdup(json_path);
    jm->persist_enabled = (jm->persist_path != NULL);
    jm->persist_interval_ms = interval_ms;
    jm->persist_dirty = 1;
    jm->dirty = 1;
    pthread_cond_signal(&jm->cv);
    pthread_mutex_unlock(&jm->mtx);
    return jm->persist_enabled ? 0 : -1;
}

int job_manager_load_persisted(job_manager_t *jm, const char *json_path,
                               const downloader_config_t *cfg, int *out_loaded) {
    if (!jm || !json_path || !cfg) return -1;
    if (out_loaded) *out_loaded = 0;

    FILE *f = fopen(json_path, "r");
    if (!f) return 0;

    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0) {
        fclose(f);
        return 0;
    }

    char *buf = (char*)calloc((size_t)len + 1, 1);
    if (!buf) {
        fclose(f);
        return -1;
    }
    if (fread(buf, 1, (size_t)len, f) != (size_t)len) {
        free(buf);
        fclose(f);
        return -1;
    }
    fclose(f);

    const char *p = buf;
    if (json_expect(&p, '{') != 0) {
        free(buf);
        return -1;
    }

    while (1) {
        p = json_skip_ws(p);
        if (*p == '}') {
            p++;
            break;
        }
        char *key = NULL;
        if (json_parse_string(&p, &key) != 0) break;
        if (json_expect(&p, ':') != 0) {
            free(key);
            break;
        }
        if (strcmp(key, "jobs") == 0) {
            if (json_expect(&p, '[') != 0) {
                free(key);
                break;
            }
            p = json_skip_ws(p);
            if (*p == ']') {
                p++;
                free(key);
                continue;
            }
            while (1) {
                job_json_t j;
                if (json_parse_job_object(&p, &j) != 0) {
                    job_json_free(&j);
                    break;
                }
                download_job_state_t st = job_state_from_string(j.state);
                if (st != JOB_DONE && j.url && j.output && j.P > 0 && j.T > 0) {
                    download_job_t *job = download_job_create(j.url, j.output, cfg);
                    if (job) {
                        if (j.range_supported >= 0 || j.file_size > 0) {
                            download_job_set_probe(job, j.file_size, j.range_supported);
                        }
                        if (j.weights && j.weights_count == j.P) {
                            download_job_set_scheduling(job, j.P, j.weights, j.T);
                        } else {
                            int *weights = (int*)calloc((size_t)j.P, sizeof(int));
                            if (weights) {
                                for (int i = 0; i < j.P; i++) weights[i] = 1;
                                download_job_set_scheduling(job, j.P, weights, j.T);
                                free(weights);
                            }
                        }
                        if (j.state_path) {
                            download_job_load_state(job, j.state_path);
                        }
                        if (st == JOB_PAUSED) {
                            download_job_set_start_paused(job, 1);
                        }
                        job_manager_add(jm, job);
                        if (out_loaded) (*out_loaded)++;
                    }
                }
                job_json_free(&j);

                p = json_skip_ws(p);
                if (*p == ',') {
                    p++;
                    continue;
                }
                if (*p == ']') {
                    p++;
                    break;
                }
                break;
            }
        } else {
            if (json_skip_value(&p) != 0) {
                free(key);
                break;
            }
        }
        free(key);
        p = json_skip_ws(p);
        if (*p == ',') {
            p++;
            continue;
        }
    }

    free(buf);
    return 0;
}

int job_manager_pause(job_manager_t *jm, int job_id) {
    download_job_t *job = job_manager_get(jm, job_id);
    return job ? download_job_pause(job) : -1;
}

int job_manager_resume(job_manager_t *jm, int job_id) {
    download_job_t *job = job_manager_get(jm, job_id);
    return job ? download_job_resume(job) : -1;
}

int job_manager_stop_job(job_manager_t *jm, int job_id) {
    download_job_t *job = job_manager_get(jm, job_id);
    return job ? download_job_stop(job) : -1;
}
