// test_correctness.cpp — Functional correctness tests for the MPSC queue.
//
// Single-threaded: verifies the basic invariants independently of
// any memory-ordering concerns.  Uses raw arrays (MpscNode deletes copy/move).

#include "mpsc/queue.hpp"
#include <cstdio>
#include <memory>
#include <cassert>

using namespace mpsc;

static int passed = 0, failed = 0;
#define CHECK(cond, msg) \
    do { if (!(cond)) { \
        std::fprintf(stderr, "FAIL [%s:%d] %s\n", __FILE__, __LINE__, msg); \
        ++failed; } else { ++passed; } \
    } while(0)

struct IntNode : MpscNode {
    int value = 0;
    explicit IntNode(int v = 0) noexcept : value(v) {}
};

static void test_empty_pop() {
    MpscQueue<IntNode> q;
    CHECK(q.pop() == nullptr, "empty queue returns nullptr");
    CHECK(q.approximately_empty(), "approximately_empty on empty queue");
}

static void test_single_push_pop() {
    MpscQueue<IntNode> q;
    IntNode n(42);
    q.push(&n);
    CHECK(!q.approximately_empty(), "non-empty after push");
    IntNode* out = q.pop();
    CHECK(out == &n,        "pop returns pushed node");
    CHECK(out->value == 42, "node value preserved");
    CHECK(q.pop() == nullptr, "empty after consuming only element");
}

static void test_fifo_order() {
    MpscQueue<IntNode> q;
    IntNode n1(1), n2(2), n3(3);
    q.push(&n1); q.push(&n2); q.push(&n3);
    CHECK(q.pop() == &n1, "FIFO: first");
    CHECK(q.pop() == &n2, "FIFO: second");
    CHECK(q.pop() == &n3, "FIFO: third");
    CHECK(q.pop() == nullptr, "empty after all consumed");
}

static void test_interleaved_push_pop() {
    MpscQueue<IntNode> q;
    IntNode n1(1), n2(2), n3(3), n4(4);
    q.push(&n1); q.push(&n2);
    CHECK(q.pop() == &n1, "interleaved: n1");
    q.push(&n3);
    CHECK(q.pop() == &n2, "interleaved: n2");
    CHECK(q.pop() == &n3, "interleaved: n3");
    CHECK(q.pop() == nullptr, "interleaved: empty");
    q.push(&n4);
    CHECK(q.pop() == &n4, "interleaved: n4");
}

static void test_many_elements() {
    MpscQueue<IntNode> q;
    constexpr int N = 10000;
    // MpscNode deletes copy/move — allocate as array.
    auto nodes = std::make_unique<IntNode[]>(N);
    for (int i = 0; i < N; ++i) nodes[i].value = i;

    for (int i = 0; i < N; ++i) q.push(&nodes[i]);

    for (int i = 0; i < N; ++i) {
        IntNode* p = q.pop();
        CHECK(p != nullptr,  "not null");
        CHECK(p->value == i, "correct order");
    }
    CHECK(q.pop() == nullptr, "empty after N pops");
}

static void test_reuse_nodes() {
    // Nodes may be re-enqueued after popping (pool allocator pattern).
    // Must reset next to nullptr before reuse.
    MpscQueue<IntNode> q;
    IntNode n(0);
    for (int i = 0; i < 100; ++i) {
        n.value = i;
        n.next.store(nullptr, std::memory_order_relaxed);
        q.push(&n);
        IntNode* p = q.pop();
        CHECK(p == &n,       "reuse: correct node");
        CHECK(p->value == i, "reuse: correct value");
    }
}

static void test_different_types() {
    struct TaggedNode : MpscNode {
        int id; double weight;
        TaggedNode(int i, double w) : id(i), weight(w) {}
    };
    MpscQueue<TaggedNode> q;
    TaggedNode a(1, 3.14), b(2, 2.71);
    q.push(&a); q.push(&b);
    auto* p1 = q.pop(); auto* p2 = q.pop();
    CHECK(p1 && p1->id == 1 && p1->weight == 3.14, "tagged node 1");
    CHECK(p2 && p2->id == 2 && p2->weight == 2.71, "tagged node 2");
}

int main() {
    std::printf("=== MPSC Queue Correctness Tests ===\n\n");
    test_empty_pop();
    test_single_push_pop();
    test_fifo_order();
    test_interleaved_push_pop();
    test_many_elements();
    test_reuse_nodes();
    test_different_types();
    std::printf("\nResults: %d passed, %d failed\n", passed, failed);
    return failed > 0 ? 1 : 0;
}
