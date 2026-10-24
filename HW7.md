# HW7 — Flat order book + open-addressing symbol map

All the code is in [`HW7.cpp`](HW7.cpp). It uses only the standard library.

```
clang++ -std=c++17 -O2 HW7.cpp -o hw7 && ./hw7
```

`Book` and `SymMap` sit at the top of the file and match the CI interface exactly. For the autograder, `order_book.hpp` is only:

```cpp
#define HW7_NO_MAIN
#include "HW7.cpp"
```

**Setup:** Apple M1, Apple clang 21, `-O2`.

- **Timer:** `cntvct_el0` on arm64 and `__rdtsc()` on x86. The counter is calibrated against `steady_clock`.
- **Method:** one warm-up run, then the median of 51 runs. Each run is about 1M calls, and each result goes through `doNotOptimize`.
- **Cycles/op:** on arm64, `cntvct_el0` is a 24 MHz timer, not a cycle counter. I measure the core clock by timing a chain of dependent 1-cycle `add`s, then compute cycles = ns × GHz. It measured 1.95 GHz, so the run was probably on an efficiency core. On x86, cycles/op is TSC ticks.

## Design

- **Book:**
  - Each side is a flat `uint32_t` array of total quantity per $0.01 tick.
  - The array holds 65,536 ticks, a band centred on the first price seen. Prices outside the band are refused, never written out of bounds. This covers the $720 NFLX case.
  - `add` updates the best slot and caches the best price as a `double`, so `best_bid()`/`best_ask()` are a single load.
  - The only scan is in `cancel`, and only when the best level empties. It walks one tick at a time over contiguous memory.
  - An empty side returns `0.0`.
- **SymMap:**
  - Linear-probing open addressing in a power-of-two table that doubles at 50% load.
  - A key (up to 16 chars) is stored as two zero-padded `uint64_t`, so comparing keys is two integer compares and the hash is one multiply.
  - `get` returns `(uint64_t)-1` when the key is absent.

## Results

| `best_bid()` | ns/op | cycles/op | vs after |
|---|---:|---:|---:|
| **Book (after):** cached top | **0.51** | **1.0** | 1× |
| BookV1 (before): scan the array on every read | 33,700 | 65,650 | 65,800× slower |
| `std::map` book (`rbegin`) | 3.59 | 7.0 | 7.0× slower |

| `SymMap::get()` (CI's 8 symbols) | ns/op | cycles/op | vs after |
|---|---:|---:|---:|
| **SymMap (after):** 2×`u64` keys | **4.63** | **9.0** | 1× |
| SymMapV1 (before): `char[16]`, FNV-1a, `strncmp` | 8.64 | 16.8 | 1.9× slower |
| `std::unordered_map<std::string>` | 23.95 | 46.7 | 5.2× slower |

CI metrics from `make book`: `book_bbo_ns_per_op` ≈ 0.51 and `symmap_get_ns_per_op` ≈ 4.7–5.0. All correctness checks pass.

**Why the gap to the standard containers:**

- **`std::map` book:** `best_bid` follows pointers. libc++ finds `rbegin()` by walking from the root down to the rightmost node, so each read is a chain of dependent loads on nodes scattered around the heap. The flat book reads one cached `double`.
- **`std::unordered_map<std::string>`:** each `get` builds a `std::string` from the `const char*` and hashes it byte by byte. It then follows a bucket pointer to a separately allocated node and does a string compare. `SymMap` does a short copy into two registers and one multiply, then probes a 768-byte table that stays in L1.

## The single change that helped most

The change that helped most was caching the best price on the write path. My first flat book, `BookV1`, already had the right layout, but `best_bid()` searched the array for the highest non-empty slot. Because the band is centred on the market, that is about 32K slots per call, or 33 µs. Keeping the best slot and its price up to date in `add` and `cancel` makes the read a single load from a cache line that stays hot: 0.51 ns, about 1 cycle. The cost moves to `add` (one compare) and to `cancel`, which scans only when the best level empties, and then only to the next non-empty tick nearby. Reads happen far more often than updates on the hot path, so that is the right trade. It is also what makes the flat book beat `std::map` by 7×. `std::map` has the same O(1) best element in principle, but reaching it still takes dependent pointer loads.
