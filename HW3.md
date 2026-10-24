# HW3 — Owning Memory: RAII and Smart Pointers

Code: [`scratch/HW3.cpp`](scratch/HW3.cpp)

```
clang++ -std=c++17 -O2 -Itests scratch/HW3.cpp -o /tmp/hw3 && /tmp/hw3
```

**Machine:** Apple M1, 8 GB RAM, macOS 26.5
**Compiler:** Apple clang 21.0.0, `-O2` (builds clean with `-Wall -Wextra`)

**How I timed it:** I used the same `bench()` helper as HW2:

1. Run the code untimed a number of times as warm-up.
2. Time 10,000 samples with `std::chrono::steady_clock`.
3. Report p50, p99 and p99.9 in ns per operation.

Each sample times a batch of 1000 operations, because `steady_clock` on the M1 only ticks every ~42 ns. `doNotOptimize` from `tests/bench.hpp` makes each pointer escape, so the compiler can't remove the `new`/`delete` pairs.

## Task 1 — Raw `new[]`/`delete[]` buffer with the Rule of Three

`RawBuffer` owns a `double*` from `new double[n]`. Each of the three special members does one job:

| Member | What it does | What goes wrong with the compiler's default |
|---|---|---|
| `~RawBuffer()` | `delete[] data_` | The array is never freed (leak). |
| `RawBuffer(const RawBuffer&)` | allocates a new array and copies the elements (deep copy) | Copies only the pointer. Two objects then share one array, and both `delete[]` it (double free). |
| `operator=(const RawBuffer&)` | allocates and copies **first**, then frees the old array; returns early on self-assignment | Leaks the old array and shares the new one. A naive "delete then copy" version also breaks on `a = a`. |

Doing the allocation before the `delete[]` means that if `new[]` throws, the object is left unchanged (the strong exception guarantee).

A global counter tracks how many arrays are alive, so the program can check for leaks:

```
a = 1 2 3 4
b = 100 2 3 4   (copy, b[0] changed)
c = 1 200 3 4   (assigned, c[1] changed)
distinct arrays: yes
live buffers inside scope: 3
live buffers after scope:  0  (0 = no leaks, no double frees)
```

Changing `b` or `c` doesn't touch `a`, so the copies really are separate. The live count goes back to 0 when the objects leave scope, so every array was freed exactly once.

## Task 2 — The same class with `std::unique_ptr<double[]>`

`UniqueBuffer` stores `std::unique_ptr<double[]>` instead of `double*`.

```
b = 100 2 3 4   (deep copy)
c = 1 2 3 4   (assigned)
move: d took a's array: yes, a is now empty: yes
```

**What the smart pointer removes:**

- **The destructor.** `unique_ptr` calls `delete[]` on its own, so `~UniqueBuffer()` is just `= default`. You can't forget the `[]` either: `unique_ptr<double[]>` always uses `delete[]`.
- **Double frees from accidental copies.** `unique_ptr` can't be copied. If I hadn't written a copy constructor, `UniqueBuffer b(a)` would fail to compile. With the raw pointer it compiles and crashes at runtime. The bug moves from runtime to compile time.
- **Most of the leak risk.** Ownership is tied to an object, not to the programmer remembering a `delete`. If a constructor throws halfway through, or a function returns early, the memory is still freed.
- **Hand-written moves.** `= default` moves are correct right away: the pointer is taken and the source is set to `nullptr`. With a raw pointer I would have had to write that by hand (Rule of Five).
- **Fiddly assignment code.** Copy-and-swap is three lines, and the old array is freed when `tmp` is destroyed.

**What it does not remove:** it can't know that "copying a buffer" should mean a deep copy. If I want `UniqueBuffer` to be copyable, I still have to write the copy constructor and copy assignment myself. If move-only is enough, the class needs no special members at all (Rule of Zero).

**Cost:** none. `sizeof(std::unique_ptr<double[]>) == sizeof(double*)`, and as the next table shows, `RawBuffer(128)` and `UniqueBuffer(128)` take the same time.

### Static vs dynamic allocation (128 doubles = 1 KB)

| | p50 | p99 | p99.9 |
|---|---:|---:|---:|
| stack `double buf[128]` | 0.54 ns | 1.25 ns | 1.38 ns |
| heap `new double[128]` / `delete[]` | 51.4 ns | 58.7 ns | 83.0 ns |
| `RawBuffer(128)` | 68.3 ns | 81.3 ns | 103.2 ns |
| `UniqueBuffer(128)` | 67.8 ns | 79.3 ns | 93.0 ns |

- **Stack (automatic) allocation** just moves the stack pointer when the function is entered, so it's basically free. What I measured is mostly the loop itself. The size has to be known at compile time, and the memory goes away when the scope ends.
- **Heap (dynamic) allocation** calls `malloc`/`free`. Those have to search free lists and keep track of the allocator's state, which costs about 50 ns per pair here. The tail is worse too, because the allocator sometimes has to do extra work. In exchange, the size can be chosen at runtime and the memory can outlive the scope.
- The two buffer classes take ~17 ns longer than plain `new double[128]` because they zero-fill the 1 KB array (`new double[n]()` and `make_unique<double[]>` both value-initialise it). RAII itself adds nothing.

In a hot path you should allocate up front and reuse memory, rather than calling `new` on every message.

## Task 3 — RAII `Timer` guard

`Timer` saves `steady_clock::now()` in its constructor and prints the elapsed time in its destructor. Copying is deleted, so each guard measures exactly one scope.

```
[Timer] sum of 10M doubles              39812.7 us
[Timer] scope left by exception          2542.1 us
caught exception — the Timer above still reported
```

The first scope allocates an 80 MB `UniqueBuffer` (zeroing it and touching every page) and sums it. Both the `Timer` and the buffer are cleaned up automatically at the closing brace.

The second scope sleeps for 2 ms and then throws. The `Timer` still reports (≈2.5 ms) because destructors run during stack unwinding. This is the main point of RAII: cleanup happens on **every** way out of a scope, including `return`, `break` and exceptions, with no `try/finally` needed. Lock guards (`std::lock_guard`), file handles and smart pointers all work the same way.

## Task 4 — `shared_ptr` vs `unique_ptr`

### Creation + destruction (one 16-byte `Payload`)

| | p50 | p99 | p99.9 |
|---|---:|---:|---:|
| `std::make_unique<Payload>()` | 39.7 ns | 47.1 ns | 59.5 ns |
| `std::make_shared<Payload>()` | 47.9 ns | 53.2 ns | 67.5 ns |
| `std::shared_ptr<Payload>(new Payload)` | 79.0 ns | 88.6 ns | 108.8 ns |

- `make_unique` is one `malloc` + one `free`, the same as a raw `new`.
- `make_shared` makes **one** allocation that holds both the control block (the strong and weak counts, plus the deleter) and the object. It's ~8 ns slower than `make_unique` because the block is bigger, the counts have to be set up, and destruction goes through the control block's virtual functions.
- `shared_ptr(new T)` makes **two** allocations (the object, then a separate control block), so it costs about twice as much. Use `make_shared` instead.

### Copying / passing ownership

| | p50 | p99 | p99.9 |
|---|---:|---:|---:|
| copy a raw `Payload*` | 0.54 ns | 0.63 ns | 0.71 ns |
| move a `unique_ptr` (and back) | 0.79 ns | 0.83 ns | 0.96 ns |
| copy a `shared_ptr` (then destroy it), 1 thread | 21.5 ns | 22.0 ns | 36.3 ns |

A `unique_ptr` can't be copied, only moved. A move copies the pointer and sets the source to null, which costs about the same as a raw pointer.

Copying a `shared_ptr` is ~40× slower, even with only one thread running.

### Copying the same `shared_ptr` from several threads

| threads | ns per copy (per thread) |
|---:|---:|
| 1 | 21.5 |
| 2 | 108.9 |
| 4 | 264.5 |

### Why: the atomic reference count

Every `shared_ptr` copy does an **atomic increment** of the strong count in the control block, and every destruction does an **atomic decrement**. The decrement also has to check whether the count reached zero, so it needs acquire/release ordering. The count has to be atomic because the standard lets different threads copy and destroy `shared_ptr`s to the same object at the same time. libc++ uses atomics every time; it can't tell whether your program is single-threaded.

1. **Uncontended cost (~21 ns per copy + destroy):** an atomic read-modify-write is much slower than a normal add. It's a locked read-modify-write on the cache line (LSE `ldadd*` on ARM, `lock xadd` on x86). Its ordering also acts as a barrier, so the CPU can't overlap the surrounding loads and stores with it. Each copy pays for two of these.
2. **Contention (5–12× worse):** when several threads copy the same `shared_ptr`, they all write to the same control-block cache line. Each atomic needs that line in exclusive state, so it has to be pulled away from the other core. Every operation then turns into a cross-core cache-line transfer instead of a hit in the local L1. The more cores fight over the line, the slower each copy gets. Adding threads doesn't increase throughput; it decreases it.
3. **Hidden costs:** the control block is extra memory, and with `shared_ptr(new T)` it's a separate allocation that's likely to miss the cache. Destruction also has to go through the control block's virtual functions.

### Takeaways

- `unique_ptr` is free: raw-pointer speed with automatic cleanup. Use it by default.
- Use `shared_ptr` only when ownership is actually shared and it isn't clear who should free the object. When you do use it, create it with `make_shared`.
- Pass `const std::shared_ptr<T>&` or a raw `T*`/`T&` to functions that only look at the object. That avoids touching the reference count.
- In a hot path, don't copy `shared_ptr`s, especially not across threads. The refcount cache line turns into a point of contention between cores.
