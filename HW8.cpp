// HW8 — Performance challenge: sliding-window event counter on a ring buffer.
// Self-contained (standard library only). Build & run:
//   clang++ -std=c++17 -O2 HW8.cpp -o hw8 && ./hw8
// To use as the CI header, rolling_counter.hpp is just:
//   #define HW8_NO_MAIN
//   #include "HW8.cpp"
#ifndef HW8_COUNTER_SECTION
#define HW8_COUNTER_SECTION
#include <cstdint>
#include <vector>

// ---------------------------------------------------------------------------
// RollingCounter — how many events have ts > now - window?
//
// Timestamps go into a power-of-two ring in arrival order, so it is sorted
// oldest -> newest. head_/tail_ only grow; a slot is idx & mask_ and the live
// count is tail_ - head_. count() sees a non-decreasing clock, so an expired
// event stays expired: it is dropped once from the front and never looked at
// again. Each event is written once and dropped once -> amortized O(1).
// Assumes add() is called in timestamp order. The ring doubles when full.
// ---------------------------------------------------------------------------
struct RollingCounter {
    static constexpr uint64_t kInitCap = 1 << 12;
    static_assert((kInitCap & (kInitCap - 1)) == 0, "capacity must be a power of two");

    explicit RollingCounter(uint64_t window_ns)
        : window_(window_ns), buf_(kInitCap), mask_(kInitCap - 1) {}

    void add(uint64_t ts_ns) {
        const uint64_t t = tail_;
        if (t - head_ > mask_) grow();       // full: never overwrite a live event
        buf_[t & mask_] = ts_ns;
        tail_ = t + 1;
    }

    uint64_t count(uint64_t now_ns) {
        // Locals: stores into buf_ (uint64_t) may alias the members, so the
        // compiler would otherwise reload them inside the loop.
        const uint64_t t = tail_;
        uint64_t h = head_;
        // now - window only exists when now >= window; below that nothing expires.
        if (now_ns >= window_) {
            const uint64_t cutoff = now_ns - window_;
            const uint64_t* b = buf_.data();
            while (h != t && b[h & mask_] <= cutoff) ++h;
            head_ = h;
        }
        return t - h;
    }

private:
    __attribute__((noinline)) void grow() {   // cold path, kept out of add()
        const uint64_t n = tail_ - head_;
        std::vector<uint64_t> bigger(2 * (mask_ + 1));
        for (uint64_t i = 0; i < n; ++i) bigger[i] = buf_[(head_ + i) & mask_];
        buf_.swap(bigger);
        mask_ = buf_.size() - 1;
        head_ = 0;
        tail_ = n;
    }

    uint64_t window_;
    std::vector<uint64_t> buf_;   // ring of timestamps
    uint64_t mask_;               // buf_.size() - 1
    uint64_t head_ = 0;           // oldest live event
    uint64_t tail_ = 0;           // one past the newest
};
#endif  // HW8_COUNTER_SECTION

#ifndef HW8_NO_MAIN
// ===========================================================================
// Tests + benchmarks
// ===========================================================================
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <deque>

template <class T> inline void doNotOptimize(T&& v) { asm volatile("" : : "g"(v) : "memory"); }

// Baseline: std::deque, same algorithm (push_back / pop_front while expired).
struct DequeCounter {
    uint64_t window;
    std::deque<uint64_t> q;
    explicit DequeCounter(uint64_t w) : window(w) {}
    void add(uint64_t t) { q.push_back(t); }
    uint64_t count(uint64_t now) {
        if (now >= window)
            while (!q.empty() && q.front() <= now - window) q.pop_front();
        return q.size();
    }
};

// Naive: rescan the whole window on every count(), O(window) per call.
struct ScanCounter {
    uint64_t window;
    std::vector<uint64_t> ts;
    explicit ScanCounter(uint64_t w) : window(w) {}
    void add(uint64_t t) { ts.push_back(t); }
    uint64_t count(uint64_t now) {
        size_t keep = 0;
        for (uint64_t t : ts)
            if (now < window || t > now - window) ts[keep++] = t;
        ts.resize(keep);
        return keep;
    }
};

static int g_fail = 0;
static void check(const char* what, bool ok) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_fail;
}

// One op = add(t) + count(t), t += 1 ns, window 1000 ns (the CI stream, 1000
// live). Timed in batches of `batch` ops with steady_clock; percentiles are
// over the per-batch ns/op.
template <class C>
static void bench(const char* name, int samples, long batch) {
    C c(1000);
    uint64_t t = 0;
    auto step = [&] { c.add(t); uint64_t n = c.count(t); doNotOptimize(n); ++t; };
    for (long i = 0; i < 100 * batch; ++i) step();   // warm-up
    std::vector<double> ns(samples);
    for (int s = 0; s < samples; ++s) {
        auto t0 = std::chrono::steady_clock::now();
        for (long i = 0; i < batch; ++i) step();
        auto t1 = std::chrono::steady_clock::now();
        ns[s] = std::chrono::duration<double, std::nano>(t1 - t0).count() / batch;
    }
    std::sort(ns.begin(), ns.end());
    std::printf("  %-22s p50 %8.2f  p99 %8.2f  p99.9 %8.2f  ns/op\n", name,
                ns[samples / 2], ns[samples * 99 / 100], ns[samples * 999 / 1000]);
}

int main() {
    std::printf("== Correctness ==\n");
    {   // the CI test
        RollingCounter rc(1000);
        for (uint64_t t = 0; t < 1000; ++t) rc.add(t);
        bool ok = rc.count(999) == 1000;
        for (uint64_t t = 1000; t < 1500; ++t) rc.add(t);
        ok = ok && rc.count(1999) == 500;
        check("CI: full window = 1000, then 500 after the clock advances", ok);
    }
    {
        RollingCounter rc(100);
        rc.add(50); rc.add(51);
        check("boundary: ts == now - window is out", rc.count(150) == 1 && rc.count(151) == 0);
    }
    {
        RollingCounter rc(1000);
        rc.add(0);
        check("unsigned edge: ts 0 live while now < window",
              rc.count(999) == 1 && rc.count(1000) == 0);
    }
    {
        RollingCounter rc(1'000'000'000);
        for (uint64_t t = 1; t <= 100'000; ++t) rc.add(t);
        check("100k live events: ring grows, nothing overwritten",
              rc.count(100'000) == 100'000 && rc.count(1'000'050'000) == 50'000);
    }
    {
        RollingCounter r(1000); DequeCounter d(1000);
        bool ok = true;
        for (uint64_t t = 0; t < 10000 && ok; ++t) {
            r.add(t); d.add(t);
            uint64_t now = (t % 4000 == 3999) ? t + 5000 : t;   // a few clock jumps
            ok = r.count(now) == d.count(now);
        }
        check("matches std::deque version (incl. clock jumps)", ok);
    }

    std::printf("\n== Benchmark: CI stream (add+count, 1000 live) ==\n");
    bench<RollingCounter>("RollingCounter (ring)", 20000, 1024);
    bench<DequeCounter>("std::deque", 20000, 1024);
    bench<ScanCounter>("ScanCounter (rescan)", 1000, 64);

    std::printf("\n%s\n", g_fail ? "SOME TESTS FAILED" : "ALL TESTS PASSED");
    return g_fail ? 1 : 0;
}
#endif  // HW8_NO_MAIN
