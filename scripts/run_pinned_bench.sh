#!/usr/bin/env bash
# scripts/run_pinned_bench.sh — run the benchmarks with the methodology
# needed to make the numbers mean something.
#
# WHY THIS EXISTS
# ────────────────
# "Ran it in a container and it printed a big number" is not a benchmark
# result anyone should trust, and it shouldn't be trusted here either.
# This script does what it takes to get a number worth publishing:
#
#   1. Checks whether the target cores are actually isolated from the
#      general scheduler (isolcpus / nohz_full) and warns loudly if not.
#   2. Pins each benchmark to specific cores with taskset.
#   3. Runs under SCHED_FIFO (chrt -f) so the benchmark thread isn't
#      pre-empted by ordinary SCHED_OTHER tasks mid-measurement.
#   4. Sets the CPU frequency governor to `performance` on the target
#      cores (if permitted) so results aren't confounded by frequency
#      scaling ramping up/down mid-run, and reports current governor and
#      Turbo Boost state either way so the reader knows what they're
#      looking at.
#   5. Runs each benchmark N times (default 5) and reports the spread
#      across runs, not just one number — a single run tells you nothing
#      about whether what you measured was signal or a scheduling fluke.
#
# WHAT THIS SCRIPT CANNOT DO
# ───────────────────────────
# It cannot isolate cores that were never configured with `isolcpus=` /
# `nohz_full=` on the kernel command line — that requires a reboot. It
# also cannot do anything useful inside a shared/virtualised CI runner or
# a container without pinned, dedicated vCPUs: taskset and chrt still
# "work" there in the sense of not erroring, but they pin threads to
# vCPUs the hypervisor is free to schedule however it likes, which
# defeats the entire point. If `check_isolation` below reports cores are
# NOT isolated, treat any numbers this script produces as a smoke test,
# not a result — see BENCHMARK_RESULTS.md for how the numbers in this
# repo are labelled accordingly.
#
# USAGE
#   ./scripts/run_pinned_bench.sh <build_dir> <core_list> [runs]
#
#   ./scripts/run_pinned_bench.sh build 4,5,6,7        # 5 runs (default)
#   ./scripts/run_pinned_bench.sh build 4,5,6,7 10      # 10 runs
#
# <core_list> is a comma-separated taskset core list. Use at least 2
# cores for bench_t2t_queue (decode thread + strategy thread each want
# their own core); bench_stress/bench_comparison scale producers up to 8,
# so 8+ cores gives every producer count its own core.

set -euo pipefail

BUILD_DIR="${1:?usage: run_pinned_bench.sh <build_dir> <core_list> [runs]}"
CORES="${2:?usage: run_pinned_bench.sh <build_dir> <core_list> [runs]}"
RUNS="${3:-5}"

# ── Environment report ───────────────────────────────────────────────────────

echo "=== Environment ==="
echo "kernel        : $(uname -r)"
echo "cmdline       : $(cat /proc/cmdline 2>/dev/null || echo 'unavailable')"
echo "cpu model     : $(grep -m1 'model name' /proc/cpuinfo 2>/dev/null | cut -d: -f2 | xargs || echo 'unavailable')"
echo "nproc         : $(nproc)"
echo

check_isolation() {
    local isolated
    isolated="$(cat /sys/devices/system/cpu/isolated 2>/dev/null || echo '')"
    if [[ -z "$isolated" ]]; then
        echo "WARNING: /sys/devices/system/cpu/isolated is empty."
        echo "         No cores are kernel-isolated (isolcpus=). taskset/chrt below"
        echo "         will still run, but the target cores remain visible to the"
        echo "         general scheduler, which can and will preempt this benchmark"
        echo "         for unrelated work. Numbers from this run are a SMOKE TEST,"
        echo "         not a result to publish. To fix: add isolcpus=${CORES} and"
        echo "         nohz_full=${CORES} to the kernel command line and reboot."
    else
        echo "Isolated cores (isolcpus): $isolated"
        for c in ${CORES//,/ }; do
            if [[ ",$isolated," != *",$c,"* ]]; then
                echo "WARNING: core $c is not in the isolated set ($isolated)."
            fi
        done
    fi
    echo
}
check_isolation

report_governor() {
    for c in ${CORES//,/ }; do
        local gov_file="/sys/devices/system/cpu/cpu${c}/cpufreq/scaling_governor"
        if [[ -r "$gov_file" ]]; then
            echo "core $c governor: $(cat "$gov_file")"
        fi
    done
    local turbo="/sys/devices/system/cpu/intel_pstate/no_turbo"
    if [[ -r "$turbo" ]]; then
        echo "Turbo Boost disabled (no_turbo=1)? $(cat "$turbo")"
    fi
    echo
}

set_performance_governor() {
    if [[ "$(id -u)" -ne 0 ]]; then
        echo "Not running as root — skipping governor/turbo changes."
        echo "For best repeatability, re-run as root or set these manually:"
        echo "  for c in ${CORES//,/ }; do"
        echo "    echo performance > /sys/devices/system/cpu/cpu\$c/cpufreq/scaling_governor"
        echo "  done"
        echo "  echo 1 > /sys/devices/system/cpu/intel_pstate/no_turbo   # Intel"
        echo
        return
    fi
    for c in ${CORES//,/ }; do
        local gov_file="/sys/devices/system/cpu/cpu${c}/cpufreq/scaling_governor"
        [[ -w "$gov_file" ]] && echo performance > "$gov_file" || true
    done
    local turbo="/sys/devices/system/cpu/intel_pstate/no_turbo"
    [[ -w "$turbo" ]] && echo 1 > "$turbo" || true
}

set_performance_governor
echo "=== Governor / Turbo state (after attempted tuning) ==="
report_governor

# ── Run helper: N repetitions, pinned + SCHED_FIFO ───────────────────────────

run_pinned() {
    local exe="$1"; shift
    local label="$1"; shift
    echo "=== $label ($exe $*) — $RUNS run(s), pinned to cores $CORES, SCHED_FIFO 80 ==="
    for i in $(seq 1 "$RUNS"); do
        echo "--- run $i/$RUNS ---"
        if [[ "$(id -u)" -eq 0 ]]; then
            taskset -c "$CORES" chrt -f 80 "$BUILD_DIR/$exe" "$@"
        else
            echo "(not root — running without chrt SCHED_FIFO; taskset only)"
            taskset -c "$CORES" "$BUILD_DIR/$exe" "$@"
        fi
        echo
    done
}

run_pinned bench_mpsc        "MPSC vs mutex throughput + ping-pong latency"
run_pinned bench_batch       "Batch push vs single push"
run_pinned bench_stress      "Sustained multi-producer contention"        3
run_pinned bench_t2t         "Tick-to-trade, single-threaded (no queue)"
run_pinned bench_t2t_queue   "Tick-to-trade THROUGH the queue"           500000
if [[ -x "$BUILD_DIR/bench_comparison" ]]; then
    run_pinned bench_comparison "MPSC vs mutex/spinlock/boost::lockfree"  2
else
    echo "bench_comparison not built (Boost not found at configure time) — skipping."
fi

echo "=== Done ==="
echo "Paste the runs above into BENCHMARK_RESULTS.md, noting the isolation"
echo "warning status from the top of this output alongside them."
