#!/usr/bin/env python3
"""Compute p50/p99/p99.9 + tail histogram from a bot log's `LAT <sym> <us>` lines.

Usage:
    python3 scripts/local_latency_report.py <logfile> [duration_seconds]
"""
import sys, math

path = sys.argv[1]
duration_s = float(sys.argv[2]) if len(sys.argv) > 2 else None

vals = []
with open(path) as f:
    for line in f:
        if line.startswith("LAT "):
            vals.append(int(line.split()[2]))

vals.sort()
n = len(vals)
if n == 0:
    sys.exit("No LAT lines found in log.")

def pct(p):
    idx = min(n - 1, max(0, math.ceil(p / 100 * n) - 1))
    return vals[idx]

print(f"n         = {n} samples")
print(f"p50       = {pct(50)} us  (median)")
print(f"p99       = {pct(99)} us  (1 in 100)")
print(f"p99.9     = {pct(99.9)} us  (1 in 1000)  <-- GRADE")
print(f"min       = {vals[0]} us")
print(f"max       = {vals[-1]} us")
print(f"avg       = {sum(vals)/n:.1f} us")
if duration_s:
    print(f"throughput= {n/duration_s:.1f} msgs/s")
print()
print("tail histogram (us):")
buckets = [0, 50, 100, 200, 300, 500, 1000, 2000, 5000, 10000, float("inf")]
for lo, hi in zip(buckets, buckets[1:]):
    count = sum(1 for v in vals if lo <= v < hi)
    hi_label = "inf" if hi == float("inf") else str(hi)
    print(f"  [{lo:>6}-{hi_label:>6}) {count:>4}  {'#' * count}")
