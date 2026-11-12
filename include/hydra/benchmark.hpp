#pragma once

// Dataset replay benchmark configuration and entry point.
// See METHODOLOGY.md for timing boundaries and validity requirements.

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

        // Required dataset file. run_benchmark() rejects an empty path.
        std::string dataset_path;

        // Include workload, percentile, and pool diagnostics in the console report.
        bool verbose = false;

        // Allow a tree that was dirty at build time; the report records that state.
        bool allow_dirty = false;
    };

    // Runs all configured trials and writes the benchmark reports.
    void run_benchmark(const BenchmarkConfig &cfg, PipelineContext &ctx);

} // namespace hydra
