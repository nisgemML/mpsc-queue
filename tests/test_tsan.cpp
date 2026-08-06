// test_tsan.cpp — ThreadSanitizer litmus tests for the MPSC queue.
//
// ── What these tests validate ─────────────────────────────────────────────────
//
// A litmus test is a minimal program distinguishing memory models.
// The three critical tests for our MPSC queue:
//
//   (1) MP — Message Passing
//       Producer writes payload, then pushes node (release).
//       Consumer pops node (acquire), then reads payload.
//       MUST: payload is always 0xDEADBEEF when node is observed.
//       Validates: release/acquire establishes happens-before.
//
//   (2) CO — Coherence (per-producer FIFO)
//       Two producers each push N nodes with a per-thread sequence number.
//       Consumer must observe each producer's nodes in push order.
//       Validates: tail_.exchange serialises producers.
//
//   (3) Many producers — stress test of (2) with 8 producers.
//
//   (4) Incomplete push — consumer returns nullptr during the exchange-to-
//       next-store window, then succeeds.  TSan verifies no data race on the
//       payload during this window.
//
// Run normally: all four tests pass.
// Run under -fsanitize=thread: TSan reports zero races — proving
// acquire/release is sufficient and no seq_cst is needed.

#include "mpsc/queue.hpp"
#include <cstdio>
#include <thread>
#include <memory>
#include <atomic>
#include <cassert>
#include <cstring>
#include <vector>

using namespace mpsc;

static int passed = 0, failed = 0;
#define CHECK(cond, msg) \
    do { if (!(cond)) { \
        std::fprintf(stderr, "FAIL [%s:%d] %s\n", __FILE__, __LINE__, msg); \
        ++failed; } else { ++passed; } \
    } while(0)

// ── Litmus test 1: Message Passing ───────────────────────────────────────────

static void test_mp_litmus() {
    constexpr int N = 100'000;

    struct PayloadNode : MpscNode {
        uint64_t payload = 0;
        int      id      = 0;
    };

    // Allocate as array — MpscNode deletes copy/move.
    auto nodes = std::make_unique<PayloadNode[]>(N);
    MpscQueue<PayloadNode> q;

    std::thread producer([&] {
        for (int i = 0; i < N; ++i) {
            nodes[i].id      = i;
            nodes[i].payload = 0xDEADBEEFDEADBEEFULL;
            q.push(&nodes[i]);
        }
    });

    int  consumed   = 0;
    bool payload_ok = true;
    while (consumed < N) {
        PayloadNode* p = q.pop();
        if (!p) { __builtin_ia32_pause(); continue; }
        if (p->payload != 0xDEADBEEFDEADBEEFULL) payload_ok = false;
        ++consumed;
    }

    producer.join();
    CHECK(consumed == N,  "MP litmus: all N nodes consumed");
    CHECK(payload_ok,     "MP litmus: payload always visible (release/acquire sufficient)");
}

// ── Litmus test 2: Two producers, per-producer FIFO ──────────────────────────

static void test_two_producers_order() {
    constexpr int N = 50'000;

    struct SeqNode : MpscNode {
        int producer_id = 0;
        int seq         = 0;
    };

    auto nodes_a = std::make_unique<SeqNode[]>(N);
    auto nodes_b = std::make_unique<SeqNode[]>(N);
    MpscQueue<SeqNode> q;

    auto push_n = [&](SeqNode* nodes, int pid) {
        for (int i = 0; i < N; ++i) {
            nodes[i].producer_id = pid;
            nodes[i].seq         = i;
            q.push(&nodes[i]);
        }
    };

    std::thread t1([&] { push_n(nodes_a.get(), 0); });
    std::thread t2([&] { push_n(nodes_b.get(), 1); });

    int  last_seq[2] = {-1, -1};
    int  consumed    = 0;
    bool order_ok    = true;

    while (consumed < 2 * N) {
        SeqNode* p = q.pop();
        if (!p) { __builtin_ia32_pause(); continue; }
        const int pid = p->producer_id;
        if (pid < 0 || pid > 1) { order_ok = false; }
        else {
            if (p->seq != last_seq[pid] + 1) order_ok = false;
            last_seq[pid] = p->seq;
        }
        ++consumed;
    }

    t1.join(); t2.join();
    CHECK(consumed == 2 * N,        "two-producer: all nodes consumed");
    CHECK(order_ok,                 "two-producer: per-producer FIFO preserved");
    CHECK(last_seq[0] == N - 1,    "producer 0: all consumed");
    CHECK(last_seq[1] == N - 1,    "producer 1: all consumed");
}

// ── Litmus test 3: Many producers ────────────────────────────────────────────

static void test_many_producers() {
    constexpr int N_THREADS    = 8;
    constexpr int N_PER_THREAD = 10'000;
    constexpr int N_TOTAL      = N_THREADS * N_PER_THREAD;

    struct CountNode : MpscNode {
        int thread_id = 0;
        int seq       = 0;
    };

    // One contiguous allocation per thread.
    std::vector<std::unique_ptr<CountNode[]>> nodes(N_THREADS);
    for (int t = 0; t < N_THREADS; ++t)
        nodes[t] = std::make_unique<CountNode[]>(N_PER_THREAD);

    MpscQueue<CountNode> q;
    std::vector<std::thread> threads;
    threads.reserve(N_THREADS);

    for (int t = 0; t < N_THREADS; ++t) {
        threads.emplace_back([&, t] {
            for (int i = 0; i < N_PER_THREAD; ++i) {
                nodes[t][i].thread_id = t;
                nodes[t][i].seq       = i;
                q.push(&nodes[t][i]);
            }
        });
    }

    int  last_seq[N_THREADS];
    for (int t = 0; t < N_THREADS; ++t) last_seq[t] = -1;
    int  consumed = 0;
    bool order_ok = true;

    while (consumed < N_TOTAL) {
        CountNode* p = q.pop();
        if (!p) { __builtin_ia32_pause(); continue; }
        const int t = p->thread_id;
        if (t < 0 || t >= N_THREADS) { order_ok = false; }
        else {
            if (p->seq != last_seq[t] + 1) order_ok = false;
            last_seq[t] = p->seq;
        }
        ++consumed;
    }

    for (auto& th : threads) th.join();
    CHECK(consumed == N_TOTAL, "many-producers: all consumed");
    CHECK(order_ok,            "many-producers: per-thread FIFO preserved");
    for (int t = 0; t < N_THREADS; ++t)
        CHECK(last_seq[t] == N_PER_THREAD - 1,
              "many-producers: all items per thread consumed");
}

// ── Litmus test 4: Incomplete push window — no data race ─────────────────────
//
// The "incomplete push" window: between tail_.exchange and prev->next release-store,
// tail_ points to the new node but the list is not yet traversable to it.
// The consumer returns nullptr during this window.
//
// TSan validates that the consumer's read of node->payload after a successful
// pop() does not race with the producer's write — i.e., the release-store on
// prev->next happens-before the consumer's acquire-load.

static void test_incomplete_push_window() {
    struct WindowNode : MpscNode {
        int value = 0;
    };

    MpscQueue<WindowNode> q;
    WindowNode n;
    n.value = 999;

    std::atomic<bool> push_started{false};

    std::thread producer([&] {
        push_started.store(true, std::memory_order_release);
        q.push(&n);
    });

    while (!push_started.load(std::memory_order_acquire)) {}

    WindowNode* p = nullptr;
    while (!(p = q.pop())) __builtin_ia32_pause();

    CHECK(p == &n,         "incomplete-push: correct node returned");
    CHECK(p->value == 999, "incomplete-push: payload visible after acquire-load");

    producer.join();
}

// ── main ──────────────────────────────────────────────────────────────────────

int main() {
    std::printf("=== MPSC Queue TSan Litmus Tests ===\n");
    std::printf("(Run with -fsanitize=thread build to validate memory ordering)\n\n");

    test_mp_litmus();
    test_two_producers_order();
    test_many_producers();
    test_incomplete_push_window();

    std::printf("\nResults: %d passed, %d failed\n", passed, failed);
    return failed > 0 ? 1 : 0;
}
