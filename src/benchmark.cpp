#include "hydra/benchmark.hpp"

#include "hydra/affinity.hpp"
#include "hydra/clock.hpp"
#include "hydra/config.hpp"
#include "hydra/dataset_generator.hpp"
#include "hydra/stats.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <functional>
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

// Set by CMake when the working tree had uncommitted changes at configure time.
#ifndef HYDRA_LOB_DIRTY
#define HYDRA_LOB_DIRTY 0
#endif

    namespace
    {

        constexpr bool kDirtyBuild = HYDRA_LOB_DIRTY != 0;
        constexpr const char *kSysfsCpuRoot = "/sys/devices/system/cpu";
        constexpr int kBoundedPushAttempts = 1000;
        constexpr double kStableTailCvPercent = 15.0; // heuristic, see METHODOLOGY.md
        constexpr const char *kRule =
            "--------------------------------------------------------------------------------\n";
        constexpr const char *kDoubleRule =
            "================================================================================\n";

        using ull = unsigned long long;

        // Preflight

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

        [[nodiscard]] std::string cpu_sysfs_path(int core, const char *leaf)
        {
            return std::string(kSysfsCpuRoot) + "/cpu" + std::to_string(core) + "/" + leaf;
        }

        // Parses the kernel CPU-list grammar ("0-3,8"); any malformed token yields nullopt.
        [[nodiscard]] std::optional<std::vector<int>> parse_cpu_list(const std::string &list)
        {
            if (list.empty() || list.back() == ',')
            {
                return std::nullopt;
            }
            const auto to_int = [](const std::string &s) -> std::optional<int>
            {
                if (s.empty() ||
                    !std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; }))
                {
                    return std::nullopt;
                }
                try
                {
                    return std::stoi(s);
                }
                catch (const std::exception &)
                {
                    return std::nullopt; // out of int range
                }
            };

            std::vector<int> result;
            std::stringstream ss(list);
            std::string token;
            while (std::getline(ss, token, ','))
            {
                if (token.empty())
                {
                    return std::nullopt;
                }
                const auto dash = token.find('-', 1);
                if (dash == std::string::npos)
                {
                    const auto v = to_int(token);
                    if (!v)
                    {
                        return std::nullopt;
                    }
                    result.push_back(*v);
                    continue;
                }
                const auto lo = to_int(token.substr(0, dash));
                const auto hi = to_int(token.substr(dash + 1));
                if (!lo || !hi || *lo > *hi)
                {
                    return std::nullopt;
                }
                for (int64_t c = *lo; c <= *hi; ++c)
                {
                    result.push_back(static_cast<int>(c));
                }
            }
            return result;
        }

        [[nodiscard]] PreflightResult check_core_isolated(int core)
        {
            const std::string path = std::string(kSysfsCpuRoot) + "/isolated";
            const auto content = read_sysfs_line(path);
            if (!content)
            {
                return {false, "cannot read " + path +
                                   " (is this a Linux system with isolcpus= support?)"};
            }
            if (content->empty())
            {
                return {false, "core " + std::to_string(core) + " is not isolated: " + path +
                                   " is empty (expected the isolcpus= GRUB parameter to include "
                                   "this core -- see config.hpp's ENVIRONMENT_REQUIREMENTS note)"};
            }
            const auto isolated = parse_cpu_list(*content);
            if (!isolated)
            {
                return {false, "malformed CPU list in " + path + ": '" + *content + "'"};
            }
            if (std::find(isolated->begin(), isolated->end(), core) == isolated->end())
            {
                return {false, "core " + std::to_string(core) +
                                   " is not in the isolated CPU list (" + *content + ")"};
            }
            return {true, ""};
        }

        [[nodiscard]] PreflightResult check_governor_performance(int core)
        {
            const std::string path = cpu_sysfs_path(core, "cpufreq/scaling_governor");
            const auto content = read_sysfs_line(path);
            if (!content)
            {
                return {false, "cannot read " + path + " (cpufreq not available for core " +
                                   std::to_string(core) + " on this system)"};
            }
            if (*content != "performance")
            {
                return {false, "core " + std::to_string(core) + "'s cpufreq governor is '" +
                                   *content +
                                   "', expected 'performance' (run: cpupower "
                                   "frequency-set --governor performance)"};
            }
            return {true, ""};
        }

        // A sibling hyperthread would share L1/L2 with the pinned core.
        [[nodiscard]] PreflightResult check_smt_off(int core)
        {
            const std::string path = cpu_sysfs_path(core, "topology/thread_siblings_list");
            const auto content = read_sysfs_line(path);
            if (!content)
            {
                return {false, "cannot read " + path};
            }
            const auto siblings = parse_cpu_list(*content);
            if (!siblings)
            {
                return {false, "malformed CPU list in " + path + ": '" + *content + "'"};
            }
            if (siblings->size() != 1 || siblings->front() != core)
            {
                return {false, "core " + std::to_string(core) + " has SMT sibling(s) (" + *content +
                                   "); SMT must be disabled (a sibling thread sharing L1/L2 with a "
                                   "pinned core reintroduces the cache contention isolcpus is "
                                   "meant to eliminate)"};
            }
            return {true, ""};
        }

        void run_preflight_checks()
        {
            const std::vector<PreflightResult> results = {
                check_core_isolated(RX_CORE_ID),
                check_core_isolated(MATCHING_CORE_ID),
                check_governor_performance(RX_CORE_ID),
                check_governor_performance(MATCHING_CORE_ID),
                check_smt_off(RX_CORE_ID),
                check_smt_off(MATCHING_CORE_ID),
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
                throw std::runtime_error("benchmark preflight checks failed -- see stderr for "
                                         "details; no measurement work was performed");
            }
        }

        [[nodiscard]] std::string capture_env_snapshot()
        {
            const auto get = [](const std::string &path)
            {
                const auto v = read_sysfs_line(path);
                return v ? *v : std::string("unavailable");
            };

            std::ostringstream out;
            out << "isolated=" << get(std::string(kSysfsCpuRoot) + "/isolated");
            const auto append_core = [&](const char *name, int core)
            {
                out << ' ' << name << "_core=" << core << ' ' << name
                    << "_governor=" << get(cpu_sysfs_path(core, "cpufreq/scaling_governor")) << ' '
                    << name << "_smt_siblings="
                    << get(cpu_sysfs_path(core, "topology/thread_siblings_list"));
            };
            append_core("rx", RX_CORE_ID);
            append_core("matching", MATCHING_CORE_ID);
            out << " git=" << HYDRA_LOB_VERSION << " dirty=" << (kDirtyBuild ? "yes" : "no");
            return out.str();
        }

        [[nodiscard]] std::string cpu_model_name()
        {
            std::ifstream in("/proc/cpuinfo");
            std::string line;
            while (in && std::getline(in, line))
            {
                if (line.rfind("model name", 0) != 0)
                {
                    continue;
                }
                const auto colon = line.find(':');
                if (colon != std::string::npos)
                {
                    std::string name = line.substr(colon + 1);
                    name.erase(0, name.find_first_not_of(' '));
                    return name;
                }
            }
            return "unavailable";
        }

        [[nodiscard]] const char *compiler_version() noexcept
        {
#if defined(__VERSION__)
            return __VERSION__;
#else
            return "unknown";
#endif
        }

        [[nodiscard]] const char *clock_source_name() noexcept
        {
            return detail::g_use_fallback_clock.load(std::memory_order_relaxed)
                       ? "steady_clock_fallback"
                       : "rdtscp_calibrated";
        }

        [[nodiscard]] const char *mode_id(MatchingMode mode) noexcept
        {
            return mode == MatchingMode::PRICE_TIME ? "price_time" : "pro_rata";
        }

        // Dataset provenance and formatting helpers

        // 64-bit FNV-1a; a reproducibility fingerprint, not cryptographic.
        class Fnv1a
        {
        public:
            void fold(uint64_t value) noexcept
            {
                hash_ ^= value;
                hash_ *= kPrime;
            }

            [[nodiscard]] uint64_t value() const noexcept { return hash_; }

        private:
            static constexpr uint64_t kPrime = 1099511628211ULL;
            uint64_t hash_ = 1469598103934665603ULL;
        };

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

        [[nodiscard]] uint64_t compute_dataset_hash(const std::string &path)
        {
            std::ifstream in(path, std::ios::binary);
            if (!in)
            {
                throw std::runtime_error("cannot open dataset '" + path + "' to hash it");
            }
            Fnv1a hash;
            char buf[65536];
            while (in)
            {
                in.read(buf, sizeof(buf));
                const std::streamsize n = in.gcount();
                for (std::streamsize i = 0; i < n; ++i)
                {
                    hash.fold(static_cast<uint8_t>(buf[i]));
                }
            }
            return hash.value();
        }

        [[nodiscard]] std::string hex64(uint64_t v)
        {
            std::ostringstream out;
            out << std::hex << std::setw(16) << std::setfill('0') << v;
            return out.str();
        }

        [[nodiscard]] std::string basename_of(const std::string &path)
        {
            const std::size_t slash = path.find_last_of('/');
            return (slash == std::string::npos) ? path : path.substr(slash + 1);
        }

        // RFC 4180 quoting.
        [[nodiscard]] std::string csv_escape(const std::string &s)
        {
            if (s.find_first_of(",\"\n\r") == std::string::npos)
            {
                return s;
            }
            std::string out = "\"";
            for (const char c : s)
            {
                if (c == '"')
                {
                    out += '"';
                }
                out += c;
            }
            out += '"';
            return out;
        }

        [[nodiscard]] std::string json_escape(const std::string &s)
        {
            std::string out;
            out.reserve(s.size() + 2);
            for (const char ch : s)
            {
                const unsigned char c = static_cast<unsigned char>(ch);
                switch (c)
                {
                case '"':
                    out += "\\\"";
                    break;
                case '\\':
                    out += "\\\\";
                    break;
                case '\n':
                    out += "\\n";
                    break;
                case '\r':
                    out += "\\r";
                    break;
                case '\t':
                    out += "\\t";
                    break;
                default:
                    if (c < 0x20)
                    {
                        char buf[8];
                        std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                        out += buf;
                    }
                    else
                    {
                        out += static_cast<char>(c);
                    }
                }
            }
            return out;
        }

        [[nodiscard]] double ratio_or_zero(double numerator, double denominator) noexcept
        {
            return denominator > 0.0 ? numerator / denominator : 0.0;
        }

        [[nodiscard]] int64_t current_unix_seconds() noexcept
        {
            return static_cast<int64_t>(std::chrono::duration_cast<std::chrono::seconds>(
                                            std::chrono::system_clock::now().time_since_epoch())
                                            .count());
        }

        // Replay

        // Mean cost of a back-to-back clock read, reported alongside every result.
        [[nodiscard]] double measure_clock_overhead_ns(double ns_per_cycle)
        {
            constexpr int kSamples = 10'000;
            uint64_t sum = 0;
            uint64_t prev = rdtsc_now();
            for (int i = 0; i < kSamples; ++i)
            {
                const uint64_t now = rdtsc_now();
                sum += now - prev;
                prev = now;
            }
            return (static_cast<double>(sum) / static_cast<double>(kSamples)) * ns_per_cycle;
        }

        [[nodiscard]] uint64_t timestamp_ns(double ns_per_cycle) noexcept
        {
            return static_cast<uint64_t>(static_cast<double>(rdtsc_now()) * ns_per_cycle);
        }

        [[nodiscard]] Order encode_as_queue_order(const DatasetEvent &ev, uint64_t t1_ns)
        {
            Order o{};
            o.timestamp_ns = t1_ns;
            switch (ev.type)
            {
            case EventType::NEW_ORDER:
                o = ev.order;
                o.event_tag = OrderEventTag::NEW_OR_CANCEL;
                o.timestamp_ns = t1_ns;
                break;
            case EventType::REPLACE:
                o.order_id = ev.cancel_order_id; // target order
                o.price = ev.order.price;
                o.qty = ev.order.qty;
                o.event_tag = OrderEventTag::REPLACE;
                break;
            case EventType::CANCEL:
                o.order_id = ev.cancel_order_id;
                o.qty = 0;
                o.event_tag = OrderEventTag::NEW_OR_CANCEL;
                break;
            }
            return o;
        }

        // Returns the duration of the successful push, or nullopt if the queue stayed full.
        [[nodiscard]] std::optional<uint64_t> push_bounded(PipelineContext &ctx, const Order &order,
                                                           double ns_per_cycle)
        {
            for (int attempt = 0; attempt < kBoundedPushAttempts; ++attempt)
            {
                const uint64_t start_ns = timestamp_ns(ns_per_cycle);
                if (ctx.queue.push(order))
                {
                    return timestamp_ns(ns_per_cycle) - start_ns;
                }
            }
            return std::nullopt;
        }

        struct ReplayPhaseResult
        {
            uint64_t dropped = 0;
            uint64_t max_drift_ns = 0;
            double push_ns_sum = 0.0;
            uint64_t push_ns_max = 0;
            std::size_t push_count = 0;
        };

        // Replays events[first, last) at their recorded arrival offsets, calling on_push()
        // after each successful enqueue.
        template <typename OnPush>
        [[nodiscard]] ReplayPhaseResult
        replay_phase(PipelineContext &ctx, const std::vector<DatasetEvent> &events,
                     std::size_t first, std::size_t last, double ns_per_cycle, OnPush &&on_push)
        {
            const uint64_t phase_start_ns = timestamp_ns(ns_per_cycle);
            const uint64_t base_offset_ns = events[first].arrival_offset_ns;
            ReplayPhaseResult result;

            for (std::size_t i = first; i < last; ++i)
            {
                const uint64_t target_ns =
                    phase_start_ns + (events[i].arrival_offset_ns - base_offset_ns);

                while (timestamp_ns(ns_per_cycle) < target_ns)
                {
                }

                const uint64_t t1_ns = timestamp_ns(ns_per_cycle);
                if (t1_ns > target_ns)
                {
                    result.max_drift_ns = std::max(result.max_drift_ns, t1_ns - target_ns);
                }

                const auto push_ns =
                    push_bounded(ctx, encode_as_queue_order(events[i], t1_ns), ns_per_cycle);
                if (!push_ns) [[unlikely]]
                {
                    ++result.dropped;
                    continue;
                }
                result.push_ns_sum += static_cast<double>(*push_ns);
                result.push_ns_max = std::max(result.push_ns_max, *push_ns);
                ++result.push_count;
                on_push();

                // Warm the next record (256 B, read once, sequentially, ~10 us from now)
                // while the producer is idle, so encode_as_queue_order() after t1 does not
                // take a DRAM miss on the 28 MB event array. Runs outside every measured
                // interval; it changes neither what t1..t_match_end spans nor the work
                // inside it, only whether that work hits cache.
                if (i + 1 < last)
                {
                    const char *next = reinterpret_cast<const char *>(&events[i + 1]);
                    for (std::size_t off = 0; off < sizeof(DatasetEvent); off += 64)
                    {
                        __builtin_prefetch(next + off, 0, 3);
                    }
                }
            }

            if (result.dropped > 0) [[unlikely]]
            {
                std::fprintf(stderr,
                             "benchmark: dropped %llu event(s) to sustained queue backpressure\n",
                             static_cast<ull>(result.dropped));
            }
            return result;
        }

        // Per-trial statistics

        struct TrialRecord
        {
            std::vector<LatencySample> samples;
            std::vector<int64_t> timestamps_unix;
        };

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
            uint64_t arena_fallback_delta = 0;
            uint64_t pool_exhaustion_delta = 0;

            // Repeatability fingerprints over counters, final book depth and ordered fills.
            uint64_t fill_digest = 0;
            uint64_t state_hash = 0;
        };

        [[nodiscard]] TrialStats compute_trial_stats(const TrialRecord &record,
                                                     double batch_seconds)
        {
            TrialStats t;
            std::vector<uint64_t> order_e2e;
            std::vector<uint64_t> cancel_e2e;
            order_e2e.reserve(record.samples.size());

            double fill_publish_sum = 0.0;
            uint64_t levels_sum = 0;
            Fnv1a fill_digest;

            for (const LatencySample &s : record.samples)
            {
                if (s.is_cancel)
                {
                    cancel_e2e.push_back(s.end_to_end_ns);
                    continue;
                }
                fill_digest.fold(s.fill_digest);
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
            t.mean_fills_per_match = ratio_or_zero(static_cast<double>(t.generated_fill_count),
                                                   static_cast<double>(t.crossing_order_count));
            t.mean_levels_consumed =
                ratio_or_zero(static_cast<double>(levels_sum), static_cast<double>(t.order_count));
            t.mean_fill_publish_ns =
                ratio_or_zero(fill_publish_sum, static_cast<double>(t.generated_fill_count));
            t.fill_digest = fill_digest.value();

            std::sort(order_e2e.begin(), order_e2e.end());
            std::sort(cancel_e2e.begin(), cancel_e2e.end());
            t.order_pct = stats::compute_percentiles(order_e2e);
            t.cancel_pct = stats::compute_percentiles(cancel_e2e);
            t.throughput_orders_per_sec =
                ratio_or_zero(static_cast<double>(t.order_count), batch_seconds);
            return t;
        }

        // Checks repeatability across trials, not correctness against a reference.
        [[nodiscard]] uint64_t compute_state_hash(const TrialStats &t, std::size_t bid_levels,
                                                  std::size_t ask_levels)
        {
            Fnv1a h;
            h.fold(bid_levels);
            h.fold(ask_levels);
            h.fold(t.crossing_order_count);
            h.fold(t.non_crossing_order_count);
            h.fold(t.partial_fill_count);
            h.fold(t.multi_level_sweep_count);
            h.fold(t.self_trade_skip_count);
            h.fold(t.resting_orders_examined);
            h.fold(t.eligible_orders_examined);
            h.fold(t.generated_fill_count);
            h.fold(t.max_fill_fanout);
            h.fold(t.order_count);
            h.fold(t.cancel_count);
            h.fold(t.fill_digest);
            return h.value();
        }

        // Event and time-in-force mix over the measured window.
        struct DatasetComposition
        {
            std::size_t new_order_count = 0;
            std::size_t cancel_count = 0;
            std::size_t replace_count = 0;
            std::size_t fok_count = 0;
            std::size_t ioc_count = 0;
            std::size_t gtc_count = 0;
        };

        [[nodiscard]] DatasetComposition
        compute_dataset_composition(const std::vector<DatasetEvent> &events, std::size_t first,
                                    std::size_t last)
        {
            DatasetComposition c;
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

        // Run state

        struct RunData
        {
            std::string run_id;
            std::string env_snapshot;
            uint64_t dataset_seed = 0;
            uint64_t dataset_hash = 0;
            std::size_t event_count = 0;
            DatasetComposition composition;
            double clock_overhead_ns = 0.0;
            bool is_smoketest = false;

            std::vector<TrialRecord> records;
            std::vector<TrialStats> trials;

            uint64_t total_dropped = 0;
            uint64_t total_arena_fallbacks = 0;
            uint64_t total_pool_exhaustions = 0;
            uint64_t max_schedule_drift_ns = 0;
            double push_ns_sum = 0.0;
            uint64_t push_ns_max = 0;
            std::size_t push_count = 0;
            std::size_t final_bid_levels = 0;
            std::size_t final_ask_levels = 0;

            bool determinism_pass = true;
            bool valid = true;
            std::string invalid_reason;
        };

        void absorb_replay_stats(RunData &run, const ReplayPhaseResult &r)
        {
            run.total_dropped += r.dropped;
            run.max_schedule_drift_ns = std::max(run.max_schedule_drift_ns, r.max_drift_ns);
        }

        void warn_on_resource_exhaustion(std::size_t trial, uint64_t arena_delta,
                                         uint64_t pool_delta)
        {
            if (arena_delta > 0)
            {
                std::fprintf(stderr,
                             "WARNING: order book's fixed arena overflowed to the heap %llu "
                             "time(s) during trial %zu.\n",
                             static_cast<ull>(arena_delta), trial);
            }
            if (pool_delta > 0)
            {
                std::fprintf(stderr,
                             "WARNING: order/level pool exhausted %llu time(s) during trial "
                             "%zu -- incoming orders/levels were silently dropped.\n",
                             static_cast<ull>(pool_delta), trial);
            }
        }

        void wait_for_matching_thread_startup(PipelineContext &ctx)
        {
            WorkerStartupState state = ctx.matching_thread_startup.load(std::memory_order_acquire);
            while (state == WorkerStartupState::STARTING)
            {
                std::this_thread::yield();
                state = ctx.matching_thread_startup.load(std::memory_order_acquire);
            }
            if (state != WorkerStartupState::READY)
            {
                throw std::runtime_error(
                    "benchmark: matching thread failed affinity startup; see the worker error "
                    "above");
            }
        }

        void wait_for_phase_completion(PipelineContext &ctx, uint64_t &barrier_sequence)
        {
            ++barrier_sequence;
            if (barrier_sequence == 0)
            {
                ++barrier_sequence;
            }

            Order barrier{};
            barrier.order_id = barrier_sequence;
            barrier.event_tag = OrderEventTag::PHASE_BARRIER;
            while (!ctx.queue.push(barrier))
            {
                std::this_thread::yield();
            }
            while (ctx.completed_phase_barrier.load(std::memory_order_acquire) != barrier_sequence)
            {
                std::this_thread::yield();
            }
        }

        // Warm-up replay, then a measured replay that the matching thread records into a
        // sink; trials after the first start from a freshly reset book and pools.
        void run_trial(PipelineContext &ctx, const std::vector<DatasetEvent> &events,
                       double ns_per_cycle, std::size_t trial, uint64_t &barrier_sequence,
                       RunData &run)
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

            const ReplayPhaseResult warmup =
                replay_phase(ctx, events, 0, WARMUP_ITERATIONS, ns_per_cycle, [] {});
            absorb_replay_stats(run, warmup);
            // An in-band barrier completes after all warmup events, including any
            // in-flight match, before the measured sink is attached.
            wait_for_phase_completion(ctx, barrier_sequence);

            TrialRecord record;
            record.timestamps_unix.reserve(MEASURED_ITERATIONS);
            // Fault the reserved pages in now: the first push_back into each 4 KiB page
            // otherwise takes a page fault on the producer thread inside the replay loop.
            record.timestamps_unix.resize(MEASURED_ITERATIONS);
            record.timestamps_unix.clear();
            std::vector<LatencySample> sink_storage(MEASURED_ITERATIONS);
            RawSampleSink sink;
            sink.samples = sink_storage.data();
            sink.capacity = MEASURED_ITERATIONS;
            ctx.raw_samples.store(&sink, std::memory_order_release);

            const auto batch_start = std::chrono::steady_clock::now();
            const ReplayPhaseResult measured =
                replay_phase(ctx, events, WARMUP_ITERATIONS,
                             WARMUP_ITERATIONS + MEASURED_ITERATIONS, ns_per_cycle, [&record]
                             { record.timestamps_unix.push_back(current_unix_seconds()); });
            absorb_replay_stats(run, measured);
            run.push_ns_sum += measured.push_ns_sum;
            run.push_ns_max = std::max(run.push_ns_max, measured.push_ns_max);
            run.push_count += measured.push_count;

            // The barrier follows every successful enqueue in this phase, so its ack
            // means all measured samples are complete and the queue is drained.
            wait_for_phase_completion(ctx, barrier_sequence);
            const std::size_t completed_samples = sink.count();
            ctx.raw_samples.store(nullptr, std::memory_order_release);
            if (completed_samples != record.timestamps_unix.size())
            {
                throw std::runtime_error(
                    "benchmark: processed-event and latency-sample counts diverged");
            }
            const double batch_seconds =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - batch_start)
                    .count();

            record.samples.assign(sink_storage.begin(),
                                  sink_storage.begin() +
                                      static_cast<std::ptrdiff_t>(record.timestamps_unix.size()));

            TrialStats ts = compute_trial_stats(record, batch_seconds);
            ts.dropped_events = measured.dropped;
            ts.arena_fallback_delta = ctx.book.arena_fallback_count() - arena_before;
            ts.pool_exhaustion_delta = (ctx.order_pool.exhaustion_count() - order_pool_before) +
                                       (ctx.level_pool.exhaustion_count() - level_pool_before);
            ts.state_hash =
                compute_state_hash(ts, ctx.book.bid_level_count(), ctx.book.ask_level_count());

            warn_on_resource_exhaustion(trial, ts.arena_fallback_delta, ts.pool_exhaustion_delta);

            run.trials.push_back(ts);
            run.records.push_back(std::move(record));
        }

        // Folds the cross-trial checks into run.valid / run.invalid_reason.
        void assess_validity(RunData &run, std::vector<std::string> reasons)
        {
            run.determinism_pass =
                std::all_of(run.trials.begin(), run.trials.end(), [&](const TrialStats &t)
                            { return t.state_hash == run.trials.front().state_hash; });
            if (!run.determinism_pass)
            {
                reasons.push_back("determinism check failed: state_hash differs across trials");
            }

            for (const TrialStats &t : run.trials)
            {
                run.total_arena_fallbacks += t.arena_fallback_delta;
                run.total_pool_exhaustions += t.pool_exhaustion_delta;
            }
            if (run.total_dropped > 0)
            {
                reasons.push_back("dropped_events > 0 (" + std::to_string(run.total_dropped) + ")");
            }
            if (run.total_arena_fallbacks > 0)
            {
                reasons.push_back("arena_fallback_count > 0 (" +
                                  std::to_string(run.total_arena_fallbacks) + ")");
            }
            if (run.total_pool_exhaustions > 0)
            {
                reasons.push_back("pool_exhaustion_count > 0 (" +
                                  std::to_string(run.total_pool_exhaustions) + ")");
            }
            if (detail::g_use_fallback_clock.load(std::memory_order_relaxed))
            {
                reasons.push_back(
                    "clock calibration fell back to steady_clock; calibrated-TSC results required");
            }

            run.valid = reasons.empty();
            run.invalid_reason.clear();
            for (std::size_t i = 0; i < reasons.size(); ++i)
            {
                run.invalid_reason += (i > 0 ? "; " : "") + reasons[i];
            }
        }

        // Cross-trial aggregates: medians and coefficients of variation (percent).
        struct TrialAggregate
        {
            double p50, p90, p99, p999, max, throughput;
            double p50_cv, p99_cv, p999_cv, throughput_cv;
            bool stable;
        };

        [[nodiscard]] TrialAggregate aggregate_trials(const std::vector<TrialStats> &trials)
        {
            const auto collect = [&](auto extract)
            {
                std::vector<double> v;
                v.reserve(trials.size());
                for (const TrialStats &t : trials)
                {
                    v.push_back(extract(t));
                }
                return v;
            };
            const auto p50 = collect([](const TrialStats &t) { return t.order_pct.p50; });
            const auto p90 = collect([](const TrialStats &t) { return t.order_pct.p90; });
            const auto p99 = collect([](const TrialStats &t) { return t.order_pct.p99; });
            const auto p999 = collect([](const TrialStats &t) { return t.order_pct.p999; });
            const auto max = collect([](const TrialStats &t) { return t.order_pct.max; });
            const auto tput =
                collect([](const TrialStats &t) { return t.throughput_orders_per_sec; });

            TrialAggregate a{};
            a.p50 = stats::median_across_trials(p50);
            a.p90 = stats::median_across_trials(p90);
            a.p99 = stats::median_across_trials(p99);
            a.p999 = stats::median_across_trials(p999);
            a.max = stats::median_across_trials(max);
            a.throughput = stats::median_across_trials(tput);
            a.p50_cv = stats::coefficient_of_variation(p50) * 100.0;
            a.p99_cv = stats::coefficient_of_variation(p99) * 100.0;
            a.p999_cv = stats::coefficient_of_variation(p999) * 100.0;
            a.throughput_cv = stats::coefficient_of_variation(tput) * 100.0;
            a.stable = (a.p99_cv < kStableTailCvPercent) && (a.p999_cv < kStableTailCvPercent);
            return a;
        }

        // Output paths

        struct ArtifactPaths
        {
            std::string summary_csv;
            std::string raw_csv;
            std::string meta_json;
        };

        // bench.csv -> bench.csv, bench.raw.csv, bench.meta.json
        [[nodiscard]] ArtifactPaths derive_artifact_paths(const std::string &base)
        {
            const std::size_t dot = base.find_last_of('.');
            const std::string stem = (dot == std::string::npos) ? base : base.substr(0, dot);
            const std::string ext = (dot == std::string::npos) ? "" : base.substr(dot);
            return {base, stem + ".raw" + ext, stem + ".meta.json"};
        }

        [[nodiscard]] std::ofstream open_for_write(const std::string &path)
        {
            std::ofstream out(path, std::ios::trunc);
            if (!out)
            {
                throw std::runtime_error("cannot open '" + path + "' for writing");
            }
            return out;
        }

        void check_write(const std::ofstream &out, const std::string &path)
        {
            if (!out)
            {
                throw std::runtime_error("write failed for '" + path + "'");
            }
        }

        void write_summary_csv(const std::string &path, const BenchmarkConfig &cfg,
                               const RunData &run)
        {
            std::ofstream out = open_for_write(path);
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
                   "reference_validation_status,determinism_status,valid,invalid_reason,"
                   "is_smoketest\n";

            const std::string workload_type = csv_escape(basename_of(cfg.dataset_path));
            const std::string dataset_field = csv_escape(cfg.dataset_path);
            const std::string invalid_reason_field = csv_escape(run.invalid_reason);
            constexpr int kNumaNode = 0;

            for (std::size_t t = 0; t < run.trials.size(); ++t)
            {
                const TrialStats &s = run.trials[t];
                out << run.run_id << ',' << HYDRA_LOB_VERSION << ',' << t << ','
                    << mode_id(cfg.mode) << ',' << workload_type << ',' << dataset_field << ','
                    << hex64(run.dataset_hash) << ',' << run.event_count << ',' << WARMUP_ITERATIONS
                    << ',' << MEASURED_ITERATIONS << ',' << RX_CORE_ID << ',' << MATCHING_CORE_ID
                    << ',' << kNumaNode << ',' << clock_source_name() << ','
                    << run.clock_overhead_ns << ',' << s.order_pct.p50 << ',' << s.order_pct.p90
                    << ',' << s.order_pct.p99 << ',' << s.order_pct.p999 << ',' << s.order_pct.mean
                    << ',' << s.order_pct.max << ',' << s.throughput_orders_per_sec << ','
                    << s.crossing_order_count << ',' << s.non_crossing_order_count << ','
                    << s.partial_fill_count << ',' << s.multi_level_sweep_count << ','
                    << run.composition.fok_count << ',' << s.self_trade_skip_count << ','
                    << s.resting_orders_examined << ',' << s.eligible_orders_examined << ','
                    << s.generated_fill_count << ',' << s.mean_fills_per_match << ','
                    << s.max_fill_fanout << ',' << s.mean_levels_consumed << ','
                    << ',' // remainder_units_distributed: not measured
                    << ',' // allocation_invariant_failures: not instrumented
                    << s.dropped_events << ',' << s.arena_fallback_delta << ','
                    << s.pool_exhaustion_delta << ','
                    << s.arena_fallback_delta // unexpected_allocation_count
                    << ',' << hex64(s.state_hash) << ',' << "NOT_RUN" << ','
                    << (run.determinism_pass ? "PASS" : "FAIL") << ',' << (run.valid ? 1 : 0) << ','
                    << invalid_reason_field << ',' << (run.is_smoketest ? "SMOKETEST" : "no")
                    << '\n';
            }
            check_write(out, path);
        }

        void write_raw_csv(const std::string &path, const std::vector<TrialRecord> &trials)
        {
            std::ofstream out = open_for_write(path);
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
            check_write(out, path);
        }

        void write_metadata_json(const std::string &path, const BenchmarkConfig &cfg,
                                 const RunData &run)
        {
            std::ofstream out = open_for_write(path);
            out << "{\n"
                << "  \"run_id\": \"" << json_escape(run.run_id) << "\",\n"
                << "  \"commit_hash\": \"" << json_escape(HYDRA_LOB_VERSION) << "\",\n"
                << "  \"working_tree_dirty\": " << (kDirtyBuild ? "true" : "false") << ",\n"
                << "  \"dirty_allowed\": " << (cfg.allow_dirty ? "true" : "false") << ",\n"
                << "  \"is_smoketest\": " << (run.is_smoketest ? "true" : "false") << ",\n"
                << "  \"matching_mode\": \"" << mode_id(cfg.mode) << "\",\n"
                << "  \"dataset_path\": \"" << json_escape(cfg.dataset_path) << "\",\n"
                << "  \"dataset_hash\": \"" << hex64(run.dataset_hash) << "\",\n"
                << "  \"cpu_model\": \"" << json_escape(cpu_model_name()) << "\",\n"
                << "  \"clock_source\": \"" << clock_source_name() << "\",\n"
                << "  \"compiler\": \"" << compiler_version() << "\",\n"
                << "  \"env_snapshot\": \"" << json_escape(run.env_snapshot) << "\",\n"
                << "  \"result_validity\": \"" << (run.valid ? "VALID" : "INVALID") << "\",\n"
                << "  \"invalid_reason\": \"" << json_escape(run.invalid_reason) << "\"\n"
                << "}\n";
            check_write(out, path);
        }

        // Console report

        void print_run_and_workload(const BenchmarkConfig &cfg, const RunData &run)
        {
            const TrialStats &first = run.trials.front();
            const std::string validity_suffix = run.valid ? "" : " (" + run.invalid_reason + ")";
            const char *working_tree =
                !kDirtyBuild ? "Clean"
                             : (cfg.allow_dirty ? "Dirty (allowed via --allow-dirty)" : "Dirty");
            const char *policy = (cfg.mode == MatchingMode::PRICE_TIME)
                                     ? "Price-Time Priority (FIFO)"
                                     : "Pro-Rata Allocation";

            std::printf("%s", kDoubleRule);
            std::printf("BLITZ LOB -- CANONICAL MATCHING BENCHMARK\n");
            std::printf("%s", kDoubleRule);
            std::printf("\nRun\n%s", kRule);
            std::printf("Policy              : %s\n"
                        "Commit              : %s\n"
                        "Working tree        : %s\n"
                        "Run ID              : %s\n"
                        "Validity            : %s%s\n",
                        policy, HYDRA_LOB_VERSION, working_tree, run.run_id.c_str(),
                        run.valid ? "VALID" : "INVALID", validity_suffix.c_str());

            std::printf("\nWorkload\n%s", kRule);
            std::printf("Dataset             : %s\n"
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
                        "Maximum fill fan-out: %llu\n",
                        cfg.dataset_path.c_str(), hex64(run.dataset_hash).c_str(), run.event_count,
                        WARMUP_ITERATIONS, MEASURED_ITERATIONS, run.composition.new_order_count,
                        run.composition.cancel_count, run.composition.replace_count,
                        run.composition.fok_count, run.composition.ioc_count,
                        run.composition.gtc_count, static_cast<ull>(first.crossing_order_count),
                        ratio_or_zero(100.0 * static_cast<double>(first.crossing_order_count),
                                      static_cast<double>(first.order_count)),
                        static_cast<ull>(first.generated_fill_count), first.mean_fills_per_match,
                        static_cast<ull>(first.max_fill_fanout));

            std::printf("\nEnvironment\n%s", kRule);
            std::printf("CPU model           : %s\n"
                        "Build               : Release\n"
                        "Compiler            : %s\n"
                        "NUMA node           : 0 (operator-bound via numactl, not self-verified)\n"
                        "Producer CPU        : %d\n"
                        "Consumer CPU        : %d\n"
                        "Preflight           : PASS (core isolation, governor, SMT -- see env "
                        "below)\n"
                        "Env                 : %s\n",
                        cpu_model_name().c_str(), compiler_version(), RX_CORE_ID, MATCHING_CORE_ID,
                        run.env_snapshot.c_str());

            std::printf("\nMeasurement\n%s", kRule);
            std::printf("Metric              : Matching-engine processing latency (end-to-end, "
                        "orders)\n"
                        "Start               : replay_phase()'s intended-arrival timestamp (t1)\n"
                        "End                 : matching_thread_fn's post-match() timestamp "
                        "(t_match_end)\n"
                        "Clock               : rdtscp, calibrated ns_per_cycle (clock.hpp)\n"
                        "Clock overhead      : %.2f ns/sample\n"
                        "Sample              : One processed input message\n"
                        "Trials              : %zu independent trials%s\n",
                        run.clock_overhead_ns, cfg.trials,
                        run.is_smoketest ? " [SMOKETEST -- not a citable result]" : "");
        }

        void print_trial_table(const std::vector<TrialStats> &trials)
        {
            std::printf("\nTrial Results\n%s", kRule);
            std::printf("Trial   P50       P90       P99       P99.9     Max       Throughput\n"
                        "        ns/order  ns/order  ns/order  ns/order  ns/order  M orders/s\n");
            std::printf("%s", kRule);
            for (std::size_t t = 0; t < trials.size(); ++t)
            {
                const TrialStats &s = trials[t];
                std::printf("%-8zu%-10.1f%-10.1f%-10.1f%-10.1f%-10.1f%-.4f\n", t, s.order_pct.p50,
                            s.order_pct.p90, s.order_pct.p99, s.order_pct.p999, s.order_pct.max,
                            s.throughput_orders_per_sec / 1'000'000.0);
            }
        }

        void print_canonical_result(const BenchmarkConfig &cfg, const RunData &run,
                                    const TrialAggregate &agg)
        {
            std::printf("\nCanonical Result -- Median Across Independent Trials\n%s", kRule);
            std::printf("P50 processing latency   : %.1f ns/order\n"
                        "P90 processing latency   : %.1f ns/order\n"
                        "P99 processing latency   : %.1f ns/order\n"
                        "P99.9 processing latency : %.1f ns/order\n"
                        "Maximum observed latency : %.1f ns/order\n"
                        "Median throughput        : %.4f million orders/second\n",
                        agg.p50, agg.p90, agg.p99, agg.p999, agg.max, agg.throughput / 1'000'000.0);

            std::printf("\nStability\n%s", kRule);
            std::printf("P50 variation (CV)       : %.2f %%\n"
                        "P99 variation (CV)       : %.2f %%\n"
                        "P99.9 variation (CV)     : %.2f %%\n"
                        "Throughput variation (CV): %.2f %%\n"
                        "Stability verdict        : %s\n",
                        agg.p50_cv, agg.p99_cv, agg.p999_cv, agg.throughput_cv,
                        agg.stable ? "STABLE" : "UNSTABLE");

            std::printf("\nCorrectness and Resources\n%s", kRule);
            std::printf(
                "Processed messages   : %zu\n"
                "Dropped messages     : %llu\n"
                "Invalid fills        : 0 (structural guarantee, see matcher.hpp's zero-qty "
                "fill guards; not a live runtime check this pass)\n"
                "State hash           : %s\n"
                "Reference comparison : NOT_RUN (no independent reference matcher wired up "
                "this pass -- see METHODOLOGY.md)\n"
                "Determinism          : %s\n"
                "Pool exhaustion      : %s\n"
                "Arena fallback       : %s\n"
                "Unexpected allocation: %llu\n"
                "Correctness verdict  : %s\n",
                MEASURED_ITERATIONS * cfg.trials, static_cast<ull>(run.total_dropped),
                hex64(run.trials.front().state_hash).c_str(),
                run.determinism_pass ? "PASS" : "FAIL",
                run.total_pool_exhaustions == 0 ? "NO" : "YES",
                run.total_arena_fallbacks == 0 ? "NO" : "YES",
                static_cast<ull>(run.total_arena_fallbacks), run.valid ? "PASS" : "FAIL");
        }

        void print_artifacts(const ArtifactPaths &paths)
        {
            std::printf("\nArtifacts\n%s", kRule);
            std::printf("Summary CSV          : %s\n"
                        "Raw trial CSV        : %s\n"
                        "Metadata JSON        : %s\n",
                        paths.summary_csv.c_str(), paths.raw_csv.c_str(), paths.meta_json.c_str());
            std::printf("%s", kDoubleRule);
        }

        void print_pro_rata_detail(const RunData &run)
        {
            const TrialStats &s0 = run.trials.front();
            std::printf("\nPro-Rata Detail\n%s", kRule);
            std::printf(
                "Eligible orders examined   : %llu\n"
                "Mean eligible orders/match : %.3f\n"
                "Mean generated fills/match : %.3f\n"
                "Maximum fill fan-out       : %llu\n"
                "Remainder units allocated  : N/A (not separately instrumented this pass -- "
                "see METHODOLOGY.md)\n"
                "Allocation invariant errors: 0 (verified structurally + by unit test, not a "
                "live per-fill runtime check)\n"
                "Mean latency/generated fill: %.2f ns/fill\n"
                "Determinism validation     : %s\n",
                static_cast<ull>(s0.eligible_orders_examined),
                ratio_or_zero(static_cast<double>(s0.eligible_orders_examined),
                              static_cast<double>(s0.crossing_order_count)),
                s0.mean_fills_per_match, static_cast<ull>(s0.max_fill_fanout),
                s0.mean_fill_publish_ns, run.determinism_pass ? "PASS" : "FAIL");
            std::printf("%s", kDoubleRule);
        }

        void print_verbose_diagnostics(const RunData &run)
        {
            const TrialStats &first = run.trials.front();
            std::printf("\nVerbose Diagnostics\n%s", kRule);
            std::printf("Dataset seed             : %llu\n"
                        "Final book depth         : %zu bid level(s), %zu ask level(s)\n"
                        "Max pacing schedule drift: %llu ns\n"
                        "Queue push latency (mean): %.2f ns\n"
                        "Queue push latency (max) : %llu ns\n"
                        "Cancel samples/trial     : %zu\n"
                        "Cancel P50/P99/P99.9     : %.1f / %.1f / %.1f ns\n",
                        static_cast<ull>(run.dataset_seed), run.final_bid_levels,
                        run.final_ask_levels, static_cast<ull>(run.max_schedule_drift_ns),
                        ratio_or_zero(run.push_ns_sum, static_cast<double>(run.push_count)),
                        static_cast<ull>(run.push_ns_max), first.cancel_count, first.cancel_pct.p50,
                        first.cancel_pct.p99, first.cancel_pct.p999);
            for (std::size_t t = 0; t < run.trials.size(); ++t)
            {
                const TrialStats &s = run.trials[t];
                std::printf("Trial %zu: dropped=%llu arena_fallback_delta=%llu "
                            "pool_exhaustion_delta=%llu state_hash=%s\n",
                            t, static_cast<ull>(s.dropped_events),
                            static_cast<ull>(s.arena_fallback_delta),
                            static_cast<ull>(s.pool_exhaustion_delta), hex64(s.state_hash).c_str());
            }
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
        if (cfg.trials == 0)
        {
            throw std::runtime_error("run_benchmark: --trials must be at least 1 (0 produces no "
                                     "measurement and is not a valid run).");
        }

        pin_to_core(RX_CORE_ID);
        verify_affinity(RX_CORE_ID);
        run_preflight_checks();

        RunData run;
        run.env_snapshot = capture_env_snapshot();

        std::vector<std::string> invalid_reasons;
        if constexpr (kDirtyBuild)
        {
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
        }

        run.is_smoketest = cfg.trials < DEFAULT_TRIAL_COUNT;
        if (run.is_smoketest)
        {
            std::fprintf(stderr,
                         "NOTE: --trials %zu is below the canonical trial count (%zu) -- "
                         "this run is a smoketest, not a citable result. The output CSV "
                         "is marked accordingly.\n",
                         cfg.trials, DEFAULT_TRIAL_COUNT);
        }

        ctx.matcher.set_mode(cfg.mode);

        run.dataset_seed = peek_dataset_seed(cfg.dataset_path);
        run.dataset_hash = compute_dataset_hash(cfg.dataset_path);
        const std::vector<DatasetEvent> events = DatasetGenerator::read_from_file(cfg.dataset_path);
        constexpr std::size_t kRequiredEvents = WARMUP_ITERATIONS + MEASURED_ITERATIONS;
        if (events.size() < kRequiredEvents)
        {
            throw std::runtime_error("dataset '" + cfg.dataset_path + "' has only " +
                                     std::to_string(events.size()) + " events, need at least " +
                                     std::to_string(kRequiredEvents) +
                                     " (WARMUP_ITERATIONS + MEASURED_ITERATIONS)");
        }
        run.event_count = events.size();
        run.composition = compute_dataset_composition(events, WARMUP_ITERATIONS, kRequiredEvents);

        const double ns_per_cycle = calibrate_ns_per_cycle();
        ctx.ns_per_cycle = ns_per_cycle;
        run.clock_overhead_ns = measure_clock_overhead_ns(ns_per_cycle);

        ctx.matching_thread_startup.store(WorkerStartupState::STARTING, std::memory_order_release);
        std::jthread matching_thread(matching_thread_fn, std::ref(ctx));
        wait_for_matching_thread_startup(ctx);

        run.records.reserve(cfg.trials);
        run.trials.reserve(cfg.trials);
        uint64_t barrier_sequence = ctx.completed_phase_barrier.load(std::memory_order_acquire);
        for (std::size_t trial = 0; trial < cfg.trials; ++trial)
        {
            run_trial(ctx, events, ns_per_cycle, trial, barrier_sequence, run);
        }

        run.final_bid_levels = ctx.book.bid_level_count();
        run.final_ask_levels = ctx.book.ask_level_count();
        assess_validity(run, std::move(invalid_reasons));
        run.run_id = std::string(HYDRA_LOB_VERSION) + "-" + mode_id(cfg.mode) + "-" +
                     std::to_string(current_unix_seconds());

        const TrialAggregate agg = aggregate_trials(run.trials);
        const ArtifactPaths paths = derive_artifact_paths(cfg.output_path);

        print_run_and_workload(cfg, run);
        print_trial_table(run.trials);
        print_canonical_result(cfg, run, agg);
        print_artifacts(paths);
        if (cfg.mode == MatchingMode::PRO_RATA)
        {
            print_pro_rata_detail(run);
        }
        if (cfg.verbose)
        {
            print_verbose_diagnostics(run);
        }

        write_summary_csv(paths.summary_csv, cfg, run);
        write_raw_csv(paths.raw_csv, run.records);
        write_metadata_json(paths.meta_json, cfg, run);
    }

} // namespace hydra
