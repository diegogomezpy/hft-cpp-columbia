# HW4 — A High-Performance Allocator + Runtime Polymorphism

Code:
- [`include/pool.hpp`](include/pool.hpp): the autograded `Pool` (free-list pool)
- [`scratch/HW4.cpp`](scratch/HW4.cpp): arena allocator, benchmarks, polymorphism demo, virtual-call benchmark

```
make pool CXX=clang++                       # course autograder tests for pool.hpp
clang++ -std=c++17 -O2 -Iinclude -Itests scratch/HW4.cpp -o /tmp/hw4 && /tmp/hw4
```

**Machine:** Apple M1 (4 performance + 4 efficiency cores, ~3.2 GHz P-core), 8 GB RAM, macOS 26.5
**Compiler:** Apple clang 21.0.0, `-O2` (builds clean with `-Wall -Wextra`)

**How I timed it:** I used `cycle_count()` from `tests/bench.hpp`. On arm64 it reads `cntvct_el0`, and I convert ticks to ns with `cycle_count_hz()`.

1. Run the benchmark untimed 100–200 times as warm-up.
2. Time 1,000–2,000 samples.
3. Sort the samples and report the **median (p50)** and p99 in ns per operation.

On the M1, `cntvct_el0` ticks at only **24 MHz** (1 tick ≈ 41.7 ns), so a single operation can't be timed on its own. Each sample times a large batch instead: 16,384 allocations, or 65,536 calls. That makes the sample thousands of ticks long, so rounding to the nearest tick is under 0.1% of the result. I give cycle counts as ns × 3.2 GHz. These are estimates, because macOS needs root to read the real per-core cycle counter. macOS also doesn't let me pin a thread to a core, but the medians agreed to within 1% across separate runs.

---

## Task 1 — Free-list pool (`pool.hpp`)

```cpp
struct Pool {
  Pool(std::size_t obj_size, std::size_t capacity);
  void* alloc();          // O(1), nullptr if exhausted
  void  free(void* p);    // O(1), returns slot to the pool
};
```

**Design:**

- **One buffer.** The constructor makes a single `::operator new` call (aligned to a 64-byte cache line) and splits the buffer into `capacity` slots. Each slot size is rounded up to `alignof(std::max_align_t)` and is at least `sizeof(void*)`. That way every slot can hold any object that fits, just like memory from `malloc`.
- **Intrusive free list.** A free slot isn't being used for anything, so its first 8 bytes store the pointer to the next free slot. The list takes no extra memory.
- **`alloc()`** pops the head of the list: one load and one store. It returns `nullptr` when the list is empty.
- **`free(p)`** pushes the slot back on top. It is LIFO, so the most recently used slot (the one most likely still in cache) is handed out next. That is exactly what `pool_reuse` checks.
- **C++ details:** copying is deleted because the pool owns its buffer, and moving is allowed. The destructor releases the buffer. `obj_size * capacity` is checked for overflow. `free(nullptr)` does nothing, like `std::free`. There is also an `owns(p)` helper for debugging and for the demo below.

**Autograder (all four green):**

```
RESULT|pool_basic|pass|4 distinct, writable, non-overlapping slots
RESULT|pool_full|pass|3 distinct slots, then alloc past capacity -> null
RESULT|pool_reuse|pass|freed slot reused
RESULT|pool_pattern|pass|100k fill/drain rounds OK
METRIC|pool_ns_per_op|0.808396
```

## Task 2 — Bump / arena allocator + allocator numbers

`Arena` owns one buffer and an `off_` offset. `alloc(n, align)` rounds the offset up to `align`, checks that `n` bytes still fit (written so the check itself can't overflow), and moves the offset forward. Individual objects are never freed. `reset()` sets the offset back to 0 and releases the whole batch at once.

### Allocator numbers

**Batch pattern:** each round allocates 1,024 × 64-byte `Order64` objects, writes to each one, then releases them all (`free` each one, or `reset()` the arena). One sample is 16 rounds. The numbers are per allocation, including its release.

| allocator | p50 ns/op | p99 ns/op | ≈ cycles (p50) | speedup vs malloc |
|---|---:|---:|---:|---:|
| `malloc` / `free` (baseline) | 33.59 | 36.22 | ~107 | 1.0× |
| `new` / `delete` | 36.65 | 38.99 | ~117 | 0.9× |
| **`Pool::alloc` / `Pool::free`** | **2.07** | 2.10 | ~7 | **16.2×** |
| **`Arena::alloc` / `Arena::reset`** | **2.04** | 2.05 | ~7 | **16.5×** |

**Churn pattern:** allocate one object, write it, free it right away. This is the same pattern the autograder's metric uses.

| allocator | p50 ns/op | p99 ns/op | speedup vs malloc |
|---|---:|---:|---:|
| `malloc` / `free` | 41.02 | 43.94 | 1.0× |
| **`Pool::alloc` / `Pool::free`** | **1.03** | 1.05 | **40.0×** |

**Why it's faster:**

- **`malloc` is general-purpose.** It has to handle any size, look up a size class, keep metadata about the allocation, and be thread-safe. On macOS it is also called through a zone pointer. That adds up to about 34–41 ns per alloc+free pair.
- **The pool knows the size ahead of time and runs on one thread.** An allocation is a pointer pop and a free is a pointer push, both hitting memory that is already in cache.
- **The arena is even simpler.** An allocation is an add and a compare, and releasing 1,024 objects is a single store (`off_ = 0`).

In the batch pattern, the pool and the arena come out about the same (~2 ns). At that point the cost is the loop itself plus writing 64 KB of objects, not the allocator. The arena's advantage is that it takes objects of any size and frees a whole batch at once. The pool's advantage is that it can free objects one at a time, in any order.

## Task 3 — Runtime polymorphism in the pool + leak check

**The class hierarchy:**

- `Order` is an abstract base class. It has a **virtual destructor** and pure virtual `notional()` and `kind()` functions. A `static_assert(std::has_virtual_destructor_v<Order>)` makes sure the destructor stays virtual.
- There are three `final` derived classes: `LimitOrder`, `MarketOrder` and `IcebergOrder`.
- `IcebergOrder` owns heap memory (a `std::vector<int>` of order slices). If its destructor were skipped, that vector would leak.
- The pool's slot size is `max(sizeof(...))` over the three classes.

**What the demo does with each object:**

```cpp
void* mem = pool.alloc();
Order* o  = new (mem) IcebergOrder(i, 100.10, 5000, 400);   // placement new into the slot
total    += o->notional();                                  // used only through Order*
o->~Order();                                                // virtual dtor -> ~IcebergOrder runs
pool.free(o);                                               // slot back on the free list
```

**How I check for leaks (three independent checks):**

1. **Live-object counters.** `g_live_base` goes up in `Order()` and down in `~Order()`. `g_live_derived` goes up and down in each derived class's constructor and destructor. A non-virtual destructor would only run `~Order`, so `g_live_derived` would stay above 0. After that, I take every slot out of the pool again to prove all of them came back.
2. **AddressSanitizer + UBSan:** `-O1 -g -fsanitize=address,undefined`. The whole program ran with no errors and exit code 0. (LeakSanitizer isn't supported on macOS/arm64, which is why I used the next tool for leaks.)
3. **macOS `leaks --atExit`.** This scans the heap when the program exits.

```
== Task 3: polymorphic objects in the pool ==
  sizeof Limit/Market/Iceberg = 32/32/48 B -> slot 48 B, capacity 6
  after construction: live Order = 6, live derived = 6, pool exhausted = yes
    id 0  Limit    notional   30075.00  in pool: yes
    id 1  Market   notional   20060.02  in pool: yes
    id 2  Iceberg  notional  500500.00  in pool: yes
    id 3  Limit    notional   30075.00  in pool: yes
    id 4  Market   notional   20060.02  in pool: yes
    id 5  Iceberg  notional  500500.00  in pool: yes
  total notional = 1101270.05
  after destroy+free:  live Order = 0, live derived = 0
  slots recovered: 6 / 6  -> NO LEAKS

$ leaks --atExit -- ./hw4
Process 96652: 0 leaks for 0 total leaked bytes.
```

**Negative control:** to show the check really catches leaks, I deleted the `o->~Order();` line in a scratch copy of the program. The counters and `leaks` both caught it, and `leaks` pointed to the exact memory: the vector buffers inside the two `IcebergOrder`s.

```
  after destroy+free:  live Order = 6, live derived = 6
  slots recovered: 6 / 6  -> LEAK!
Process 96689: 2 leaks for 160 total leaked bytes.
  ROOT LEAK: <malloc in std::vector<int>::push_back(int const&)> [80]
  ROOT LEAK: <malloc in std::vector<int>::push_back(int const&)> [80]
```

`pool.free()` only gives back the **slot**. It never runs a destructor. That's why the order matters: call `p->~Base()` first, then `pool.free(p)`. The destructor has to be virtual so that this call reaches the derived class's destructor.

## Task 4 — The cost of a virtual call

**Setup:**

- Each case calls `apply()` 65,536 times in one sample, through an array of pointers.
- Every call is part of a **dependency chain**, `acc = ptr[i]->apply(acc) + x[i]`, with random `x[i]` values. Each call needs the previous result, so the compiler can't vectorize the loop or compute it in closed form, and the CPU can't overlap calls.
- `acc` is the **benchmark sink**. It is added to a global and printed at the end, so the optimizer can't delete the work.
- The loops are `noinline` templates, and the pointers come from an array that is filled at runtime, so the compiler never knows the dynamic type of an object.
- Every case has the same per-call work (`x + k` for the Add cases), and all the objects live in a `Pool`.

| call | p50 ns/call | p99 | ≈ cycles | vs direct |
|---|---:|---:|---:|---:|
| direct call, non-virtual (inlined) | **0.51** | 0.51 | ~1.6 | 1.0× |
| `final` class through `AddFinal*` | **0.50** | 0.58 | ~1.6 | 1.0× |
| direct call, `noinline` | 1.53 | 1.73 | ~4.9 | 3.0× |
| virtual, 1 hot target | **1.54** | 1.66 | ~4.9 | 3.0× |
| virtual, 4 targets, repeating A-B-C-D cycle | 4.09 | 4.31 | ~13 | 8.1× |
| virtual, 4 targets, random order | **10.48** | 11.74 | ~34 | 20.8× |

I checked the generated assembly (`clang++ -S`) to confirm what each case compiles to:

- **Virtual loop:** 3 dependent loads and an indirect call:
  ```
  ldr x0, [x21], #8     ; load the object pointer
  ldr x8, [x0]          ; load the vptr
  ldr x8, [x8, #16]     ; load the vtable slot for apply()
  blr x8                ; indirect branch-and-link
  ```
- **`final` loop:** has no `blr` at all. It loads `k` straight from the object (`ldr [x, #8]`, just past the vptr) and `add`s it. The compiler even unrolled it by 4, the same as the direct loop.

### Why a virtual call costs what it does

A virtual call is an **indirect branch**. The CPU loads the object's vptr, loads the function's address from the vtable, and jumps to whatever address it finds, so the target isn't known until those loads finish. A modern core doesn't wait for them. It predicts the target with the **branch-target buffer (BTB)** and its indirect-target predictor, and keeps executing speculatively.

When one target is hot, the prediction is always right. The call then costs about the same as an ordinary direct call that isn't inlined: 1.54 ns vs 1.53 ns here. That is the call/return overhead plus the vptr and vtable loads, which run in the background.

When the target changes unpredictably (4 types in random order), about ¾ of the predictions are wrong. Each miss flushes the pipeline and restarts at the correct target. That cost about 12 ns per mispredicted call here, roughly 38 cycles. This is more than the M1's ~14-cycle pipeline-refill penalty. My guess is that the extra comes from the speculative work that also gets thrown away, including the loads for the next pointer and vptr that were already in flight. The repeating A-B-C-D order falls in between (4.1 ns). The predictor uses the history of recent targets, so it can follow a pattern like this only partly.

The biggest cost, though, is the one the table shows even with perfect prediction: **a virtual call can't be inlined.** The compiler doesn't know which function will run until runtime, so it can't paste the body in. Without inlining it also can't do anything that depends on seeing the body: keeping `acc` in a register across the call, folding constants, or unrolling and scheduling the loop. That alone makes the call 3× slower than the inlined version, even with a perfect BTB hit.

`final` fixes this. If the static type is a `final` class, no subclass can override `apply()`, so the compiler knows exactly which function will run. It **devirtualizes** the call into a direct call, inlines it, and the cost goes back to the direct-call number (0.50 vs 0.51 ns).

**Takeaway for a hot path:** use concrete or `final` types (or templates/CRTP) where the type is known. If you need runtime polymorphism, group objects by type, so the same call site keeps hitting the same target and the BTB keeps predicting it correctly.
