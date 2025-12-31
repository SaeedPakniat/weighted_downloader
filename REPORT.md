# Weighted Parallel Downloader Report

Name: Saeed Pakniat  
Student ID: 40139537

## Environment

- OS: Arch Linux
- Kernel: Linux 6.18.2-arch2-1 x86_64
- CPU: 12th Gen Intel(R) Core(TM) i7-12700H (20 logical CPUs)
- RAM: 15 GiB
- libcurl: 8.17.0
- gcc: 15.2.1

## Architecture Summary

- Thread model: worker thread pool (pthread) + progress UI thread.
- Scheduler: user-space Deficit Round Robin (DRR) over fixed-size blocks.
- File I/O: workers use `pwrite()` to write blocks at explicit offsets.
- UI: ncurses progress view for CLI; GTK GUI for multi-job management.
- Persistence: `downloads.json` for job metadata and `<output>.wdstate` for resume state.

## DRR Algorithm Explanation (Why It Enforces Weights)

Each partition i has:
- Weight wi in [1..10]
- Deficit counter Di (bytes)
- Next byte offset to serve

DRR uses fixed block size B. On each round, for every active partition:
1) Add quantum Qi = wi * B to Di once per round.
2) If Di >= size(next block), schedule a block and subtract that size from Di.
3) If Di still covers another block, the partition can be served again before moving on.
4) Otherwise move to the next partition. Round counter advances on wrap.

Why it works:
- In each round, partition i gains credit proportional to wi.
- Over time, the total scheduled bytes for each partition converges to the ratio of wi.
- Because Di can carry over, short-term jitter is smoothed, preventing starvation.
- Last blocks smaller than B are charged by actual length, keeping byte accounting correct.

In this implementation, the scheduler:
- Adds quantum once per round per partition.
- Uses `deficit >= block_len` to decide service.
- Advances the round when the round-robin index wraps.

## Synchronization Rationale (Race/Deadlock Safety)

- Scheduler state (deficit, offsets, done flags) is protected by one mutex.
- Workers call `scheduler_get_task()` under the mutex and wait on a condition variable
  if nothing is schedulable or if paused.
- When a block finishes or a retry is queued, workers broadcast to wake waiters.
- The `pwrite()` callback writes at explicit offsets, so no shared file pointer exists.
  This eliminates races that would occur with concurrent `write()` on a shared fd.
- Pausing uses a condition variable: threads sleep without holding the lock and wake
  cleanly, avoiding deadlock and busy spinning.

## Requirements Compliance (OSLabProject.pdf)

Functional requirements:
- HTTP(S) download correctness: Implemented via libcurl in workers and single-thread fallback.
- Range detection + fallback: Probe checks Range support; falls back to single-thread.
- Partitioning: File size is split into P equal ranges; each is scheduled independently.
- Weighted scheduling: DRR scheduler enforces proportional service in user space.
- Threading model: pthread worker pool + progress thread.
- Progress reporting: per-partition and total progress shown in ncurses UI; GUI shows per-job progress.
- File correctness: `pwrite()` used for block writes; end-of-download verification checks bytes.
- Clean shutdown: workers are joined, scheduler shutdown signaled, resources released.

Non-functional:
- Correctness first: file size + `sha256sum` validation recorded below.
- No data races: scheduler guarded by mutex; file writes use `pwrite()`.
- Robustness: retry queue on transient curl errors; single-thread fallback retries.
- Portability: POSIX APIs, gcc + pthreads + libcurl on Linux.

Optional (implemented):
- Resume support: `.wdstate` files + `downloads.json` job restore.
- GUI (bonus): GTK multi-download UI with add/pause/resume/delete.

## Test Artifacts

- Files:
  - `x-system_02_download_10_1_report.jpg`
  - `x-system_02_download_10_5_1_report.jpg`
- CSV logs:
  - `weights_log_7968308.csv` (10:1)
  - `weights_log_8165564.csv` (10:5:1)

## Test 1: Weight Ratio 10:1

Command:
./weighted_downloader --no-ui https://cdn.hasselblad.com/f/77891/11656x8742/0b7db9801e/x-system_02_download.jpg x-system_02_download_10_1_report.jpg

Inputs:
- P = 2
- Weights = 10, 1
- T = 2

Run summary:
- total_time = 127.889 s
- avg_throughput = 0.840 MiB/s
- bytes = 112676898
- high-weight completion time (partition 0) = 70440 ms

analyze_weights.py output:
File: weights_log_7968308.csv
Partitions: 2
Total bytes (sum of partition totals): 112568337

Final bytes / share / ratio(vs smallest):
  part  0:     56338449  share=50.048%  ratio=   1.002
  part  1:     56229888  share=49.952%  ratio=   1.000

Approx avg rate (first half) bytes/sec:
  part  0:   801090.5 B/s
  part  1:    78255.6 B/s

Fairness analysis:
- Rate ratio ~ 801090.5 / 78255.6 = 10.24:1, close to the target 10:1.
- Final byte totals are equal because partitions are equal size; weights affect
  service rate while both partitions are active, not final totals.

## Test 2: Weight Ratio 10:5:1

Command:
./weighted_downloader --no-ui https://cdn.hasselblad.com/f/77891/11656x8742/0b7db9801e/x-system_02_download.jpg x-system_02_download_10_5_1_report.jpg

Inputs:
- P = 3
- Weights = 10, 5, 1
- T = 3

Run summary:
- total_time = 110.283 s
- avg_throughput = 0.974 MiB/s
- bytes = 112676898
- high-weight completion time (partition 0) = 59379 ms

analyze_weights.py output:
File: weights_log_8165564.csv
Partitions: 3
Total bytes (sum of partition totals): 112473452

Final bytes / share / ratio(vs smallest):
  part  0:     37558966  share=33.394%  ratio=   1.005
  part  1:     37558966  share=33.394%  ratio=   1.005
  part  2:     37355520  share=33.213%  ratio=   1.000

Approx avg rate (first half) bytes/sec:
  part  0:   630935.2 B/s
  part  1:   310687.8 B/s
  part  2:    62137.6 B/s

Fairness analysis:
- Rate ratios ~ 630935.2 : 310687.8 : 62137.6 = 10.15 : 5.00 : 1.
- As above, final totals are equal because partitions are equal size.

## Correctness Verification (SHA-256)

sha256sum outputs:
- c11d3bc70125de08bf2cbc83602cf31834fedd1141565202f6cfc029532b3e23  x-system_02_download_10_1_report.jpg
- c11d3bc70125de08bf2cbc83602cf31834fedd1141565202f6cfc029532b3e23  x-system_02_download_10_5_1_report.jpg

Both files match each other byte-for-byte and match the server content for this URL.

## Appendix: Requirement-to-Code Mapping

- HTTP(S) URL input: `main.c` (`main`, CLI parsing) and `downloader.c` (`downloader_prepare_job_interactive`).
- Range detection + fallback: `job.c` (`download_job_probe`, `single_thread_download`) and `downloader.c` (range-supported branch).
- Partitioning logic: `job.c` (`download_job_start`, range setup) and `scheduler.c` (`scheduler_init`).
- Weighted scheduling (DRR): `scheduler.c` (`drr_pick_next_block_nolock`, `scheduler_get_task`).
- Proof of proportionality (logging): `progress.c` (`csv_thread_main`) and `analyze_weights.py`.
- Threading model (pthread pool): `scheduler.c` (`worker_pool_start`, `worker_main`).
- Synchronization (mutex/condvar): `scheduler.c` (`scheduler_get_task`, `scheduler_set_paused`, `scheduler_requeue_task`).
- Race-free file I/O (pwrite): `scheduler.c` (`worker_pwrite_cb`).
- Retry logic and robustness: `scheduler.c` (`is_transient_curl`, `scheduler_requeue_task`) and `job.c` (`single_thread_download` retry loop).
- Progress reporting (TUI): `progress.c` (`draw_tui`, `progress_thread_main`).
- GUI (bonus): `gui.c` (`gui_run`, list + dialog wiring) and `job_manager.c` (job control APIs).
- Persistence (optional): `job_manager.c` (`job_manager_load_persisted`, `job_manager_enable_persistence`) and `job.c` (`download_job_save_state`, `download_job_load_state`).
- Portability (POSIX): use of pthreads, pwrite, and standard C in `scheduler.c`, `job.c`, `util.c`.
- Clean shutdown/resource cleanup: `job.c` (`download_job_join`, cleanup blocks), `scheduler.c` (`worker_pool_join`, `scheduler_destroy`).
