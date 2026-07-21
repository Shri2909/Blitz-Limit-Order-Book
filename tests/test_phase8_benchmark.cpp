//===----------------------------------------------------------------------===
// tests/test_phase8_benchmark.cpp
//
// Phase 8 exit condition (see HYDRA-LOB-Roadmap.md):
//   "5-trial run produces CSV with the exact columns; median-P99/variance
//   summary present; confirmed to drive orders through the real
//   RX->SPSC->matching pipeline in both live-generation and dataset-replay
//   modes; a dataset generated via hydra_gen_dataset with a fixed seed
//   reproduces byte-identical files across invocations."
//
// WHY this file links src/benchmark.cpp AND src/pipeline.cpp (see
// CMakeLists.txt's blitz_lob_test_phase8_benchmark target): run_benchmark()
// is defined in benchmark.cpp (a real .cpp, not header-only) and internally
// spawns matching_thread_fn, defined in the separate pipeline.cpp -- both
// translation units are needed to link.
//
// WHY the run_benchmark() test gracefully skips instead of asserting a
// hard pass/fail on this specific machine: run_benchmark() correctly
// refuses to run (throwing before any measurement work) unless the host is
// a properly isolated, tuned benchmark machine (isolcpus=, performance
// governor, SMT off -- see config.hpp's ENVIRONMENT_REQUIREMENTS). The
// roadmap's own Phase 11 CI section explicitly excludes the real benchmark
// from CI runners for exactly this reason ("both need root, isolated
// cores, and specific kernel features a shared runner won't have"). This
// test still attempts the real call -- on a correctly tuned machine it
// runs for real and validates the CSV; anywhere else, the preflight
// rejection *is* the correct, tested behavior, not a gap.
//===----------------------------------------------------------------------===

#include "hydra/benchmark.hpp"
#include "hydra/dataset_generator.hpp"
#include "hydra/order_book.hpp"

#include "test_harness.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace hydra::test
{
    namespace
    {

        [[nodiscard]] DatasetConfig make_small_config(uint64_t seed, std::size_t order_count)
        {
            DatasetConfig cfg{};
            cfg.seed = seed;
            cfg.order_count = order_count;
            cfg.mid_price = 100'000;
            cfg.price_spread_ticks = 500;
            cfg.min_qty = 1;
            cfg.max_qty = 1000;
            cfg.cancel_ratio = 0.15;
            cfg.arrival_rate_hz = 100'000.0;
            cfg.ioc_ratio = 0.05;
            cfg.fok_ratio = 0.02;
            return cfg;
        }

        [[nodiscard]] std::vector<char> read_whole_file(const std::string &path)
        {
            std::ifstream in(path, std::ios::binary);
            HYDRA_CHECK(static_cast<bool>(in));
            return std::vector<char>(std::istreambuf_iterator<char>(in),
                                     std::istreambuf_iterator<char>());
        }

        // Roadmap's dataset-determinism validation, run as an actual
        // in-process check rather than trusted from the earlier CLI
        // spot-check: generate with a fixed seed twice, independently, and
        // confirm the written files are byte-identical.
        void test_dataset_generation_is_byte_identical_with_same_seed()
        {
            const auto dir = std::filesystem::temp_directory_path();
            const std::string path_a = (dir / "hydra_phase8_test_a.bin").string();
            const std::string path_b = (dir / "hydra_phase8_test_b.bin").string();

            const DatasetConfig cfg = make_small_config(42, 2000);

            {
                DatasetGenerator gen_a(cfg);
                gen_a.write_to_file(gen_a.generate(), path_a);
            }
            {
                DatasetGenerator gen_b(cfg);
                gen_b.write_to_file(gen_b.generate(), path_b);
            }

            const auto bytes_a = read_whole_file(path_a);
            const auto bytes_b = read_whole_file(path_b);
            HYDRA_CHECK_EQ(bytes_a.size(), bytes_b.size());
            HYDRA_CHECK(bytes_a == bytes_b);

            std::filesystem::remove(path_a);
            std::filesystem::remove(path_b);
        }

        // Closes the loophole the previous test alone can't rule out: a
        // generator that ignores its seed entirely would also produce
        // "identical" output every time, incorrectly passing the test
        // above. Confirm the seed actually matters.
        void test_dataset_different_seed_produces_different_output()
        {
            DatasetGenerator gen_a(make_small_config(1, 2000));
            DatasetGenerator gen_b(make_small_config(2, 2000));

            const auto events_a = gen_a.generate();
            const auto events_b = gen_b.generate();

            bool any_difference = false;
            for (std::size_t i = 0; i < events_a.size() && i < events_b.size(); ++i)
            {
                if (events_a[i].type != events_b[i].type ||
                    events_a[i].order.order_id != events_b[i].order.order_id ||
                    events_a[i].arrival_offset_ns != events_b[i].arrival_offset_ns)
                {
                    any_difference = true;
                    break;
                }
            }
            HYDRA_CHECK(any_difference);
        }

        void test_dataset_write_read_round_trip_preserves_events()
        {
            const auto dir = std::filesystem::temp_directory_path();
            const std::string path = (dir / "hydra_phase8_test_roundtrip.bin").string();

            DatasetGenerator gen(make_small_config(7, 1000));
            const auto original = gen.generate();
            gen.write_to_file(original, path);

            const auto read_back = DatasetGenerator::read_from_file(path);
            HYDRA_CHECK_EQ(read_back.size(), original.size());

            for (std::size_t i = 0; i < original.size(); ++i)
            {
                HYDRA_CHECK(read_back[i].type == original[i].type);
                HYDRA_CHECK_EQ(read_back[i].arrival_offset_ns, original[i].arrival_offset_ns);
                if (original[i].type == EventType::NEW_ORDER)
                {
                    HYDRA_CHECK_EQ(read_back[i].order.order_id, original[i].order.order_id);
                    HYDRA_CHECK_EQ(read_back[i].order.price, original[i].order.price);
                    HYDRA_CHECK_EQ(read_back[i].order.qty, original[i].order.qty);
                    HYDRA_CHECK(read_back[i].order.side == original[i].order.side);
                    HYDRA_CHECK(read_back[i].order.tif == original[i].order.tif);
                }
                else
                {
                    HYDRA_CHECK_EQ(read_back[i].cancel_order_id, original[i].cancel_order_id);
                }
            }

            std::filesystem::remove(path);
        }

        void test_dataset_config_validation_rejects_bad_ratios()
        {
            DatasetConfig cfg = make_small_config(1, 100);
            cfg.ioc_ratio = 0.6;
            cfg.fok_ratio = 0.6; // sums > 1.0

            bool threw = false;
            try
            {
                DatasetGenerator gen(cfg);
                (void)gen;
            }
            catch (const std::invalid_argument &)
            {
                threw = true;
            }
            HYDRA_CHECK(threw);
        }

        // Cross-phase integration check: replay the generated event
        // sequence against a REAL OrderBook (Phase 5's component) and
        // confirm every CANCEL event's cancel_order_id resolves against a
        // still-resting order at the moment it fires -- validates the
        // generator's "currently resting" bookkeeping end to end, not just
        // in isolation.
        void test_dataset_cancels_always_resolve_against_a_real_order_book()
        {
            DatasetGenerator gen(make_small_config(99, 5000));
            const auto events = gen.generate();

            auto order_pool = std::make_unique<ObjectPool<Order, ORDER_POOL_SIZE>>();
            auto level_pool = std::make_unique<ObjectPool<Level, LEVEL_POOL_SIZE>>();
            auto book = std::make_unique<OrderBook>(*order_pool, *level_pool);

            std::size_t new_order_count = 0;
            std::size_t cancel_count = 0;
            for (const DatasetEvent &ev : events)
            {
                if (ev.type == EventType::NEW_ORDER)
                {
                    ++new_order_count;
                    if (ev.order.tif == TimeInForce::GTC)
                    {
                        // IOC/FOK events are never added as resting orders
                        // in the real pipeline either -- only GTC rests.
                        HYDRA_CHECK(book->add_order(ev.order) != nullptr);
                    }
                }
                else
                {
                    ++cancel_count;
                    HYDRA_CHECK(book->cancel_order(ev.cancel_order_id));
                }
            }
            HYDRA_CHECK(new_order_count > 0);
            HYDRA_CHECK(cancel_count > 0);
        }

        // The core Phase 8 exit-condition test. See file header for why a
        // preflight rejection here is treated as a pass, not a failure.
        //
        // run_benchmark() requires a real dataset file (there is no
        // live-generation fallback -- see benchmark.hpp's file-level WHY),
        // so this test generates one sized to WARMUP_ITERATIONS +
        // MEASURED_ITERATIONS, exactly like a real invocation would.
        void test_run_benchmark_produces_valid_csv_when_environment_permits()
        {
            const auto dir = std::filesystem::temp_directory_path();
            const std::string csv_path = (dir / "hydra_phase8_bench.csv").string();
            const std::string dataset_path = (dir / "hydra_phase8_bench_dataset.bin").string();
            std::filesystem::remove(csv_path);
            std::filesystem::remove(dataset_path);

            {
                DatasetGenerator gen(make_small_config(42, WARMUP_ITERATIONS + MEASURED_ITERATIONS));
                gen.write_to_file(gen.generate(), dataset_path);
            }

            auto order_pool = std::make_unique<ObjectPool<Order, ORDER_POOL_SIZE>>();
            auto level_pool = std::make_unique<ObjectPool<Level, LEVEL_POOL_SIZE>>();
            auto book = std::make_unique<OrderBook>(*order_pool, *level_pool);
            auto matcher = std::make_unique<Matcher>(*book, MatchingMode::PRICE_TIME);
            auto queue = std::make_unique<SpscQueue<Order, SPSC_CAPACITY>>();

            PipelineContext ctx{
                .queue = *queue,
                .book = *book,
                .matcher = *matcher,
                .order_pool = *order_pool,
                .level_pool = *level_pool,
            };

            BenchmarkConfig cfg{
                .mode = MatchingMode::PRICE_TIME,
                .trials = 1, // a smoketest trial count on purpose -- this test
                             // only validates CSV shape, not a citable result
                .output_path = csv_path,
                .dataset_path = dataset_path,
            };

            bool ran_for_real = false;
            try
            {
                run_benchmark(cfg, ctx);
                ran_for_real = true;
            }
            catch (const std::runtime_error &e)
            {
                const std::string message = e.what();
                if (message.find("preflight") == std::string::npos)
                {
                    // Not the expected "this isn't an isolated benchmark
                    // machine" rejection -- something else broke.
                    std::fprintf(stderr, "    unexpected failure (not a preflight "
                                         "rejection): %s\n",
                                 message.c_str());
                    HYDRA_CHECK(false);
                }
                std::fprintf(stderr,
                             "    SKIPPED (asserted, not just observed): run_benchmark() "
                             "correctly refused to run -- this host isn't configured as "
                             "an isolated benchmark machine (isolcpus=/performance "
                             "governor/SMT-off; see config.hpp's "
                             "ENVIRONMENT_REQUIREMENTS). Preflight rejection: %s\n",
                             message.c_str());
            }

            if (!ran_for_real)
            {
                return;
            }

            // Reaching here means this IS a properly configured benchmark
            // machine -- validate the real artifacts it produced: the
            // summary CSV (one row per trial, header is the very first
            // line -- no leading '#' comment lines in the new schema),
            // the raw per-sample CSV, and the metadata JSON, all derived
            // from cfg.output_path (see derive_path() in benchmark.cpp).
            std::ifstream csv(csv_path);
            HYDRA_CHECK(static_cast<bool>(csv));
            std::string header_line;
            HYDRA_CHECK(static_cast<bool>(std::getline(csv, header_line)));
            HYDRA_CHECK_EQ(header_line,
                          std::string("run_id,commit_hash,trial,matching_mode,workload_type,"
                                      "dataset,dataset_hash,dataset_records,warmup_operations,"
                                      "measured_operations,producer_cpu,consumer_cpu,numa_node,"
                                      "clock_source,timestamp_overhead_ns,p50_ns_per_order,"
                                      "p90_ns_per_order,p99_ns_per_order,p999_ns_per_order,"
                                      "mean_ns_per_order,max_ns_per_order,"
                                      "throughput_orders_per_second,crossing_order_count,"
                                      "non_crossing_order_count,partial_fill_count,"
                                      "multi_level_sweep_count,fok_order_count,"
                                      "self_trade_skip_count,resting_orders_examined,"
                                      "eligible_orders_examined,generated_fill_count,"
                                      "mean_fills_per_match,max_fill_fanout,"
                                      "mean_levels_consumed,remainder_units_distributed,"
                                      "allocation_invariant_failures,dropped_events,"
                                      "arena_fallback_count,pool_exhaustion_count,"
                                      "unexpected_allocation_count,state_hash,"
                                      "reference_validation_status,determinism_status,valid,"
                                      "invalid_reason"));

            std::size_t data_row_count = 0;
            std::string row;
            while (std::getline(csv, row))
            {
                if (!row.empty())
                {
                    ++data_row_count;
                }
            }
            std::fprintf(stderr, "    summary CSV has %zu data row(s) after the header\n",
                         data_row_count);
            HYDRA_CHECK(data_row_count > 0);

            const std::string raw_csv_path =
                csv_path.substr(0, csv_path.find_last_of('.')) + ".raw.csv";
            const std::string meta_json_path =
                csv_path.substr(0, csv_path.find_last_of('.')) + ".meta.json";
            std::ifstream raw_csv(raw_csv_path);
            HYDRA_CHECK(static_cast<bool>(raw_csv));
            std::ifstream meta_json(meta_json_path);
            HYDRA_CHECK(static_cast<bool>(meta_json));
        }

    } // namespace
} // namespace hydra::test

int main()
{
    using namespace hydra::test;

    RUN_TEST(test_dataset_generation_is_byte_identical_with_same_seed);
    RUN_TEST(test_dataset_different_seed_produces_different_output);
    RUN_TEST(test_dataset_write_read_round_trip_preserves_events);
    RUN_TEST(test_dataset_config_validation_rejects_bad_ratios);
    RUN_TEST(test_dataset_cancels_always_resolve_against_a_real_order_book);
    RUN_TEST(test_run_benchmark_produces_valid_csv_when_environment_permits);

    return report_and_exit_code();
}
