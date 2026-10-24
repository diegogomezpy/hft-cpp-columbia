// HW7 — Performance challenge: flat price-indexed order book + open-addressing symbol map.
// Self-contained (standard library only). Build & run:
//   clang++ -std=c++17 -O2 HW7.cpp -o hw7 && ./hw7
// To use as the CI header, order_book.hpp is just:
//   #define HW7_NO_MAIN
//   #include "HW7.cpp"
// Everything above the HW7_NO_MAIN line is the graded Book/SymMap; below it are
// tests and benchmarks.
#ifndef HW7_BOOK_SECTION
#define HW7_BOOK_SECTION
#include <cmath>
#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <vector>

// ---------------------------------------------------------------------------
// Book — one symbol. Prices sit on a $0.01 grid, so a price is an array index.
// Each side is a flat array of aggregated qty per tick, covering a 65,536-tick
// ($655) band centred on the first price seen. best_bid/best_ask are cached
// doubles, so a read is one load. The only scan is in cancel, when the inside
// level empties.
// ---------------------------------------------------------------------------
struct Book {
    static constexpr int kLevels = 1 << 16;

    Book() : bid_qty_(kLevels), ask_qty_(kLevels) {}

    void add(uint64_t id, char side, double px, uint32_t qty) {
        if (qty == 0 || (side != 'B' && side != 'S') || !(std::fabs(px) < 1e9)) return;
        const int64_t tick = std::llround(px * 100.0);
        if (!based_) { base_ = tick - kLevels / 2; based_ = true; }
        const int64_t s64 = tick - base_;
        if (s64 < 0 || s64 >= kLevels) return;  // outside the band: refuse, never write out of bounds
        const int s = static_cast<int>(s64);
        cancel(id);                             // re-using an id replaces the old order
        orders_[id] = {side, s, qty};
        if (side == 'B') {
            bid_qty_[s] += qty;
            if (s > bid_top_) { bid_top_ = s; best_bid_ = px_of(s); }
        } else {
            ask_qty_[s] += qty;
            if (s < ask_top_) { ask_top_ = s; best_ask_ = px_of(s); }
        }
    }

    void cancel(uint64_t id) {
        auto it = orders_.find(id);
        if (it == orders_.end()) return;
        const Rec r = it->second;
        orders_.erase(it);
        if (r.side == 'B') {
            if ((bid_qty_[r.slot] -= r.qty) == 0 && r.slot == bid_top_) {
                while (bid_top_ >= 0 && bid_qty_[bid_top_] == 0) --bid_top_;
                best_bid_ = bid_top_ < 0 ? 0.0 : px_of(bid_top_);
            }
        } else {
            if ((ask_qty_[r.slot] -= r.qty) == 0 && r.slot == ask_top_) {
                while (ask_top_ < kLevels && ask_qty_[ask_top_] == 0) ++ask_top_;
                best_ask_ = ask_top_ == kLevels ? 0.0 : px_of(ask_top_);
            }
        }
    }

    double best_bid() const { return best_bid_; }  // 0.0 when the side is empty
    double best_ask() const { return best_ask_; }

    uint32_t qty_at(char side, double px) const {   // for tests
        const int64_t s = std::llround(px * 100.0) - base_;
        if (!based_ || s < 0 || s >= kLevels) return 0;
        return (side == 'B' ? bid_qty_ : ask_qty_)[s];
    }

private:
    struct Rec { char side; int slot; uint32_t qty; };
    double px_of(int slot) const { return static_cast<double>(slot + base_) / 100.0; }

    double best_bid_ = 0.0, best_ask_ = 0.0;   // the hot reads
    int bid_top_ = -1, ask_top_ = kLevels;     // slot of best level; -1 / kLevels = empty
    int64_t base_ = 0;                         // absolute tick of slot 0
    bool based_ = false;
    std::vector<uint32_t> bid_qty_, ask_qty_;  // qty per slot
    std::unordered_map<uint64_t, Rec> orders_; // id -> order, only used by add/cancel
};

// ---------------------------------------------------------------------------
// SymMap — symbol (<= 16 chars) -> id. Linear-probing open addressing. A key is
// stored as two uint64 words (zero padded), so comparing keys is two integer
// compares and hashing is one multiply. A slot with a == 0 is empty (a real
// key's first byte is non-zero). The table doubles at 50% load.
// ---------------------------------------------------------------------------
struct SymMap {
    static constexpr uint64_t kAbsent = static_cast<uint64_t>(-1);

    SymMap() : t_(16) {}

    void put(const char* sym, uint64_t id) {
        if (!sym) return;
        uint64_t a, b;
        load(sym, a, b);
        if (a == 0) { empty_key_id_ = id; return; }        // "" key
        if (2 * (size_ + 1) > t_.size()) grow();
        Slot& s = probe(a, b);
        if (s.a == 0) { s.a = a; s.b = b; ++size_; }
        s.id = id;                                         // insert or overwrite
    }

    uint64_t get(const char* sym) const {
        if (!sym) return kAbsent;
        uint64_t a, b;
        load(sym, a, b);
        if (a == 0) return empty_key_id_;
        const size_t mask = t_.size() - 1;
        for (size_t i = home(a, b);; i = (i + 1) & mask) {
            const Slot& s = t_[i];
            if (s.a == a && s.b == b) return s.id;
            if (s.a == 0) return kAbsent;
        }
    }

private:
    struct Slot { uint64_t a = 0, b = 0, id = kAbsent; };

    static void load(const char* sym, uint64_t& a, uint64_t& b) {
        char buf[16] = {};
        for (int i = 0; i < 16 && sym[i]; ++i) buf[i] = sym[i];
        std::memcpy(&a, buf, 8);
        std::memcpy(&b, buf + 8, 8);
    }
    size_t home(uint64_t a, uint64_t b) const {
        return ((a ^ (b * 0x9E3779B97F4A7C15ull)) * 0xBF58476D1CE4E5B9ull) & (t_.size() - 1);
    }
    Slot& probe(uint64_t a, uint64_t b) {                  // matching slot, or the empty one to fill
        const size_t mask = t_.size() - 1;
        size_t i = home(a, b);
        while (t_[i].a != 0 && (t_[i].a != a || t_[i].b != b)) i = (i + 1) & mask;
        return t_[i];
    }
    void grow() {
        std::vector<Slot> old(t_.size() * 2);
        old.swap(t_);
        for (const Slot& s : old)
            if (s.a != 0) probe(s.a, s.b) = s;
    }

    std::vector<Slot> t_;      // size is a power of two
    size_t size_ = 0;
    uint64_t empty_key_id_ = kAbsent;
};
#endif  // HW7_BOOK_SECTION

#ifndef HW7_NO_MAIN
// ===========================================================================
// Tests + benchmarks
// ===========================================================================
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <functional>
#include <map>
#include <string>
#if defined(__x86_64__) || defined(__i386__)
#include <x86intrin.h>
#endif

template <class T> inline void doNotOptimize(T&& v) { asm volatile("" : : "g"(v) : "memory"); }

// ---- "Before" versions: my first correct attempt -------------------------
// BookV1: same flat arrays, but best_bid/best_ask scan from the band edge
// every call (no cached top).
struct BookV1 {
    static constexpr int kLevels = 1 << 16;
    std::vector<uint32_t> bid = std::vector<uint32_t>(kLevels), ask = std::vector<uint32_t>(kLevels);
    int64_t base = 0; bool based = false;
    struct Rec { char side; int slot; uint32_t qty; };
    std::unordered_map<uint64_t, Rec> orders;
    void add(uint64_t id, char side, double px, uint32_t qty) {
        int64_t t = std::llround(px * 100.0);
        if (!based) { base = t - kLevels / 2; based = true; }
        int64_t s = t - base;
        if (s < 0 || s >= kLevels) return;
        orders[id] = {side, int(s), qty};
        (side == 'B' ? bid : ask)[s] += qty;
    }
    void cancel(uint64_t id) {
        auto it = orders.find(id);
        if (it == orders.end()) return;
        (it->second.side == 'B' ? bid : ask)[it->second.slot] -= it->second.qty;
        orders.erase(it);
    }
    double best_bid() const {
        for (int s = kLevels - 1; s >= 0; --s) if (bid[s]) return (s + base) / 100.0;
        return 0.0;
    }
    double best_ask() const {
        for (int s = 0; s < kLevels; ++s) if (ask[s]) return (s + base) / 100.0;
        return 0.0;
    }
};

// SymMapV1: open addressing too, but char[16] keys, FNV-1a byte-at-a-time
// hash, strncmp compare, fixed 4096-slot table.
struct SymMapV1 {
    static constexpr size_t CAP = 4096;
    struct Slot { char key[16]; uint64_t id; bool used; };
    std::vector<Slot> t = std::vector<Slot>(CAP);
    static size_t hash(const char* s) {
        uint64_t h = 1469598103934665603ull;
        for (int i = 0; i < 16 && s[i]; ++i) { h ^= uint8_t(s[i]); h *= 1099511628211ull; }
        return h & (CAP - 1);
    }
    void put(const char* s, uint64_t id) {
        for (size_t i = hash(s);; i = (i + 1) & (CAP - 1))
            if (!t[i].used || std::strncmp(t[i].key, s, 16) == 0) {
                std::strncpy(t[i].key, s, 16); t[i].id = id; t[i].used = true; return;
            }
    }
    uint64_t get(const char* s) const {
        for (size_t i = hash(s);; i = (i + 1) & (CAP - 1)) {
            if (!t[i].used) return uint64_t(-1);
            if (std::strncmp(t[i].key, s, 16) == 0) return t[i].id;
        }
    }
};

// ---- Baselines: the standard containers ----------------------------------
struct MapBook {
    std::map<int64_t, uint64_t> bids, asks;   // bids best = rbegin, asks best = begin
    struct Rec { char side; int64_t tick; uint32_t qty; };
    std::unordered_map<uint64_t, Rec> orders;
    void add(uint64_t id, char side, double px, uint32_t qty) {
        int64_t t = std::llround(px * 100.0);
        orders[id] = {side, t, qty};
        (side == 'B' ? bids : asks)[t] += qty;
    }
    void cancel(uint64_t id) {
        auto it = orders.find(id);
        if (it == orders.end()) return;
        auto& m = it->second.side == 'B' ? bids : asks;
        auto lv = m.find(it->second.tick);
        if ((lv->second -= it->second.qty) == 0) m.erase(lv);
        orders.erase(it);
    }
    double best_bid() const { return bids.empty() ? 0.0 : bids.rbegin()->first / 100.0; }
    double best_ask() const { return asks.empty() ? 0.0 : asks.begin()->first / 100.0; }
};

struct UMapSym {
    std::unordered_map<std::string, uint64_t> m;
    void put(const char* s, uint64_t id) { m[s] = id; }
    uint64_t get(const char* s) const { auto it = m.find(s); return it == m.end() ? uint64_t(-1) : it->second; }
};

// ---- Timer: rdtsc on x86, cntvct_el0 on arm64 ----------------------------
inline uint64_t cycle_count() {
#if defined(__x86_64__) || defined(__i386__)
    return __rdtsc();
#elif defined(__aarch64__)
    uint64_t t; asm volatile("mrs %0, cntvct_el0" : "=r"(t)); return t;
#else
    return std::chrono::steady_clock::now().time_since_epoch().count();
#endif
}
static double counter_hz() {   // calibrate the counter against steady_clock
    auto t0 = std::chrono::steady_clock::now(); uint64_t c0 = cycle_count();
    while (std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(100)) {}
    auto t1 = std::chrono::steady_clock::now(); uint64_t c1 = cycle_count();
    return (c1 - c0) / std::chrono::duration<double>(t1 - t0).count();
}
// Core clock. On x86 the TSC runs at ~nominal core clock, so cycles = TSC ticks.
// On arm64 cntvct_el0 is a fixed 24 MHz timer, not a cycle counter, so I time a
// chain of dependent 1-cycle adds to get the core GHz.
static double core_ghz(double hz) {
#if defined(__aarch64__)
    (void)hz;
    double best = 0;
    for (int rep = 0; rep < 5; ++rep) {   // first reps let the core ramp up; keep the fastest
        uint64_t x = 0, n = 50'000'000;
        auto t0 = std::chrono::steady_clock::now();
        asm volatile("1:\n add %0,%0,#1\n add %0,%0,#1\n add %0,%0,#1\n add %0,%0,#1\n"
                     " add %0,%0,#1\n add %0,%0,#1\n add %0,%0,#1\n add %0,%0,#1\n"
                     " subs %1,%1,#1\n b.ne 1b\n" : "+r"(x), "+r"(n));
        double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        best = std::max(best, 8 * 50e6 / s / 1e9);
    }
    return best;
#else
    return hz / 1e9;
#endif
}

static double g_hz, g_ghz;

// Warm up, then time `samples` batches of `ops` calls; report median ns/op.
static double bench(const char* name, const std::function<void(long)>& run, long ops,
                    int samples = 51) {
    run(ops);  // warm-up
    std::vector<double> t(samples);
    for (int i = 0; i < samples; ++i) {
        uint64_t c0 = cycle_count();
        run(ops);
        uint64_t c1 = cycle_count();
        t[i] = (c1 - c0) / g_hz * 1e9 / ops;
    }
    std::sort(t.begin(), t.end());
    double p50 = t[samples / 2];
    std::printf("  %-34s %9.2f ns/op %9.1f cyc/op %9.1f Mops/s\n", name, p50, p50 * g_ghz, 1e3 / p50);
    return p50;
}

static int g_fail = 0;
static void check(const char* what, bool ok) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_fail;
}
static bool eq(double a, double b) { return std::fabs(a - b) < 1e-6; }

template <class B> static void fill_ci_book(B& b) {   // same book the CI benchmark builds
    for (uint64_t i = 0; i < 2000; ++i)
        b.add(i, (i & 1) ? 'B' : 'S', 100.0 + ((i * 7) % 50) * 0.01, 1 + (i % 9));
}

int main() {
    std::printf("== Correctness ==\n");
    {   // the CI test
        Book b;
        b.add(1, 'B', 100.00, 5); b.add(2, 'B', 100.02, 3);
        b.add(3, 'S', 100.06, 4); b.add(4, 'S', 100.04, 2);
        bool ok = eq(b.best_bid(), 100.02) && eq(b.best_ask(), 100.04);
        b.cancel(2); ok = ok && eq(b.best_bid(), 100.00);
        b.cancel(4); ok = ok && eq(b.best_ask(), 100.06);
        b.cancel(1); b.cancel(3);
        ok = ok && b.best_bid() == 0.0 && b.best_ask() == 0.0;
        check("book: best bid/ask, cancel, empty side -> 0.0", ok);
    }
    {   // $720 stock: band centres on it, a far-away price is refused instead of overflowing
        Book b;
        b.add(1, 'B', 719.98, 10); b.add(2, 'S', 720.02, 10); b.add(3, 'B', 10.00, 10);
        check("book: $720 BBO, out-of-band order refused",
              eq(b.best_bid(), 719.98) && eq(b.best_ask(), 720.02) && b.qty_at('B', 10.00) == 0);
    }
    {   // the CI test + a 16-char key
        SymMap m;
        m.put("AAPL", 10); m.put("MSFT", 20); m.put("NVDA", 30); m.put("SPY", 40); m.put("BRK.B", 99);
        bool ok = m.get("AAPL") == 10 && m.get("NVDA") == 30 && m.get("BRK.B") == 99;
        ok = ok && m.get("TSLA") == uint64_t(-1);
        m.put("AAPL", 11); ok = ok && m.get("AAPL") == 11;
        m.put("ABCDEFGHIJKLMNOP", 16);
        ok = ok && m.get("ABCDEFGHIJKLMNOP") == 16 && m.get("ABCDEFGHIJKLMNO") == uint64_t(-1);
        check("symmap: put/get/overwrite/absent, 16-char key", ok);
    }

    g_hz = counter_hz();
    g_ghz = core_ghz(g_hz);
    std::printf("\n== Benchmarks ==  (core %.2f GHz, median of 51 runs)\n", g_ghz);
    const long kOps = 1 << 20;

    std::printf("best_bid() on the CI book (2000 orders):\n");
    Book b2; BookV1 b1; MapBook bm;
    fill_ci_book(b2); fill_ci_book(b1); fill_ci_book(bm);
    double bb_after  = bench("Book (after: cached top)",      [&](long n) { for (long i = 0; i < n; ++i) { double x = b2.best_bid(); doNotOptimize(x); } }, kOps);
    double bb_before = bench("BookV1 (before: scan on read)", [&](long n) { for (long i = 0; i < n; ++i) { double x = b1.best_bid(); doNotOptimize(x); } }, kOps / 64);
    double bb_map    = bench("std::map book",                 [&](long n) { for (long i = 0; i < n; ++i) { double x = bm.best_bid(); doNotOptimize(x); } }, kOps);
    std::printf("  -> %.0fx faster than before, %.1fx faster than std::map\n", bb_before / bb_after, bb_map / bb_after);

    std::printf("SymMap::get() with the CI's 8 symbols:\n");
    const char* syms[] = {"AAPL", "MSFT", "NVDA", "TSLA", "AMZN", "GOOGL", "META", "SPY"};
    SymMap m2; SymMapV1 m1; UMapSym mu;
    for (uint64_t i = 0; i < 8; ++i) { m2.put(syms[i], i); m1.put(syms[i], i); mu.put(syms[i], i); }
    double g_after  = bench("SymMap (after: 2x u64 keys)",    [&](long n) { for (long i = 0; i < n; ++i) { uint64_t v = m2.get(syms[i & 7]); doNotOptimize(v); } }, kOps);
    double g_before = bench("SymMapV1 (before: FNV+strncmp)", [&](long n) { for (long i = 0; i < n; ++i) { uint64_t v = m1.get(syms[i & 7]); doNotOptimize(v); } }, kOps);
    double g_umap   = bench("std::unordered_map<string>",     [&](long n) { for (long i = 0; i < n; ++i) { uint64_t v = mu.get(syms[i & 7]); doNotOptimize(v); } }, kOps);
    std::printf("  -> %.1fx faster than before, %.1fx faster than std::unordered_map\n", g_before / g_after, g_umap / g_after);

    std::printf("\n%s\n", g_fail ? "SOME TESTS FAILED" : "ALL TESTS PASSED");
    return g_fail ? 1 : 0;
}
#endif  // HW7_NO_MAIN
