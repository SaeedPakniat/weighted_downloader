
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <pthread.h>

typedef struct task_block {
    int partition_id;
    int64_t start;
    size_t len;
    int retry;
    struct task_block *next;
} task_block_t;

typedef struct partition_desc {
    int id;
    int64_t start;
    int64_t end;              // inclusive
    int64_t next_off;         // next offset to schedule (monotonic within partition)
    int64_t total_bytes;
    int64_t downloaded_bytes; // updated on successful writes
    int weight;

    // DRR deficit in bytes
    int64_t deficit;
    int finished;
} partition_desc_t;

typedef struct scheduler {
    pthread_mutex_t mtx;
    pthread_cond_t  cv;

    int num_partitions;
    int64_t file_size;
    size_t block_size;
    int max_retries;

    partition_desc_t *parts;

    // DRR round-robin pointer
    int rr_index;

    // retry queue for failed blocks
    task_block_t *retry_head;
    task_block_t *retry_tail;

    int shutdown;
} scheduler_t;

int scheduler_init(scheduler_t *s, int P, int64_t file_size, size_t block_size,
                   const int *weights, int max_retries);
void scheduler_destroy(scheduler_t *s);

int scheduler_get_task(scheduler_t *s, task_block_t *out); // returns 1 if got task, 0 if no work and done/shutdown
void scheduler_requeue_task(scheduler_t *s, const task_block_t *t);
void scheduler_mark_downloaded(scheduler_t *s, int partition_id, int64_t bytes);

int scheduler_all_done(scheduler_t *s);
void scheduler_signal_shutdown(scheduler_t *s);

// For progress thread
int scheduler_snapshot(scheduler_t *s, int64_t *per_part_downloaded, int *per_part_finished,
                       int64_t *out_total_downloaded);

// Worker pool / networking
typedef struct downloader_config downloader_config_t;

typedef struct worker_pool {
    pthread_t *threads;
    int num_threads;
} worker_pool_t;

int worker_pool_start(worker_pool_t *pool, scheduler_t *sched,
                      const char *url, int out_fd, int T,
                      const downloader_config_t *cfg);
void worker_pool_join(worker_pool_t *pool);
