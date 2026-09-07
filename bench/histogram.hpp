#pragma once
// histogram.hpp — fixed-bucket latency histogram shared by all benchmarks.
//
// Why not a sorted std::vector<uint64_t> of samples (as the older
// bench_mpsc.cpp ping-pong benchmark does)?  Sustained multi-hour or
// high-rate runs can generate hundreds of millions of samples; sorting
// that is slow and the vector itself can dominate memory bandwidth,
// perturbing the very latencies being measured.  A fixed-bucket
// histogram is O(1) to record and O(buckets) to report, with a small,
// constant memory footprint recorded outside the timed region.
//
// Two-tier bucketing: 1ns resolution up to kFineBuckets, then
// logarithmic (power-of-two) buckets beyond that.  Sub-microsecond
// latencies are the common case for this queue and deserve 1ns
// resolution; occasional multi-microsecond outliers (scheduler
// preemption, page fault, noisy-neighbour jitter on a shared host)
// still land somewhere sane instead of being clipped into one
// overflow bucket, which would hide exactly the tail behaviour a
// latency-sensitive reader cares about most.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <x86intrin.h>

namespace bench {

class LatencyHistogram {
public:
    static constexpr int kFineBuckets = 4096;   // 1ns each: [0, 4096) ns
    static constexpr int kLogBuckets  = 32;     // powers of two beyond that
    static constexpr int kNumBuckets  = kFineBuckets + kLogBuckets;

    void record(uint64_t ns) noexcept {
        ++total_;
        if (ns < uint64_t(kFineBuckets)) {
            ++fine_[ns];
        } else {
            int log2 = 63 - __builtin_clzll(ns);           // ns >= 4096 so log2 >= 12
            int bucket = std::min(log2 - 12, kLogBuckets - 1);
            ++coarse_[bucket];
        }
        max_ = std::max(max_, ns);
        min_ = std::min(min_, ns);
    }

    uint64_t count() const noexcept { return total_; }
    uint64_t max_ns() const noexcept { return total_ ? max_ : 0; }
    uint64_t min_ns() const noexcept { return total_ ? min_ : 0; }

    // Returns the smallest bucket boundary b such that at least `p` of
    // samples are <= b. Approximate for coarse buckets (returns the
    // bucket's lower edge, i.e. a slight underestimate of the true value).
    uint64_t percentile(double p) const noexcept {
        if (total_ == 0) return 0;
        const uint64_t target = uint64_t(p * double(total_));
        uint64_t cum = 0;
        for (int i = 0; i < kFineBuckets; ++i) {
            cum += fine_[i];
            if (cum >= target) return uint64_t(i);
        }
        for (int i = 0; i < kLogBuckets; ++i) {
            cum += coarse_[i];
            if (cum >= target) return uint64_t(1) << (i + 12);
        }
        return max_;
    }

    void print(const char* label = "") const {
        if (total_ == 0) { std::printf("%s: no samples\n", label); return; }
        std::printf("%s  n=%llu  p50=%llu ns  p90=%llu ns  p99=%llu ns"
                    "  p99.9=%llu ns  max=%llu ns\n",
                    label, (unsigned long long)total_,
                    (unsigned long long)percentile(0.50),
                    (unsigned long long)percentile(0.90),
                    (unsigned long long)percentile(0.99),
                    (unsigned long long)percentile(0.999),
                    (unsigned long long)max_ns());
    }

    // Merge another histogram's counts into this one (for combining
    // per-producer histograms into a single sustained-run view).
    void merge(const LatencyHistogram& o) noexcept {
        for (int i = 0; i < kFineBuckets; ++i) fine_[i] += o.fine_[i];
        for (int i = 0; i < kLogBuckets;  ++i) coarse_[i] += o.coarse_[i];
        total_ += o.total_;
        max_ = std::max(max_, o.max_);
        min_ = std::min(min_, o.min_);
    }

private:
    uint64_t fine_[kFineBuckets]   = {};
    uint64_t coarse_[kLogBuckets]  = {};
    uint64_t total_ = 0;
    uint64_t max_ = 0;
    uint64_t min_ = UINT64_MAX;
};

// ── TSC timer (shared) ───────────────────────────────────────────────────────

struct TscClock {
    double ns_per_cycle = 1.0;

    void calibrate() {
        using Clock = std::chrono::steady_clock;
        const auto t0 = Clock::now();
        const uint64_t c0 = __rdtsc();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        const uint64_t c1 = __rdtsc();
        const auto t1 = Clock::now();
        ns_per_cycle =
            double(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count())
            / double(c1 - c0);
    }

    inline uint64_t now_ns() const noexcept {
        unsigned aux;
        return uint64_t(double(__rdtscp(&aux)) * ns_per_cycle);
    }
};

// ── Escalating backoff for spin-wait loops ───────────────────────────────────
//
// Pure `pause()`-only spin loops assume the waited-on thread is running
// concurrently on another core and will make progress shortly. That
// assumption fails when producers/consumers are oversubscribed onto fewer
// hardware threads than the benchmark spawns (shared CI runners,
// virtualised single-vCPU hosts, or simply P > core count). Without a
// fallback, a thread can spin forever waiting on another thread that the
// scheduler never runs, because a plain PAUSE never signals the scheduler.
//
// This is the same escalation a production hot-path retry loop should use
// (see queue.hpp's "recommended retry pattern"): spin first (cheap, low
// latency when the wait is genuinely short), then yield (let the scheduler
// run someone else if we're not making progress), then sleep briefly (stop
// burning a full core if we're badly oversubscribed or truly idle).
//
// On a dedicated/isolated core with producers <= physical cores, `wait()`
// only ever executes the pure-spin path in practice, so this does not
// change steady-state latency numbers gathered under the intended
// methodology (see BENCHMARK_RESULTS.md) — it only prevents livelock when
// that precondition isn't met.
struct Backoff {
    int spins = 0;

    void wait() noexcept {
        if (spins < 1000) {
            __builtin_ia32_pause();
        } else if (spins < 1100) {
            std::this_thread::yield();
        } else {
            std::this_thread::sleep_for(std::chrono::microseconds(20));
        }
        ++spins;
    }

    void reset() noexcept { spins = 0; }
};

} // namespace bench
