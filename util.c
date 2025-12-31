
#define _POSIX_C_SOURCE 200809L

#include "util.h"

#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <string.h>
#include <unistd.h>

void *xcalloc(size_t n, size_t sz) {
    void *p = calloc(n, sz);
    if (!p) {
        fprintf(stderr, "Out of memory\n");
        exit(1);
    }
    return p;
}

int64_t now_millis(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + (int64_t)ts.tv_nsec / 1000000;
}

void msleep(int ms) {
    if (ms <= 0) return;
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

void trim_newline(char *s) {
    if (!s) return;
    size_t n = strlen(s);
    while (n > 0 && (s[n-1] == '\n' || s[n-1] == '\r')) {
        s[n-1] = '\0';
        n--;
    }
}
