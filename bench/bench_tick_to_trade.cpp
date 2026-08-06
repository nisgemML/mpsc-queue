// bench_tick_to_trade.cpp — software tick-to-trade latency, end to end.
//
// Measures the full decision path a market maker executes per market event:
//
//   raw ITCH 5.0 bytes  →  wire decode  →  order book update
//     →  top-of-book read  →  Avellaneda-Stoikov quote decision
//
// One RDTSCP timestamp before the wire bytes are touched, one after
// compute_quotes() returns.  That interval is the software tick-to-trade
// number: "market data arrived; what do we quote?"  It deliberately
// excludes NIC/kernel receive time (see udp-multicast-receiver for the
// SO_TIMESTAMPING treatment of that half) and order egress.
//
// Methodology (same discipline as bench_latency.cpp):
//   • Wire messages are pre-generated into a contiguous buffer — the
//     generator is outside the timed region.
//   • Zero allocation inside the timed loop; book and strategy state are
//     pre-warmed with WARMUP_EVENTS untimed events.
//   • Per-event latency recorded into LatencyHistogram (p50/p99/p99.9),
//     not a sorted vector.
//   • Run pinned for stable numbers:  taskset -c 4 chrt -f 80 ./bench_t2t
//
// Event mix: 70% Add Order ('A', 36 bytes), 30% Order Delete ('D', 19
// bytes) over a random-walk price — enough churn to keep top-of-book
// moving so compute_quotes() sees realistic state, not a frozen book.

#include "core/itch_parser.hpp"     // read_u16/u32/u48/u64, MsgType
#include "core/order_book.hpp"
#include "core/inventory_market_maker.hpp"
#include "core/types.hpp"
#include "util/latency_histogram.hpp"
#include "util/tsc.hpp"

#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

using namespace engine;

// ── ITCH 5.0 wire sizes ────────────────────────────────────────────────────────
static constexpr size_t kAddOrderLen    = 36;  // 'A'
static constexpr size_t kOrderDeleteLen = 19;  // 'D'

// ── Wire message generator (untimed, runs before the benchmark loop) ──────────
//
// Layout per ITCH 5.0 spec:
//   'A': type(1) locate(2) tracking(2) ts(6) ref(8) side(1) shares(4)
//        stock(8) price(4)
//   'D': type(1) locate(2) tracking(2) ts(6) ref(8)

static void write_be16(uint8_t* p, uint16_t v) { p[0] = uint8_t(v >> 8); p[1] = uint8_t(v); }
static void write_be32(uint8_t* p, uint32_t v) {
    p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v);
}
static void write_be48(uint8_t* p, uint64_t v) {
    for (int i = 0; i < 6; ++i) p[i] = uint8_t(v >> (8 * (5 - i)));
}
static void write_be64(uint8_t* p, uint64_t v) {
    for (int i = 0; i < 8; ++i) p[i] = uint8_t(v >> (8 * (7 - i)));
}

struct WireEvent {
    size_t  offset;   // into the shared byte buffer
    uint8_t type;     // 'A' or 'D'
};

struct Generated {
    std::vector<uint8_t>   bytes;
    std::vector<WireEvent> events;
};

static Generated generate_events(size_t n_events, uint32_t seed) {
    Generated g;
    g.bytes.reserve(n_events * kAddOrderLen);
    g.events.reserve(n_events);

    std::mt19937 rng(seed);
    std::uniform_int_distribution<int> pct(0, 99);
    std::uniform_int_distribution<int> side_d(0, 1);
    std::uniform_int_distribution<int> tick_d(-3, 3);
    std::uniform_int_distribution<uint32_t> qty_d(1, 10);

    // Random-walk price in ITCH units (price * 10^4), around $100.00.
    int64_t px = 100'0000;
    uint64_t next_ref = 1;
    std::vector<uint64_t> live;          // refs currently in the book
    live.reserve(n_events);

    for (size_t i = 0; i < n_events; ++i) {
        const bool do_delete = !live.empty() && pct(rng) < 30;
        if (do_delete) {
            const size_t idx = rng() % live.size();
            const uint64_t ref = live[idx];
            live[idx] = live.back(); live.pop_back();

            const size_t off = g.bytes.size();
            g.bytes.resize(off + kOrderDeleteLen);
            uint8_t* p = g.bytes.data() + off;
            p[0] = itch::MsgType::OrderDelete;
            write_be16(p + 1, 1);            // stock locate
            write_be16(p + 3, 0);            // tracking
            write_be48(p + 5, uint64_t(i));  // timestamp
            write_be64(p + 11, ref);
            g.events.push_back({off, itch::MsgType::OrderDelete});
        } else {
            px += tick_d(rng) * 100;         // ±3 ticks of $0.01
            if (px < 90'0000) px = 90'0000;
            const uint64_t ref = next_ref++;
            live.push_back(ref);

            const size_t off = g.bytes.size();
            g.bytes.resize(off + kAddOrderLen);
            uint8_t* p = g.bytes.data() + off;
            p[0] = itch::MsgType::AddOrder;
            write_be16(p + 1, 1);
            write_be16(p + 3, 0);
            write_be48(p + 5, uint64_t(i));
            write_be64(p + 11, ref);
            p[19] = side_d(rng) ? 'B' : 'S';
            write_be32(p + 20, qty_d(rng) * 100);
            std::memcpy(p + 24, "BENCH   ", 8);
            write_be32(p + 32, uint32_t(px));
            g.events.push_back({off, itch::MsgType::AddOrder});
        }
    }
    return g;
}

// ── The timed hot path ─────────────────────────────────────────────────────────
//
// Decode is done inline with the repo's read_u* helpers so the timed region
// is a pure single-thread path (no SPSC hop, no second core): this measures
// algorithmic tick-to-trade, the number bounded below by parse + book +
// strategy math.  ITCH prices are 10^-4 dollars; internal Price is 10^-6,
// hence the ×100.

int main(int argc, char** argv) {
    const size_t N_EVENTS      = (argc > 1) ? strtoull(argv[1], nullptr, 10) : 1'000'000;
    const size_t WARMUP_EVENTS = N_EVENTS / 10;

    TscClock tsc;
    tsc.calibrate();

    const Generated warm = generate_events(WARMUP_EVENTS, 7);
    const Generated run  = generate_events(N_EVENTS, 42);

    OrderBook book(/*symbol=*/1, /*on_match=*/[](const ExecutionReport&) noexcept {});
    InventoryMarketMaker mm{MarketMakerParams{}};

    LatencyHistogram hist;
    uint64_t quotes_emitted = 0;   // consumed so compute_quotes can't be DCE'd
    Price    sink = 0;

    auto process = [&](const Generated& g, bool timed) {
        double elapsed = 0.0;
        for (const WireEvent& ev : g.events) {
            const uint8_t* p = g.bytes.data() + ev.offset;

            const uint64_t c0 = TscClock::now_cycles();

            // 1. Wire decode (big-endian ITCH 5.0)
            if (ev.type == itch::MsgType::AddOrder) {
                Order o{};
                o.id            = itch::read_u64(p + 11);
                o.side          = (p[19] == 'B') ? Side::Buy : Side::Sell;
                o.qty           = itch::read_u32(p + 20);
                o.qty_remaining = o.qty;
                o.price         = Price(itch::read_u32(p + 32)) * 100;
                o.symbol        = 1;
                o.type          = OrderType::Limit;
                // 2. Book update
                book.add_order(o);
            } else {
                book.cancel_order(itch::read_u64(p + 11));
            }

            // 3. Top-of-book → 4. quote decision
            const BestQuote tob = book.best_quote();
            if (tob.bid_qty > 0 && tob.ask_qty > 0) {
                mm.update_book_state(tob.bid_price, tob.bid_qty,
                                     tob.ask_price, tob.ask_qty, elapsed);
                const QuoteDecision qd = mm.compute_quotes();
                sink ^= qd.bid_price ^ qd.ask_price;
                ++quotes_emitted;
            }

            const uint64_t c1 = TscClock::now_cycles();
            if (timed) hist.record(tsc.cycles_to_ns(c1 - c0));
            elapsed += 1e-6;
        }
    };

    process(warm, /*timed=*/false);
    quotes_emitted = 0;                 // don't count warmup decisions
    process(run,  /*timed=*/true);

    std::printf("=== Software tick-to-trade: ITCH decode -> book -> quote ===\n");
    std::printf("events           : %zu (70%% add / 30%% delete)\n", N_EVENTS);
    std::printf("quote decisions  : %llu\n", (unsigned long long)quotes_emitted);
    std::printf("p50              : %llu ns\n", (unsigned long long)hist.percentile(0.50));
    std::printf("p99              : %llu ns\n", (unsigned long long)hist.percentile(0.99));
    std::printf("p99.9            : %llu ns\n", (unsigned long long)hist.percentile(0.999));
    std::printf("(sink=%lld to defeat dead-code elimination)\n", (long long)sink);
    return 0;
}
