# Weighted Parallel File Downloader (pthreads + libcurl)

A download-manager-like tool that downloads a file in parallel using HTTP Range requests,
while enforcing **weighted priorities (1–10)** across partitions using a **user-space DRR scheduler**.

## Build

Requirements:
- gcc
- libcurl development package
- pthreads (via glibc)
- ncurses development package
- gtk4 development package (for `--gui`)
- pkgconf (for `pkg-config`)

Build:
```bash
make
```

Arch install:
```bash
sudo pacman -S gtk4 pkgconf
```

## Run

```bash
./weighted_downloader <URL> [output_file]
```

Optional flags:
```bash
./weighted_downloader --no-ui <URL> [output_file]
./weighted_downloader --gui <URL> [output_file]
```
Flags are mutually exclusive.

Resume persisted jobs:
```bash
./weighted_downloader
./weighted_downloader --no-ui
./weighted_downloader --gui
```

Example GUI run:
```bash
./weighted_downloader --gui <URL> out.bin
```
You can also start the GUI with no URL and add downloads from the dialog.

### Controls (ncurses UI)

- `p` pause downloads (in-flight transfers finish; workers wait)
- `r` resume
- `q` quit gracefully (shutdown, join threads, close files, flush CSV)

### GUI Controls

- Add Download dialog (URL + output path)
- Per-job Pause / Resume / Delete
- Live per-job progress, speed, ETA (via periodic snapshots)

## Architecture (Phase 1)

- `job.c`/`job.h` wrap the existing scheduler + worker pool into a reusable `download_job_t` API.
- `job_manager.c`/`job_manager.h` provide a thread-safe manager that starts jobs with a max-active limit.
- The CLI now prepares jobs interactively and runs them through the job manager.

## Persistence (Phase 2)

- `downloads.json` stores job metadata (URL, output, scheduling, and state).
- Per-job resume state is saved in `<output>.wdstate`.
- On startup, the app loads `downloads.json` and resumes incomplete downloads automatically.
