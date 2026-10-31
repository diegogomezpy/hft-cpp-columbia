// HW9 — Performance challenge: SeqLock for publishing a quote to many readers.
// Self-contained (standard library only). Build & run:
//   clang++ -std=c++17 -O2 -pthread HW9.cpp -o hw9 && ./hw9
// ThreadSanitizer (correctness test only):
//   clang++ -std=c++17 -O1 -g -pthread -fsanitize=thread -DHW9_TSAN HW9.cpp -o hw9_tsan && ./hw9_tsan
// Same, but stress SeqLockV1 (plain-memcpy payload) to see TSan flag it:
//   add -DHW9_TSAN_V1
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <type_traits>
#include <vector>

// ---------------------------------------------------------------------------
// SeqLock<T> — single writer, any number of lock-free readers.
//
// seq_ is even when the data is stable and odd while a write is in progress.
// A reader loads seq_ (acquire), copies the data, then loads seq_ again; if
// the two match and are even, no write overlapped the copy and the value is
// whole. Otherwise read() returns false and the caller retries.
//
// The payload is stored as relaxed std::atomic<uint64_t> words rather than a
// plain T. A reader copying a plain T while the writer stores it is a data
// race (UB), even though the seq check would throw the copy away. Relaxed
// word-sized atomics cost the same as plain loads/stores on x86 and arm64,
// and make the program race-free by the C++ memory model. The fences are the
// standard seqlock pattern (Boehm, "Can Seqlocks Get Along with Programming
// Language Memory Models?", 2012).
//
// seq_ and the data share one 64-byte line, so a reader gets both in one miss.
// ---------------------------------------------------------------------------
template <class T>
struct SeqLock {
    static_assert(std::is_trivially_copyable_v<T>, "SeqLock copies T as raw bytes");
    static constexpr size_t kWords = (sizeof(T) + 7) / 8;
    static_assert(8 * (kWords + 1) <= 64, "seq + payload should fit in one cache line");

    void write(const T& v) {                       // single writer only
        uint64_t w[kWords] = {};
        std::memcpy(w, &v, sizeof(T));
        const uint64_t s = seq_.load(std::memory_order_relaxed);   // only we write seq_
        seq_.store(s + 1, std::memory_order_relaxed);              // odd: write in progress
        std::atomic_thread_fence(std::memory_order_release);       // odd seq before any data store
        for (size_t i = 0; i < kWords; ++i) data_[i].store(w[i], std::memory_order_relaxed);
        seq_.store(s + 2, std::memory_order_release);              // data before the even seq
    }

    bool read(T& out) const {                      // lock-free; false = torn, retry
        const uint64_t s0 = seq_.load(std::memory_order_acquire);  // data loads stay after this
        if (s0 & 1) return false;                                  // writer is mid-update
        uint64_t w[kWords];
        for (size_t i = 0; i < kWords; ++i) w[i] = data_[i].load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);       // data loads before the re-check
        if (seq_.load(std::memory_order_relaxed) != s0) return false;
        std::memcpy(&out, w, sizeof(T));
        return true;
    }

    T load() const {                               // read() until it gets a whole value
        T v;
        while (!read(v)) {}
        return v;
    }

private:
    alignas(64) std::atomic<uint64_t> seq_{0};
    std::atomic<uint64_t> data_[kWords] = {};
};

// The quote from the assignment: 24 bytes = 3 words.
struct Quote {
    double bid, ask;
    uint32_t bq, aq;
};

// ---- "Before" version: my first seqlock ----------------------------------
// Plain T payload copied with memcpy (a data race: TSan flags it), seq_cst
// on every seq access, and seq on its own cache line apart from the data, so
// a read touches two lines.
template <class T>
struct SeqLockV1 {
    void write(const T& v) {
        seq.fetch_add(1);
        std::memcpy(&data, &v, sizeof(T));
        seq.fetch_add(1);
    }
    bool read(T& out) const {
        const uint64_t s0 = seq.load();
        if (s0 & 1) return false;
        std::memcpy(&out, &data, sizeof(T));
        return seq.load() == s0;
    }
    T load() const { T v; while (!read(v)) {} return v; }
    alignas(64) std::atomic<uint64_t> seq{0};
    alignas(64) T data{};
};

// ---- Baseline: std::shared_mutex -----------------------------------------
template <class T>
struct SharedMutexBox {
    void write(const T& v) { std::unique_lock<std::shared_mutex> g(m); data = v; }
    T load() const { std::shared_lock<std::shared_mutex> g(m); return data; }
    mutable std::shared_mutex m;
    T data{};
};

// ---------------------------------------------------------------------------
// Tests + benchmarks
// ---------------------------------------------------------------------------
// Every quote the writer publishes is built from one counter k, so a reader
// can tell a whole value from a mix of two writes.
static Quote make_quote(uint64_t k) {
    return {double(k), double(k) + 0.5, uint32_t(k), ~uint32_t(k)};
}
static bool consistent(const Quote& q) {
    const uint64_t k = uint64_t(q.bid);
    return q.ask == q.bid + 0.5 && q.bq == uint32_t(k) && q.aq == ~uint32_t(k);
}

// Unprotected box: same payload, no seq check. Used once to show the test
// really catches torn reads.
struct NoLock {
    void write(const Quote& v) { bid = v.bid; ask = v.ask; bq = v.bq; aq = v.aq; }
    Quote load() const { return {bid, ask, bq, aq}; }
    volatile double bid = 0, ask = 0.5;   // volatile so every access really happens
    volatile uint32_t bq = 0, aq = ~0u;
};

// One writer publishing k = 1, 2, 3, ... as fast as it can, `readers` threads
// each doing `reads` loads. Returns the number of torn values readers got back
// and checks that each reader's k never goes backwards.
template <class Box>
static uint64_t stress(int readers, long reads, bool& monotonic) {
    Box box;
    box.write(make_quote(0));
    std::atomic<bool> stop{false};
    std::atomic<uint64_t> torn{0};
    std::atomic<bool> mono{true};
    std::thread w([&] { for (uint64_t k = 1; !stop.load(std::memory_order_relaxed); ++k) box.write(make_quote(k)); });
    std::vector<std::thread> rs;
    for (int r = 0; r < readers; ++r)
        rs.emplace_back([&] {
            uint64_t bad = 0, last = 0;
            for (long i = 0; i < reads; ++i) {
                const Quote q = box.load();
                if (!consistent(q)) { ++bad; continue; }
                const uint64_t k = uint64_t(q.bid);
                if (k < last) mono = false;
                last = k;
            }
            torn += bad;
        });
    for (auto& t : rs) t.join();
    stop = true;
    w.join();
    monotonic = mono;
    return torn;
}

// ---- Timing: core clock for cycles/op ------------------------------------
// On x86 the TSC runs at ~nominal core clock, so cycles = ns * TSC GHz. On
// arm64 the system timer is not a cycle counter, so I time a chain of
// dependent 1-cycle adds to get the core GHz (same as HW7).
static double core_ghz() {
#if defined(__aarch64__)
    double best = 0;
    for (int rep = 0; rep < 5; ++rep) {
        uint64_t x = 0, n = 50'000'000;
        auto t0 = std::chrono::steady_clock::now();
        asm volatile("1:\n add %0,%0,#1\n add %0,%0,#1\n add %0,%0,#1\n add %0,%0,#1\n"
                     " add %0,%0,#1\n add %0,%0,#1\n add %0,%0,#1\n add %0,%0,#1\n"
                     " subs %1,%1,#1\n b.ne 1b\n" : "+r"(x), "+r"(n));
        double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        best = std::max(best, 8 * 50e6 / s / 1e9);
    }
    return best;
#elif defined(__x86_64__)
    auto t0 = std::chrono::steady_clock::now(); uint64_t c0 = __builtin_ia32_rdtsc();
    while (std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(100)) {}
    auto t1 = std::chrono::steady_clock::now(); uint64_t c1 = __builtin_ia32_rdtsc();
    return (c1 - c0) / std::chrono::duration<double, std::nano>(t1 - t0).count();
#else
    return 1.0;
#endif
}

template <class T> inline void doNotOptimize(T&& v) { asm volatile("" : : "g"(v) : "memory"); }

struct Result { double ns_per_read, mreads_per_s, writes_per_us; };

// One run: writer publishes back-to-back (or every `gap_ns`, if > 0) while
// `readers` threads each do `reads` loads. ns/read is the median reader's
// time per load; throughput is total reads / wall time.
template <class Box>
static Result run_once(int readers, long reads, long gap_ns) {
    Box box;
    box.write(make_quote(0));
    std::atomic<bool> stop{false};
    std::atomic<int> ready{0};
    std::atomic<uint64_t> writes{0};
    std::thread w([&] {
        uint64_t k = 1;
        while (ready.load() < readers) {}
        while (!stop.load(std::memory_order_relaxed)) {
            box.write(make_quote(k++));
            if (gap_ns > 0) {
                auto until = std::chrono::steady_clock::now() + std::chrono::nanoseconds(gap_ns);
                while (std::chrono::steady_clock::now() < until) {}
            }
        }
        writes = k - 1;
    });
    std::vector<double> per(readers);
    std::vector<std::thread> rs;
    std::atomic<bool> go{false};
    for (int r = 0; r < readers; ++r)
        rs.emplace_back([&, r] {
            ++ready;
            while (!go.load()) {}
            auto t0 = std::chrono::steady_clock::now();
            double sum = 0;
            for (long i = 0; i < reads; ++i) sum += box.load().bid;
            auto t1 = std::chrono::steady_clock::now();
            doNotOptimize(sum);
            per[r] = std::chrono::duration<double, std::nano>(t1 - t0).count() / reads;
        });
    while (ready.load() < readers) {}
    auto t0 = std::chrono::steady_clock::now();
    go = true;
    for (auto& t : rs) t.join();
    auto t1 = std::chrono::steady_clock::now();
    stop = true;
    w.join();
    const double wall_ns = std::chrono::duration<double, std::nano>(t1 - t0).count();
    std::sort(per.begin(), per.end());
    return {per[readers / 2], readers * double(reads) / wall_ns * 1e3, writes / wall_ns * 1e3};
}

// Warm up once, then the median over `runs` runs.
template <class Box>
static Result bench(const char* name, int readers, long reads, long gap_ns, double ghz, int runs = 21) {
    run_once<Box>(readers, reads / 4, gap_ns);
    std::vector<Result> v;
    for (int i = 0; i < runs; ++i) v.push_back(run_once<Box>(readers, reads, gap_ns));
    std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.ns_per_read < b.ns_per_read; });
    const Result r = v[runs / 2];
    std::printf("  %-26s %8.2f ns/read %8.1f cyc/read %9.1f Mreads/s  (writer %.1f writes/us)\n",
                name, r.ns_per_read, r.ns_per_read * ghz, r.mreads_per_s, r.writes_per_us);
    return r;
}

int main() {
    const int kReaders = 3;   // + 1 writer = the 4 performance cores on an M1
    int fail = 0;
    std::printf("== Correctness: 1 writer (back-to-back) + %d readers ==\n", kReaders);
    {
        bool mono;
#ifdef HW9_TSAN_V1
        uint64_t torn = stress<SeqLockV1<Quote>>(kReaders, 200'000, mono);
#else
        uint64_t torn = stress<SeqLock<Quote>>(kReaders, 2'000'000, mono);
#endif
        bool ok = torn == 0 && mono;
        std::printf("  [%s] SeqLock: %llu torn values returned, k never goes back: %s\n",
                    ok ? "PASS" : "FAIL", (unsigned long long)torn, mono ? "yes" : "no");
        fail += !ok;
    }
#ifndef HW9_TSAN
    {
        bool mono;
        uint64_t torn = stress<NoLock>(kReaders, 2'000'000, mono);
        std::printf("  [info] no lock at all: %llu torn values (shows the check catches tearing)\n",
                    (unsigned long long)torn);
    }
    {
        bool mono;
        uint64_t torn = stress<SharedMutexBox<Quote>>(kReaders, 200'000, mono);
        bool ok = torn == 0 && mono;
        std::printf("  [%s] shared_mutex: %llu torn values\n", ok ? "PASS" : "FAIL", (unsigned long long)torn);
        fail += !ok;
    }

    const double ghz = core_ghz();
    std::printf("\n== Reader cost, %d readers, median of 21 runs (core %.2f GHz) ==\n", kReaders, ghz);
    std::printf("Writer publishing back-to-back (worst case):\n");
    bench<SeqLock<Quote>>("SeqLock (after)", kReaders, 2'000'000, 0, ghz);
    bench<SeqLockV1<Quote>>("SeqLockV1 (before)", kReaders, 2'000'000, 0, ghz);
    bench<SharedMutexBox<Quote>>("std::shared_mutex", kReaders, 200'000, 0, ghz);
    std::printf("Writer publishing every ~1 us (a busy quote feed):\n");
    bench<SeqLock<Quote>>("SeqLock (after)", kReaders, 4'000'000, 1000, ghz);
    bench<SeqLockV1<Quote>>("SeqLockV1 (before)", kReaders, 4'000'000, 1000, ghz);
    bench<SharedMutexBox<Quote>>("std::shared_mutex", kReaders, 400'000, 1000, ghz);
#endif
    std::printf("\n%s\n", fail ? "SOME TESTS FAILED" : "ALL TESTS PASSED");
    return fail ? 1 : 0;
}
