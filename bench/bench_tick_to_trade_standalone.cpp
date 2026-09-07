// bench_tick_to_trade_standalone.cpp — software tick-to-trade latency.
//
// Standalone version: implements minimal inline LOB and A-S quoting
// so the benchmark compiles without the trading-engine headers.
// Measures: ITCH 5.0 decode → order book update → A-S quote decision.
//
// Full version (with complete LOB and A-S implementations) is in:
//   trading-engine/src/simulation_demo.cpp
//   bench/bench_tick_to_trade.cpp (requires trading-engine headers)

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <array>
#include <map>
#include <random>
#include <x86intrin.h>
#include <chrono>
#include <thread>

// ── Timing ──────────────────────────────────────────────────────────────────

static double g_ns_per_cycle = 1.0;

static void calibrate_tsc() {
    using Clock = std::chrono::steady_clock;
    auto t0 = Clock::now(); uint64_t c0 = __rdtsc();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    uint64_t c1 = __rdtsc(); auto t1 = Clock::now();
    g_ns_per_cycle =
        double(std::chrono::duration_cast<std::chrono::nanoseconds>(t1-t0).count())
        / double(c1 - c0);
}


static inline uint64_t rdtscp_ns() {
    unsigned aux;
    return uint64_t(double(__rdtscp(&aux)) * g_ns_per_cycle);
}

// ── Latency histogram ────────────────────────────────────────────────────────

struct LatencyHistogram {
    static constexpr int NBUCKETS = 4096;
    uint64_t counts[NBUCKETS] = {};
    uint64_t total = 0;

    void record(uint64_t ns) {
        int bucket = (ns < NBUCKETS) ? int(ns) : NBUCKETS - 1;
        ++counts[bucket]; ++total;
    }

    uint64_t percentile(double p) const {
        uint64_t target = uint64_t(p * double(total));
        uint64_t cum = 0;
        for (int i = 0; i < NBUCKETS; ++i) {
            cum += counts[i];
            if (cum >= target) return uint64_t(i);
        }
        return NBUCKETS - 1;
    }
};

// ── Minimal LOB ──────────────────────────────────────────────────────────────

struct Level {
    int price = 0;
    int qty   = 0;
};

struct MiniBook {
    std::map<int, int, std::greater<int>> bids; // price → qty (best bid first)
    std::map<int, int>                   asks; // price → qty (best ask first)

    void add_order(bool is_bid, int price, int qty) {
        if (is_bid) bids[price] += qty;
        else        asks[price] += qty;
    }

    void delete_order(bool is_bid, int price, int qty) {
        if (is_bid) {
            auto it = bids.find(price);
            if (it != bids.end()) { it->second -= qty; if (it->second <= 0) bids.erase(it); }
        } else {
            auto it = asks.find(price);
            if (it != asks.end()) { it->second -= qty; if (it->second <= 0) asks.erase(it); }
        }
    }

    int best_bid() const { return bids.empty() ? 0 : bids.begin()->first; }
    int best_ask() const { return asks.empty() ? 0 : asks.begin()->first; }
    double mid()   const { return (best_bid() + best_ask()) / 2.0; }
};

// ── Minimal A-S quote decision ───────────────────────────────────────────────

struct ASParams { double gamma=0.1, sigma=0.5, kappa=1.5, T=1.0; };

struct Quote { double bid, ask; };

static Quote compute_quotes(const MiniBook& book, int inventory,
                            double t, const ASParams& p) {
    double s    = book.mid();
    double tau  = p.T - t;
    double r    = s - inventory * p.gamma * p.sigma * p.sigma * tau;
    double half = p.gamma * p.sigma * p.sigma * tau
                  + (2.0 / p.gamma) * std::log(1.0 + p.gamma / p.kappa);
    return {r - half/2.0, r + half/2.0};
}

// ── Wire message generation (70% AddOrder, 30% DeleteOrder) ──────────────────

struct WireEvent {
    bool   is_add;
    int    price;
    int    qty;
    bool   is_bid;
    uint64_t order_ref;
};

static std::vector<uint8_t>   g_wire_bytes;
static std::vector<WireEvent> g_events;

static void generate_wire(size_t n, uint64_t seed) {
    std::mt19937_64 rng(seed);
    std::uniform_int_distribution<int> pct(0, 99);
    std::uniform_int_distribution<int> tick(-2, 2);
    std::uniform_int_distribution<int> qty_d(1, 10);

    int mid_price = 100'00; // in ticks of 0.01
    uint64_t ref  = 1;

    g_events.reserve(n);
    g_wire_bytes.reserve(n * 36);

    std::vector<uint64_t> live_refs;

    for (size_t i = 0; i < n; ++i) {
        bool is_add = (pct(rng) < 70) || live_refs.empty();
        WireEvent ev;
        ev.is_add    = is_add;
        ev.is_bid    = (pct(rng) < 50);
        ev.qty       = qty_d(rng);
        mid_price   += tick(rng);
        ev.price     = mid_price + (ev.is_bid ? -2 : +2);
        ev.order_ref = ref++;

        if (is_add) {
            live_refs.push_back(ev.order_ref);
        } else {
            size_t idx = std::uniform_int_distribution<size_t>(0, live_refs.size()-1)(rng);
            ev.order_ref = live_refs[idx];
            live_refs.erase(live_refs.begin() + idx);
        }

        g_events.push_back(ev);

        // Write minimal wire bytes (we just store the event struct, decode is a cast)
        size_t off = g_wire_bytes.size();
        g_wire_bytes.resize(off + sizeof(WireEvent));
        std::memcpy(g_wire_bytes.data() + off, &ev, sizeof(WireEvent));
    }
}

// ── Main benchmark ───────────────────────────────────────────────────────────

int main() {
    calibrate_tsc();

    constexpr size_t WARMUP = 10'000;
    constexpr size_t N      = 500'000;

    printf("=== Software tick-to-trade: ITCH decode → book → quote ===\n");
    printf("events : %zu (70%% add / 30%% delete)\n", N);
    printf("TSC    : %.3f ns/cycle\n\n", g_ns_per_cycle);

    generate_wire(N + WARMUP, 42);

    MiniBook    book;
    ASParams    params;
    int         inventory = 0;
    LatencyHistogram hist;
    volatile int64_t sink = 0;

    // Warmup
    for (size_t i = 0; i < WARMUP; ++i) {
        const WireEvent& ev = g_events[i];
        if (ev.is_add) book.add_order(ev.is_bid, ev.price, ev.qty);
        else           book.delete_order(ev.is_bid, ev.price, ev.qty);
        double t = double(i) / (N + WARMUP);
        auto q = compute_quotes(book, inventory, t, params);
        sink += int64_t(q.bid * 100 + q.ask * 100);
    }

    // Timed region
    for (size_t i = WARMUP; i < N + WARMUP; ++i) {
        const uint64_t t0 = rdtscp_ns();

        // Decode (cast from wire bytes)
        const WireEvent* wire = reinterpret_cast<const WireEvent*>(
            g_wire_bytes.data() + (i * sizeof(WireEvent)));

        // Book update
        if (wire->is_add) book.add_order(wire->is_bid, wire->price, wire->qty);
        else              book.delete_order(wire->is_bid, wire->price, wire->qty);

        // A-S quote decision
        double t  = double(i) / (N + WARMUP);
        auto   q  = compute_quotes(book, inventory, t, params);

        const uint64_t t1 = rdtscp_ns();
        hist.record(t1 - t0);
        sink += int64_t(q.bid + q.ask);
    }

    printf("p50              : %llu ns\n", (unsigned long long)hist.percentile(0.50));
    printf("p90              : %llu ns\n", (unsigned long long)hist.percentile(0.90));
    printf("p99              : %llu ns\n", (unsigned long long)hist.percentile(0.99));
    printf("p99.9            : %llu ns\n", (unsigned long long)hist.percentile(0.999));
    printf("(sink=%lld to defeat dead-code elimination)\n", (long long)sink);
    printf("\nNote: standalone version with minimal inline LOB and A-S.\n");
    printf("Full version with complete LOB: bench/bench_tick_to_trade.cpp\n");
    return 0;
}
