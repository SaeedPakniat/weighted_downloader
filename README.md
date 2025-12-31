# Weighted Parallel File Downloader (pthreads + libcurl)

A download-manager-like tool that downloads a file in parallel using HTTP Range requests,
while enforcing **weighted priorities (1–10)** across partitions using a **user-space DRR scheduler**.

## Build

Requirements:
- gcc
- libcurl development package
- pthreads (via glibc)
- ncurses development package

Build:
```bash
make
```

## Run

```bash
./weighted_downloader <URL> [output_file]
```

Optional flag for headless/CI:
```bash
./weighted_downloader --no-ui <URL> [output_file]
```

### Controls (ncurses UI)

- `p` pause downloads (in-flight transfers finish; workers wait)
- `r` resume
- `q` quit gracefully (shutdown, join threads, close files, flush CSV)
