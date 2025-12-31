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

Example GUI run:
```bash
./weighted_downloader --gui <URL> out.bin
```

### Controls (ncurses UI)

- `p` pause downloads (in-flight transfers finish; workers wait)
- `r` resume
- `q` quit gracefully (shutdown, join threads, close files, flush CSV)

### GUI Controls

- Pause / Resume buttons
- Quit button or window close (signals shutdown and exits cleanly)
