#pragma once
// mpsc/cpu_relax.hpp — spin-wait hint, portable across x86-64 and AArch64.
//
// x86:     PAUSE — de-pipelines the spin loop and avoids the memory-order
//          machine clear when the awaited store lands.
// AArch64: YIELD — the architectural spin hint. (Some runtimes prefer ISB
//          for a longer delay on certain cores; YIELD is the portable one.)
// Other:   no-op; correctness never depends on it.

namespace mpsc {

inline void cpu_relax() noexcept {
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    asm volatile("yield" ::: "memory");
#endif
}

} // namespace mpsc
