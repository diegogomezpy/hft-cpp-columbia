// HW5 — Templates + STL
// Self-contained (standard library only). Build & run:
//   clang++ -std=c++17 -O2 HW5.cpp -o hw5 && ./hw5
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <numeric>
#include <random>
#include <type_traits>
#include <vector>

// ---------------------------------------------------------------------------
// 1. Generic container: RingStat<T, N> — fixed-size window with O(1) mean/stdev
// ---------------------------------------------------------------------------
template <class T, std::size_t N>
class RingStat {
    static_assert(std::is_arithmetic_v<T>, "RingStat needs a numeric type");
    static_assert(N >= 2, "window must hold at least 2 values");

public:
    void push(T x) {
        if (count_ == N) {            // window full: drop the oldest value
            double old = buf_[head_];
            sum_ -= old;
            sumsq_ -= old * old;
        } else {
            ++count_;
        }
        buf_[head_] = x;
        sum_ += x;
        sumsq_ += static_cast<double>(x) * x;
        head_ = (head_ + 1) % N;
    }

    bool full() const { return count_ == N; }
    std::size_t size() const { return count_; }
    double mean() const { return count_ ? sum_ / count_ : 0.0; }
    double stdev() const {
        if (count_ < 2) return 0.0;
        double var = (sumsq_ - sum_ * sum_ / count_) / (count_ - 1);
        return var > 0 ? std::sqrt(var) : 0.0;
    }

private:
    double sum_ = 0, sumsq_ = 0;
    std::size_t head_ = 0, count_ = 0;
    std::array<T, N> buf_{};
};

// Generic clamp and z-score, only for numeric types (SFINAE with enable_if).
template <class T, std::enable_if_t<std::is_arithmetic_v<T>, int> = 0>
T clamp_val(T x, T lo, T hi) { return x < lo ? lo : (x > hi ? hi : x); }

template <class T, std::enable_if_t<std::is_arithmetic_v<T>, int> = 0>
double zscore(T x, double mean, double sd) { return sd > 0 ? (x - mean) / sd : 0.0; }

// ---------------------------------------------------------------------------
// 3. Variadic template + fold expression: type-safe sum(...)
// ---------------------------------------------------------------------------
template <class... Ts>
auto sum(Ts... xs) {
    static_assert(sizeof...(Ts) > 0, "sum needs at least one argument");
    static_assert((std::is_arithmetic_v<Ts> && ...), "sum only takes numbers");
    return (xs + ...);
}

// ---------------------------------------------------------------------------
// 4. if constexpr: describe a value differently depending on its type
// ---------------------------------------------------------------------------
template <class T>
void print_value(const char* name, T x) {
    if constexpr (std::is_integral_v<T>)
        std::printf("  %s = %lld (integer)\n", name, static_cast<long long>(x));
    else if constexpr (std::is_floating_point_v<T>)
        std::printf("  %s = %.4f (floating point)\n", name, static_cast<double>(x));
    else
        static_assert(std::is_arithmetic_v<T>, "print_value only takes numbers");
}

// ---------------------------------------------------------------------------
// 2. STL rolling return statistics on a simulated price series
// ---------------------------------------------------------------------------
std::vector<double> make_prices(std::size_t n) {
    std::mt19937 rng(42);
    std::normal_distribution<double> z(0.0, 1.0);
    const double dt = 1.0 / 252, mu = 0.08, sigma = 0.20;
    std::vector<double> p(n);
    p[0] = 100.0;
    for (std::size_t i = 1; i < n; ++i)
        p[i] = p[i - 1] * std::exp((mu - 0.5 * sigma * sigma) * dt + sigma * std::sqrt(dt) * z(rng));
    return p;
}

// Log returns with std::transform.
std::vector<double> log_returns(const std::vector<double>& p) {
    std::vector<double> r(p.size() - 1);
    std::transform(p.begin() + 1, p.end(), p.begin(), r.begin(),
                   [](double now, double prev) { return std::log(now / prev); });
    return r;
}

// Rolling stdev, recomputing each window with std::accumulate (O(n * w)).
std::vector<double> rolling_stdev_stl(const std::vector<double>& r, std::size_t w) {
    std::vector<double> out;
    for (std::size_t i = 0; i + w <= r.size(); ++i) {
        auto first = r.begin() + i, last = first + w;
        double m = std::accumulate(first, last, 0.0) / w;
        double ss = std::accumulate(first, last, 0.0,
                                    [m](double acc, double x) { return acc + (x - m) * (x - m); });
        out.push_back(std::sqrt(ss / (w - 1)));
    }
    return out;
}

// Same thing with RingStat (O(n)).
template <std::size_t W>
std::vector<double> rolling_stdev_ring(const std::vector<double>& r) {
    std::vector<double> out;
    RingStat<double, W> rs;
    for (double x : r) {
        rs.push(x);
        if (rs.full()) out.push_back(rs.stdev());
    }
    return out;
}

volatile double sink;  // keeps the optimizer from skipping timed work

template <class F>
double time_ms(F f) {
    auto t0 = std::chrono::steady_clock::now();
    f();
    auto t1 = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>(t1 - t0).count();
}

int main() {
    std::printf("== 1. RingStat + clamp/zscore ==\n");
    RingStat<int, 5> rs;
    for (int i = 1; i <= 7; ++i) rs.push(i);  // keeps 3,4,5,6,7
    std::printf("  RingStat<int,5> after 1..7: mean = %.2f, stdev = %.4f (expected 5, 1.5811)\n",
                rs.mean(), rs.stdev());
    std::printf("  clamp_val(4.2, -3.0, 3.0) = %.1f, clamp_val(-7, -3, 3) = %d\n",
                clamp_val(4.2, -3.0, 3.0), clamp_val(-7, -3, 3));
    std::printf("  zscore(110, 100, 5) = %.1f\n", zscore(110, 100.0, 5.0));

    std::printf("\n== 2. STL rolling return statistics ==\n");
    auto prices = make_prices(2521);  // ~10 years of daily prices
    auto r = log_returns(prices);
    double mean = std::accumulate(r.begin(), r.end(), 0.0) / r.size();
    double ss = std::accumulate(r.begin(), r.end(), 0.0,
                                [mean](double a, double x) { return a + (x - mean) * (x - mean); });
    double sd = std::sqrt(ss / (r.size() - 1));
    std::vector<double> sorted = r;
    std::sort(sorted.begin(), sorted.end());
    double var95 = -sorted[sorted.size() * 5 / 100];
    std::printf("  %zu daily returns: mean = %.6f, stdev = %.6f, annualized vol = %.2f%%\n",
                r.size(), mean, sd, 100 * sd * std::sqrt(252.0));
    std::printf("  median = %.6f, 1-day 95%% historical VaR = %.2f%%\n",
                sorted[sorted.size() / 2], 100 * var95);

    auto vol_stl = rolling_stdev_stl(r, 20);
    auto vol_ring = rolling_stdev_ring<20>(r);
    double max_diff = 0;
    for (std::size_t i = 0; i < vol_stl.size(); ++i)
        max_diff = std::max(max_diff, std::abs(vol_stl[i] - vol_ring[i]));
    auto [lo, hi] = std::minmax_element(vol_stl.begin(), vol_stl.end());
    std::printf("  rolling 20d annualized vol: min %.2f%%, max %.2f%%\n",
                100 * *lo * std::sqrt(252.0), 100 * *hi * std::sqrt(252.0));
    std::printf("  STL vs RingStat max difference: %.2e\n", max_diff);

    std::printf("\n== 3. Variadic sum with fold expression ==\n");
    std::printf("  sum(1, 2, 3) = %d\n", sum(1, 2, 3));
    std::printf("  sum(1, 2.5, 3.5f) = %.1f (int + double + float -> double)\n", sum(1, 2.5, 3.5f));
    // sum(1, "hello");  // does not compile: "sum only takes numbers"

    std::printf("\n== 4. SFINAE / if constexpr ==\n");
    print_value("42", 42);
    print_value("3.14", 3.14);
    // clamp_val("a", "b", "c");  // does not compile: no matching function (enable_if)

    std::printf("\n== Measurements: rolling stdev on 1,000,000 returns ==\n");
    auto big = log_returns(make_prices(1'000'001));
    for (int run = 0; run < 2; ++run) {  // first pass is a warm-up
        double s20 = time_ms([&] { sink = rolling_stdev_stl(big, 20).back(); });
        double r20 = time_ms([&] { sink = rolling_stdev_ring<20>(big).back(); });
        double s252 = time_ms([&] { sink = rolling_stdev_stl(big, 252).back(); });
        double r252 = time_ms([&] { sink = rolling_stdev_ring<252>(big).back(); });
        if (run == 1) {
            std::printf("  w=20 : STL %.1f ms, RingStat %.1f ms (%.1fx faster)\n", s20, r20, s20 / r20);
            std::printf("  w=252: STL %.1f ms, RingStat %.1f ms (%.1fx faster)\n", s252, r252, s252 / r252);
        }
    }
    return 0;
}
