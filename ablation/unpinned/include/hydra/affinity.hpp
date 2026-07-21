#pragma once

// ablation/unpinned/include/hydra/affinity.hpp
//
// Rejected-alternative ablation for core pinning (include/hydra/affinity.hpp):
// pin_to_core()/verify_affinity() become no-ops, so rx_thread_fn and
// matching_thread_fn run as ordinary OS-scheduled threads instead of pinned
// to RX_CORE_ID/MATCHING_CORE_ID. Deliberately does NOT touch
// run_preflight_checks() in benchmark.cpp (a separate file, not shadowed by
// this ablation) -- the system-level isolcpus/governor/SMT checks stay
// real; this ablation is specifically about whether *this process's
// threads* are pinned, not about the machine-level tuning around them (see
// docs/DESIGN.md's "Core pinning / CPU isolation" section).
//
// See spsc_queue.hpp's ablation counterpart for the include-path-shadowing
// mechanism this relies on.

namespace hydra
{

    inline void pin_to_core(int /*core_id*/)
    {
        // No-op: the thread keeps the OS-default affinity mask (all cores).
    }

    inline void verify_affinity(int /*expected_core_id*/)
    {
        // No-op: never throws, so callers proceed exactly as if pinning had
        // succeeded, even though nothing was actually pinned.
    }

} // namespace hydra
