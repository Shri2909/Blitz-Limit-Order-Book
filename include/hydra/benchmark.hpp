#pragma once

// include/hydra/benchmark.hpp
//
// This header only declares the shape of the benchmark harness --
// BenchmarkConfig and the single run_benchmark() entry point.
//
// WHY there is exactly one entry point and one config shape: this project
// used to also support a live-generation fallback (no dataset file, an
// in-process RNG stream instead) so a "quick look" number and a "reproducible"
// number could both come out of the same --benchmark flag. That meant two
// invocations of the same command could report different figures depending
// on a flag the caller might not even notice was missing, which is exactly
// the failure mode a preflight-gated benchmark exists to prevent. There is
// now exactly one way to produce a latency number from this codebase:
// replaying a committed, seeded dataset file through the real pipeline.

#include "hydra/matcher.hpp"
#include "hydra/pipeline.hpp"

#include <cstddef>
#include <string>

namespace hydra
{

    struct BenchmarkConfig
    {
        MatchingMode mode;
        std::size_t trials;
        std::string output_path;

        // Required, never empty -- run_benchmark() throws if it is. No
        // live-generation fallback exists; see the file-level WHY above.
        // No `core` field either: the producer thread is always pinned to
        // RX_CORE_ID (the same core the preflight gate actually checks) --
        // a previous version accepted an independent --core flag here,
        // which let the pinned core and the preflight-checked core silently
        // disagree.
        std::string dataset_path;

        // Gates the detailed diagnostics block (per-workload breakdown,
        // full raw percentile context, individual pool telemetry) in the
        // console report -- the default report stays concise regardless.
        bool verbose = false;

        // A dirty working tree is an INVALID-run trigger by default (see
        // run_benchmark()'s fail-closed checks) -- this explicitly opts
        // out of that gate for local iteration, and the resulting report
        // prominently marks the run as measured against a dirty tree
        // rather than silently proceeding as if it were clean.
        bool allow_dirty = false;
    };

    // The single entry point main.cpp calls for --benchmark. Owns the full
    // trial loop internally -- callers invoke this exactly once per benchmark run,
    // regardless of cfg.trials.
    void run_benchmark(const BenchmarkConfig &cfg, PipelineContext &ctx);

} // namespace hydra
