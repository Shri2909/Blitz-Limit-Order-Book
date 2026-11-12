#pragma once

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

    // Queue event semantics: NEW_OR_CANCEL with qty == 0 cancels order_id;
    // positive quantity submits an order. REPLACE modifies that live ID.
    enum class OrderEventTag : uint8_t
    {
        NEW_OR_CANCEL = 0,
        REPLACE = 1,
    };

    // Two cache lines separate core order fields and FIFO links from metadata.
    // client_id is on the second line and is read for self-trade checks.
    // See DESIGN.md for layout measurements.
    struct alignas(64) Order
    {
        uint64_t order_id;
        int64_t price;
        uint32_t qty;
        Side side;
        TimeInForce tif;
        OrderEventTag event_tag;
        uint8_t hot_padding[25];
        Order *prev_;
        Order *next_;

        uint64_t timestamp_ns;
        uint64_t client_id;
        char client_tag[32];
        uint8_t cold_padding[16];
    };

    static_assert(sizeof(Order) == 128, "Order must be two 64-byte cache lines (hot + cold); "
                                        "recompute hot_padding/cold_padding if fields change");
    static_assert(sizeof(Order) % 64 == 0, "Order must be cache-line aligned/sized");
    static_assert(std::is_trivially_copyable_v<Order>,
                  "Order must be POD for by-value SPSC transport");
    static_assert(alignof(Order) == 64, "Order must be 64-byte aligned");

    struct alignas(64) Level
    {
        int64_t price;
        // A level can hold many uint32_t order quantities, requiring a 64-bit sum.
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

    // Counters from the matching commit pass; FOK availability scans are excluded.
    struct MatchStats
    {
        uint32_t fills_generated = 0;
        uint32_t levels_consumed = 0;
        // Visited orders = eligible_orders_examined + self_trade_skips.
        uint32_t resting_orders_examined = 0;
        // Visited orders that are eligible for a fill after self-trade checks.
        uint32_t eligible_orders_examined = 0;
        uint32_t self_trade_skips = 0;
        // Incoming quantity still unfilled when the sweep stops.
        uint32_t remaining_qty = 0;
    };
    static_assert(std::is_trivially_copyable_v<MatchStats>);

    // 36 bytes of fields plus 4 bytes of trailing alignment padding.
    static_assert(sizeof(FillEvent) == 40, "FillEvent layout changed");
    static_assert(std::is_trivially_copyable_v<FillEvent>,
                  "FillEvent must be POD for by-value transport");

} // namespace hydra
