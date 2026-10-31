# HW8 — Sliding-window event counter (ring buffer)

All the code is in [`HW8.cpp`](HW8.cpp). It uses only the standard library.

```
clang++ -std=c++17 -O2 HW8.cpp -o hw8 && ./hw8
```

`RollingCounter` sits at the top of the file and matches the CI interface exactly. For the autograder, `rolling_counter.hpp` is only:

```cpp
#define HW8_NO_MAIN
#include "HW8.cpp"
```

**Setup:** Apple M1, Apple clang 21, `-O2`.

- **Method:** each op is one `add(t)` plus one `count(t)`, with `t` advancing 1 ns per op and a 1000 ns window. This is the CI stream, with about 1000 live events.
- **Timing:** after a warm-up, ops are timed in batches of 1024 with `steady_clock`, over 20,000 batches. The percentiles are over the per-batch ns/op, because one op is far below the timer's resolution.

## Design

- **Ring of timestamps:**
  - The buffer is a power-of-two `std::vector<uint64_t>`, starting at 4096 slots (32 KB).
  - `head_` and `tail_` only ever increase. A slot index is `idx & mask_`, and the live count is `tail_ - head_`.
  - Events arrive in time order, so the ring is sorted from oldest to newest.
- **`add`:** writes the timestamp at the tail. If the ring is full, it first doubles the buffer, so a live event is never overwritten. `grow()` is out of line, so the hot path is one compare, one store and one add.
- **`count`:** pops events from the front while `ts <= now - window`, then returns `tail_ - head_`.
- **Unsigned edge:** the cutoff `now - window` is only computed when `now >= window`. Below that, the cutoff would be negative and nothing can expire. Clamping it to 0 would be wrong: it would expire a real event at `ts == 0`. When `now == window`, the cutoff is 0, and an event at `ts == 0` correctly expires.

## Why it is amortized O(1)

`count()` is called with a non-decreasing clock, so an event that has expired stays expired. Each event is pushed once and popped once, so over `n` operations the `while` loop runs at most `n` times in total. That makes `add` and `count` amortized O(1).

A single `count()` after a big clock jump can pop many events at once. The total work is still bounded by the number of adds. Growing the ring copies the live events, but the capacity doubles each time, so the copying is also amortized O(1) per add.

## Results

CI stream, ns per `add` + `count`:

| | p50 | p99 | p99.9 | vs ring (p50) |
|---|---:|---:|---:|---:|
| **RollingCounter (ring)** | **3.95** | **8.14** | **19.45** | 1× |
| `std::deque` (same algorithm) | 7.00 | 22.09 | 224.16 | 1.8× slower |
| ScanCounter (rescan the window every `count`) | 738.94 | 5,722 | 18,791 | 187× slower |

- **CI metric (`make rolling`):** `rolling_ns_per_op` ≈ 3.8–4.9. The two CI checks, `rc_window` and `rc_mixed`, pass.
- **Local tests:** they also cover the `ts == now - window` boundary, the `ts == 0` unsigned edge, ring growth past 4096 live events, and a cross-check against the deque version that includes clock jumps.
- **Noise:** this laptop is noisy run to run. The p50 numbers are stable to within about 15%, but the tail numbers vary a lot between runs.

**Why the ring beats the others:**

- **ScanCounter** does O(window) work per call: it walks 1000 timestamps on every `count`. That is the cost the amortized design avoids.
- **`std::deque`** runs the same algorithm, but its storage is a chain of blocks reached through a map of pointers. Every few hundred events, it allocates a new block and frees an old one, which is where its p99.9 spikes come from. The ring is one contiguous buffer and never allocates at steady state.

## The single change that helped most

Copying the members into locals in `count()`, and moving `grow()` out of line, helped most. My first ring had the same algorithm but used `head_`, `tail_` and `mask_` directly. The buffer holds `uint64_t` values, and so do those members, so the compiler had to assume a store into the buffer might change them, and it reloaded them on every iteration. Inlining `grow()` also cluttered the register allocation in the hot loop. Making these two changes took the ring from about 7.5 ns to about 3.7 ns per op. Before the change it was slower than `std::deque`; after it, it was faster.
