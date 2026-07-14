//===----------------------------------------------------------------------===
// tests/test_phase1_types.cpp
//
// Phase 1 exit condition (see HYDRA-LOB-Roadmap.md):
//   "cmake -B build configures cleanly; types.hpp compiles standalone with
//   all static_asserts passing."
//
// This file intentionally includes ONLY hydra/types.hpp and hydra/config.hpp
// -- no other project header -- so a compile failure here means one of those
// two headers secretly depends on something being included first by whoever
// happens to include it, which the "compiles standalone" requirement rules
// out. Every static_assert already in types.hpp/config.hpp runs at compile
// time regardless of whether this file's main() ever executes; the runtime
// checks below re-verify the same invariants in a human-readable pass/fail
// report (and, for config.hpp's DEFAULT_* dataset constants, cross-check
// them against the exact validation rules DatasetGenerator's constructor
// enforces at runtime -- see dataset_generator.hpp -- so a drifted default
// that would make DatasetGenerator itself throw is caught here first).
//===----------------------------------------------------------------------===

#include "hydra/config.hpp"
#include "hydra/types.hpp"

#include "test_harness.hpp"

#include <cstddef>
#include <cstdio>
#include <type_traits>

namespace hydra::test
{
    namespace
    {

        void test_order_layout()
        {
            std::fprintf(stderr, "    sizeof(Order)=%zu alignof(Order)=%zu\n",
                         sizeof(Order), alignof(Order));

            HYDRA_CHECK_EQ(sizeof(Order), std::size_t{128});
            HYDRA_CHECK_EQ(alignof(Order), std::size_t{64});
            HYDRA_CHECK(std::is_trivially_copyable_v<Order>);
            HYDRA_CHECK(sizeof(Order) % 64 == 0);

            // Hot fields (order_id, price, qty, side, tif, prev_, next_) must
            // all fit inside the first 64-byte line -- this is the entire
            // point of the hot/cold split. next_ is the last hot field, so
            // its end offset is the boundary to check.
            const std::size_t hot_boundary =
                offsetof(Order, next_) + sizeof(Order::next_);
            HYDRA_CHECK(hot_boundary <= 64);

            // Cold fields (timestamp_ns, client_id, client_tag) must start
            // at exactly the second cache line, not bleed into the first.
            HYDRA_CHECK_EQ(offsetof(Order, timestamp_ns), std::size_t{64});
        }

        void test_order_default_state()
        {
            // OrderBook::append_to_level() relies on a freshly value-
            // constructed Order's prev_/next_ starting as nullptr (see
            // ObjectPool::acquire()'s placement-new T()) -- verify that
            // invariant directly here rather than trusting it implicitly.
            Order o{};
            HYDRA_CHECK(o.prev_ == nullptr);
            HYDRA_CHECK(o.next_ == nullptr);
            HYDRA_CHECK_EQ(o.order_id, uint64_t{0});
            HYDRA_CHECK_EQ(o.qty, uint32_t{0});
        }

        void test_side_and_tif_enums()
        {
            HYDRA_CHECK(sizeof(Side) == 1);
            HYDRA_CHECK(sizeof(TimeInForce) == 1);
            HYDRA_CHECK(Side::BUY != Side::SELL);
            HYDRA_CHECK(TimeInForce::GTC != TimeInForce::IOC);
            HYDRA_CHECK(TimeInForce::IOC != TimeInForce::FOK);
            HYDRA_CHECK(TimeInForce::GTC != TimeInForce::FOK);
        }

        void test_level_layout()
        {
            std::fprintf(stderr, "    sizeof(Level)=%zu alignof(Level)=%zu\n",
                         sizeof(Level), alignof(Level));

            HYDRA_CHECK_EQ(sizeof(Level), std::size_t{64});
            HYDRA_CHECK_EQ(alignof(Level), std::size_t{64});
            HYDRA_CHECK(sizeof(Level) % 64 == 0);
        }

        void test_level_default_state()
        {
            Level l{};
            HYDRA_CHECK(l.head_ == nullptr);
            HYDRA_CHECK(l.tail_ == nullptr);
            HYDRA_CHECK_EQ(l.total_qty, uint32_t{0});
            HYDRA_CHECK_EQ(l.order_count, uint32_t{0});
        }

        void test_fillevent_layout()
        {
            std::fprintf(stderr, "    sizeof(FillEvent)=%zu\n", sizeof(FillEvent));

            HYDRA_CHECK_EQ(sizeof(FillEvent), std::size_t{40});
            HYDRA_CHECK(std::is_trivially_copyable_v<FillEvent>);
        }

        void test_config_core_affinity()
        {
            HYDRA_CHECK(RX_CORE_ID >= 0);
            HYDRA_CHECK(MATCHING_CORE_ID >= 0);
            HYDRA_CHECK(RX_CORE_ID != MATCHING_CORE_ID);
        }

        void test_config_pool_sizes()
        {
            HYDRA_CHECK(SPSC_CAPACITY > 0);
            HYDRA_CHECK((SPSC_CAPACITY & (SPSC_CAPACITY - 1)) == 0);
            HYDRA_CHECK(ORDER_POOL_SIZE > 0);
            HYDRA_CHECK(LEVEL_POOL_SIZE > 0);
            HYDRA_CHECK(FILL_EVENT_POOL_SIZE > 0);
        }

        void test_config_benchmark_constants()
        {
            HYDRA_CHECK(WARMUP_ITERATIONS > 0);
            HYDRA_CHECK(MEASURED_ITERATIONS > 0);
            HYDRA_CHECK(DEFAULT_TRIAL_COUNT > 0);

            // config.hpp's own WHY comment documents this equality as load-
            // bearing for benchmark.cpp's replay mode -- catch drift if
            // either side is ever edited without the other.
            HYDRA_CHECK_EQ(DEFAULT_DATASET_ORDER_COUNT,
                           WARMUP_ITERATIONS + MEASURED_ITERATIONS);
        }

        void test_config_dataset_defaults()
        {
            // These mirror exactly the validation DatasetGenerator's
            // constructor performs at runtime (dataset_generator.hpp) -- if
            // a DEFAULT_* constant here ever drifted to a value
            // DatasetGenerator would reject, blitz_gen_dataset with zero
            // flags would throw immediately. Catch that here, at the config
            // layer, before it surfaces as a confusing tool-startup crash.
            HYDRA_CHECK(DEFAULT_MIN_QTY > 0);
            HYDRA_CHECK(DEFAULT_MIN_QTY <= DEFAULT_MAX_QTY);
            HYDRA_CHECK(DEFAULT_PRICE_SPREAD_TICKS >= 0);
            HYDRA_CHECK(DEFAULT_CANCEL_RATIO >= 0.0 && DEFAULT_CANCEL_RATIO <= 1.0);
            HYDRA_CHECK(DEFAULT_ARRIVAL_RATE_HZ > 0.0);
            HYDRA_CHECK(DEFAULT_IOC_RATIO >= 0.0);
            HYDRA_CHECK(DEFAULT_FOK_RATIO >= 0.0);
            HYDRA_CHECK(DEFAULT_IOC_RATIO + DEFAULT_FOK_RATIO <= 1.0);
        }

    } // namespace
} // namespace hydra::test

int main()
{
    using namespace hydra::test;

    RUN_TEST(test_order_layout);
    RUN_TEST(test_order_default_state);
    RUN_TEST(test_side_and_tif_enums);
    RUN_TEST(test_level_layout);
    RUN_TEST(test_level_default_state);
    RUN_TEST(test_fillevent_layout);
    RUN_TEST(test_config_core_affinity);
    RUN_TEST(test_config_pool_sizes);
    RUN_TEST(test_config_benchmark_constants);
    RUN_TEST(test_config_dataset_defaults);

    return report_and_exit_code();
}
