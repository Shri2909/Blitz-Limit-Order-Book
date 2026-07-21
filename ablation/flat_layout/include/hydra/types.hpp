#pragma once

// ablation/flat_layout/include/hydra/types.hpp
//
// Rejected-alternative ablation for the hot/cold cache-line split
// (include/hydra/types.hpp): Order declared as the "obvious" flat layout --
// same fields, no hot/cold split, no hot_padding/cold_padding, no
// alignas(64). This shrinks sizeof(Order) from 128 to 88 bytes, which is
// why this ablation ALSO needs its own dataset_generator.hpp (Order is
// embedded in DatasetRecord, so the on-disk record size changes too -- see
// that file) and its own blitz_gen_dataset build
// (blitz_gen_dataset_ablation_flat_layout) to produce a compatible dataset
// file. Side/TimeInForce/Level/FillEvent are byte-for-byte identical to the
// real types.hpp -- only Order's layout is the ablated variable.
//
// See spsc_queue.hpp's ablation counterpart for the include-path-shadowing
// mechanism this relies on.

#include <cstdint>
#include <cstddef>
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

    // Kept identical to the real types.hpp (not part of what this ablation
    // isolates) -- see that file's own comment for why this field/enum
    // exists.
    enum class OrderEventTag : uint8_t
    {
        NEW_OR_CANCEL = 0,
        REPLACE = 1,
    };

    // Flat: no hot/cold split, no alignas(64) -- the "obvious" layout the
    // real types.hpp's own comment names as the rejected baseline.
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

    static_assert(sizeof(Order) == 88,
                  "flat-layout ablation Order should be 88 bytes -- if this "
                  "fails, the field set has drifted from the real "
                  "types.hpp's Order and this ablation no longer isolates "
                  "just the layout variable");
    static_assert(std::is_trivially_copyable_v<Order>,
                  "Order must be POD for by-value SPSC transport");

    struct alignas(64) Level
    {
        int64_t price;
        uint32_t total_qty;
        uint32_t order_count;
        Order *head_;
        Order *tail_;
    };

    static_assert(sizeof(Level) == 64,
                  "Level must be cache-line aligned/sized");
    static_assert(sizeof(Level) % 64 == 0,
                  "Level must be cache-line aligned/sized");
    static_assert(alignof(Level) == 64, "Level must be 64-byte aligned");

    struct FillEvent
    {
        uint64_t maker_order_id;
        uint64_t taker_order_id;
        int64_t price;
        uint64_t timestamp_ns;
        uint32_t qty;
    };

    static_assert(sizeof(FillEvent) == 40,
                  "FillEvent's reordered layout should total 36 bytes of fields "
                  "plus 4 bytes of trailing alignment padding");
    static_assert(std::is_trivially_copyable_v<FillEvent>,
                  "FillEvent must be POD for by-value pooled/queued transport");

    // Kept identical to the real types.hpp -- not part of what this
    // ablation isolates, but matcher.hpp (shared, unshadowed) requires it
    // to exist under this include path too.
    struct MatchStats
    {
        uint32_t fills_generated = 0;
        uint32_t levels_consumed = 0;
        uint32_t resting_orders_examined = 0;
        uint32_t eligible_orders_examined = 0;
        uint32_t self_trade_skips = 0;
        uint32_t remaining_qty = 0;
    };
    static_assert(std::is_trivially_copyable_v<MatchStats>,
                  "MatchStats is a plain counters struct, no reason for it "
                  "not to be trivially copyable");

} // namespace hydra
