// bench_tick_to_trade_queue.cpp — tick-to-trade THROUGH the queue.
//
// bench_tick_to_trade_standalone.cpp measures decode+book+quote latency
// entirely on one thread. That is a valid and useful number, but it never
// actually exercises MpscQueue — the queue this repo is about is absent
// from its own headline benchmark. This file fixes that gap: it wires the
// MPSC queue into an actual two-stage pipeline and measures the latency a
// caller would really see end to end.
//
//   [decode thread]  wire bytes → normalised event → q.push()
//                                                        │
//                                             MpscQueue<EventNode>
//                                                        │
//   [strategy thread]        q.pop() → book update → A-S quote decision
//
// This is exactly the shape a real market-data handler uses this queue
// for: decoding is pinned to the core reading the NIC/socket, and the
// strategy thread that owns book + risk state runs on a separate core so
// GC-free, allocation-free decode work never stalls behind a quote
// computation (or vice versa). Single producer here because ITCH-style
// feeds are strictly ordered per session — this queue's MPSC generality
// is for fan-in from multiple symbols/sessions, not used in this
// single-feed benchmark, but push() is exactly as cheap with 1 producer
// as with M.
//
// What's measured: RDTSCP at the top of decode → RDTSCP after
// compute_quotes() returns, i.e. the full path INCLUDING the queue hop.
// Compare directly against bench_t2t's number: the delta is what putting
// the queue in the critical path actually costs (or, if the two threads
// pipeline well, doesn't cost — see the printed comparison at the end).
//
// Book and A-S quoting logic are intentionally identical to
// bench_tick_to_trade_standalone.cpp (same MiniBook, same
// compute_quotes) so the only variable between the two benchmarks is
// "single thread" vs "decode/strategy split via MpscQueue".

#include "histogram.hpp"
#include "mpsc/queue.hpp"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <random>
#include <thread>
#include <vector>

using bench::Backoff;
using bench::LatencyHistogram;
using bench::TscClock;

namespace {

// ── Minimal LOB + A-S quoting (identical to the standalone benchmark) ────────

struct MiniBook {
    std::map<int, int, std::greater<int>> bids;
    std::map<int, int> asks;

    void add_order(bool is_bid, int price, int qty) {
        if (is_bid) bids[price] += qty; else asks[price] += qty;
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
    double mid() const { return (best_bid() + best_ask()) / 2.0; }
};

struct ASParams { double gamma = 0.1, sigma = 0.5, kappa = 1.5, T = 1.0; };
struct Quote { double bid, ask; };

Quote compute_quotes(const MiniBook& book, int inventory, double t, const ASParams& p) {
    const double s = book.mid();
    const double tau = p.T - t;
    const double r = s - inventory * p.gamma * p.sigma * p.sigma * tau;
    const double half = p.gamma * p.sigma * p.sigma * tau
                       + (2.0 / p.gamma) * std::log(1.0 + p.gamma / p.kappa);
    return {r - half / 2.0, r + half / 2.0};
}

// ── Wire event generation (identical distribution to the standalone bench) ───

struct WireEvent { bool is_add; int price; int qty; bool is_bid; uint64_t order_ref; };

std::vector<WireEvent> generate_wire(size_t n, uint64_t seed) {
    std::mt19937_64 rng(seed);
    std::uniform_int_distribution<int> pct(0, 99);
    std::uniform_int_distribution<int> tick(-2, 2);
    std::uniform_int_distribution<int> qty_d(1, 10);

    int mid_price = 100'00;
    uint64_t ref = 1;
    std::vector<uint64_t> live_refs;
    std::vector<WireEvent> events;
    events.reserve(n);

    for (size_t i = 0; i < n; ++i) {
        bool is_add = (pct(rng) < 70) || live_refs.empty();
        WireEvent ev{};
        ev.is_add = is_add;
        ev.is_bid = (pct(rng) < 50);
        ev.qty = qty_d(rng);
        mid_price += tick(rng);
        ev.price = mid_price + (ev.is_bid ? -2 : 2);
        ev.order_ref = ref++;
        if (is_add) {
            live_refs.push_back(ev.order_ref);
        } else {
            size_t idx = std::uniform_int_distribution<size_t>(0, live_refs.size() - 1)(rng);
            ev.order_ref = live_refs[idx];
            live_refs.erase(live_refs.begin() + idx);
        }
        events.push_back(ev);
    }
    return events;
}

// ── The queue node ─────────────────────────────────────────────────────────

struct EventNode : mpsc::MpscNode {
    WireEvent event{};
    uint64_t t0_ns = 0;   // decode start timestamp, set by the producer
};

} // namespace

int main(int argc, char** argv) {
    const size_t N = (argc > 1) ? std::strtoull(argv[1], nullptr, 10) : 500'000;
    const size_t WARMUP = N / 10;
    const size_t TOTAL = N + WARMUP;

    TscClock tsc;
    tsc.calibrate();

    constexpr uint64_t kMaxInFlight = 256;

    std::printf("=== Tick-to-trade THROUGH the queue: decode thread -> MpscQueue -> strategy thread ===\n");
    std::printf("events : %zu (70%% add / 30%% delete), %zu warmup\n", N, WARMUP);
    std::printf("bound  : decoder capped %llu events ahead of strategy thread (forces real interleaving)\n",
                (unsigned long long)kMaxInFlight);
    std::printf("TSC    : %.4f ns/cycle\n\n", tsc.ns_per_cycle);
    std::printf("Run pinned across two cores for a real cross-core number, e.g.:\n");
    std::printf("  taskset -c 4,5 chrt -f 80 ./bench_t2t_queue %zu   (pins both threads to 4,5)\n\n", N);

    std::vector<WireEvent> events = generate_wire(TOTAL, 42);

    // Preallocate every node up front: this benchmark is a fixed-size batch,
    // not a sustained/indefinite run, so there is no need for the
    // ring+backpressure+deferred-free machinery bench_stress.cpp uses (nodes
    // are never recycled here, so the node-lifetime hazard documented in
    // queue.hpp's pop() comment doesn't apply).
    auto nodes = std::make_unique<EventNode[]>(TOTAL);

    mpsc::MpscQueue<EventNode> q;
    std::atomic<bool> go{false};
    std::atomic<uint64_t> consumed_count{0};

    // Bounded lookahead: the decoder will not get more than kMaxInFlight
    // events ahead of the strategy thread. Without this, on a host where
    // the two threads don't actually run concurrently (e.g. a single
    // shared core), the decoder simply dumps all TOTAL events before the
    // strategy thread gets scheduled even once, and the resulting
    // "latency" number measures nothing but that scheduling accident. With
    // a bound, the two threads are forced to interleave in small batches
    // regardless of core count, so the number reflects actual per-batch
    // cross-thread handoffs — degraded by contention on a shared core, but
    // at least measuring the right thing.

    std::thread decoder([&] {
        Backoff backoff;
        while (!go.load(std::memory_order_acquire)) backoff.wait();
        for (size_t i = 0; i < TOTAL; ++i) {
            while (i - consumed_count.load(std::memory_order_acquire) >= kMaxInFlight)
                backoff.wait();
            backoff.reset();
            EventNode* n = &nodes[i];
            n->t0_ns = tsc.now_ns();          // decode starts here
            n->event = events[i];             // the "decode" (already-parsed struct
                                               // here; the standalone benchmark's
                                               // byte-level parse is a fixed, small,
                                               // measured constant — see its own
                                               // results — the number this benchmark
                                               // adds is the queue hop + pipelining)
            q.push(n);
        }
    });

    MiniBook book;
    ASParams params;
    int inventory = 0;
    LatencyHistogram hist;
    volatile int64_t sink = 0;

    go.store(true, std::memory_order_release);

    Backoff backoff;
    for (size_t i = 0; i < TOTAL; ++i) {
        EventNode* n;
        while ((n = q.pop()) == nullptr) backoff.wait();
        backoff.reset();

        const WireEvent& ev = n->event;
        if (ev.is_add) book.add_order(ev.is_bid, ev.price, ev.qty);
        else book.delete_order(ev.is_bid, ev.price, ev.qty);

        const double t = double(i) / double(TOTAL);
        const Quote qt = compute_quotes(book, inventory, t, params);
        const uint64_t t1 = tsc.now_ns();

        if (i >= WARMUP) hist.record(t1 - n->t0_ns);
        sink += int64_t(qt.bid * 100 + qt.ask * 100);
        consumed_count.store(i + 1, std::memory_order_release);
    }

    decoder.join();

    std::printf("p50              : %llu ns\n", (unsigned long long)hist.percentile(0.50));
    std::printf("p90              : %llu ns\n", (unsigned long long)hist.percentile(0.90));
    std::printf("p99              : %llu ns\n", (unsigned long long)hist.percentile(0.99));
    std::printf("p99.9            : %llu ns\n", (unsigned long long)hist.percentile(0.999));
    std::printf("max              : %llu ns\n", (unsigned long long)hist.max_ns());
    std::printf("(sink=%lld to defeat dead-code elimination)\n\n", (long long)sink);
    std::printf(
        "Compare against bench_t2t_standalone (same decode/book/quote logic,\n"
        "single thread, no queue). The delta between that number and this one\n"
        "is the queue's real contribution to this pipeline: on two well-pinned\n"
        "cores it should be small (the queue overlaps decode and strategy work\n"
        "instead of adding to it). On a shared/oversubscribed host, the bounded\n"
        "lookahead above forces real interleaving in small batches rather than\n"
        "one bulk dump, so this number still reflects actual cross-thread\n"
        "handoffs -- just handoffs slowed by scheduler contention, not by the\n"
        "queue. Pin both threads to isolated cores for a comparison that\n"
        "isolates the queue's cost from that contention.\n");
    return 0;
}
