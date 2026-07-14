#pragma once

// include/hydra/benchmark.hpp
//
// This header only declares the shape of the benchmark harness --
// BenchmarkConfig and the single run_benchmark() entry point.

#include "hydra/matcher.hpp"
#include "hydra/pipeline.hpp"

#include <cstddef>
#include <optional>
#include <string>

namespace hydra
{

    struct BenchmarkConfig
    {
        MatchingMode mode;
        int core;
        std::size_t trials;
        std::string output_path;

        // Unset (default): live-generation mode.
        // Set: replay mode -- loads the file once, before the trial loop starts.
        std::optional<std::string> dataset_path;
    };

    // The single entry point main.cpp calls for --benchmark. Owns the full
    // trial loop internally -- callers invoke this exactly once per benchmark run,
    // regardless of cfg.trials.
    void run_benchmark(const BenchmarkConfig &cfg, PipelineContext &ctx);

} // namespace hydra