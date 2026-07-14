#include "hydra/benchmark.hpp"

#include "hydra/affinity.hpp"
#include "hydra/clock.hpp"
#include "hydra/config.hpp"
#include "hydra/dataset_generator.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace hydra
{

#ifndef HYDRA_LOB_VERSION
#define HYDRA_LOB_VERSION "dev-build"
#endif

    namespace
    {

        struct PreflightResult
        {
            bool ok;
            std::string reason;
        };

        [[nodiscard]] std::optional<std::string> read_sysfs_line(const std::string &path)
        {
            std::ifstream in(path);
            if (!in)
            {
                return std::nullopt;
            }
            std::string line;
            std::getline(in, line);
            return line;
        }

        [[nodiscard]] std::vector<int> parse_cpu_list(const std::string &list)
        {
            std::vector<int> result;
            std::stringstream ss(list);
            std::string token;
            while (std::getline(ss, token, ','))
            {
                if (token.empty())
                {
                    continue;
                }
                const auto dash = token.find('-');
                if (dash == std::string::npos)
                {
                    result.push_back(std::stoi(token));
                }
                else
                {
                    const int lo = std::stoi(token.substr(0, dash));
                    const int hi = std::stoi(token.substr(dash + 1));
                    for (int c = lo; c <= hi; ++c)
                    {
                        result.push_back(c);
                    }
                }
            }
            return result;
        }

        [[nodiscard]] PreflightResult check_core_isolated(int core, const std::string &sysfs_root)
        {
            const std::string path = sysfs_root + "/isolated";
            const auto content = read_sysfs_line(path);
            if (!content)
            {
                return {false, "cannot read " + path + " (is this a Linux system with "
                                                       "isolcpus= support?)"};
            }
            if (content->empty())
            {
                return {false, "core " + std::to_string(core) + " is not isolated: " + path +
                                   " is empty (expected the isolcpus= GRUB parameter to "
                                   "include this core -- see config.hpp's "
                                   "ENVIRONMENT_REQUIREMENTS note)"};
            }
            const auto isolated = parse_cpu_list(*content);
            if (std::find(isolated.begin(), isolated.end(), core) == isolated.end())
            {
                return {false, "core " + std::to_string(core) + " is not in the isolated "
                                                                "CPU list (" +
                                   *content + ")"};
            }
            return {true, ""};
        }

        [[nodiscard]] PreflightResult check_governor_performance(int core, const std::string &sysfs_root)
        {
            const std::string path =
                sysfs_root + "/cpu" + std::to_string(core) + "/cpufreq/scaling_governor";
            const auto content = read_sysfs_line(path);
            if (!content)
            {
                return {false, "cannot read " + path + " (cpufreq not available for core " +
                                   std::to_string(core) + " on this system)"};
            }
            if (*content != "performance")
            {
                return {false, "core " + std::to_string(core) + "'s cpufreq governor is '" +
                                   *content + "', expected 'performance' (run: cpupower "
                                              "frequency-set --governor performance)"};
            }
            return {true, ""};
        }

        [[nodiscard]] PreflightResult check_smt_off(int core, const std::string &sysfs_root)
        {
            const std::string path =
                sysfs_root + "/cpu" + std::to_string(core) + "/topology/thread_siblings_list";
            const auto content = read_sysfs_line(path);
            if (!content)
            {
                return {false, "cannot read " + path};
            }
            const auto siblings = parse_cpu_list(*content);
            if (siblings.size() != 1 || siblings.front() != core)
            {
                return {false, "core " + std::to_string(core) + " has SMT sibling(s) (" +
                                   *content + "); SMT must be disabled (a sibling thread "
                                              "sharing L1/L2 with a pinned core reintroduces the "
                                              "cache contention isolcpus is meant to eliminate)"};
            }
            return {true, ""};
        }

        void run_preflight_checks(const std::string &sysfs_root)
        {
            const std::vector<PreflightResult> results = {
                check_core_isolated(RX_CORE_ID, sysfs_root),
                check_core_isolated(MATCHING_CORE_ID, sysfs_root),
                check_governor_performance(RX_CORE_ID, sysfs_root),
                check_governor_performance(MATCHING_CORE_ID, sysfs_root),
                check_smt_off(RX_CORE_ID, sysfs_root),
                check_smt_off(MATCHING_CORE_ID, sysfs_root),
            };

            bool any_failed = false;
            for (const auto &r : results)
            {
                if (!r.ok)
                {
                    std::fprintf(stderr, "PREFLIGHT FAILED: %s\n", r.reason.c_str());
                    any_failed = true;
                }
            }
            if (any_failed)
            {
                throw std::runtime_error(
                    "benchmark preflight checks failed -- see stderr for details; "
                    "no measurement work was performed");
            }
        }

        [[nodiscard]] DatasetConfig make_live_generation_config()
        {
            DatasetConfig cfg{};
            cfg.seed = DEFAULT_DATASET_SEED;
            cfg.order_count = DEFAULT_DATASET_ORDER_COUNT;
            cfg.mid_price = DEFAULT_MID_PRICE;
            cfg.price_spread_ticks = DEFAULT_PRICE_SPREAD_TICKS;
            cfg.min_qty = DEFAULT_MIN_QTY;
            cfg.max_qty = DEFAULT_MAX_QTY;
            cfg.cancel_ratio = DEFAULT_CANCEL_RATIO;
            cfg.arrival_rate_hz = DEFAULT_ARRIVAL_RATE_HZ;
            cfg.ioc_ratio = DEFAULT_IOC_RATIO;
            cfg.fok_ratio = DEFAULT_FOK_RATIO;
            return cfg;
        }

        [[nodiscard]] uint64_t peek_dataset_seed(const std::string &path)
        {
            std::ifstream in(path, std::ios::binary);
            if (!in)
            {
                throw std::runtime_error("cannot open dataset '" + path + "' to read its seed");
            }
            DatasetFileHeader header{};
            in.read(reinterpret_cast<char *>(&header), sizeof(header));
            if (!in)
            {
                throw std::runtime_error("truncated header while reading dataset seed from '" +
                                         path + "'");
            }
            return header.config.seed;
        }

        [[nodiscard]] Order encode_as_queue_order(const DatasetEvent &ev, uint64_t t1_ns)
        {
            if (ev.type == EventType::NEW_ORDER)
            {
                Order o = ev.order;
                o.timestamp_ns = t1_ns;
                return o;
            }
            Order o{};
            o.order_id = ev.cancel_order_id;
            o.qty = 0;
            o.timestamp_ns = t1_ns;
            return o;
        }

        constexpr int kBoundedPushAttempts = 1000;

        template <typename OnPush>
        void replay_phase(PipelineContext &ctx, const std::vector<DatasetEvent> &events,
                          std::size_t first, std::size_t last, double ns_per_cycle,
                          OnPush &&on_push)
        {
            const auto now_ns = [ns_per_cycle]() -> uint64_t
            {
                return static_cast<uint64_t>(static_cast<double>(rdtsc_now()) * ns_per_cycle);
            };

            const uint64_t phase_start_ns = now_ns();
            const uint64_t base_offset_ns = events[first].arrival_offset_ns;
            uint64_t dropped = 0;

            for (std::size_t i = first; i < last; ++i)
            {
                const uint64_t target_ns =
                    phase_start_ns + (events[i].arrival_offset_ns - base_offset_ns);

                while (now_ns() < target_ns)
                {
                }

                const uint64_t t1_ns = now_ns();
                const Order order = encode_as_queue_order(events[i], t1_ns);

                bool pushed = false;
                for (int attempt = 0; attempt < kBoundedPushAttempts; ++attempt)
                {
                    if (ctx.queue.push(order))
                    {
                        pushed = true;
                        break;
                    }
                }
                if (!pushed) [[unlikely]]
                {
                    ++dropped;
                    continue;
                }
                on_push(i - first, events[i]);
            }

            if (dropped > 0) [[unlikely]]
            {
                std::fprintf(stderr,
                             "benchmark: dropped %llu event(s) to sustained queue "
                             "backpressure\n",
                             static_cast<unsigned long long>(dropped));
            }
        }

        struct TrialRecord
        {
            std::vector<LatencySample> samples;
            std::vector<int64_t> timestamps_unix;
        };

        [[nodiscard]] int64_t current_unix_seconds() noexcept
        {
            return static_cast<int64_t>(
                std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::system_clock::now().time_since_epoch())
                    .count());
        }

        [[nodiscard]] double compute_percentile(std::vector<uint64_t> values, double p)
        {
            if (values.empty())
            {
                return 0.0;
            }
            std::sort(values.begin(), values.end());
            std::size_t idx = static_cast<std::size_t>((p / 100.0) * static_cast<double>(values.size()));
            if (idx >= values.size())
            {
                idx = values.size() - 1;
            }
            return static_cast<double>(values[idx]);
        }

        void write_csv_report(const std::string &output_path, const std::vector<TrialRecord> &trials,
                              bool is_replay, uint64_t dataset_seed,
                              const std::optional<std::string> &dataset_path)
        {
            std::ofstream out(output_path, std::ios::trunc);
            if (!out)
            {
                throw std::runtime_error("cannot open '" + output_path + "' for writing");
            }

            if (is_replay)
            {
                out << "# dataset_seed=" << dataset_seed << " dataset_path=" << *dataset_path
                    << "\n";
            }
            out << "version,trial,iteration,latency_ns,queue_transit_ns,match_time_ns,"
                   "timestamp_unix\n";

            for (std::size_t t = 0; t < trials.size(); ++t)
            {
                const TrialRecord &rec = trials[t];
                std::size_t iteration = 0;
                for (std::size_t i = 0; i < rec.samples.size(); ++i)
                {
                    if (rec.samples[i].is_cancel)
                    {
                        continue;
                    }
                    out << HYDRA_LOB_VERSION << ',' << t << ',' << iteration << ','
                        << rec.samples[i].end_to_end_ns << ',' << rec.samples[i].queue_transit_ns
                        << ',' << rec.samples[i].match_time_ns << ',' << rec.timestamps_unix[i]
                        << '\n';
                    ++iteration;
                }
            }

            if (!out)
            {
                throw std::runtime_error("write failed for '" + output_path + "'");
            }
        }

    }

    void run_benchmark(const BenchmarkConfig &cfg, PipelineContext &ctx)
    {
        pin_to_core(cfg.core);
        verify_affinity(cfg.core);

        run_preflight_checks("/sys/devices/system/cpu");

        ctx.matcher.set_mode(cfg.mode);

        const bool is_replay = cfg.dataset_path.has_value();
        std::vector<DatasetEvent> events;
        uint64_t dataset_seed = 0;

        if (is_replay)
        {
            dataset_seed = peek_dataset_seed(*cfg.dataset_path);
            events = DatasetGenerator::read_from_file(*cfg.dataset_path);
            if (events.size() < WARMUP_ITERATIONS + MEASURED_ITERATIONS)
            {
                throw std::runtime_error(
                    "dataset '" + *cfg.dataset_path + "' has only " +
                    std::to_string(events.size()) + " events, need at least " +
                    std::to_string(WARMUP_ITERATIONS + MEASURED_ITERATIONS) +
                    " (WARMUP_ITERATIONS + MEASURED_ITERATIONS)");
            }
        }
        else
        {
            const DatasetConfig gen_cfg = make_live_generation_config();
            DatasetGenerator generator(gen_cfg);
            events = generator.generate();
            dataset_seed = gen_cfg.seed;
        }

        // Calibrated ONCE here and shared via ctx.ns_per_cycle with
        // matching_thread_fn below -- both threads read the same physical
        // TSC, and independently-calibrated scale factors would disagree by
        // ordinary measurement noise, corrupting every cross-thread latency
        // subtraction (see PipelineContext::ns_per_cycle in pipeline.hpp).
        const double ns_per_cycle = calibrate_ns_per_cycle();
        ctx.ns_per_cycle = ns_per_cycle;

        // run_benchmark() is the producer (via replay_phase() below); it
        // needs a live consumer draining ctx.queue and populating
        // ctx.histogram / ctx.raw_samples, or every trial's "wait for the
        // sink to fill" loop spins forever.
        std::jthread matching_thread(matching_thread_fn, std::ref(ctx));

        std::vector<TrialRecord> trial_records;
        std::vector<double> trial_p99s;
        trial_records.reserve(cfg.trials);
        trial_p99s.reserve(cfg.trials);

        for (std::size_t trial = 0; trial < cfg.trials; ++trial)
        {
            replay_phase(ctx, events, 0, WARMUP_ITERATIONS, ns_per_cycle,
                         [](std::size_t, const DatasetEvent &) {});

            TrialRecord record;
            record.timestamps_unix.reserve(MEASURED_ITERATIONS);
            std::vector<LatencySample> sink_storage(MEASURED_ITERATIONS);
            RawSampleSink sink;
            sink.samples = sink_storage.data();
            sink.capacity = MEASURED_ITERATIONS;

            ctx.raw_samples.store(&sink, std::memory_order_release);

            replay_phase(ctx, events, WARMUP_ITERATIONS, WARMUP_ITERATIONS + MEASURED_ITERATIONS,
                         ns_per_cycle, [&record](std::size_t, const DatasetEvent &)
                         { record.timestamps_unix.push_back(current_unix_seconds()); });

            while (sink.count() < record.timestamps_unix.size())
            {
                std::this_thread::yield();
            }
            ctx.raw_samples.store(nullptr, std::memory_order_release);

            record.samples.assign(sink_storage.begin(),
                                  sink_storage.begin() +
                                      static_cast<std::ptrdiff_t>(record.timestamps_unix.size()));

            std::vector<uint64_t> end_to_end_values;
            end_to_end_values.reserve(record.samples.size());
            for (const LatencySample &s : record.samples)
            {
                if (!s.is_cancel)
                {
                    end_to_end_values.push_back(s.end_to_end_ns);
                }
            }
            trial_p99s.push_back(compute_percentile(end_to_end_values, 99.0));
            trial_records.push_back(std::move(record));
        }

        std::vector<double> sorted_p99s = trial_p99s;
        std::sort(sorted_p99s.begin(), sorted_p99s.end());
        const double median_p99 = sorted_p99s[sorted_p99s.size() / 2];
        const double variance_ns = sorted_p99s.back() - sorted_p99s.front();

        const std::string mode_description =
            is_replay ? ("replay of " + *cfg.dataset_path) : std::string("live-generation");
        std::fprintf(stdout,
                     "blitz_lob benchmark: %zu trials, %s mode, %s\n"
                     "  median P99 (end-to-end): %.1f ns\n"
                     "  variance (max-min P99 across trials): %.1f ns\n",
                     cfg.trials, cfg.mode == MatchingMode::PRICE_TIME ? "PRICE_TIME" : "PRO_RATA",
                     mode_description.c_str(), median_p99, variance_ns);

        write_csv_report(cfg.output_path, trial_records, is_replay, dataset_seed, cfg.dataset_path);
    }

}