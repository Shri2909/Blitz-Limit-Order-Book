// CLI entry point for dataset benchmarks and the live order pipeline.

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
#include <limits>
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
        std::fprintf(
            stderr,
            "Usage: %s [--benchmark | --test] [options]\n"
            "\n"
            "  --benchmark            replay a dataset and report latency; requires\n"
            "                         --output and --dataset\n"
            "  --test                 print standalone test commands and exit\n"
            "  (neither flag)         run the live pipeline until interrupted (Ctrl+C)\n"
            "                         without reporting latency\n"
            "\n"
            "  --mode [price_time|pro_rata]  matching policy (default: price_time)\n"
            "\n"
            "Benchmark options:\n"
            "  --trials N             trial count (default: %zu; fewer than %zu\n"
            "                         trials are marked SMOKETEST)\n"
            "  --output PATH          required CSV report path\n"
            "  --dataset PATH         required dataset file; generate with blitz_gen_dataset\n"
            "  --verbose              include workload and resource diagnostics\n"
            "  --allow-dirty          allow a tree that was dirty at build time;\n"
            "                         the report records this state\n",
            prog, hydra::DEFAULT_TRIAL_COUNT, hydra::DEFAULT_TRIAL_COUNT);
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

    [[nodiscard]] std::size_t parse_size(const std::string &s, const char *flag)
    {
        if (s.empty() || s.front() == '-')
        {
            std::fprintf(stderr, "error: %s expects a non-negative integer, got '%s'\n", flag,
                         s.c_str());
            std::exit(1);
        }
        try
        {
            std::size_t consumed = 0;
            const unsigned long long v = std::stoull(s, &consumed);
            if (consumed != s.size() || v > std::numeric_limits<std::size_t>::max())
            {
                throw std::invalid_argument("trailing characters");
            }
            return static_cast<std::size_t>(v);
        }
        catch (const std::exception &)
        {
            std::fprintf(stderr, "error: %s expects a non-negative integer, got '%s'\n", flag,
                         s.c_str());
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
            std::fprintf(stderr, "error: --benchmark requires --dataset PATH -- there is no "
                                 "live-generation fallback. Generate one first:\n"
                                 "  cmake --build build --target blitz_gen_dataset\n"
                                 "  ./build/blitz_gen_dataset --seed 42 --count 110000 "
                                 "--output datasets/bench_110k.bin\n");
            std::exit(1);
        }
        if (opts.want_benchmark && opts.trials == 0)
        {
            std::fprintf(stderr, "error: --trials must be at least 1\n");
            std::exit(1);
        }

        return opts;
    }

    // The handler only sets this flag; the main thread requests the stop.
    volatile std::sig_atomic_t g_interrupted = 0;

    extern "C" void handle_sigint(int)
    {
        g_interrupted = 1;
    }

    // Heap-allocated pools, book, matcher and queue shared by live and benchmark modes.
    struct PipelineResources
    {
        std::unique_ptr<hydra::ObjectPool<hydra::Order, hydra::ORDER_POOL_SIZE>> order_pool =
            std::make_unique<hydra::ObjectPool<hydra::Order, hydra::ORDER_POOL_SIZE>>();
        std::unique_ptr<hydra::ObjectPool<hydra::Level, hydra::LEVEL_POOL_SIZE>> level_pool =
            std::make_unique<hydra::ObjectPool<hydra::Level, hydra::LEVEL_POOL_SIZE>>();
        std::unique_ptr<hydra::OrderBook> book =
            std::make_unique<hydra::OrderBook>(*order_pool, *level_pool);
        std::unique_ptr<hydra::Matcher> matcher;
        std::unique_ptr<hydra::SpscQueue<hydra::Order, hydra::SPSC_CAPACITY>> queue =
            std::make_unique<hydra::SpscQueue<hydra::Order, hydra::SPSC_CAPACITY>>();

        explicit PipelineResources(hydra::MatchingMode mode)
            : matcher(std::make_unique<hydra::Matcher>(*book, mode))
        {
        }

        [[nodiscard]] hydra::PipelineContext context()
        {
            return hydra::PipelineContext{
                .queue = *queue,
                .book = *book,
                .matcher = *matcher,
                .order_pool = *order_pool,
                .level_pool = *level_pool,
            };
        }
    };

    void run_live_pipeline(const CliOptions &opts)
    {
        // Install the handler before the blocking calibration step.
        std::stop_source stop_source;
        g_interrupted = 0;
        std::signal(SIGINT, handle_sigint);

        PipelineResources resources(opts.mode);
        hydra::PipelineContext ctx = resources.context();
        ctx.ns_per_cycle = hydra::calibrate_ns_per_cycle();

        if (g_interrupted != 0)
        {
            // A startup interrupt must not launch worker threads.
            std::fprintf(stdout, "hydra_lob: interrupted during startup, exiting.\n");
            std::signal(SIGINT, SIG_DFL);
            return;
        }

        std::fprintf(stdout,
                     "hydra_lob: running live pipeline (mode=%s, RX_CORE_ID=%d, "
                     "MATCHING_CORE_ID=%d, rx_source=synthetic) -- press Ctrl+C to stop.\n"
                     "hydra_lob: this is a manual smoke test, not a benchmark -- it prints "
                     "no latency numbers. For a real latency figure, run "
                     "'blitz_lob --benchmark --dataset <path> --output <path>' instead.\n",
                     opts.mode == hydra::MatchingMode::PRICE_TIME ? "price_time" : "pro_rata",
                     hydra::RX_CORE_ID, hydra::MATCHING_CORE_ID);

        ctx.rx_thread_startup.store(hydra::WorkerStartupState::STARTING, std::memory_order_release);
        ctx.matching_thread_startup.store(hydra::WorkerStartupState::STARTING,
                                          std::memory_order_release);
        std::thread rx_thread(hydra::rx_thread_fn, stop_source.get_token(), std::ref(ctx));
        std::thread matching_thread(hydra::matching_thread_fn, stop_source.get_token(),
                                    std::ref(ctx));

        bool worker_startup_failed = false;
        while (!stop_source.stop_requested())
        {
            if (g_interrupted != 0)
            {
                stop_source.request_stop();
                break;
            }
            const bool rx_failed = ctx.rx_thread_startup.load(std::memory_order_acquire) ==
                                   hydra::WorkerStartupState::FAILED;
            const bool matching_failed =
                ctx.matching_thread_startup.load(std::memory_order_acquire) ==
                hydra::WorkerStartupState::FAILED;
            if (rx_failed || matching_failed)
            {
                std::fprintf(stderr,
                             "hydra_lob: worker affinity startup failed; stopping the pipeline.\n");
                worker_startup_failed = true;
                stop_source.request_stop();
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }

        rx_thread.join();
        matching_thread.join();

        std::signal(SIGINT, SIG_DFL);

        std::fprintf(stdout, "hydra_lob: shutdown complete%s.\n",
                     worker_startup_failed ? " after worker startup failure" : "");
    }

    void run_benchmark_mode(const CliOptions &opts)
    {
        PipelineResources resources(opts.mode);
        // run_benchmark() calibrates the clock itself, so ns_per_cycle is left at its default.
        hydra::PipelineContext ctx = resources.context();

        hydra::BenchmarkConfig cfg{
            .mode = opts.mode,
            .trials = opts.trials,
            .output_path = opts.output_path,
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
        std::fprintf(stdout, "Build and run all standalone tests:\n"
                             "  cmake --build build -j$(nproc)\n"
                             "  ctest --test-dir build --output-on-failure\n"
                             "\n"
                             "Or build and run a single test target:\n"
                             "  cmake --build build --target blitz_lob_test_phase6_matcher\n"
                             "  ./build/blitz_lob_test_phase6_matcher\n"
                             "See CMakeLists.txt for the complete target list.\n");
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
