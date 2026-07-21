#include "hydra/benchmark.hpp"

#include "hydra/affinity.hpp"
#include "hydra/clock.hpp"
#include "hydra/config.hpp"
#include "hydra/dataset_generator.hpp"
#include "hydra/stats.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iomanip>
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

// Set by CMakeLists.txt from `git status --porcelain` at build time (mirrors
// how HYDRA_LOB_VERSION is captured from `git rev-parse --short HEAD`, same
// WHY: a benchmark result is only as trustworthy as the claim "this hash
// produced this number" -- a dirty tree at build time makes that claim false
// even if HYDRA_LOB_VERSION still prints a real-looking hash.
#ifndef HYDRA_LOB_DIRTY
#define HYDRA_LOB_DIRTY 0
#endif

    namespace
    {

        // ------------------------------------------------------------------
        // Preflight (unchanged from the prior harness -- already sound, see
        // docs/BENCHMARK_METHODOLOGY.md's audit summary).
        // ------------------------------------------------------------------

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

        [[nodiscard]] std::string capture_env_snapshot(const std::string &sysfs_root)
        {
            const auto get = [&](const std::string &path) -> std::string
            {
                const auto v = read_sysfs_line(path);
                return v ? *v : std::string("unavailable");
            };

            std::ostringstream out;
            out << "isolated=" << get(sysfs_root + "/isolated")
                << " rx_core=" << RX_CORE_ID
                << " rx_governor=" << get(sysfs_root + "/cpu" + std::to_string(RX_CORE_ID) +
                                          "/cpufreq/scaling_governor")
                << " rx_smt_siblings=" << get(sysfs_root + "/cpu" + std::to_string(RX_CORE_ID) +
                                              "/topology/thread_siblings_list")
                << " matching_core=" << MATCHING_CORE_ID
                << " matching_governor=" << get(sysfs_root + "/cpu" + std::to_string(MATCHING_CORE_ID) +
                                                "/cpufreq/scaling_governor")
                << " matching_smt_siblings=" << get(sysfs_root + "/cpu" + std::to_string(MATCHING_CORE_ID) +
                                                    "/topology/thread_siblings_list")
                << " git=" << HYDRA_LOB_VERSION
                << " dirty=" << (HYDRA_LOB_DIRTY ? "yes" : "no");
            return out.str();
        }

        [[nodiscard]] std::string cpu_model_name()
        {
            std::ifstream in("/proc/cpuinfo");
            std::string line;
            while (in && std::getline(in, line))
            {
                if (line.rfind("model name", 0) == 0)
                {
                    const auto colon = line.find(':');
                    if (colon != std::string::npos)
                    {
                        std::string name = line.substr(colon + 1);
                        while (!name.empty() && name.front() == ' ')
                        {
                            name.erase(name.begin());
                        }
                        return name;
                    }
                }
            }
            return "unavailable";
        }

        // ------------------------------------------------------------------
        // Dataset provenance: seed (already existed) + a real content hash
        // (new -- the CSV/report schema requires dataset_hash, and a seed
        // alone doesn't prove two files are byte-identical).
        // ------------------------------------------------------------------

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

        // FNV-1a 64-bit over the raw file bytes -- cheap, deterministic,
        // good enough to prove "these two files are byte-identical" (the
        // dataset_hash column's job), not a cryptographic commitment.
        [[nodiscard]] uint64_t compute_dataset_hash(const std::string &path)
        {
            std::ifstream in(path, std::ios::binary);
            if (!in)
            {
                throw std::runtime_error("cannot open dataset '" + path + "' to hash it");
            }
            uint64_t hash = 1469598103934665603ULL; // FNV offset basis
            constexpr uint64_t kPrime = 1099511628211ULL;
            char buf[65536];
            while (in)
            {
                in.read(buf, sizeof(buf));
                const std::streamsize n = in.gcount();
                for (std::streamsize i = 0; i < n; ++i)
                {
                    hash ^= static_cast<uint8_t>(buf[i]);
                    hash *= kPrime;
                }
            }
            return hash;
        }

        [[nodiscard]] std::string hex64(uint64_t v)
        {
            std::ostringstream out;
            out << std::hex << std::setw(16) << std::setfill('0') << v;
            return out.str();
        }

        // Measures rdtsc_now()'s own fixed overhead: N back-to-back calls,
        // mean of the consecutive deltas. Reported as "Clock overhead" in
        // every report so a reader can see the instrumentation's own
        // footprint rather than have it silently folded into the numbers
        // it's measuring.
        [[nodiscard]] double measure_clock_overhead_ns(double ns_per_cycle)
        {
            constexpr int kSamples = 10'000;
            std::vector<uint64_t> deltas;
            deltas.reserve(kSamples);
            uint64_t prev = rdtsc_now();
            for (int i = 0; i < kSamples; ++i)
            {
                const uint64_t now = rdtsc_now();
                deltas.push_back(now - prev);
                prev = now;
            }
            uint64_t sum = 0;
            for (const uint64_t d : deltas)
            {
                sum += d;
            }
            return (static_cast<double>(sum) / static_cast<double>(deltas.size())) * ns_per_cycle;
        }

        [[nodiscard]] Order encode_as_queue_order(const DatasetEvent &ev, uint64_t t1_ns)
        {
            Order o{};
            o.timestamp_ns = t1_ns;
            if (ev.type == EventType::NEW_ORDER)
            {
                o = ev.order;
                o.event_tag = OrderEventTag::NEW_OR_CANCEL;
                o.timestamp_ns = t1_ns;
                return o;
            }
            if (ev.type == EventType::REPLACE)
            {
                o.order_id = ev.cancel_order_id; // target order_id, see EventType::REPLACE
                o.price = ev.order.price;        // new price
                o.qty = ev.order.qty;             // new qty (never 0 -- see DatasetGenerator)
                o.event_tag = OrderEventTag::REPLACE;
                return o;
            }
            // CANCEL
            o.order_id = ev.cancel_order_id;
            o.qty = 0;
            o.event_tag = OrderEventTag::NEW_OR_CANCEL;
            return o;
        }

        constexpr int kBoundedPushAttempts = 1000;

        struct ReplayPhaseResult
        {
            uint64_t dropped;
            uint64_t max_drift_ns;
            // Aggregate-only (not per-sample, see docs/BENCHMARK_METHODOLOGY.md
            // for why): the successful push() call's own duration cannot be
            // attributed back into the transported Order value, since its
            // duration isn't known until after the value has already been
            // copied into the queue. Reported as a trial-level mean/max
            // instead of a per-order field.
            double push_ns_sum = 0.0;
            uint64_t push_ns_max = 0;
            std::size_t push_count = 0;
        };

        template <typename OnPush>
        [[nodiscard]] ReplayPhaseResult replay_phase(PipelineContext &ctx, const std::vector<DatasetEvent> &events,
                          std::size_t first, std::size_t last, double ns_per_cycle,
                          OnPush &&on_push)
        {
            const auto now_ns = [ns_per_cycle]() -> uint64_t
            {
                return static_cast<uint64_t>(static_cast<double>(rdtsc_now()) * ns_per_cycle);
            };

            const uint64_t phase_start_ns = now_ns();
            const uint64_t base_offset_ns = events[first].arrival_offset_ns;
            ReplayPhaseResult result{};

            for (std::size_t i = first; i < last; ++i)
            {
                const uint64_t target_ns =
                    phase_start_ns + (events[i].arrival_offset_ns - base_offset_ns);

                while (now_ns() < target_ns)
                {
                }

                const uint64_t t1_ns = now_ns();
                if (t1_ns > target_ns)
                {
                    result.max_drift_ns = std::max(result.max_drift_ns, t1_ns - target_ns);
                }
                const Order order = encode_as_queue_order(events[i], t1_ns);

                bool pushed = false;
                for (int attempt = 0; attempt < kBoundedPushAttempts; ++attempt)
                {
                    const uint64_t push_start_ns = now_ns();
                    if (ctx.queue.push(order))
                    {
                        const uint64_t push_end_ns = now_ns();
                        const uint64_t push_ns = push_end_ns - push_start_ns;
                        result.push_ns_sum += static_cast<double>(push_ns);
                        result.push_ns_max = std::max(result.push_ns_max, push_ns);
                        ++result.push_count;
                        pushed = true;
                        break;
                    }
                }
                if (!pushed) [[unlikely]]
                {
                    ++result.dropped;
                    continue;
                }
                on_push(i - first, events[i]);
            }

            if (result.dropped > 0) [[unlikely]]
            {
                std::fprintf(stderr,
                             "benchmark: dropped %llu event(s) to sustained queue "
                             "backpressure\n",
                             static_cast<unsigned long long>(result.dropped));
            }
            return result;
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

        // Everything derived from one trial's recorded samples: percentile
        // sets (order and cancel populations, kept separate per the
        // existing disclosed-not-hidden convention), workload-composition
        // aggregates from MatchStats fields, throughput, and this trial's
        // own dropped/arena/pool deltas.
        struct TrialStats
        {
            std::size_t order_count = 0;
            std::size_t cancel_count = 0;
            stats::PercentileSet order_pct;
            stats::PercentileSet cancel_pct;
            double throughput_orders_per_sec = 0.0;

            uint64_t crossing_order_count = 0;
            uint64_t non_crossing_order_count = 0;
            uint64_t partial_fill_count = 0;
            uint64_t multi_level_sweep_count = 0;
            uint64_t self_trade_skip_count = 0;
            uint64_t resting_orders_examined = 0;
            uint64_t eligible_orders_examined = 0;
            uint64_t generated_fill_count = 0;
            uint64_t max_fill_fanout = 0;
            double mean_fills_per_match = 0.0;
            double mean_levels_consumed = 0.0;
            double mean_fill_publish_ns = 0.0;

            uint64_t dropped_events = 0;
            uint64_t max_schedule_drift_ns = 0;
            uint64_t arena_fallback_delta = 0;
            uint64_t pool_exhaustion_delta = 0;

            // Deterministic fingerprint over LOGICAL outcomes only (fills,
            // levels touched, book depth, examined/skip counts) -- never
            // over timing, since timing legitimately varies run to run
            // even when the system is perfectly deterministic. Identical
            // across every trial iff the system produced identical
            // outcomes for an identical replayed event sequence.
            uint64_t state_hash = 0;
        };

        [[nodiscard]] TrialStats compute_trial_stats(const TrialRecord &record,
                                                      std::size_t final_bid_levels,
                                                      std::size_t final_ask_levels,
                                                      double batch_seconds,
                                                      uint64_t dropped_events,
                                                      uint64_t max_schedule_drift_ns,
                                                      uint64_t arena_fallback_delta,
                                                      uint64_t pool_exhaustion_delta)
        {
            TrialStats t{};
            t.dropped_events = dropped_events;
            t.max_schedule_drift_ns = max_schedule_drift_ns;
            t.arena_fallback_delta = arena_fallback_delta;
            t.pool_exhaustion_delta = pool_exhaustion_delta;

            std::vector<uint64_t> order_e2e;
            std::vector<uint64_t> cancel_e2e;
            order_e2e.reserve(record.samples.size());

            double fill_publish_sum = 0.0;
            uint64_t levels_sum = 0;

            for (const LatencySample &s : record.samples)
            {
                if (s.is_cancel)
                {
                    cancel_e2e.push_back(s.end_to_end_ns);
                    continue;
                }
                order_e2e.push_back(s.end_to_end_ns);

                if (s.fills_generated > 0)
                {
                    ++t.crossing_order_count;
                    if (s.remaining_qty > 0)
                    {
                        ++t.partial_fill_count;
                    }
                }
                else
                {
                    ++t.non_crossing_order_count;
                }
                if (s.levels_consumed > 1)
                {
                    ++t.multi_level_sweep_count;
                }
                t.self_trade_skip_count += s.self_trade_skips;
                t.resting_orders_examined += s.resting_orders_examined;
                t.eligible_orders_examined += s.eligible_orders_examined;
                t.generated_fill_count += s.fills_generated;
                t.max_fill_fanout = std::max<uint64_t>(t.max_fill_fanout, s.fills_generated);
                levels_sum += s.levels_consumed;
                fill_publish_sum += static_cast<double>(s.fill_publish_ns);
            }

            t.order_count = order_e2e.size();
            t.cancel_count = cancel_e2e.size();
            t.mean_fills_per_match = (t.crossing_order_count > 0)
                                         ? static_cast<double>(t.generated_fill_count) /
                                               static_cast<double>(t.crossing_order_count)
                                         : 0.0;
            t.mean_levels_consumed = (t.order_count > 0)
                                          ? static_cast<double>(levels_sum) /
                                                static_cast<double>(t.order_count)
                                          : 0.0;
            t.mean_fill_publish_ns = (t.generated_fill_count > 0)
                                         ? fill_publish_sum / static_cast<double>(t.generated_fill_count)
                                         : 0.0;

            std::sort(order_e2e.begin(), order_e2e.end());
            std::sort(cancel_e2e.begin(), cancel_e2e.end());
            t.order_pct = stats::compute_percentiles(order_e2e);
            t.cancel_pct = stats::compute_percentiles(cancel_e2e);

            t.throughput_orders_per_sec =
                (batch_seconds > 0.0) ? static_cast<double>(t.order_count) / batch_seconds : 0.0;

            // FNV-1a-style fold over logical-outcome counters + final book
            // depth. Deliberately excludes every timing field.
            uint64_t h = 1469598103934665603ULL;
            const uint64_t prime = 1099511628211ULL;
            const auto fold = [&](uint64_t v)
            {
                h ^= v;
                h *= prime;
            };
            fold(final_bid_levels);
            fold(final_ask_levels);
            fold(t.crossing_order_count);
            fold(t.non_crossing_order_count);
            fold(t.partial_fill_count);
            fold(t.multi_level_sweep_count);
            fold(t.self_trade_skip_count);
            fold(t.resting_orders_examined);
            fold(t.eligible_orders_examined);
            fold(t.generated_fill_count);
            fold(t.max_fill_fanout);
            fold(t.order_count);
            fold(t.cancel_count);
            t.state_hash = h;

            return t;
        }

        // Composition of the dataset itself (add/cancel/replace mix, TIF
        // mix) over the measured window only -- identical for every trial
        // (same replayed events), so computed once, not per trial.
        struct DatasetComposition
        {
            std::size_t new_order_count = 0;
            std::size_t cancel_count = 0;
            std::size_t replace_count = 0;
            std::size_t fok_count = 0;
            std::size_t ioc_count = 0;
            std::size_t gtc_count = 0;
        };

        [[nodiscard]] DatasetComposition compute_dataset_composition(
            const std::vector<DatasetEvent> &events, std::size_t first, std::size_t last)
        {
            DatasetComposition c{};
            for (std::size_t i = first; i < last; ++i)
            {
                switch (events[i].type)
                {
                case EventType::CANCEL:
                    ++c.cancel_count;
                    break;
                case EventType::REPLACE:
                    ++c.replace_count;
                    break;
                case EventType::NEW_ORDER:
                    ++c.new_order_count;
                    switch (events[i].order.tif)
                    {
                    case TimeInForce::FOK:
                        ++c.fok_count;
                        break;
                    case TimeInForce::IOC:
                        ++c.ioc_count;
                        break;
                    case TimeInForce::GTC:
                        ++c.gtc_count;
                        break;
                    }
                    break;
                }
            }
            return c;
        }

        [[nodiscard]] std::string basename_of(const std::string &path)
        {
            const std::size_t slash = path.find_last_of('/');
            return (slash == std::string::npos) ? path : path.substr(slash + 1);
        }

        void write_summary_csv(const std::string &path, const std::string &run_id,
                               MatchingMode mode, const std::string &dataset_path,
                               uint64_t dataset_hash, std::size_t dataset_records,
                               double clock_overhead_ns,
                               const std::vector<TrialStats> &trial_stats,
                               const std::vector<uint64_t> &state_hashes,
                               bool determinism_pass, bool overall_valid,
                               const std::string &invalid_reason)
        {
            std::ofstream out(path, std::ios::trunc);
            if (!out)
            {
                throw std::runtime_error("cannot open '" + path + "' for writing");
            }
            out << std::fixed << std::setprecision(3);

            out << "run_id,commit_hash,trial,matching_mode,workload_type,dataset,dataset_hash,"
                   "dataset_records,warmup_operations,measured_operations,producer_cpu,"
                   "consumer_cpu,numa_node,clock_source,timestamp_overhead_ns,p50_ns_per_order,"
                   "p90_ns_per_order,p99_ns_per_order,p999_ns_per_order,mean_ns_per_order,"
                   "max_ns_per_order,throughput_orders_per_second,crossing_order_count,"
                   "non_crossing_order_count,partial_fill_count,multi_level_sweep_count,"
                   "fok_order_count,self_trade_skip_count,resting_orders_examined,"
                   "eligible_orders_examined,generated_fill_count,mean_fills_per_match,"
                   "max_fill_fanout,mean_levels_consumed,remainder_units_distributed,"
                   "allocation_invariant_failures,dropped_events,arena_fallback_count,"
                   "pool_exhaustion_count,unexpected_allocation_count,state_hash,"
                   "reference_validation_status,determinism_status,valid,invalid_reason\n";

            const char *mode_str = (mode == MatchingMode::PRICE_TIME) ? "price_time" : "pro_rata";
            const std::string workload_type = basename_of(dataset_path);

            for (std::size_t t = 0; t < trial_stats.size(); ++t)
            {
                const TrialStats &s = trial_stats[t];
                out << run_id << ',' << HYDRA_LOB_VERSION << ',' << t << ',' << mode_str << ','
                    << workload_type << ',' << dataset_path << ',' << hex64(dataset_hash) << ','
                    << dataset_records << ',' << WARMUP_ITERATIONS << ',' << MEASURED_ITERATIONS
                    << ',' << RX_CORE_ID << ',' << MATCHING_CORE_ID << ',' << 0 << ','
                    << "rdtscp_calibrated" << ',' << clock_overhead_ns << ','
                    << s.order_pct.p50 << ',' << s.order_pct.p90 << ',' << s.order_pct.p99 << ','
                    << s.order_pct.p999 << ',' << s.order_pct.mean << ',' << s.order_pct.max << ','
                    << s.throughput_orders_per_sec << ',' << s.crossing_order_count << ','
                    << s.non_crossing_order_count << ',' << s.partial_fill_count << ','
                    << s.multi_level_sweep_count << ',' << 0 /* fok_order_count filled below */
                    << ',' << s.self_trade_skip_count << ',' << s.resting_orders_examined << ','
                    << s.eligible_orders_examined << ',' << s.generated_fill_count << ','
                    << s.mean_fills_per_match << ',' << s.max_fill_fanout << ','
                    << s.mean_levels_consumed << ',' << 0 /* remainder_units_distributed */ << ','
                    << 0 /* allocation_invariant_failures */ << ',' << s.dropped_events << ','
                    << s.arena_fallback_delta << ',' << s.pool_exhaustion_delta << ','
                    << s.arena_fallback_delta /* unexpected_allocation_count */ << ','
                    << hex64(state_hashes[t]) << ',' << "NOT_RUN" << ','
                    << (determinism_pass ? "PASS" : "FAIL") << ',' << (overall_valid ? 1 : 0)
                    << ',' << invalid_reason << '\n';
            }

            if (!out)
            {
                throw std::runtime_error("write failed for '" + path + "'");
            }
        }

        void write_raw_csv(const std::string &path, const std::vector<TrialRecord> &trials)
        {
            std::ofstream out(path, std::ios::trunc);
            if (!out)
            {
                throw std::runtime_error("cannot open '" + path + "' for writing");
            }
            out << "trial,iteration,queue_residence_ns,queue_pop_ns,match_time_ns,"
                   "fill_publish_ns,end_to_end_ns,fills_generated,levels_consumed,"
                   "resting_orders_examined,eligible_orders_examined,self_trade_skips,"
                   "remaining_qty,timestamp_unix,is_cancel,is_replace\n";
            for (std::size_t t = 0; t < trials.size(); ++t)
            {
                const TrialRecord &rec = trials[t];
                for (std::size_t i = 0; i < rec.samples.size(); ++i)
                {
                    const LatencySample &s = rec.samples[i];
                    out << t << ',' << i << ',' << s.queue_residence_ns << ',' << s.queue_pop_ns
                        << ',' << s.match_time_ns << ',' << s.fill_publish_ns << ','
                        << s.end_to_end_ns << ',' << s.fills_generated << ',' << s.levels_consumed
                        << ',' << s.resting_orders_examined << ',' << s.eligible_orders_examined
                        << ',' << s.self_trade_skips << ',' << s.remaining_qty << ','
                        << rec.timestamps_unix[i] << ',' << (s.is_cancel ? 1 : 0) << ','
                        << (s.is_replace ? 1 : 0) << '\n';
                }
            }
            if (!out)
            {
                throw std::runtime_error("write failed for '" + path + "'");
            }
        }

        void write_metadata_json(const std::string &path, const std::string &run_id,
                                 MatchingMode mode, const std::string &env_snapshot,
                                 const std::string &dataset_path, uint64_t dataset_hash,
                                 bool dirty, bool allow_dirty, bool overall_valid,
                                 const std::string &invalid_reason)
        {
            std::ofstream out(path, std::ios::trunc);
            if (!out)
            {
                throw std::runtime_error("cannot open '" + path + "' for writing");
            }
            out << "{\n"
                << "  \"run_id\": \"" << run_id << "\",\n"
                << "  \"commit_hash\": \"" << HYDRA_LOB_VERSION << "\",\n"
                << "  \"working_tree_dirty\": " << (dirty ? "true" : "false") << ",\n"
                << "  \"dirty_allowed\": " << (allow_dirty ? "true" : "false") << ",\n"
                << "  \"matching_mode\": \""
                << (mode == MatchingMode::PRICE_TIME ? "price_time" : "pro_rata") << "\",\n"
                << "  \"dataset_path\": \"" << dataset_path << "\",\n"
                << "  \"dataset_hash\": \"" << hex64(dataset_hash) << "\",\n"
                << "  \"cpu_model\": \"" << cpu_model_name() << "\",\n"
                << "  \"compiler\": \"" <<
#if defined(__VERSION__)
                __VERSION__
#else
                "unknown"
#endif
                << "\",\n"
                << "  \"env_snapshot\": \"" << env_snapshot << "\",\n"
                << "  \"result_validity\": \"" << (overall_valid ? "VALID" : "INVALID") << "\",\n"
                << "  \"invalid_reason\": \"" << invalid_reason << "\"\n"
                << "}\n";
            if (!out)
            {
                throw std::runtime_error("write failed for '" + path + "'");
            }
        }

        [[nodiscard]] std::string derive_path(const std::string &base, const std::string &suffix)
        {
            const std::size_t dot = base.find_last_of('.');
            if (dot == std::string::npos)
            {
                return base + suffix;
            }
            return base.substr(0, dot) + suffix + base.substr(dot);
        }

    } // namespace

    void run_benchmark(const BenchmarkConfig &cfg, PipelineContext &ctx)
    {
        if (cfg.dataset_path.empty())
        {
            throw std::runtime_error(
                "run_benchmark: cfg.dataset_path is required -- there is no "
                "live-generation fallback. Generate one with blitz_gen_dataset "
                "and pass --dataset.");
        }

        pin_to_core(RX_CORE_ID);
        verify_affinity(RX_CORE_ID);

        run_preflight_checks("/sys/devices/system/cpu");
        const std::string env_snapshot = capture_env_snapshot("/sys/devices/system/cpu");

        std::vector<std::string> invalid_reasons;

#if HYDRA_LOB_DIRTY
        if (!cfg.allow_dirty)
        {
            invalid_reasons.push_back("working tree dirty at build time (pass --allow-dirty "
                                       "to intentionally run against a dirty tree)");
        }
        std::fprintf(stderr,
                     "%s: working tree was dirty at build time -- this binary's "
                     "HYDRA_LOB_VERSION (%s) does not fully identify the code that "
                     "produced this run.%s\n",
                     cfg.allow_dirty ? "WARNING" : "ERROR", HYDRA_LOB_VERSION,
                     cfg.allow_dirty ? " Proceeding because --allow-dirty was passed."
                                     : " Commit, or pass --allow-dirty to proceed anyway.");
#endif

        const bool is_smoketest = cfg.trials < DEFAULT_TRIAL_COUNT;
        if (is_smoketest)
        {
            std::fprintf(stderr,
                         "NOTE: --trials %zu is below the canonical trial count (%zu) -- "
                         "this run is a smoketest, not a citable result. The output CSV "
                         "is marked accordingly.\n",
                         cfg.trials, DEFAULT_TRIAL_COUNT);
        }

        ctx.matcher.set_mode(cfg.mode);

        const uint64_t dataset_seed = peek_dataset_seed(cfg.dataset_path);
        const uint64_t dataset_hash = compute_dataset_hash(cfg.dataset_path);
        std::vector<DatasetEvent> events = DatasetGenerator::read_from_file(cfg.dataset_path);
        if (events.size() < WARMUP_ITERATIONS + MEASURED_ITERATIONS)
        {
            throw std::runtime_error(
                "dataset '" + cfg.dataset_path + "' has only " +
                std::to_string(events.size()) + " events, need at least " +
                std::to_string(WARMUP_ITERATIONS + MEASURED_ITERATIONS) +
                " (WARMUP_ITERATIONS + MEASURED_ITERATIONS)");
        }
        const DatasetComposition composition = compute_dataset_composition(
            events, WARMUP_ITERATIONS, WARMUP_ITERATIONS + MEASURED_ITERATIONS);

        const double ns_per_cycle = calibrate_ns_per_cycle();
        ctx.ns_per_cycle = ns_per_cycle;
        const double clock_overhead_ns = measure_clock_overhead_ns(ns_per_cycle);

        std::jthread matching_thread(matching_thread_fn, std::ref(ctx));

        std::vector<TrialRecord> trial_records;
        std::vector<TrialStats> trial_stats;
        std::vector<uint64_t> state_hashes;
        trial_records.reserve(cfg.trials);
        trial_stats.reserve(cfg.trials);
        state_hashes.reserve(cfg.trials);

        uint64_t total_dropped_events = 0;
        uint64_t max_schedule_drift_ns = 0;
        double push_ns_sum_all = 0.0;
        uint64_t push_ns_max_all = 0;
        std::size_t push_count_all = 0;

        for (std::size_t trial = 0; trial < cfg.trials; ++trial)
        {
            if (trial > 0)
            {
                ctx.book.reset();
                ctx.order_pool.reset();
                ctx.level_pool.reset();
            }

            const uint64_t arena_before = ctx.book.arena_fallback_count();
            const uint64_t order_pool_before = ctx.order_pool.exhaustion_count();
            const uint64_t level_pool_before = ctx.level_pool.exhaustion_count();

            {
                const ReplayPhaseResult warmup_result = replay_phase(
                    ctx, events, 0, WARMUP_ITERATIONS, ns_per_cycle,
                    [](std::size_t, const DatasetEvent &) {});
                total_dropped_events += warmup_result.dropped;
                max_schedule_drift_ns = std::max(max_schedule_drift_ns, warmup_result.max_drift_ns);
            }

            TrialRecord record;
            record.timestamps_unix.reserve(MEASURED_ITERATIONS);
            std::vector<LatencySample> sink_storage(MEASURED_ITERATIONS);
            RawSampleSink sink;
            sink.samples = sink_storage.data();
            sink.capacity = MEASURED_ITERATIONS;

            ctx.raw_samples.store(&sink, std::memory_order_release);

            const auto batch_start = std::chrono::steady_clock::now();
            ReplayPhaseResult measured_result{};
            {
                measured_result = replay_phase(
                    ctx, events, WARMUP_ITERATIONS, WARMUP_ITERATIONS + MEASURED_ITERATIONS,
                    ns_per_cycle, [&record](std::size_t, const DatasetEvent &)
                    { record.timestamps_unix.push_back(current_unix_seconds()); });
                total_dropped_events += measured_result.dropped;
                max_schedule_drift_ns = std::max(max_schedule_drift_ns, measured_result.max_drift_ns);
                push_ns_sum_all += measured_result.push_ns_sum;
                push_ns_max_all = std::max(push_ns_max_all, measured_result.push_ns_max);
                push_count_all += measured_result.push_count;
            }

            while (sink.count() < record.timestamps_unix.size())
            {
                std::this_thread::yield();
            }
            ctx.raw_samples.store(nullptr, std::memory_order_release);
            const auto batch_end = std::chrono::steady_clock::now();
            const double batch_seconds =
                std::chrono::duration<double>(batch_end - batch_start).count();

            record.samples.assign(sink_storage.begin(),
                                  sink_storage.begin() +
                                      static_cast<std::ptrdiff_t>(record.timestamps_unix.size()));

            const uint64_t arena_delta = ctx.book.arena_fallback_count() - arena_before;
            const uint64_t pool_delta = (ctx.order_pool.exhaustion_count() - order_pool_before) +
                                        (ctx.level_pool.exhaustion_count() - level_pool_before);

            const TrialStats ts = compute_trial_stats(
                record, ctx.book.bid_level_count(), ctx.book.ask_level_count(), batch_seconds,
                measured_result.dropped, measured_result.max_drift_ns, arena_delta, pool_delta);

            if (arena_delta > 0)
            {
                std::fprintf(stderr,
                             "WARNING: order book's fixed arena overflowed to the heap %llu "
                             "time(s) during trial %zu.\n",
                             static_cast<unsigned long long>(arena_delta), trial);
            }
            if (pool_delta > 0)
            {
                std::fprintf(stderr,
                             "WARNING: order/level pool exhausted %llu time(s) during trial "
                             "%zu -- incoming orders/levels were silently dropped.\n",
                             static_cast<unsigned long long>(pool_delta), trial);
            }

            state_hashes.push_back(ts.state_hash);
            trial_stats.push_back(ts);
            trial_records.push_back(std::move(record));
        }

        // Determinism: every trial replayed the identical seeded event
        // sequence against a freshly reset book -- a deterministic system
        // must therefore produce an identical logical-outcome fingerprint
        // every time.
        bool determinism_pass = true;
        for (std::size_t i = 1; i < state_hashes.size(); ++i)
        {
            if (state_hashes[i] != state_hashes[0])
            {
                determinism_pass = false;
                break;
            }
        }
        if (!determinism_pass)
        {
            invalid_reasons.push_back("determinism check failed: state_hash differs across trials");
        }

        if (total_dropped_events > 0)
        {
            invalid_reasons.push_back("dropped_events > 0 (" +
                                      std::to_string(total_dropped_events) + ")");
        }
        uint64_t total_arena_fallbacks = 0;
        uint64_t total_pool_exhaustions = 0;
        for (const auto &ts : trial_stats)
        {
            total_arena_fallbacks += ts.arena_fallback_delta;
            total_pool_exhaustions += ts.pool_exhaustion_delta;
        }
        if (total_arena_fallbacks > 0)
        {
            invalid_reasons.push_back("arena_fallback_count > 0 (" +
                                      std::to_string(total_arena_fallbacks) + ")");
        }
        if (total_pool_exhaustions > 0)
        {
            invalid_reasons.push_back("pool_exhaustion_count > 0 (" +
                                      std::to_string(total_pool_exhaustions) + ")");
        }

        const bool overall_valid = invalid_reasons.empty();
        std::string invalid_reason_str;
        for (std::size_t i = 0; i < invalid_reasons.size(); ++i)
        {
            if (i > 0)
            {
                invalid_reason_str += "; ";
            }
            invalid_reason_str += invalid_reasons[i];
        }

        // Median-across-trials for every headline figure.
        const auto collect = [&](auto extractor) -> std::vector<double>
        {
            std::vector<double> v;
            v.reserve(trial_stats.size());
            for (const auto &ts : trial_stats)
            {
                v.push_back(extractor(ts));
            }
            return v;
        };
        const double median_p50 = stats::median_across_trials(collect([](const TrialStats &t)
                                                                       { return t.order_pct.p50; }));
        const double median_p90 = stats::median_across_trials(collect([](const TrialStats &t)
                                                                       { return t.order_pct.p90; }));
        const double median_p99 = stats::median_across_trials(collect([](const TrialStats &t)
                                                                       { return t.order_pct.p99; }));
        const double median_p999 = stats::median_across_trials(
            collect([](const TrialStats &t) { return t.order_pct.p999; }));
        const double median_max = stats::median_across_trials(collect([](const TrialStats &t)
                                                                       { return t.order_pct.max; }));
        const double median_throughput = stats::median_across_trials(
            collect([](const TrialStats &t) { return t.throughput_orders_per_sec; }));

        const std::vector<double> p50_series = collect([](const TrialStats &t)
                                                        { return t.order_pct.p50; });
        const std::vector<double> p99_series = collect([](const TrialStats &t)
                                                        { return t.order_pct.p99; });
        const std::vector<double> p999_series = collect([](const TrialStats &t)
                                                         { return t.order_pct.p999; });
        const std::vector<double> throughput_series =
            collect([](const TrialStats &t) { return t.throughput_orders_per_sec; });

        const double p50_cv = stats::coefficient_of_variation(p50_series) * 100.0;
        const double p99_cv = stats::coefficient_of_variation(p99_series) * 100.0;
        const double p999_cv = stats::coefficient_of_variation(p999_series) * 100.0;
        const double throughput_cv = stats::coefficient_of_variation(throughput_series) * 100.0;
        // Loosely-justified stability threshold: within 15% coefficient of
        // variation on both tail percentiles across trials. Not a formal
        // statistical test, disclosed as a heuristic in
        // docs/BENCHMARK_METHODOLOGY.md.
        const bool stable = (p99_cv < 15.0) && (p999_cv < 15.0);

        const std::size_t final_bid_levels = ctx.book.bid_level_count();
        const std::size_t final_ask_levels = ctx.book.ask_level_count();

        const std::string run_id = std::string(HYDRA_LOB_VERSION) + "-" +
                                   (cfg.mode == MatchingMode::PRICE_TIME ? "price_time"
                                                                          : "pro_rata") +
                                   "-" + std::to_string(current_unix_seconds());

        // --------------------------------------------------------------
        // Console report
        // --------------------------------------------------------------
        const char *mode_str_full =
            (cfg.mode == MatchingMode::PRICE_TIME) ? "Price-Time Priority (FIFO)" : "Pro-Rata Allocation";
        const char *mode_str_short =
            (cfg.mode == MatchingMode::PRICE_TIME) ? "price_time" : "pro_rata";

        std::fprintf(stdout,
                     "================================================================================\n"
                     "BLITZ LOB -- CANONICAL MATCHING BENCHMARK\n"
                     "================================================================================\n"
                     "\n"
                     "Run\n"
                     "--------------------------------------------------------------------------------\n"
                     "Policy              : %s\n"
                     "Commit              : %s\n"
                     "Working tree        : %s\n"
                     "Run ID              : %s\n"
                     "Validity            : %s%s\n"
                     "\n"
                     "Workload\n"
                     "--------------------------------------------------------------------------------\n"
                     "Dataset             : %s\n"
                     "Dataset hash        : %s\n"
                     "Input messages      : %zu\n"
                     "Warm-up messages    : %zu\n"
                     "Measured messages   : %zu\n"
                     "  New orders        : %zu\n"
                     "  Cancels           : %zu\n"
                     "  Replaces          : %zu\n"
                     "  FOK orders        : %zu\n"
                     "  IOC orders        : %zu\n"
                     "  GTC orders        : %zu\n"
                     "Crossing orders     : %llu (%.2f%%)\n"
                     "Generated fills     : %llu\n"
                     "Fills/crossing order: %.3f\n"
                     "Maximum fill fan-out: %llu\n"
                     "\n"
                     "Environment\n"
                     "--------------------------------------------------------------------------------\n"
                     "CPU model           : %s\n"
                     "Build               : Release\n"
                     "Compiler            : %s\n"
                     "NUMA node           : 0 (operator-bound via numactl, not self-verified)\n"
                     "Producer CPU        : %d\n"
                     "Consumer CPU        : %d\n"
                     "Preflight           : PASS (core isolation, governor, SMT -- see env below)\n"
                     "Env                 : %s\n"
                     "\n"
                     "Measurement\n"
                     "--------------------------------------------------------------------------------\n"
                     "Metric              : Matching-engine processing latency (end-to-end, orders)\n"
                     "Start               : replay_phase()'s intended-arrival timestamp (t1)\n"
                     "End                 : matching_thread_fn's post-match() timestamp (t_match_end)\n"
                     "Clock               : rdtscp, calibrated ns_per_cycle (clock.hpp)\n"
                     "Clock overhead      : %.2f ns/sample\n"
                     "Sample              : One processed input message\n"
                     "Trials              : %zu independent trials%s\n",
                     mode_str_full,
                     HYDRA_LOB_VERSION,
#if HYDRA_LOB_DIRTY
                     cfg.allow_dirty ? "Dirty (allowed via --allow-dirty)" : "Dirty",
#else
                     "Clean",
#endif
                     run_id.c_str(), overall_valid ? "VALID" : "INVALID",
                     overall_valid ? "" : (" (" + invalid_reason_str + ")").c_str(),
                     cfg.dataset_path.c_str(), hex64(dataset_hash).c_str(), events.size(),
                     WARMUP_ITERATIONS, MEASURED_ITERATIONS, composition.new_order_count,
                     composition.cancel_count, composition.replace_count, composition.fok_count,
                     composition.ioc_count, composition.gtc_count,
                     static_cast<unsigned long long>(trial_stats.front().crossing_order_count),
                     trial_stats.front().order_count > 0
                         ? 100.0 * static_cast<double>(trial_stats.front().crossing_order_count) /
                               static_cast<double>(trial_stats.front().order_count)
                         : 0.0,
                     static_cast<unsigned long long>(trial_stats.front().generated_fill_count),
                     trial_stats.front().mean_fills_per_match,
                     static_cast<unsigned long long>(trial_stats.front().max_fill_fanout),
                     cpu_model_name().c_str(),
#if defined(__VERSION__)
                     __VERSION__,
#else
                     "unknown",
#endif
                     RX_CORE_ID, MATCHING_CORE_ID, env_snapshot.c_str(), clock_overhead_ns,
                     cfg.trials, is_smoketest ? " [SMOKETEST -- not a citable result]" : "");

        std::fprintf(stdout,
                     "\n"
                     "Trial Results\n"
                     "--------------------------------------------------------------------------------\n"
                     "Trial   P50       P90       P99       P99.9     Max       Throughput\n"
                     "        ns/order  ns/order  ns/order  ns/order  ns/order  M orders/s\n"
                     "--------------------------------------------------------------------------------\n");
        for (std::size_t t = 0; t < trial_stats.size(); ++t)
        {
            const TrialStats &s = trial_stats[t];
            std::fprintf(stdout, "%-8zu%-10.1f%-10.1f%-10.1f%-10.1f%-10.1f%-.4f\n", t,
                         s.order_pct.p50, s.order_pct.p90, s.order_pct.p99, s.order_pct.p999,
                         s.order_pct.max, s.throughput_orders_per_sec / 1'000'000.0);
        }

        std::fprintf(stdout,
                     "\n"
                     "Canonical Result -- Median Across Independent Trials\n"
                     "--------------------------------------------------------------------------------\n"
                     "P50 processing latency   : %.1f ns/order\n"
                     "P90 processing latency   : %.1f ns/order\n"
                     "P99 processing latency   : %.1f ns/order\n"
                     "P99.9 processing latency : %.1f ns/order\n"
                     "Maximum observed latency : %.1f ns/order\n"
                     "Median throughput        : %.4f million orders/second\n"
                     "\n"
                     "Stability\n"
                     "--------------------------------------------------------------------------------\n"
                     "P50 variation (CV)       : %.2f %%\n"
                     "P99 variation (CV)       : %.2f %%\n"
                     "P99.9 variation (CV)     : %.2f %%\n"
                     "Throughput variation (CV): %.2f %%\n"
                     "Stability verdict        : %s\n"
                     "\n"
                     "Correctness and Resources\n"
                     "--------------------------------------------------------------------------------\n"
                     "Processed messages   : %zu\n"
                     "Dropped messages     : %llu\n"
                     "Invalid fills        : 0 (structural guarantee, see matcher.hpp's zero-qty "
                     "fill guards; not a live runtime check this pass)\n"
                     "State hash           : %s\n"
                     "Reference comparison : NOT_RUN (no independent reference matcher wired up "
                     "this pass -- see docs/BENCHMARK_METHODOLOGY.md)\n"
                     "Determinism          : %s\n"
                     "Pool exhaustion      : %s\n"
                     "Arena fallback       : %s\n"
                     "Unexpected allocation: %llu\n"
                     "Correctness verdict  : %s\n",
                     median_p50, median_p90, median_p99, median_p999, median_max,
                     median_throughput / 1'000'000.0, p50_cv, p99_cv, p999_cv, throughput_cv,
                     stable ? "STABLE" : "UNSTABLE", MEASURED_ITERATIONS * cfg.trials,
                     static_cast<unsigned long long>(total_dropped_events),
                     hex64(state_hashes.front()).c_str(), determinism_pass ? "PASS" : "FAIL",
                     total_pool_exhaustions == 0 ? "NO" : "YES",
                     total_arena_fallbacks == 0 ? "NO" : "YES",
                     static_cast<unsigned long long>(total_arena_fallbacks),
                     overall_valid ? "PASS" : "FAIL");

        const std::string summary_csv_path = cfg.output_path;
        const std::string raw_csv_path = derive_path(cfg.output_path, ".raw");
        // Extension replaced (not preserved-with-insertion, unlike
        // derive_path() above) -- "bench.csv" -> "bench.meta.json", not
        // "bench.meta.csv.json".
        const std::size_t out_dot = cfg.output_path.find_last_of('.');
        const std::string meta_json_path =
            (out_dot == std::string::npos ? cfg.output_path : cfg.output_path.substr(0, out_dot)) +
            ".meta.json";

        std::fprintf(stdout,
                     "\n"
                     "Artifacts\n"
                     "--------------------------------------------------------------------------------\n"
                     "Summary CSV          : %s\n"
                     "Raw trial CSV        : %s\n"
                     "Metadata JSON        : %s\n"
                     "================================================================================\n",
                     summary_csv_path.c_str(), raw_csv_path.c_str(), meta_json_path.c_str());

        if (cfg.mode == MatchingMode::PRO_RATA)
        {
            const TrialStats &s0 = trial_stats.front();
            std::fprintf(stdout,
                         "\n"
                         "Pro-Rata Detail\n"
                         "--------------------------------------------------------------------------------\n"
                         "Eligible orders examined   : %llu\n"
                         "Mean eligible orders/match : %.3f\n"
                         "Mean generated fills/match : %.3f\n"
                         "Maximum fill fan-out       : %llu\n"
                         "Remainder units allocated  : N/A (not separately instrumented this pass -- "
                         "see docs/BENCHMARK_METHODOLOGY.md)\n"
                         "Allocation invariant errors: 0 (verified structurally + by unit test, not a "
                         "live per-fill runtime check)\n"
                         "Mean latency/generated fill: %.2f ns/fill\n"
                         "Determinism validation     : %s\n"
                         "================================================================================\n",
                         static_cast<unsigned long long>(s0.eligible_orders_examined),
                         s0.crossing_order_count > 0
                             ? static_cast<double>(s0.eligible_orders_examined) /
                                   static_cast<double>(s0.crossing_order_count)
                             : 0.0,
                         s0.mean_fills_per_match,
                         static_cast<unsigned long long>(s0.max_fill_fanout),
                         s0.mean_fill_publish_ns, determinism_pass ? "PASS" : "FAIL");
        }

        if (cfg.verbose)
        {
            std::fprintf(stdout,
                         "\n"
                         "Verbose Diagnostics\n"
                         "--------------------------------------------------------------------------------\n"
                         "Dataset seed             : %llu\n"
                         "Final book depth         : %zu bid level(s), %zu ask level(s)\n"
                         "Max pacing schedule drift: %llu ns\n"
                         "Queue push latency (mean): %.2f ns\n"
                         "Queue push latency (max) : %llu ns\n"
                         "Cancel samples/trial     : %zu\n"
                         "Cancel P50/P99/P99.9     : %.1f / %.1f / %.1f ns\n",
                         static_cast<unsigned long long>(dataset_seed), final_bid_levels,
                         final_ask_levels,
                         static_cast<unsigned long long>(max_schedule_drift_ns),
                         push_count_all > 0 ? push_ns_sum_all / static_cast<double>(push_count_all)
                                             : 0.0,
                         static_cast<unsigned long long>(push_ns_max_all),
                         trial_stats.front().cancel_count, trial_stats.front().cancel_pct.p50,
                         trial_stats.front().cancel_pct.p99, trial_stats.front().cancel_pct.p999);
            for (std::size_t t = 0; t < trial_stats.size(); ++t)
            {
                const TrialStats &s = trial_stats[t];
                std::fprintf(stdout,
                             "Trial %zu: dropped=%llu arena_fallback_delta=%llu "
                             "pool_exhaustion_delta=%llu state_hash=%s\n",
                             t, static_cast<unsigned long long>(s.dropped_events),
                             static_cast<unsigned long long>(s.arena_fallback_delta),
                             static_cast<unsigned long long>(s.pool_exhaustion_delta),
                             hex64(s.state_hash).c_str());
            }
        }

        write_summary_csv(summary_csv_path, run_id, cfg.mode, cfg.dataset_path, dataset_hash,
                          events.size(), clock_overhead_ns, trial_stats, state_hashes,
                          determinism_pass, overall_valid, invalid_reason_str);
        write_raw_csv(raw_csv_path, trial_records);
        write_metadata_json(meta_json_path, run_id, cfg.mode, env_snapshot, cfg.dataset_path,
                            dataset_hash,
#if HYDRA_LOB_DIRTY
                            true,
#else
                            false,
#endif
                            cfg.allow_dirty, overall_valid, invalid_reason_str);

        (void)mode_str_short;
    }

} // namespace hydra
