// test_batch.cpp — Correctness tests for push_chain / push_batch.
//
// Covers:
//   • Single-threaded: batch FIFO order, batch-then-single interleave,
//     chain of one, large batch drain.
//   • Multi-threaded: P producers each pushing batches concurrently;
//     consumer verifies per-producer FIFO and zero loss/duplication —
//     the same invariants test_correctness.cpp checks for single push.
//
// Style matches test_correctness.cpp (CHECK macro, raw arrays; MpscNode
// is non-copyable so nodes live in fixed storage).

#include "mpsc/queue.hpp"
#include <atomic>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

using namespace mpsc;

static int passed = 0, failed = 0;
#define CHECK(cond, msg) \
    do { if (!(cond)) { \
        std::fprintf(stderr, "FAIL [%s:%d] %s\n", __FILE__, __LINE__, msg); \
        ++failed; } else { ++passed; } \
    } while(0)

struct TaggedNode : MpscNode {
    uint32_t producer = 0;
    uint32_t seq      = 0;
};

// ── Single-threaded ────────────────────────────────────────────────────────────

static void test_batch_fifo() {
    MpscQueue<TaggedNode> q;
    constexpr int N = 8;
    TaggedNode nodes[N];
    TaggedNode* ptrs[N];
    for (int i = 0; i < N; ++i) { nodes[i].seq = uint32_t(i); ptrs[i] = &nodes[i]; }

    q.push_batch(ptrs, N);

    for (int i = 0; i < N; ++i) {
        TaggedNode* p = q.pop();
        CHECK(p != nullptr,            "batch: node available");
        CHECK(p && p->seq == uint32_t(i), "batch: FIFO order preserved");
    }
    CHECK(q.pop() == nullptr, "batch: empty after full drain");
}

static void test_batch_of_one() {
    MpscQueue<TaggedNode> q;
    TaggedNode n; n.seq = 7;
    TaggedNode* ptrs[1] = { &n };
    q.push_batch(ptrs, 1);
    TaggedNode* p = q.pop();
    CHECK(p == &n && p->seq == 7, "batch of one behaves like single push");
    CHECK(q.pop() == nullptr,     "empty after batch of one");
}

static void test_batch_single_interleave() {
    MpscQueue<TaggedNode> q;
    TaggedNode a, b, c, d, e;
    a.seq = 0; b.seq = 1; c.seq = 2; d.seq = 3; e.seq = 4;

    q.push(&a);
    TaggedNode* mid[3] = { &b, &c, &d };
    q.push_batch(mid, 3);
    q.push(&e);

    for (uint32_t want = 0; want < 5; ++want) {
        TaggedNode* p = q.pop();
        CHECK(p && p->seq == want, "interleave: global order single,batch,single");
    }
    CHECK(q.pop() == nullptr, "interleave: drained");
}

static void test_push_chain_prelinked() {
    MpscQueue<TaggedNode> q;
    constexpr int N = 5;
    TaggedNode nodes[N];
    for (int i = 0; i < N; ++i) nodes[i].seq = uint32_t(i);
    for (int i = 0; i + 1 < N; ++i)
        nodes[i].next.store(&nodes[i + 1], std::memory_order_relaxed);
    // Deliberately leave nodes[N-1].next dirty to verify push_chain resets it.
    nodes[N - 1].next.store(&nodes[0], std::memory_order_relaxed);

    q.push_chain(&nodes[0], &nodes[N - 1]);

    for (int i = 0; i < N; ++i) {
        TaggedNode* p = q.pop();
        CHECK(p && p->seq == uint32_t(i), "push_chain: order preserved");
    }
    CHECK(q.pop() == nullptr, "push_chain: terminator overwritten, no cycle");
}

// ── Multi-threaded ─────────────────────────────────────────────────────────────
//
// P producers × B batches × K nodes per batch.  Each producer stamps
// (producer, seq).  Consumer checks per-producer sequence is strictly
// increasing by 1 (per-producer FIFO across batches, Claim 5 + Claim 6)
// and that exactly P*B*K distinct nodes arrive.

static void test_concurrent_batch_producers() {
    constexpr uint32_t P = 4, B = 250, K = 16;
    constexpr uint32_t TOTAL = P * B * K;

    MpscQueue<TaggedNode> q;
    std::vector<TaggedNode> storage(TOTAL);   // fixed storage, never reallocated
    std::atomic<bool> go{false};

    std::vector<std::thread> producers;
    producers.reserve(P);
    for (uint32_t p = 0; p < P; ++p) {
        producers.emplace_back([&, p] {
            while (!go.load(std::memory_order_acquire)) { /* spin */ }
            TaggedNode* base = storage.data() + p * B * K;
            uint32_t seq = 0;
            for (uint32_t b = 0; b < B; ++b) {
                TaggedNode* ptrs[K];
                for (uint32_t k = 0; k < K; ++k) {
                    TaggedNode* n = base + b * K + k;
                    n->producer = p;
                    n->seq      = seq++;
                    ptrs[k]     = n;
                }
                q.push_batch(ptrs, K);
            }
        });
    }

    uint32_t next_seq[P];
    std::memset(next_seq, 0, sizeof(next_seq));
    uint32_t received = 0;

    go.store(true, std::memory_order_release);

    while (received < TOTAL) {
        TaggedNode* n = q.pop();
        if (n == nullptr) continue;   // empty or incomplete-push window: retry
        CHECK(n->producer < P, "mt: valid producer id");
        CHECK(n->seq == next_seq[n->producer], "mt: per-producer FIFO across batches");
        ++next_seq[n->producer];
        ++received;
    }
    for (auto& t : producers) t.join();

    CHECK(received == TOTAL,  "mt: no loss, no duplication");
    CHECK(q.pop() == nullptr, "mt: queue drained");
}

int main() {
    test_batch_fifo();
    test_batch_of_one();
    test_batch_single_interleave();
    test_push_chain_prelinked();
    test_concurrent_batch_producers();
    std::printf("%d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
