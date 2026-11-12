// Type layout, default-state, and configuration invariants.

#include "hydra/config.hpp"
#include "hydra/types.hpp"

#include "test_harness.hpp"

#include <cstddef>
#include <type_traits>

namespace hydra::test
{
    namespace
    {

        void test_order_layout()
        {
            HYDRA_CHECK_EQ(sizeof(Order), std::size_t{128});
            HYDRA_CHECK_EQ(alignof(Order), std::size_t{64});
            HYDRA_CHECK(std::is_trivially_copyable_v<Order>);
            HYDRA_CHECK(sizeof(Order) % 64 == 0);

            // Hot fields end within the first cache line.
            const std::size_t hot_boundary = offsetof(Order, next_) + sizeof(Order::next_);
            HYDRA_CHECK(hot_boundary <= 64);

            // Cold fields start exactly at the second cache line.
            HYDRA_CHECK_EQ(offsetof(Order, timestamp_ns), std::size_t{64});
        }

        void test_order_default_state()
        {
            // Value-initialization clears the intrusive list links.
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
        }

        void test_config_benchmark_constants()
        {
            HYDRA_CHECK(WARMUP_ITERATIONS > 0);
            HYDRA_CHECK(MEASURED_ITERATIONS > 0);
            HYDRA_CHECK(DEFAULT_TRIAL_COUNT > 0);

            // A replay dataset covers warmup plus measurement.
            HYDRA_CHECK_EQ(DEFAULT_DATASET_ORDER_COUNT, WARMUP_ITERATIONS + MEASURED_ITERATIONS);
        }

        void test_config_dataset_defaults()
        {
            // Defaults satisfy DatasetGenerator validation.
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
