# HW2 — C++ Performance Foundations

Code: [`scratch/HW2.cpp`](scratch/HW2.cpp)

```
clang++ -std=c++17 -O2 -Itests scratch/HW2.cpp -o /tmp/hw2 && /tmp/hw2
```

**Machine:** Apple M1, 8 GB RAM, macOS 26.5
**Compiler:** Apple clang 21.0.0, `-O2`

**How I timed it:** the `bench()` helper in `HW2.cpp` does the following:

1. Runs the code a number of times untimed as warm-up.
2. Times many samples with `std::chrono::steady_clock`.
3. Reports p50, p99 and p99.9 in ns per operation.

Each sample times a batch of 1000 calls rather than a single call, because `steady_clock` on the M1 only ticks every ~42 ns. `doNotOptimize` and `clobber` from `tests/bench.hpp` stop the compiler from deleting the work.

## Task 1 — swap_ref vs swap_ptr

```
after swap_ref: x=2 y=1
after swap_ptr: x=1 y=2
```

Both swap correctly. `swap_ref(int& a, int& b)` takes references: `a` and `b` are just other names for the caller's variables, so the call looks like `swap_ref(x, y)`. `swap_ptr(int* a, int* b)` takes pointers, which are variables that hold addresses. The caller has to pass `&x`, and the function has to dereference with `*a`. A pointer can be null or be changed to point somewhere else; a reference can't be null and always refers to the same object. After compiling, both end up passing an address, so there's no speed difference.

## Task 2 — pass by value vs pass by const reference (4 KB struct)

| | p50 | p99 | p99.9 |
|---|---:|---:|---:|
| pass by value | 85.3 ns | 91.5 ns | 117.1 ns |
| pass by const& | 1.5 ns | 1.7 ns | 1.8 ns |

Passing by value copies the whole 4 KB struct on every call. Passing by `const&` only passes an address (8 bytes). The functions do the same work, so the ~84 ns difference is the cost of the copy.

Note: my first version showed the *same* time for both. The optimiser noticed the function never changed its copy and skipped the copy entirely. Adding `clobber()` inside the functions fixed that.

## Task 3 — vector vs linked list (1,000,000 ints)

| | p50 | p99 | p99.9 |
|---|---:|---:|---:|
| `std::vector<int>` | 0.077 ns/elem | 0.081 ns/elem | 0.099 ns/elem |
| `std::list<int>` | 1.81 ns/elem | 1.88 ns/elem | 2.22 ns/elem |

The list is about 24× slower for the same number of elements:

- **Vector:** the ints sit next to each other in memory. Each cache line the CPU loads (128 bytes on the M1) holds 32 ints, so every byte fetched gets used. The prefetcher can also see the pattern and load the next lines early. The compiler can even use SIMD to add several ints at once.
- **List:** each int lives in its own node together with two pointers, so far fewer useful values fit in each cache line. The CPU also can't find node *k+1* until it has loaded node *k*'s `next` pointer, so the loads happen one at a time instead of overlapping.
- **Scattered nodes:** here the list nodes were allocated back-to-back, so they're still fairly close together in memory. In a real program the nodes would be spread out, which means a cache miss on almost every step and a much bigger gap.
