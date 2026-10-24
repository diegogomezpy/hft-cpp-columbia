# HW5 — Templates + STL

All the code is in [`HW5.cpp`](HW5.cpp). It uses only the standard library.

```
clang++ -std=c++17 -O2 HW5.cpp -o hw5 && ./hw5
```

Machine: Apple M1, Apple clang 21, `-O2`.

## What's implemented

1. **Generic container:** `RingStat<T, N>` is a fixed-size rolling window that keeps a running sum and sum of squares, so its mean and stdev update in O(1). There are also generic `clamp_val` and `zscore` functions.
2. **STL rolling stats:** a simulated 10-year daily price series (GBM, 20% vol):
   - log returns with `std::transform`
   - mean and stdev with `std::accumulate`
   - median and 95% VaR with `std::sort`
   - 20-day rolling volatility, with its min and max from `std::minmax_element`
3. **Variadic template + fold:** `sum(xs...)` uses the fold `(xs + ...)`. A `static_assert` rejects anything that isn't a number, so `sum(1, "hello")` won't compile.
4. **SFINAE / if constexpr:** `clamp_val` and `zscore` use `enable_if` to accept only numeric types. `print_value` uses `if constexpr` to format integers and floating-point values differently.

## Results

- RingStat gives the same rolling volatility as the STL version that recomputes each window; the largest difference is 4.7e-17.
- On 1,000,000 returns, the STL version recomputes every window (O(n·w)), while RingStat does an O(1) update per return:

| Window | STL `accumulate` | RingStat | Speedup |
|---|---|---|---|
| 20  | 31.5 ms  | 5.0 ms | 6.3x |
| 252 | 838.5 ms | 5.2 ms | 160.7x |

RingStat's time barely changes with the window size, while the STL version gets slower as the window grows.

## Program output

```
== 1. RingStat + clamp/zscore ==
  RingStat<int,5> after 1..7: mean = 5.00, stdev = 1.5811 (expected 5, 1.5811)
  clamp_val(4.2, -3.0, 3.0) = 3.0, clamp_val(-7, -3, 3) = -3
  zscore(110, 100, 5) = 2.0

== 2. STL rolling return statistics ==
  2520 daily returns: mean = 0.000699, stdev = 0.012840, annualized vol = 20.38%
  median = 0.000797, 1-day 95% historical VaR = 2.01%
  rolling 20d annualized vol: min 11.92%, max 30.32%
  STL vs RingStat max difference: 4.68e-17

== 3. Variadic sum with fold expression ==
  sum(1, 2, 3) = 6
  sum(1, 2.5, 3.5f) = 7.0 (int + double + float -> double)

== 4. SFINAE / if constexpr ==
  42 = 42 (integer)
  3.14 = 3.1400 (floating point)

== Measurements: rolling stdev on 1,000,000 returns ==
  w=20 : STL 31.5 ms, RingStat 5.0 ms (6.3x faster)
  w=252: STL 838.5 ms, RingStat 5.2 ms (160.7x faster)
```
