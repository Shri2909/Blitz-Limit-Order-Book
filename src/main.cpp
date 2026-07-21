// src/main.cpp
//
// The single production entry point. This is the one place every hot-path
// dependency gets wired together by reference (ObjectPools, OrderBook,
// Matcher, SpscQueue, PipelineContext) -- no hidden globals anywhere else
// in the codebase. Parses a minimal hand-rolled CLI (no external
// dependency, matching tools/gen_dataset.cpp's own style) and dispatches to
// either the benchmark harness (--benchmark, Phase 8) or the live
// RX->SPSC->matching pipeline.

#include "hydra/affinity.hpp"
#include "hydra/benchmark.hpp"
#include "hydra/clock.hpp"
#include "hydra/config.hpp"
#include "hydra/matcher.hpp"
#include "hydra/object_pool.hpp"
#include "hydra/order_book.hpp"
#include "hydra/pipeline.hpp"
#include "hydra/spsc_queue.hpp"
#include "hydra/types.hpp"

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <thread>

namespace
{

    void print_usage(const char *prog)
    {
        std::fprintf(stderr,
                     "Usage: %s [--benchmark | --test] [options]\n"
                     "\n"
                     "  --benchmark            run THE latency benchmark (Phase 8) and exit;\n"
                     "                         requires --output and --dataset. This is the\n"
                     "                         only code path in this project that produces a\n"
                     "                         latency number -- there is no live-generation\n"
                     "                         fallback and no other command to run instead.\n"
                     "  --test                 print how to run the standalone test binaries\n"
                     "                         and exit (tests are a separate target, never\n"
                     "                         linked into this binary -- see Phase 9)\n"
                     "  (neither flag)         run the live RX -> SPSC -> matching pipeline\n"
                     "                         until interrupted (Ctrl+C). This is a manual\n"
                     "                         smoke test -- it prints no latency numbers and\n"
                     "                         is never a substitute for --benchmark.\n"
                     "\n"
                     "  --mode [price_time|pro_rata]  matching mode (default: price_time, the\n"
                     "                         canonical claim; pro_rata is a secondary\n"
                     "                         comparison run, not an interchangeable result)\n"
                     "  --trials N             benchmark-only: trial count (default: %zu;\n"
                     "                         below %zu is marked SMOKETEST in the output\n"
                     "                         and should not be quoted as a result)\n"
                     "  --output PATH          benchmark-only, required: CSV output path\n"
                     "  --dataset PATH         benchmark-only, required: replay this dataset\n"
                     "                         file (generate one with blitz_gen_dataset)\n"
                     "  --verbose              benchmark-only: print the detailed diagnostics\n"
                     "                         block (per-workload breakdown, pool telemetry)\n"
                     "                         in addition to the concise default report\n"
                     "  --allow-dirty          benchmark-only: don't fail closed on a dirty\n"
                     "                         working tree -- the report still marks the run\n"
                     "                         as measured against a dirty tree\n"
                     "\n"
                     "  --xdp-iface IFACE      live-pipeline-only: receive real packets over\n"
                     "                         AF_XDP on this interface instead of the\n"
                     "                         synthetic RNG generator (requires a build\n"
                     "                         with -DENABLE_AFXDP=ON; see net/xdp_prog.bpf.c)\n"
                     "  --xdp-queue N          AF_XDP RX queue index to bind (default: %u)\n"
                     "  --xdp-prog PATH        compiled XDP program to load (default: %s)\n"
                     "  --xdp-attach-mode [native|skb|hw]  XDP attach mode (default: %s --\n"
                     "                         veth test interfaces only support skb/generic)\n",
                     prog, hydra::DEFAULT_TRIAL_COUNT, hydra::DEFAULT_TRIAL_COUNT,
                     hydra::config::AFXDP_DEFAULT_QUEUE_ID, "net/xdp_prog.o", "skb");
    }

    bool match_flag(int argc, char **argv, int &i, const char *flag, std::string &out_value)
    {
        if (std::strcmp(argv[i], flag) != 0)
        {
            return false;
        }
        if (i + 1 >= argc)
        {
            std::fprintf(stderr, "error: %s requires a value\n", flag);
            std::exit(1);
        }
        out_value = argv[++i];
        return true;
    }

    [[nodiscard]] hydra::MatchingMode parse_mode(const std::string &s)
    {
        if (s == "price_time")
        {
            return hydra::MatchingMode::PRICE_TIME;
        }
        if (s == "pro_rata")
        {
            return hydra::MatchingMode::PRO_RATA;
        }
        std::fprintf(stderr, "error: --mode expects 'price_time' or 'pro_rata', got '%s'\n",
                     s.c_str());
        std::exit(1);
    }

#ifdef ENABLE_AFXDP
    [[nodiscard]] xdp_attach_mode parse_attach_mode(const std::string &s)
    {
        if (s == "native")
        {
            return XDP_MODE_NATIVE;
        }
        if (s == "skb")
        {
            return XDP_MODE_SKB;
        }
        if (s == "hw")
        {
            return XDP_MODE_HW;
        }
        std::fprintf(stderr,
                     "error: --xdp-attach-mode expects 'native', 'skb', or 'hw', got '%s'\n",
                     s.c_str());
        std::exit(1);
    }
#endif

    [[nodiscard]] std::size_t parse_size(const std::string &s, const char *flag)
    {
        try
        {
            std::size_t consumed = 0;
            const unsigned long long v = std::stoull(s, &consumed);
            if (consumed != s.size())
            {
                throw std::invalid_argument("trailing characters");
            }
            return static_cast<std::size_t>(v);
        }
        catch (const std::exception &)
        {
            std::fprintf(stderr, "error: %s expects a non-negative integer, got '%s'\n",
                         flag, s.c_str());
            std::exit(1);
        }
    }

    struct CliOptions
    {
        bool want_benchmark = false;
        bool want_test = false;
        hydra::MatchingMode mode = hydra::MatchingMode::PRICE_TIME;
        std::size_t trials = hydra::DEFAULT_TRIAL_COUNT;
        std::string output_path;
        std::optional<std::string> dataset_path;
        bool verbose = false;
        bool allow_dirty = false;

        // Live-pipeline-only: unset xdp_iface means "use the synthetic
        // rx_thread_fn generator" regardless of build configuration. These
        // stay plain std::string/uint32_t (not the ENABLE_AFXDP-only
        // xdp_attach_mode enum) so CliOptions itself compiles identically
        // in both build configurations -- only run_live_pipeline's use of
        // them is #ifdef-guarded.
        std::optional<std::string> xdp_iface;
        uint32_t xdp_queue = hydra::config::AFXDP_DEFAULT_QUEUE_ID;
        std::string xdp_prog_path = "net/xdp_prog.o";
        std::string xdp_attach_mode = "skb";
    };

    [[nodiscard]] CliOptions parse_cli(int argc, char **argv)
    {
        CliOptions opts;

        for (int i = 1; i < argc; ++i)
        {
            std::string val;
            if (std::strcmp(argv[i], "--benchmark") == 0)
            {
                opts.want_benchmark = true;
                continue;
            }
            if (std::strcmp(argv[i], "--test") == 0)
            {
                opts.want_test = true;
                continue;
            }
            if (match_flag(argc, argv, i, "--mode", val))
            {
                opts.mode = parse_mode(val);
                continue;
            }
            if (match_flag(argc, argv, i, "--trials", val))
            {
                opts.trials = parse_size(val, "--trials");
                continue;
            }
            if (match_flag(argc, argv, i, "--output", val))
            {
                opts.output_path = val;
                continue;
            }
            if (match_flag(argc, argv, i, "--dataset", val))
            {
                // Forwarded into BenchmarkConfig::dataset_path unchanged --
                // file loading/validation happens inside run_benchmark()
                // (Phase 8), not here. Required alongside --benchmark; there
                // is no live-generation fallback.
                opts.dataset_path = val;
                continue;
            }
            if (std::strcmp(argv[i], "--verbose") == 0)
            {
                opts.verbose = true;
                continue;
            }
            if (std::strcmp(argv[i], "--allow-dirty") == 0)
            {
                opts.allow_dirty = true;
                continue;
            }
            if (match_flag(argc, argv, i, "--xdp-iface", val))
            {
                opts.xdp_iface = val;
                continue;
            }
            if (match_flag(argc, argv, i, "--xdp-queue", val))
            {
                opts.xdp_queue = static_cast<uint32_t>(parse_size(val, "--xdp-queue"));
                continue;
            }
            if (match_flag(argc, argv, i, "--xdp-prog", val))
            {
                opts.xdp_prog_path = val;
                continue;
            }
            if (match_flag(argc, argv, i, "--xdp-attach-mode", val))
            {
                // Validated for real inside run_live_pipeline (only
                // ENABLE_AFXDP builds have the xdp_attach_mode enum to
                // validate against); stored as a plain string here so
                // parse_cli() itself needs no #ifdef.
                opts.xdp_attach_mode = val;
                continue;
            }
            if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0)
            {
                print_usage(argv[0]);
                std::exit(0);
            }
            std::fprintf(stderr, "error: unknown flag '%s'\n", argv[i]);
            print_usage(argv[0]);
            std::exit(1);
        }

        if (opts.want_benchmark && opts.want_test)
        {
            std::fprintf(stderr, "error: --benchmark and --test are mutually exclusive\n");
            std::exit(1);
        }
        if (opts.want_benchmark && opts.output_path.empty())
        {
            std::fprintf(stderr, "error: --benchmark requires --output PATH\n");
            std::exit(1);
        }
        if (opts.want_benchmark && !opts.dataset_path.has_value())
        {
            std::fprintf(stderr,
                         "error: --benchmark requires --dataset PATH -- there is no "
                         "live-generation fallback. Generate one first:\n"
                         "  cmake --build build --target blitz_gen_dataset\n"
                         "  ./build/blitz_gen_dataset --seed 42 --count 110000 "
                         "--output datasets/bench_110k.bin\n");
            std::exit(1);
        }

        return opts;
    }

    // WHY a raw pointer, not e.g. an atomic<stop_source*>: main() constructs
    // exactly one stop_source on the stack, and its address is stable for
    // the entire cooperative-shutdown window this handler can fire during;
    // the handler only ever reads it, never owns or outlives it.
    // WHY calling request_stop() directly from the handler (rather than the
    // more conservative "set a volatile sig_atomic_t flag, let main()'s
    // poll loop call request_stop()" pattern): this is the literal, widely-
    // used simplification for this class of scaffold -- SIGINT arrives
    // asynchronously to the main thread's quiescent poll loop below, not
    // concurrently with the stop_source's own construction/registration, so
    // the theoretical async-signal-safety gap here is a known, accepted
    // tradeoff, not an oversight.
    std::stop_source *g_stop_source = nullptr;

    extern "C" void handle_sigint(int)
    {
        if (g_stop_source != nullptr)
        {
            g_stop_source->request_stop();
        }
    }

    void run_live_pipeline(const CliOptions &opts)
    {
        const hydra::MatchingMode mode = opts.mode;

#ifndef ENABLE_AFXDP
        // Caught here, before any pool/thread setup, rather than letting an
        // #ifdef'd-out afxdp_rx_thread_fn simply not exist -- a build
        // without AF_XDP support must say so clearly, not silently fall
        // back to the synthetic generator the caller didn't ask for.
        if (opts.xdp_iface)
        {
            std::fprintf(stderr,
                         "error: --xdp-iface requires a build with -DENABLE_AFXDP=ON\n");
            std::exit(1);
        }
#endif

        // Registered before any blocking startup work (below,
        // calibrate_ns_per_cycle() alone blocks for ~2s) -- a SIGINT that
        // arrives during that window must still be caught cooperatively,
        // not fall through to the default terminate-the-process
        // disposition just because the handler wasn't wired up yet.
        std::stop_source stop_source;
        g_stop_source = &stop_source;
        std::signal(SIGINT, handle_sigint);

        auto order_pool =
            std::make_unique<hydra::ObjectPool<hydra::Order, hydra::ORDER_POOL_SIZE>>();
        auto level_pool =
            std::make_unique<hydra::ObjectPool<hydra::Level, hydra::LEVEL_POOL_SIZE>>();
        auto book = std::make_unique<hydra::OrderBook>(*order_pool, *level_pool);
        auto matcher = std::make_unique<hydra::Matcher>(*book, mode);
        auto queue = std::make_unique<hydra::SpscQueue<hydra::Order, hydra::SPSC_CAPACITY>>();

        hydra::PipelineContext ctx{
            .queue = *queue,
            .book = *book,
            .matcher = *matcher,
            .order_pool = *order_pool,
            .level_pool = *level_pool,
            .ns_per_cycle = hydra::calibrate_ns_per_cycle(),
        };

        if (stop_source.stop_requested())
        {
            // Interrupted during startup/calibration, before either thread
            // was spawned -- nothing to join, just exit cleanly rather than
            // starting a pipeline the caller already asked to stop.
            std::fprintf(stdout, "hydra_lob: interrupted during startup, exiting.\n");
            g_stop_source = nullptr;
            std::signal(SIGINT, SIG_DFL);
            return;
        }

        std::fprintf(stdout,
                     "hydra_lob: running live pipeline (mode=%s, RX_CORE_ID=%d, "
                     "MATCHING_CORE_ID=%d, rx_source=%s) -- press Ctrl+C to stop.\n"
                     "hydra_lob: this is a manual smoke test, not a benchmark -- it prints "
                     "no latency numbers. For a real latency figure, run "
                     "'blitz_lob --benchmark --dataset <path> --output <path>' instead.\n",
                     mode == hydra::MatchingMode::PRICE_TIME ? "price_time" : "pro_rata",
                     hydra::RX_CORE_ID, hydra::MATCHING_CORE_ID,
                     opts.xdp_iface ? opts.xdp_iface->c_str() : "synthetic");

        // std::thread, not std::jthread: rx_thread_fn/matching_thread_fn
        // must both observe the SAME external stop_source, so one
        // request_stop() call (from the SIGINT handler above) stops both --
        // but std::jthread's automatic stop_token injection always supplies
        // its OWN internal stop_source, with no way for a caller to
        // substitute an external one in its place. Passing
        // stop_source.get_token() as a plain constructor argument (which
        // std::thread does with no special-casing at all) is what actually
        // shares one source across both threads.
        std::thread rx_thread;
#ifdef ENABLE_AFXDP
        if (opts.xdp_iface)
        {
            hydra::AfxdpConfig xdp_cfg{
                .ifname = *opts.xdp_iface,
                .queue_id = opts.xdp_queue,
                .bpf_prog_path = opts.xdp_prog_path,
                .attach_mode = parse_attach_mode(opts.xdp_attach_mode),
            };
            rx_thread = std::thread(hydra::afxdp_rx_thread_fn, stop_source.get_token(),
                                    std::ref(ctx), xdp_cfg);
        }
        else
        {
            rx_thread = std::thread(hydra::rx_thread_fn, stop_source.get_token(), std::ref(ctx));
        }
#else
        rx_thread = std::thread(hydra::rx_thread_fn, stop_source.get_token(), std::ref(ctx));
#endif
        std::thread matching_thread(hydra::matching_thread_fn, stop_source.get_token(),
                                    std::ref(ctx));

        // Deliberately no latency reporting here. This mode exists to prove
        // the pipeline runs, not to produce a number. There is exactly one
        // command in this project that produces a latency number:
        // --benchmark.
        while (!stop_source.stop_requested())
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }

        rx_thread.join();
        matching_thread.join();

        g_stop_source = nullptr;
        std::signal(SIGINT, SIG_DFL);

        std::fprintf(stdout, "hydra_lob: shutdown complete.\n");
    }

    void run_benchmark_mode(const CliOptions &opts)
    {
        auto order_pool =
            std::make_unique<hydra::ObjectPool<hydra::Order, hydra::ORDER_POOL_SIZE>>();
        auto level_pool =
            std::make_unique<hydra::ObjectPool<hydra::Level, hydra::LEVEL_POOL_SIZE>>();
        auto book = std::make_unique<hydra::OrderBook>(*order_pool, *level_pool);
        auto matcher = std::make_unique<hydra::Matcher>(*book, opts.mode);
        auto queue = std::make_unique<hydra::SpscQueue<hydra::Order, hydra::SPSC_CAPACITY>>();

        // ns_per_cycle is deliberately left at its default here, not
        // calibrated -- run_benchmark() calibrates once internally and
        // assigns ctx.ns_per_cycle itself before spawning matching_thread_fn
        // (see benchmark.cpp); calibrating twice would just throw away ~2s
        // on a redundant measurement.
        hydra::PipelineContext ctx{
            .queue = *queue,
            .book = *book,
            .matcher = *matcher,
            .order_pool = *order_pool,
            .level_pool = *level_pool,
        };

        hydra::BenchmarkConfig cfg{
            .mode = opts.mode,
            .trials = opts.trials,
            .output_path = opts.output_path,
            // opts.dataset_path is guaranteed set here -- parse_cli() exits
            // with an error before reaching this point otherwise.
            .dataset_path = *opts.dataset_path,
            .verbose = opts.verbose,
            .allow_dirty = opts.allow_dirty,
        };

        hydra::run_benchmark(cfg, ctx);
    }

} // namespace

int main(int argc, char **argv)
{
    const CliOptions opts = parse_cli(argc, argv);

    if (opts.want_test)
    {
        std::fprintf(stdout,
                     "Tests are a separate binary (Phase 9's \"standalone target, no GTest "
                     "dependency\" design) -- this binary never links test code. Run:\n"
                     "  cmake --build build --target blitz_lob_tests\n"
                     "  ./build/blitz_lob_tests\n"
                     "\n"
                     "Per-phase exit-condition tests (Phases 1-9) are separate targets too,\n"
                     "e.g. blitz_lob_test_phase6_matcher -- see CMakeLists.txt's per-phase\n"
                     "target block for the full list.\n");
        return 0;
    }

    try
    {
        if (opts.want_benchmark)
        {
            run_benchmark_mode(opts);
        }
        else
        {
            run_live_pipeline(opts);
        }
    }
    catch (const std::runtime_error &e)
    {
        std::fprintf(stderr, "fatal: %s\n", e.what());
        return 1;
    }

    return 0;
}
