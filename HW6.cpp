// HW6 — Compile-time & policy-based design: CRTP, constexpr tables, policies.
// Self-contained (standard library only). Build & run:
//   clang++ -std=c++17 -O2 HW6.cpp -o hw6 && ./hw6
// See the inlining:
//   clang++ -std=c++17 -O2 -S HW6.cpp -o HW6.s
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <random>
#include <type_traits>
#include <variant>
#include <vector>

// ---------------------------------------------------------------------------
// Timer — same as HW4 (bench.hpp's cycle_count, copied in so this file stands
// alone): cntvct_el0 on arm64, rdtsc on x86, steady_clock otherwise.
// ---------------------------------------------------------------------------
#if defined(__x86_64__) || defined(__i386__)
#include <x86intrin.h>
#endif

inline std::uint64_t cycle_count() {
#if defined(__x86_64__) || defined(__i386__)
    return __rdtsc();
#elif defined(__aarch64__)
    std::uint64_t t;
    asm volatile("mrs %0, cntvct_el0" : "=r"(t));
    return t;
#else
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
#endif
}

static double ticks_to_ns(double ticks) {
    static const double hz = [] {
#if defined(__aarch64__)
        std::uint64_t f;
        asm volatile("mrs %0, cntfrq_el0" : "=r"(f));
        return static_cast<double>(f);
#else
        // Calibrate the counter against steady_clock.
        auto t0 = std::chrono::steady_clock::now(); auto c0 = cycle_count();
        while (std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(50)) {}
        auto t1 = std::chrono::steady_clock::now(); auto c1 = cycle_count();
        return (c1 - c0) / std::chrono::duration<double>(t1 - t0).count();
#endif
    }();
    return ticks * 1e9 / hz;
}

struct Result { double p50, p99; };

// Warm up, time `samples` batches of `ops` calls, report median and p99 ns/op.
template <class F>
Result bench(const char* name, F f, int warmup, int samples, long ops) {
    for (int i = 0; i < warmup; ++i) f();
    std::vector<double> t(samples);
    for (int i = 0; i < samples; ++i) {
        auto c0 = cycle_count();
        f();
        auto c1 = cycle_count();
        t[i] = ticks_to_ns(static_cast<double>(c1 - c0)) / ops;
    }
    std::sort(t.begin(), t.end());
    Result r{t[samples / 2], t[samples * 99 / 100]};
    std::printf("  %-40s p50 %6.3f   p99 %6.3f  ns/op\n", name, r.p50, r.p99);
    return r;
}

// ---------------------------------------------------------------------------
// HW4 strategy hierarchy (virtual), unchanged
// ---------------------------------------------------------------------------
struct Op {
    virtual ~Op() = default;
    virtual std::uint64_t apply(std::uint64_t x) const = 0;
};
struct AddOp : Op { std::uint64_t k; explicit AddOp(std::uint64_t k) : k(k) {}
    std::uint64_t apply(std::uint64_t x) const override { return x + k; } };
struct XorOp : Op { std::uint64_t k; explicit XorOp(std::uint64_t k) : k(k) {}
    std::uint64_t apply(std::uint64_t x) const override { return x ^ k; } };
struct SubOp : Op { std::uint64_t k; explicit SubOp(std::uint64_t k) : k(k) {}
    std::uint64_t apply(std::uint64_t x) const override { return x - k; } };
struct RotOp : Op { unsigned k; explicit RotOp(unsigned k) : k(k) {}
    std::uint64_t apply(std::uint64_t x) const override { return (x << k) | (x >> (64 - k)); } };

struct AddDirect {             // no virtual functions at all
    std::uint64_t k;
    std::uint64_t apply(std::uint64_t x) const { return x + k; }
};
struct AddFinal final : Op {   // virtual, but final: calls through AddFinal* devirtualise
    std::uint64_t k; explicit AddFinal(std::uint64_t k) : k(k) {}
    std::uint64_t apply(std::uint64_t x) const override { return x + k; }
};

// ---------------------------------------------------------------------------
// Task 1 — the same strategies ported to CRTP (no virtual functions)
// ---------------------------------------------------------------------------
// The base holds the shared behaviour (the public apply(), a call counter
// helper, apply_n) and reaches the derived implementation with a static_cast,
// which the compiler resolves at compile time.
template <class Derived>
struct Strategy {
    std::uint64_t apply(std::uint64_t x) const {
        static_assert(std::is_base_of_v<Strategy<Derived>, Derived>,
                      "CRTP misuse: Derived must inherit Strategy<Derived>");
        static_assert(std::is_same_v<decltype(std::declval<const Derived&>().apply_impl(x)),
                                     std::uint64_t>,
                      "Derived must define: std::uint64_t apply_impl(std::uint64_t) const");
        return self().apply_impl(x);
    }
    // Shared behaviour written once in the base, works for every strategy.
    std::uint64_t apply_n(std::uint64_t x, int n) const {
        for (int i = 0; i < n; ++i) x = apply(x);
        return x;
    }

protected:
    ~Strategy() = default;  // not polymorphic: can't delete a Derived through Strategy*

private:
    const Derived& self() const { return static_cast<const Derived&>(*this); }
};

struct AddC : Strategy<AddC> { std::uint64_t k; explicit AddC(std::uint64_t k) : k(k) {}
    std::uint64_t apply_impl(std::uint64_t x) const { return x + k; } };
struct XorC : Strategy<XorC> { std::uint64_t k; explicit XorC(std::uint64_t k) : k(k) {}
    std::uint64_t apply_impl(std::uint64_t x) const { return x ^ k; } };
struct SubC : Strategy<SubC> { std::uint64_t k; explicit SubC(std::uint64_t k) : k(k) {}
    std::uint64_t apply_impl(std::uint64_t x) const { return x - k; } };
struct RotC : Strategy<RotC> { unsigned k; explicit RotC(unsigned k) : k(k) {}
    std::uint64_t apply_impl(std::uint64_t x) const { return (x << k) | (x >> (64 - k)); } };

static_assert(!std::is_polymorphic_v<AddC>, "CRTP strategy must have no vtable");
static_assert(sizeof(AddC) == sizeof(std::uint64_t), "no vptr: just the data member");
static_assert(sizeof(AddOp) == 2 * sizeof(std::uint64_t), "virtual version carries a vptr");

// What a mixed container of CRTP strategies costs: there's no common base
// type, so the closest thing is a std::variant + std::visit.
using AnyC = std::variant<AddC, XorC, SubC, RotC>;

// Benchmark loop from HW4: a dependency chain through the calls.
template <class P>
__attribute__((noinline)) std::uint64_t run_chain(P* const* ptrs, const std::uint64_t* x,
                                                  std::size_t n) {
    std::uint64_t acc = 1;
    for (std::size_t i = 0; i < n; ++i) acc = ptrs[i]->apply(acc) + x[i];
    return acc;
}
__attribute__((noinline)) std::uint64_t run_chain_variant(const AnyC* const* ptrs,
                                                          const std::uint64_t* x, std::size_t n) {
    std::uint64_t acc = 1;
    for (std::size_t i = 0; i < n; ++i)
        acc = std::visit([acc](const auto& s) { return s.apply(acc); }, *ptrs[i]) + x[i];
    return acc;
}

// ---------------------------------------------------------------------------
// Task 2 — constexpr lookup table: CRC-32 (the checksum on Ethernet/zip, and on
// many exchange feeds), all 256 entries computed by the compiler.
// ---------------------------------------------------------------------------
template <std::size_t N>
struct Crc32Table {
    static_assert((N & (N - 1)) == 0, "table size must be a power of two");
    std::uint32_t v[N]{};
    constexpr Crc32Table() {
        for (std::uint32_t i = 0; i < N; ++i) {
            std::uint32_t c = i;
            for (int b = 0; b < 8; ++b) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            v[i] = c;
        }
    }
};
constexpr Crc32Table<256> kCrcTable{};  // lives in read-only data, zero startup cost

// Checked by the compiler: well-known entries of the standard CRC-32 table.
static_assert(kCrcTable.v[0] == 0x00000000u && kCrcTable.v[1] == 0x77073096u &&
              kCrcTable.v[255] == 0x2D02EF8Du, "CRC-32 table is wrong");

constexpr std::uint32_t crc32_table(const unsigned char* p, std::size_t n) {
    std::uint32_t c = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < n; ++i) c = kCrcTable.v[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}
// Same result without the table: 8 shift/xor steps per byte.
std::uint32_t crc32_bitwise(const unsigned char* p, std::size_t n) {
    std::uint32_t c = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < n; ++i) {
        c ^= p[i];
        for (int b = 0; b < 8; ++b) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
    }
    return c ^ 0xFFFFFFFFu;
}

constexpr unsigned char kCheck[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
static_assert(crc32_table(kCheck, 9) == 0xCBF43926u, "CRC-32 check value");  // even runs at compile time

// ---------------------------------------------------------------------------
// Task 3 — policy-based design: a position tracker whose risk-limit behaviour
// is a template parameter. Policies are empty types with static functions, so
// they add no storage and no indirect call.
// ---------------------------------------------------------------------------
struct ClampPolicy {   // fill what fits, cap the position at the limit
    static std::int64_t apply(std::int64_t pos, std::int64_t qty, std::int64_t lim) {
        return std::clamp(pos + qty, -lim, lim);
    }
};
struct RejectPolicy {  // all-or-nothing: drop any fill that would breach the limit
    static std::int64_t apply(std::int64_t pos, std::int64_t qty, std::int64_t lim) {
        const std::int64_t next = pos + qty;
        return (next > lim || next < -lim) ? pos : next;
    }
};
struct NoLimitPolicy { // baseline: plain addition
    static std::int64_t apply(std::int64_t pos, std::int64_t qty, std::int64_t) { return pos + qty; }
};

template <class LimitPolicy, std::int64_t MaxPos>
class Position {
    static_assert(MaxPos > 0, "position limit must be positive");
    static_assert(std::is_same_v<decltype(LimitPolicy::apply(0, 0, 0)), std::int64_t>,
                  "LimitPolicy must provide static int64_t apply(pos, qty, limit)");

public:
    void fill(std::int64_t qty) { pos_ = LimitPolicy::apply(pos_, qty, MaxPos); }
    std::int64_t get() const { return pos_; }

private:
    std::int64_t pos_ = 0;
};

static_assert(sizeof(Position<ClampPolicy, 100>) == sizeof(std::int64_t),
              "a policy adds no storage");

// ---------------------------------------------------------------------------
static std::uint64_t g_sink;

int main() {
    // ---------------- Task 1: CRTP vs HW4 ----------------
    std::puts("== Task 1: CRTP vs HW4 virtual (64K calls/sample, dependency chain, median of 1000) ==");
    constexpr std::size_t N = 1 << 16;
    constexpr int kWarm = 100, kSamples = 1000;

    std::mt19937_64 rng(42);
    std::vector<std::uint64_t> x(N);
    for (auto& v : x) v = rng() & 0xff;

    AddDirect direct{3};
    AddFinal fin(3);
    AddC crtp(3);
    AddOp a(3); XorOp xo(0x5a); SubOp s(7); RotOp ro(13);
    Op* targets[4] = {&a, &xo, &s, &ro};
    AnyC vtargets[4] = {AddC(3), XorC(0x5a), SubC(7), RotC(13)};

    std::vector<AddDirect*> p_direct(N, &direct);
    std::vector<AddFinal*> p_final(N, &fin);
    std::vector<AddC*> p_crtp(N, &crtp);
    std::vector<Op*> p_hot(N, targets[0]), p_rand(N);
    std::vector<const AnyC*> p_vrand(N);
    for (std::size_t i = 0; i < N; ++i) {
        const auto j = rng() % 4;
        p_rand[i] = targets[j];
        p_vrand[i] = &vtargets[j];  // same random order of types as p_rand
    }

    auto row = [&](const char* name, auto fn) {
        return bench(name, [&] { g_sink += fn(); }, kWarm, kSamples, N);
    };
    Result d  = row("direct (HW4)",                      [&] { return run_chain(p_direct.data(), x.data(), N); });
    Result f  = row("final (HW4)",                       [&] { return run_chain(p_final.data(), x.data(), N); });
    Result v  = row("virtual, 1 hot target (HW4)",       [&] { return run_chain(p_hot.data(), x.data(), N); });
    Result vr = row("virtual, 4 types random (HW4)",     [&] { return run_chain(p_rand.data(), x.data(), N); });
    Result c  = row("CRTP",                              [&] { return run_chain(p_crtp.data(), x.data(), N); });
    Result cv = row("CRTP in std::variant, 4 types rand",[&] { return run_chain_variant(p_vrand.data(), x.data(), N); });
    std::printf("  CRTP vs virtual-hot: %.1fx faster   CRTP vs direct: %.2fx   variant vs virtual (random): %.2fx\n",
                v.p50 / c.p50, c.p50 / d.p50, cv.p50 / vr.p50);
    (void)f;

    // Correctness: every implementation gives the same answer.
    std::uint64_t r_virt = run_chain(p_hot.data(), x.data(), N);
    std::uint64_t r_crtp = run_chain(p_crtp.data(), x.data(), N);
    std::uint64_t r_vr = run_chain(p_rand.data(), x.data(), N);
    std::uint64_t r_cv = run_chain_variant(p_vrand.data(), x.data(), N);
    std::printf("  same results: CRTP == virtual %s, variant == virtual (random) %s, apply_n(1,4) = %llu\n",
                r_virt == r_crtp ? "yes" : "NO", r_vr == r_cv ? "yes" : "NO",
                static_cast<unsigned long long>(crtp.apply_n(1, 4)));

    // ---------------- Task 2: constexpr CRC table ----------------
    std::puts("\n== Task 2: constexpr CRC-32 lookup table ==");
    std::printf("  table[1] = 0x%08X, CRC32(\"123456789\") = 0x%08X (expected 0xCBF43926)\n",
                kCrcTable.v[1], crc32_table(kCheck, 9));
    std::vector<unsigned char> msg(4096);
    for (auto& b : msg) b = static_cast<unsigned char>(rng());
    std::printf("  4 KB message: table 0x%08X, bitwise 0x%08X\n",
                crc32_table(msg.data(), msg.size()), crc32_bitwise(msg.data(), msg.size()));
    Result t1 = bench("CRC-32 with constexpr table", [&] { g_sink += crc32_table(msg.data(), msg.size()); },
                      100, 1000, static_cast<long>(msg.size()));
    Result t2 = bench("CRC-32 bitwise (no table)", [&] { g_sink += crc32_bitwise(msg.data(), msg.size()); },
                      100, 1000, static_cast<long>(msg.size()));
    std::printf("  (ns per byte) table is %.1fx faster\n", t2.p50 / t1.p50);

    // ---------------- Task 3: policy-based Position ----------------
    std::puts("\n== Task 3: policy-based Position<LimitPolicy, 100> ==");
    Position<ClampPolicy, 100> pc;
    Position<RejectPolicy, 100> pr;
    for (std::int64_t q : {60, 30, 40, -250, 20}) {
        pc.fill(q);
        pr.fill(q);
        std::printf("  fill %+5lld -> clamp: %4lld   reject: %4lld\n", static_cast<long long>(q),
                    static_cast<long long>(pc.get()), static_cast<long long>(pr.get()));
    }
    std::vector<std::int64_t> fills(N);
    for (auto& q : fills) q = static_cast<std::int64_t>(rng() % 21) - 10;
    auto run_pos = [&](auto pos) {
        for (auto q : fills) pos.fill(q);
        return pos.get();
    };
    bench("Position<NoLimitPolicy>", [&] { g_sink += run_pos(Position<NoLimitPolicy, 100>{}); }, 100, 1000, N);
    bench("Position<ClampPolicy>",   [&] { g_sink += run_pos(Position<ClampPolicy, 100>{}); }, 100, 1000, N);
    bench("Position<RejectPolicy>",  [&] { g_sink += run_pos(Position<RejectPolicy, 100>{}); }, 100, 1000, N);
    bench("raw int64 += (no class)", [&] {
        std::int64_t p = 0;
        for (auto q : fills) p += q;
        g_sink += p;
    }, 100, 1000, N);

    std::printf("\n(sink %llu)\n", static_cast<unsigned long long>(g_sink & 0xffff));
    return 0;
}
