#pragma once

// Flat-layout ablation: 88-byte unaligned Order; other types match include/hydra/types.hpp.

#include <cstdint>
#include <type_traits>

namespace hydra
{

    enum class Side : uint8_t
    {
        BUY,
        SELL
    };

    enum class TimeInForce : uint8_t
    {
        GTC,
        IOC,
        FOK
    };

    enum class OrderEventTag : uint8_t
    {
        NEW_OR_CANCEL = 0,
        REPLACE = 1,
    };

    // Flat field layout without cache-line separation or alignment.
    struct Order
    {
        uint64_t order_id;
        int64_t price;
        uint32_t qty;
        Side side;
        TimeInForce tif;
        OrderEventTag event_tag;
        Order *prev_;
        Order *next_;

        uint64_t timestamp_ns;
        uint64_t client_id;
        char client_tag[32];
    };

    static_assert(sizeof(Order) == 88, "flat Order must keep the production field set (88 bytes)");
    static_assert(std::is_trivially_copyable_v<Order>,
                  "Order must be POD for by-value SPSC transport");

    struct alignas(64) Level
    {
        int64_t price;
        uint64_t total_qty;
        uint32_t order_count;
        Order *head_;
        Order *tail_;
    };

    static_assert(sizeof(Level) == 64, "Level must be cache-line aligned/sized");
    static_assert(sizeof(Level) % 64 == 0, "Level must be cache-line aligned/sized");
    static_assert(alignof(Level) == 64, "Level must be 64-byte aligned");

    struct FillEvent
    {
        uint64_t maker_order_id;
        uint64_t taker_order_id;
        int64_t price;
        uint64_t timestamp_ns;
        uint32_t qty;
    };

    // 36 bytes of fields plus 4 bytes of trailing alignment padding.
    static_assert(sizeof(FillEvent) == 40, "FillEvent layout changed");
    static_assert(std::is_trivially_copyable_v<FillEvent>,
                  "FillEvent must be POD for by-value transport");

    // Must match the production MatchStats.
    struct MatchStats
    {
        uint32_t fills_generated = 0;
        uint32_t levels_consumed = 0;
        uint32_t resting_orders_examined = 0;
        uint32_t eligible_orders_examined = 0;
        uint32_t self_trade_skips = 0;
        uint32_t remaining_qty = 0;
    };
    static_assert(std::is_trivially_copyable_v<MatchStats>);

} // namespace hydra
