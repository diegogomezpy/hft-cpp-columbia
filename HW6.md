# HW6 — CRTP, constexpr tables, policy-based design

All the code is in [`HW6.cpp`](HW6.cpp). It uses only the standard library.

```
clang++ -std=c++17 -O2 HW6.cpp -o hw6 && ./hw6
clang++ -std=c++17 -O2 -S HW6.cpp -o HW6.s      # assembly
```

**Setup:**

- Apple M1, Apple clang 21, `-O2`, the same machine as HW4.
- The timer and method are the same as HW4: the `cntvct_el0` counter, with 65,536 calls per sample through an array of pointers.
- The calls form a dependency chain, `acc = p[i]->apply(acc) + x[i]`.
- I report the median of 1,000 samples, after 100 warm-up samples.
- The HW4 virtual classes are copied into HW6.cpp unchanged, so the HW4 rows below were re-measured in the same run as the CRTP rows.

## Task 1 — CRTP port of the HW4 strategy

`Strategy<Derived>` is the base class template. Its `apply()` calls `static_cast<const Derived&>(*this).apply_impl(x)`. The base also holds shared behavior: `apply_n()` is written once and works for every strategy. There are no virtual functions. The four HW4 strategies (`Add`, `Xor`, `Sub`, `Rot`) are ported as `AddC`, `XorC`, `SubC` and `RotC`.

Compile-time checks:

- `static_assert`s in the base check that `Derived` really inherits `Strategy<Derived>` and defines `apply_impl`, with a readable error message if not.
- `!std::is_polymorphic_v<AddC>` and `sizeof(AddC) == 8` show there is no vptr. The virtual `AddOp` is 16 bytes.

### Results

These are p50 numbers in ns/call. The HW4 column is from my HW4 writeup.

| Call | HW4 | HW6 rerun | vs direct |
|---|---:|---:|---:|
| direct (non-virtual) | 0.51 | 0.50 | 1.0× |
| `final` through `AddFinal*` | 0.50 | 0.50 | 1.0× |
| virtual, 1 hot target | 1.54 | 1.54 | 3.1× |
| virtual, 4 types in random order | 10.48 | 10.46 | 20.9× |
| **CRTP** | — | **0.50** | **1.0×** |
| CRTP in `std::variant`, 4 types random | — | 10.94 | 21.9× |

All implementations return identical results. The rerun HW4 numbers match HW4 to within 0.02 ns. (In the first of three runs, the direct row was 0.65 ns because it is the first thing that runs. The other two runs gave 0.50 ns.)

### It inlines (`-O2 -S`)

`run_chain<Op>` is the virtual version. Every iteration does three dependent loads and an indirect call:

```
ldr  x0, [x21], #8      ; object pointer
ldr  x8, [x0]           ; vptr
ldr  x8, [x8, #16]      ; apply() from the vtable
blr  x8                 ; indirect call
```

`run_chain<AddC>` is the CRTP version. It has no call at all: `apply()` → `apply_impl()` is inlined down to a load of `k` and an `add`. Because the compiler can see the body, it also unrolled the loop by 4 and split the sum into 4 independent accumulators. That's legal because integer `+` is associative.

```
ldp  x16, x17, [x12, #-16]   ; 4 object pointers
ldp  x3, x4, [x12], #32
ldr  x16, [x16]              ; k of each object
...
add  x14, x14, x5            ; 4 independent add chains
add  x14, x16, x14
...
```

### What I gained and what I gave up

- **Gained:** CRTP is **3.1× faster** than a virtual call, even when the virtual call is perfectly predicted (0.50 vs 1.54 ns). It runs at exactly the speed of the direct call or the `final` class. The win isn't just skipping the vtable load. Inlining lets the compiler reorder and parallelize the loop, which it can never do across an indirect call. Objects also shrink by 8 bytes because there's no vptr.
- **Gave up:** a heterogeneous container. `AddC` and `XorC` are unrelated types, since `Strategy<AddC>` and `Strategy<XorC>` are different base classes. There is no `std::vector<Strategy*>` that holds both. The closest replacement is `std::variant<AddC, XorC, SubC, RotC>` + `std::visit`. With the types in random order it costs 10.9 ns, slightly worse than virtual (10.5 ns), because `visit` dispatches through its own jump table and mispredicts the same way. The other costs:
  - the set of types is fixed at compile time
  - every template is instantiated once per strategy, so the code gets bigger
  - you can't load a new strategy at runtime
- **Takeaway:** CRTP pays off when the strategy type is known at compile time, which is the usual case on a hot path. When you really need a mix of types, group the objects by type, or accept the cost of virtual dispatch or a variant.

## Task 2 — constexpr lookup table

`Crc32Table<256>` has a `constexpr` constructor, so the compiler computes all 256 CRC-32 entries and puts them in read-only data. Nothing runs at startup. `crc32_table()` uses the table at runtime to checksum messages. CRC-32 is the checksum used on Ethernet frames and in zip files.

- The `static_assert`s check known table entries. They also run the CRC of `"123456789"` at compile time and check it against the standard check value, `0xCBF43926`.
- At runtime, the table version and the bitwise version (8 shift/xor steps per byte, no table) give the same CRC on a 4 KB message.
- **Speed:** the table version takes 4.06 ns/byte, against 12.78 ns/byte for the bitwise version. That's **3.1× faster**, with no runtime cost to build the table.

## Task 3 — Policy-based class

`Position<LimitPolicy, MaxPos>` tracks a position. The template parameter decides what happens when a fill would go past the limit:

- `ClampPolicy` fills what fits and caps the position at ±limit.
- `RejectPolicy` is all-or-nothing: it drops any fill that would breach the limit.
- `NoLimitPolicy` is the baseline.

Each policy is an empty struct with a `static` function, so there is no virtual call and no storage cost. A `static_assert` checks that `sizeof(Position<...>) == 8`.

With a limit of 100, the fills +60, +30, +40, −250, +20 give these positions:

| Policy | +60 | +30 | +40 | −250 | +20 |
|---|---:|---:|---:|---:|---:|
| `ClampPolicy` | 60 | 90 | 100 | −100 | −80 |
| `RejectPolicy` | 60 | 90 | 90 | 90 | 90 |

| 64K random fills | ns/fill |
|---|---:|
| raw `int64 +=`, no class | 0.151 |
| `Position<NoLimitPolicy>` | 0.151 |
| `Position<RejectPolicy>` | 2.05 |
| `Position<ClampPolicy>` | 2.56 |

The policy machinery itself costs nothing: `NoLimitPolicy` runs at the same speed as a bare `+=`. The 2–2.5 ns for Clamp and Reject is the cost of their logic. Each fill has to compare against the limit and select a value before the next fill can start, so the loop is one serial chain and can't vectorize like a plain sum. That cost isn't from the policy dispatch.

## Task 4 — static_assert invariants

Each of these is checked at compile time:

- **CRTP:** `Derived` inherits `Strategy<Derived>` and has `uint64_t apply_impl(uint64_t) const`. `AddC` has no vtable, and it is 8 bytes.
- **CRC table:** the size is a power of two. The known entries are correct, and the check value `0xCBF43926` is computed at compile time.
- **Position:** `MaxPos > 0`, and `LimitPolicy` provides `static int64_t apply(pos, qty, limit)`. Adding a policy adds no bytes.

For example, writing `Position<ClampPolicy, 0>` fails to compile with `position limit must be positive`.

## Program output

```
== Task 1: CRTP vs HW4 virtual (64K calls/sample, dependency chain, median of 1000) ==
  direct (HW4)                             p50  0.500   p99  0.687  ns/op
  final (HW4)                              p50  0.501   p99  0.756  ns/op
  virtual, 1 hot target (HW4)              p50  1.535   p99  1.969  ns/op
  virtual, 4 types random (HW4)            p50 10.455   p99 13.442  ns/op
  CRTP                                     p50  0.498   p99  0.703  ns/op
  CRTP in std::variant, 4 types rand       p50 10.939   p99 13.898  ns/op
  CRTP vs virtual-hot: 3.1x faster   CRTP vs direct: 1.00x   variant vs virtual (random): 1.05x
  same results: CRTP == virtual yes, variant == virtual (random) yes, apply_n(1,4) = 13

== Task 2: constexpr CRC-32 lookup table ==
  table[1] = 0x77073096, CRC32("123456789") = 0xCBF43926 (expected 0xCBF43926)
  4 KB message: table 0xCEC2DA59, bitwise 0xCEC2DA59
  CRC-32 with constexpr table              p50  4.059   p99  5.941  ns/op
  CRC-32 bitwise (no table)                p50 12.777   p99 14.181  ns/op
  (ns per byte) table is 3.1x faster

== Task 3: policy-based Position<LimitPolicy, 100> ==
  fill   +60 -> clamp:   60   reject:   60
  fill   +30 -> clamp:   90   reject:   90
  fill   +40 -> clamp:  100   reject:   90
  fill  -250 -> clamp: -100   reject:   90
  fill   +20 -> clamp:  -80   reject:   90
  Position<NoLimitPolicy>                  p50  0.151   p99  0.153  ns/op
  Position<ClampPolicy>                    p50  2.557   p99  2.822  ns/op
  Position<RejectPolicy>                   p50  2.045   p99  2.198  ns/op
  raw int64 += (no class)                  p50  0.151   p99  0.152  ns/op

(sink 45364)
```
