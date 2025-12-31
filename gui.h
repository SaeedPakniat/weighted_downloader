#pragma once

#include <stdint.h>

#include "scheduler.h"

int gui_run(scheduler_t *sched, int P, int64_t file_size,
            const int *weights, const int64_t *part_sizes);
