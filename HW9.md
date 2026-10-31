# HW9 — SeqLock: one writer, many lock-free readers

All the code is in [`HW9.cpp`](HW9.cpp). It uses only the standard library.

```
clang++ -std=c++17 -O2 -pthread HW9.cpp -o hw9 && ./hw9
clang++ -std=c++17 -O1 -g -pthread -fsanitize=thread -DHW9_TSAN HW9.cpp -o hw9_tsan && ./hw9_tsan
```

**Setup:**

- **Machine:** Apple M1, Apple clang 21, `-O2`.
- **Threads:** 1 writer and 3 readers, which fills the M1's 4 performance cores. The payload is `Quote { double bid, ask; uint32_t bq, aq; }`, which is 24 bytes.
- **Method:** one warm-up run, then the median of 21 runs. Within each run, I take the median reader's ns per read.
- **Cycles/op:** cycles = ns × core GHz. On arm64 the system timer isn't a cycle counter, so I measure the core clock by timing a chain of dependent adds, as in HW7. It measured 1.95 GHz.

## Design

- **The protocol:**
  - `seq_` is even while the data is stable and odd while a write is in progress.
  - `write`:
    1. Store `seq+1`.
    2. Issue a release fence.
    3. Store the payload.
    4. Store `seq+2` with release.
  - `read`:
    1. Load `seq` with acquire. If it is odd, give up.
    2. Copy the payload.
    3. Issue an acquire fence.
    4. Load `seq` again.
  - If the two loads of `seq` match, no write overlapped the copy, so the value is whole. Otherwise `read()` returns `false`, and `load()` retries.
- **Why these memory orders:**
  - **Writer:** the release store of the even `seq` makes all the payload stores visible before the "stable" signal. The release fence after the odd store keeps the payload stores from moving ahead of it.
  - **Reader:** the acquire load stops the payload loads from moving above the first `seq` check. The acquire fence stops them from moving below the second check.
- **Payload as relaxed atomics:** the payload is stored as `std::atomic<uint64_t>` words accessed with `memory_order_relaxed`, not as a plain `T`. On arm64 and x86 these compile to ordinary loads and stores, so they cost nothing. They also make the program race-free in the C++ memory model.
- **One cache line:** `seq_` and the 3 payload words share a single 64-byte line, so a read touches only one line.

## Correctness

The writer publishes `k = 1, 2, 3, …` back to back. Every field of a quote is derived from `k`: `bid = k`, `ask = k + 0.5`, `bq = k`, `aq = ~k`. A mix of two writes therefore cannot pass the consistency check. Three readers each do 2M reads.

| Box | Torn values returned | `k` ever goes backwards |
|---|---:|---|
| **SeqLock** | **0** | no |
| `std::shared_mutex` | 0 | no |
| No lock (control) | 5,789,943 | — |

The no-lock control shows that the test really detects tearing: without the seq check, readers see mixed quotes millions of times.

## Results

Reader cost with 3 readers. These are the numbers from the full benchmark program:

| Writer back-to-back (worst case) | ns/read | cycles/read | total Mreads/s | vs shared_mutex |
|---|---:|---:|---:|---:|
| **SeqLock (after)** | **8.54** | **16.6** | **214.7** | **96× faster** |
| SeqLockV1 (before) | 138.98 | 270.7 | 21.2 | 5.9× faster |
| `std::shared_mutex` | 817.39 | 1,591.9 | 3.6 | 1× |

| Writer every ~1 µs (a busy feed) | ns/read | cycles/read | total Mreads/s | vs shared_mutex |
|---|---:|---:|---:|---:|
| **SeqLock (after)** | **2.48** | **4.8** | **1,186.6** | **265× faster** |
| SeqLockV1 (before) | 2.38 | 4.6 | 1,232.0 | 276× faster |
| `std::shared_mutex` | 656.63 | 1,278.8 | 4.5 | 1× |

- **Why `shared_mutex` is slow even for readers:** every `lock_shared` is an atomic read-modify-write on the reader count. So the "readers" all write to the same cache line, and it bounces between cores on every read.
- **Writer starvation:** in the paced run, the `shared_mutex` writer managed only 0.1 writes/µs instead of 1. The readers kept the lock held, so the writer starved. A seqlock writer never waits for readers.
- **When before and after match:** with a slow writer, V1 and the final version are the same. The line almost never changes, so it stays in every reader's cache. The difference only appears when the writer is constantly invalidating the line.

## The single change that helped most

The change that helped most was putting `seq` and the payload on the same cache line. My first seqlock (`SeqLockV1`) gave `seq` and the data each their own `alignas(64)` line. That looks tidy, but it means each read misses on two lines that the writer is constantly invalidating, one after the other, and the writer has to take ownership of two lines per write.

I isolated the change by flipping only the layout:

- **SeqLockV1 → same line:** 139 ns → 6.8 ns per read.
- **Final version → split across two lines:** 8.5 ns → 48.6 ns per read.

The other differences mattered much less. Replacing V1's `fetch_add` with plain stores only took it from 139 ns to 28 ns.

## Why a seqlock trips ThreadSanitizer, and why this one doesn't

- **Why the textbook version trips TSan:** the textbook seqlock copies a plain `T` while the writer may be storing it. That is a data race by definition: two threads, one writing, and no happens-before between them. TSan reports it. Building with `-DHW9_TSAN_V1` runs `SeqLockV1` under TSan, and it prints `WARNING: ThreadSanitizer: data race` (3 reports) on the payload `memcpy`.
- **Why it works in practice anyway:** the reader never *uses* a torn copy. Any copy that overlapped a write is caught by the seq check and thrown away. But the C++ standard calls any data race undefined behavior, so "we discard it" is not a valid argument in the language.
- **How I make it sound:** the payload is made of relaxed atomics, so there is no data race at all. The correctness argument rests only on the acquire/release pairs and fences. If the reader's second load of `seq` sees the same even value, then none of the payload stores of a later write can have been observed, so the copy is whole.
- **Result:** the final `SeqLock` runs clean under TSan (`-DHW9_TSAN`), at no runtime cost.
- **What TSan can't check:** TSan only proves there are no data races. It doesn't prove the protocol is right. That part is backed by the torn-read stress test above, plus the monotonic-`k` check.
