#pragma once
// mpsc/queue.hpp — Lock-free Multi-Producer Single-Consumer queue.
//
// ═══════════════════════════════════════════════════════════════════════════
// WHAT THIS IS AND IS NOT
// ═══════════════════════════════════════════════════════════════════════════
//
// This is the Dmitry Vyukov intrusive MPSC queue (2010), adapted to C++20
// atomics.  Its progress and consistency guarantees, stated precisely
// (proof/memory_model.md, "Progress and linearizability"):
//
//   • push(): wait-free — one atomic exchange and two stores, no loops —
//     PROVIDED the hardware exchange is itself wait-free: x86 LOCK XCHG
//     and AArch64 LSE SWPAL are; on ARMv8.0 without LSE the exchange is an
//     LDAXR/STLXR retry loop, so push is only lock-free there.
//   • pop(): never loops and never blocks — it returns nullptr instead.
//     But the ALGORITHM is not lock-free. A producer preempted between
//     its tail_ exchange and its link store hides every node pushed after
//     it from the consumer until that producer runs again, so overall
//     progress can depend on one suspended thread. This is inherent to
//     Vyukov's design (his own write-up says so); the window is two
//     instructions wide, which makes it rare, not impossible.
//   • NOT linearizable, for the same reason: pop() can report "empty"
//     after a push that STARTED later has already COMPLETED. What does
//     hold: per-producer FIFO, and every completed push becomes visible
//     once all pushes that started before it have completed.
//
// It is NOT:
//   • Safe for multiple consumers (MPSC, not MPMC)
//   • Suitable for types that cannot tolerate a transient inconsistent state
//     during the enqueue (the "incomplete push" window — see proof below)
//
// ═══════════════════════════════════════════════════════════════════════════
// DATA STRUCTURE
// ═══════════════════════════════════════════════════════════════════════════
//
// The queue is an intrusive linked list with a stub node:
//
//   head_ → [stub] → [node1] → [node2] → ... → [nodeN] ← tail_
//
// `tail_` always points to the last enqueued node (or the stub on empty).
// `head_` always points to the node *before* the next node to dequeue.
//
// A node is "ready to consume" when head_->next is non-null.
// The "incomplete push" window: after swap(tail_, new_node) but before
// setting prev->next = new_node, head_->next may be null even though the
// queue is non-empty.  The consumer handles this by returning nullptr and
// retrying — see the recommended retry pattern in pop()'s doc comment
// below. Node lifetime after a successful pop() has its own subtlety
// (a popped node remains the queue's sentinel until a LATER pop() returns
// another node) — documented on pop() below.
//
// ═══════════════════════════════════════════════════════════════════════════
// MEMORY ORDERING PROOF
// ═══════════════════════════════════════════════════════════════════════════
//
// We use acquire/release exclusively.  No seq_cst fences anywhere.
// This section proves that is sufficient.
//
// ── Producer (push) ────────────────────────────────────────────────────────
//
// Two writes must be ordered relative to the consumer:
//   (W1) node->next.store(nullptr, relaxed)       — initialise the node
//   (W2) node->next.store(new_node, release)       — link into the list
//         [on prev, which is the old tail_]
//
// Wait — W1 is relaxed and W2 is a release.  Is W1 guaranteed to be visible
// before W2?  Yes: within a single thread, W1 happens-before W2 by program
// order (sequenced-before).  The release on W2 ensures that all writes
// sequenced-before it (including W1) are visible to any thread that
// subsequently acquires on the same address.
//
// The consumer does:
//   (R1) next = head_->next.load(acquire)
//
// R1 acquire-loads the address written by W2 release-store.
// The C++ memory model guarantees:
//   W2 happens-before R1 (release-acquire synchronises-with)
//   W1 happens-before W2 (sequenced-before)
//   ∴ W1 happens-before R1 (transitivity of happens-before)
//
// The consumer can safely read node->next after R1.
//
// ── The tail_ swap ────────────────────────────────────────────────────────
//
//   (W3) prev = tail_.exchange(node, acq_rel)
//
// acq_rel on tail_.exchange:
//   • The release-side ensures that W1 (node->next = nullptr) is visible to
//     any thread that subsequently acquires on tail_.
//   • The acquire-side ensures that when we observe the previous tail_ value,
//     we see all writes that the previous producer made before its own
//     tail_.exchange.
//
// The acquire side is not optional. It makes the previous producer's
// `node->next = nullptr` (its W1) happen-before OUR `prev->next = node`
// (W2), so by write-write coherence our link is later in prev->next's
// modification order and cannot be overwritten by that null — which would
// silently drop our node and everything after it.
//
// acq_rel vs seq_cst on the exchange makes no difference on x86: both
// compile to the same LOCK XCHG (checked with g++ 13 -O2). The ordering
// choice only changes code for the plain stores (below).
//
// ── On x86-TSO ────────────────────────────────────────────────────────────
//
// x86 provides Total Store Order: stores become visible in one global
// order, and the ONLY reordering a thread can observe is a later load
// passing an earlier store to a different address (via the store buffer).
// Under TSO:
//   • release stores compile to plain MOV (the TSO guarantee subsumes the
//     release fence)
//   • acquire loads compile to plain MOV
//   • acq_rel exchanges compile to LOCK XCHG (which is inherently a full
//     barrier, but this is required by the atomicity of exchange, not by
//     the memory ordering)
//
// Therefore, on x86, acquire/release costs zero extra instructions vs
// relaxed for plain loads and stores.  The only cost vs relaxed is the
// LOCK prefix on exchange, which we need regardless for atomicity.
//
// seq_cst STORES compile to XCHG (GCC emits XCHG, not MOV+MFENCE);
// seq_cst loads stay plain MOV. Writing push() with every op seq_cst —
// the std::atomic default — gives three locked XCHGs instead of one.
// Measured cost: bench/bench_ordering.cpp, BENCHMARK_RESULTS.md.
//
// On AArch64 the orderings matter at the instruction level: release
// store = STLR, acquire load = LDAR, relaxed = plain STR/LDR. CI runs the
// suite on an ARM runner for that reason — x86 TSO hides most ordering
// mistakes.
//
// ── Why NOT seq_cst ───────────────────────────────────────────────────────
//
// seq_cst provides a single total order over ALL seq_cst operations in the
// program.  We do not need that.  We need only:
//   "producer's writes to a node are visible to the consumer before the
//    consumer processes that node"
//
// That is exactly what release/acquire provides: a directed happens-before
// edge from producer to consumer via the `next` pointer write/read.
// The stronger total order of seq_cst is neither needed nor useful here.
//
// ── ABA problem ───────────────────────────────────────────────────────────
//
// Classic ABA: thread A reads ptr=P, thread B pops P and reuses it, thread A
// CAS(ptr, P, new) succeeds spuriously.
//
// This queue does NOT use CAS.  The tail_ exchange is unconditional (no
// compare).  The head_ pointer is only advanced by the single consumer.
// Therefore ABA cannot occur.
//
// ═══════════════════════════════════════════════════════════════════════════
// USAGE
// ═══════════════════════════════════════════════════════════════════════════
//
// Nodes must inherit from MpscNode.  Callers manage node lifetime.
// The queue does not own nodes and does not allocate memory.
//
//   struct MyNode : MpscNode { int value; };
//
//   MpscQueue<MyNode> q;
//   MyNode n1, n2;
//   q.push(&n1);    // thread 1
//   q.push(&n2);    // thread 2 (concurrent)
//   MyNode* p = q.pop();  // consumer thread only

#include <atomic>
#include <cstddef>
#include <type_traits>

namespace mpsc {

// ── Base node ─────────────────────────────────────────────────────────────────
//
// Using a separate base avoids the diamond-inheritance problem and makes
// it explicit that the `next` field is owned by the queue, not the user.

struct MpscNode {
    std::atomic<MpscNode*> next{nullptr};

    MpscNode() noexcept = default;
    // Non-copyable: an atomic cannot be meaningfully copied.
    MpscNode(const MpscNode&)            = delete;
    MpscNode& operator=(const MpscNode&) = delete;
};

// ── MpscQueue ─────────────────────────────────────────────────────────────────

template<typename T>
    requires std::is_base_of_v<MpscNode, T>
class MpscQueue {
public:
    MpscQueue() noexcept
        : head_(&stub_), tail_(&stub_)
    {
        stub_.next.store(nullptr, std::memory_order_relaxed);
    }

    // Non-copyable, non-movable.
    MpscQueue(const MpscQueue&)            = delete;
    MpscQueue& operator=(const MpscQueue&) = delete;

    // ── push ─────────────────────────────────────────────────────────────────
    //
    // Called from any thread.  Wait-free.
    //
    // Step 1: node->next = nullptr  (relaxed: sequenced-before step 2)
    // Step 2: prev = tail_.exchange(node, acq_rel)
    //           • release: node->next=nullptr visible after any acquire on tail_
    //           • acquire: see all writes from previous tail_ holder
    // Step 3: prev->next = node  (release: publishes the new node to consumer)
    //
    // The window between step 2 and step 3 is the "incomplete push":
    // tail_ points to `node` but prev->next is still null.  The consumer
    // sees this as a momentarily empty queue and retries.

    void push(T* node) noexcept {
        node->next.store(nullptr, std::memory_order_relaxed);   // Step 1
        MpscNode* prev = tail_.exchange(node, std::memory_order_acq_rel); // Step 2
        prev->next.store(node, std::memory_order_release);      // Step 3
    }

    // ── push_chain ───────────────────────────────────────────────────────────
    //
    // Batch publish: enqueue a pre-linked chain first → ... → last with a
    // SINGLE tail_.exchange.  Called from any thread.  Wait-free.
    //
    // Contract: the caller has linked the chain via relaxed stores such that
    // following `next` from `first` reaches `last`, and no other thread
    // touches these nodes until they are consumed.  `last->next` may hold
    // anything on entry — it is overwritten here.
    //
    // Why this exists: under P producers, single push serialises every node
    // on one LOCK XCHG against the shared tail_ cache line.  For bursty
    // workloads (drain N items from a local buffer), linking locally and
    // publishing once amortises the contended operation to 1/N per node.
    // Producer-side cost drops from N contended exchanges to N relaxed
    // stores + 1 contended exchange.
    //
    // Correctness (Claim 6, mirroring Claims 1–5 in proof/memory_model.md):
    //
    //   (B1) chain-internal next stores        — relaxed, thread-local
    //   (B2) last->next.store(nullptr, relaxed)
    //   (B3) prev = tail_.exchange(last, acq_rel)
    //   (B4) prev->next.store(first, release)
    //
    //   All of B1/B2 are sequenced-before B4 in the producer.  The consumer's
    //   acquire load of prev->next (pop step 1) synchronises-with B4's
    //   release store, so by transitivity every chain-internal link (B1) and
    //   the chain terminator (B2) happen-before any consumer traversal of the
    //   chain.  The consumer never observes a partially linked interior:
    //   the entire chain is published atomically by the single edge B4.
    //   Producer FIFO within the batch follows from B1's program order.
    //   The incomplete-push window between B3 and B4 is identical in shape
    //   to the single-push window and is handled by the same pop() retry.
    //   No CAS is introduced, so the no-ABA argument (Claim 4) is unchanged.

    void push_chain(T* first, T* last) noexcept {
        last->next.store(nullptr, std::memory_order_relaxed);              // B2
        MpscNode* prev = tail_.exchange(last, std::memory_order_acq_rel);  // B3
        prev->next.store(first, std::memory_order_release);                // B4
    }

    // ── push_batch ───────────────────────────────────────────────────────────
    //
    // Convenience wrapper: links `nodes[0..n)` into a chain (relaxed,
    // thread-local — B1 above) and publishes with one exchange.
    // Precondition: n >= 1 and all pointers non-null and exclusively owned
    // by the caller until consumed.

    void push_batch(T* const* nodes, std::size_t n) noexcept {
        for (std::size_t i = 0; i + 1 < n; ++i)                            // B1
            nodes[i]->next.store(nodes[i + 1], std::memory_order_relaxed);
        push_chain(nodes[0], nodes[n - 1]);
    }

    // ── pop ──────────────────────────────────────────────────────────────────
    //
    // Called from exactly ONE thread (the consumer).
    //
    // Returns nullptr if the queue is empty or transiently in the
    // "incomplete push" window (see the file header). The caller must
    // retry on nullptr — this is not an error condition.
    //
    // head_ is the sentinel pointing *before* the first real node.
    // We advance head_ past the consumed node, leaving the consumed node
    // as the new sentinel.
    //
    // (1) next = head_->next.load(acquire)
    //     acquire: synchronises-with the producer's release-store in step 3.
    //     After this load, all of the producer's writes to `next` (and
    //     transitively all writes sequenced-before the release) are visible.
    //
    // (2) if next == nullptr: queue is empty or incomplete push → return nullptr
    //
    // (3) head_ = next  (plain store — only the consumer writes head_)
    //
    // (4) return static_cast<T*>(next)
    //
    // ── Node lifetime after pop() — the constraint that actually bites ───────
    //
    // The node returned by pop() does not simply become "owned by the
    // caller" the way a value popped off std::queue would. Internally it
    // becomes the new sentinel: head_ now points at it, and the next pop()
    // reads THIS node's `next` field. If the queue is now empty, tail_
    // also points at it, so the next push() will write its `next` field.
    //
    // The rule: a popped node `a` belongs to the queue until a LATER pop()
    // returns a DIFFERENT node. A pop() that returns nullptr does NOT
    // release `a` — head_ and possibly tail_ still point at it. (An
    // earlier version of this comment said "until the next pop(), whether
    // it returns a node or nullptr". That rule is wrong:
    // tests/test_lifetime.cpp follows it — push a, pop a, pop nullptr,
    // re-push a — and gets a->next == a, after which pop() returns `a`
    // forever.)
    //
    // Consequences:
    //   • Pools: free node k only once pop() has returned node k+1. This
    //     is what bench/bench_stress.cpp does (an earlier draft of it hit
    //     the recycle-too-early race in practice).
    //   • A drained queue keeps its last node indefinitely. Vyukov's
    //     original re-pushes the stub to release it; this version trades
    //     that for a simpler pop(). Stop all producers before freeing the
    //     final node.
    //
    // Note: the old head_ (stub or previous consumed node) is NOT freed here.
    // The caller is responsible for managing the lifetime of popped nodes,
    // subject to the one-pop-delay constraint above.
    //
    // ── Recommended retry pattern ─────────────────────────────────────────────
    //
    // A bare `while (!(p = q.pop())) ;` is correct but burns a full core
    // even when the queue is genuinely idle, and on an oversubscribed or
    // shared host (more runnable threads than cores) a pure spin can even
    // starve the producer that would otherwise complete the pending push.
    // Escalate instead — spin briefly (the incomplete-push window is a few
    // instructions wide, so this is the common case), then yield, then
    // sleep if the wait continues:
    //
    //   T* p; int spins = 0;
    //   while ((p = q.pop()) == nullptr) {
    //       if (spins < 1000)      { mpsc::cpu_relax(); ++spins; }   // mpsc/cpu_relax.hpp
    //       else if (spins < 1100) { std::this_thread::yield(); ++spins; }
    //       else                    std::this_thread::sleep_for(20us);
    //   }
    //
    // This is exactly the bench::Backoff helper in bench/histogram.hpp,
    // used throughout this repo's own benchmarks for the same reason.

    [[nodiscard]] T* pop() noexcept {
        MpscNode* head = head_.load(std::memory_order_relaxed);   // consumer-only
        MpscNode* next = head->next.load(std::memory_order_acquire); // (1)

        if (next == nullptr) return nullptr;       // (2) empty or incomplete push

        // `next` becomes the new head sentinel (see "Node lifetime" above);
        // the stub is only ever the initial sentinel and is never returned.
        head_.store(next, std::memory_order_relaxed);              // (3)
        return static_cast<T*>(next);                              // (4)
    }

    // ── approximate_empty ────────────────────────────────────────────────────
    //
    // Returns true if the queue *appears* empty from the consumer's perspective.
    // Not linearisable with respect to concurrent pushes: a producer may be
    // mid-push when this is called.

    [[nodiscard]] bool approximately_empty() const noexcept {
        const MpscNode* head = head_.load(std::memory_order_relaxed);
        return head->next.load(std::memory_order_acquire) == nullptr;
    }

private:
    // The stub node serves as the initial sentinel for head_.
    // It is never returned to the caller.
    MpscNode stub_;

    // head_ is only written by the consumer; relaxed loads by consumer are safe.
    // Producers only read it indirectly (they never touch head_).
    std::atomic<MpscNode*> head_;

    // tail_ is written by all producers (via exchange) and read by no one
    // except as part of an exchange.
    alignas(64) std::atomic<MpscNode*> tail_;
    // Separate cache line from head_ to prevent false sharing between:
    //   • the consumer (hot on head_)
    //   • the producers (hot on tail_)
};

} // namespace mpsc
