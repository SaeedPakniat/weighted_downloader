
#pragma once

#include <stddef.h>
#include <stdint.h>

void *xcalloc(size_t n, size_t sz);

int64_t now_millis(void);
void msleep(int ms);

void trim_newline(char *s);
