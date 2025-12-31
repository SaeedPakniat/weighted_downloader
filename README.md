# Weighted Parallel File Downloader (pthreads + libcurl)

A download-manager-like tool that downloads a file in parallel using HTTP Range requests,
while enforcing **weighted priorities (1–10)** across partitions using a **user-space DRR scheduler**.

## Build

Requirements:
- gcc
- libcurl development package
- pthreads (via glibc)

Build:
```bash
make
