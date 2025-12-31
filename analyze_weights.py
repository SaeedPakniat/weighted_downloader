
import csv
import sys
from collections import defaultdict

def main(path: str):
    rows = []
    with open(path, "r", newline="") as f:
        r = csv.DictReader(f)
        for row in r:
            rows.append((int(row["timestamp_ms"]),
                         int(row["partition_id"]),
                         int(row["bytes_downloaded_total"])))

    if not rows:
        print("No data.")
        return

    # Final bytes per partition = max over time
    final = defaultdict(int)
    for ts, pid, b in rows:
        if b > final[pid]:
            final[pid] = b

    pids = sorted(final.keys())
    total = sum(final[pid] for pid in pids)
    smallest = min(final[pid] for pid in pids) if pids else 1

    print(f"File: {path}")
    print(f"Partitions: {len(pids)}")
    print(f"Total bytes (sum of partition totals): {total}")
    print()
    print("Final bytes / share / ratio(vs smallest):")
    for pid in pids:
        b = final[pid]
        share = (b / total) if total > 0 else 0.0
        ratio = (b / smallest) if smallest > 0 else 0.0
        print(f"  part {pid:2d}: {b:12d}  share={share:7.3%}  ratio={ratio:8.3f}")

    # Optional: crude overlap window estimate by checking mid-run samples:
    # compute average slope per partition over first half of timeline
    max_ts = max(ts for ts, _, _ in rows)
    mid = max_ts // 2
    first = defaultdict(list)
    for ts, pid, b in rows:
        if ts <= mid:
            first[pid].append((ts, b))
    print()
    print("Approx avg rate (first half) bytes/sec:")
    for pid in pids:
        pts = sorted(first[pid])
        if len(pts) < 2:
            print(f"  part {pid:2d}: n/a")
            continue
        t0, b0 = pts[0]
        t1, b1 = pts[-1]
        dt = max(1, t1 - t0)
        rate = (b1 - b0) * 1000.0 / dt
        print(f"  part {pid:2d}: {rate:10.1f} B/s")

    print("\nIf ratios are not close to weights, use a larger file or fewer threads (to reduce noise).")

if __name__ == "__main__":
    if len(sys.argv) != 2:
        print(f"Usage: {sys.argv[0]} <weights_log.csv>")
        sys.exit(1)
    main(sys.argv[1])
