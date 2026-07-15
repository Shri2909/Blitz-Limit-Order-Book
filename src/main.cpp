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
#include "hydra/histogram.hpp"
#include "hydra/matcher.hpp"
#include "hydra/object_pool.hpp"
#include "hydra/order_book.hpp"
#include "hydra/pipeline.hpp"
#include "hydra/spsc_queue.hpp"
#include "hydra/types.hpp"

#include <algorithm>
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
#include <vector>

namespace
{

    void print_usage(const char *prog)
    {
        std::fprintf(stderr,
                     "Usage: %s [--benchmark | --test] [options]\n"
                     "\n"
                     "  --benchmark            run the latency benchmark harness (Phase 8)\n"
                     "                         and exit; requires --output\n"
                     "  --test                 print how to run the standalone test binaries\n"
                     "                         and exit (tests are a separate target, never\n"
                     "                         linked into this binary -- see Phase 9)\n"
                     "  (neither flag)         run the live RX -> SPSC -> matching pipeline\n"
                     "                         until interrupted (Ctrl+C)\n"
                     "\n"
                     "  --mode [price_time|pro_rata]  matching mode (default: price_time)\n"
                     "  --core N               benchmark-only: core the benchmark harness's\n"
                     "                         own thread pins to (default: %d, RX_CORE_ID --\n"
                     "                         idle during --benchmark since rx_thread_fn isn't\n"
                     "                         spawned in that mode)\n"
                     "  --trials N             benchmark-only: trial count (default: %zu)\n"
                     "  --output PATH          benchmark-only, required: CSV output path\n"
                     "  --dataset PATH         benchmark-only, optional: replay this dataset\n"
                     "                         file instead of live-generating one\n"
                     "\n"
                     "  --xdp-iface IFACE      live-pipeline-only: receive real packets over\n"
                     "                         AF_XDP on this interface instead of the\n"
                     "                         synthetic RNG generator (requires a build\n"
                     "                         with -DENABLE_AFXDP=ON; see net/xdp_prog.bpf.c)\n"
                     "  --xdp-queue N          AF_XDP RX queue index to bind (default: %u)\n"
                     "  --xdp-prog PATH        compiled XDP program to load (default: %s)\n"
                     "  --xdp-attach-mode [native|skb|hw]  XDP attach mode (default: %s --\n"
                     "                         veth test interfaces only support skb/generic)\n",
                     prog, hydra::RX_CORE_ID, hydra::DEFAULT_TRIAL_COUNT,
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

    [[nodiscard]] int parse_int(const std::string &s, const char *flag)
    {
        try
        {
            std::size_t consumed = 0;
            const int v = std::stoi(s, &consumed);
            if (consumed != s.size())
            {
                throw std::invalid_argument("trailing characters");
            }
            return v;
        }
        catch (const std::exception &)
        {
            std::fprintf(stderr, "error: %s expects an integer, got '%s'\n", flag, s.c_str());
            std::exit(1);
        }
    }

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
        int core = hydra::RX_CORE_ID;
        std::size_t trials = hydra::DEFAULT_TRIAL_COUNT;
        std::string output_path;
        std::optional<std::string> dataset_path;

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
            if (match_flag(argc, argv, i, "--core", val))
            {
                opts.core = parse_int(val, "--core");
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
                // (Phase 8), not here. Only meaningful alongside
                // --benchmark; harmlessly unused otherwise.
                opts.dataset_path = val;
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

        if (opts.want_benchmark && opts.output_path.empty())
        {
            std::fprintf(stderr, "error: --benchmark requires --output PATH\n");
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
        auto fill_pool =
            std::make_unique<hydra::ObjectPool<hydra::FillEvent, hydra::FILL_EVENT_POOL_SIZE>>();
        auto book = std::make_unique<hydra::OrderBook>(*order_pool, *level_pool);
        auto matcher = std::make_unique<hydra::Matcher>(*book, *fill_pool, mode);
        auto queue = std::make_unique<hydra::SpscQueue<hydra::Order, hydra::SPSC_CAPACITY>>();
        auto histogram = std::make_unique<hydra::HdrHistogram>();

        hydra::PipelineContext ctx{
            .queue = *queue,
            .book = *book,
            .matcher = *matcher,
            .order_pool = *order_pool,
            .level_pool = *level_pool,
            .fill_pool = *fill_pool,
            .histogram = *histogram,
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
                     "MATCHING_CORE_ID=%d, rx_source=%s) -- press Ctrl+C to stop\n",
                     mode == hydra::MatchingMode::PRICE_TIME ? "price_time" : "pro_rata",
                     hydra::RX_CORE_ID, hydra::MATCHING_CORE_ID,
                     opts.xdp_iface ? opts.xdp_iface->c_str() : "synthetic");

        // DIAG-TEMP: decomposed (non-blended) raw-sample capture, for
        // diagnosing which leg (queue-transit / match-time / end-to-end)
        // is actually elevated in a live run -- revert after this
        // investigation. Reuses the exact RawSampleSink mechanism
        // --benchmark already uses (src/benchmark.cpp), just installed for
        // this run's whole duration instead of a bounded per-trial window,
        // and read out once after both threads have joined below -- at
        // that point nothing can still be writing, so (unlike a periodic
        // mid-run reset would need) there is no reset-vs-concurrent-writer
        // race to guard against.
        constexpr std::size_t kDiagRawCapacity = 2'000'000;
        auto diag_raw_storage = std::make_unique<hydra::LatencySample[]>(kDiagRawCapacity);
        hydra::RawSampleSink diag_raw_sink;
        diag_raw_sink.samples = diag_raw_storage.get();
        diag_raw_sink.capacity = kDiagRawCapacity;
        ctx.raw_samples.store(&diag_raw_sink, std::memory_order_release);

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

        // Live latency reporting: ctx.histogram already receives every
        // order's queue_transit_ns/match_time_ns/end_to_end_ns from
        // matching_thread_fn regardless of RX source (synthetic or
        // AF_XDP) -- it was simply never surfaced before. HdrHistogram is
        // double-buffered specifically so a control thread can
        // swap_buffers()+query_percentile() concurrently with the hot
        // path's record() calls (see histogram.hpp); each swap finalizes
        // the samples recorded since the previous one into a queryable
        // snapshot and starts a fresh window, so these numbers cover a
        // rolling ~5s window, not a cumulative all-time distribution.
        // NOTE: this is a blended distribution across all three latency
        // fields (not the same single end-to-end-only metric the
        // preflight-gated --benchmark reports) -- useful as a live signal
        // that real traffic is flowing and roughly how fast, not a
        // substitute for --benchmark's controlled, reproducible number.
        constexpr int kPollIntervalMs = 100;
        constexpr int kPollsPerReport = 5000 / kPollIntervalMs; // ~5s
        int polls_since_report = 0;

        while (!stop_source.stop_requested())
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(kPollIntervalMs));
            if (++polls_since_report >= kPollsPerReport)
            {
                polls_since_report = 0;
                histogram->swap_buffers();
                std::fprintf(stdout,
                             "hydra_lob: live latency, last ~5s (blended queue-transit + "
                             "match-time + end-to-end): p50=%.1fns p99=%.1fns p99.9=%.1fns "
                             "p99.99=%.1fns\n",
                             histogram->query_percentile(50.0), histogram->query_percentile(99.0),
                             histogram->query_percentile(99.9), histogram->query_percentile(99.99));
            }
        }

        rx_thread.join();
        matching_thread.join();

        histogram->swap_buffers();
        std::fprintf(stdout,
                     "hydra_lob: final live latency snapshot, since last window (blended "
                     "queue-transit + match-time + end-to-end): p50=%.1fns p99=%.1fns "
                     "p99.9=%.1fns p99.99=%.1fns\n",
                     histogram->query_percentile(50.0), histogram->query_percentile(99.0),
                     histogram->query_percentile(99.9), histogram->query_percentile(99.99));

        // DIAG-TEMP: decomposed readout -- both threads above have
        // definitively stopped writing by this point (join() guarantees
        // it), so this is a plain, race-free read of everything captured
        // over the whole run.
        {
            ctx.raw_samples.store(nullptr, std::memory_order_release);

            const std::size_t n = diag_raw_sink.count();
            std::vector<uint64_t> qt, mt, ee;
            qt.reserve(n);
            mt.reserve(n);
            ee.reserve(n);
            for (std::size_t i = 0; i < n; ++i)
            {
                const hydra::LatencySample &s = diag_raw_storage[i];
                if (s.is_cancel)
                {
                    continue;
                }
                qt.push_back(s.queue_transit_ns);
                mt.push_back(s.match_time_ns);
                ee.push_back(s.end_to_end_ns);
            }

            const auto pct = [](std::vector<uint64_t> v, double p) -> double
            {
                if (v.empty())
                {
                    return 0.0;
                }
                std::sort(v.begin(), v.end());
                std::size_t idx = static_cast<std::size_t>((p / 100.0) * static_cast<double>(v.size()));
                if (idx >= v.size())
                {
                    idx = v.size() - 1;
                }
                return static_cast<double>(v[idx]);
            };

            std::fprintf(stdout,
                         "DIAG decomposed (n=%zu non-cancel samples, whole run, unblended):\n"
                         "  queue_transit_ns: p50=%.1f p99=%.1f p99.9=%.1f\n"
                         "  match_time_ns:    p50=%.1f p99=%.1f p99.9=%.1f\n"
                         "  end_to_end_ns:    p50=%.1f p99=%.1f p99.9=%.1f\n",
                         qt.size(), pct(qt, 50.0), pct(qt, 99.0), pct(qt, 99.9),
                         pct(mt, 50.0), pct(mt, 99.0), pct(mt, 99.9),
                         pct(ee, 50.0), pct(ee, 99.0), pct(ee, 99.9));
        }

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
        auto fill_pool =
            std::make_unique<hydra::ObjectPool<hydra::FillEvent, hydra::FILL_EVENT_POOL_SIZE>>();
        auto book = std::make_unique<hydra::OrderBook>(*order_pool, *level_pool);
        auto matcher = std::make_unique<hydra::Matcher>(*book, *fill_pool, opts.mode);
        auto queue = std::make_unique<hydra::SpscQueue<hydra::Order, hydra::SPSC_CAPACITY>>();
        auto histogram = std::make_unique<hydra::HdrHistogram>();

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
            .fill_pool = *fill_pool,
            .histogram = *histogram,
        };

        hydra::BenchmarkConfig cfg{
            .mode = opts.mode,
            .core = opts.core,
            .trials = opts.trials,
            .output_path = opts.output_path,
            .dataset_path = opts.dataset_path,
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
