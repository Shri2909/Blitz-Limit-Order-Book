#pragma once

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

    // Disambiguates what a wire-transported Order means when it reaches
    // matching_thread_fn, since the SPSC queue only ever carries Order
    // values (never a richer tagged-union event type). NEW_OR_CANCEL
    // preserves the original convention (qty == 0 means "cancel the order
    // named by order_id", anything else is a real new order); REPLACE means
    // "modify the resting order named by order_id to (price, qty)" -- see
    // Matcher::replace() for the priority-preserving vs. priority-losing
    // split this drives.
    enum class OrderEventTag : uint8_t
    {
        NEW_OR_CANCEL = 0,
        REPLACE = 1,
    };

    // WHY the hot/cold split below: fields touched on every match-path
    // access (order_id, price, qty, side, tif, event_tag, prev_/next_) are
    // separated from fields only touched for audit/reporting (timestamp_ns,
    // client_id, client_tag) into two distinct 64-byte lines, so the
    // matching hot path never drags cold/reporting-only bytes into cache
    // alongside the fields it actually needs. Target: perf stat IPC shift
    // from a ~1.2 baseline (flat, unseparated layout) to ~2.1 after this
    // split -- a design target and rationale, not a measured claim, until a
    // real perf-stat-capable host records an actually-measured number
    // against a real run (see docs/BENCHMARK_METHODOLOGY.md).
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

    static_assert(sizeof(Order) == 128,
                  "Order must be exactly two 64-byte cache lines (hot + cold); "
                  "if this fails, the hand-computed padding above no longer "
                  "matches the actual field layout (hot_padding is 25 bytes -- "
                  "23 data bytes [order_id/price/qty/side/tif/event_tag] + 25 "
                  "padding + 16 pointer bytes = 64 -- to leave room for the "
                  "prev_/next_ intrusive-list pointers used by "
                  "order_book.hpp's per-level FIFO) and must be recomputed, "
                  "not papered over with a bigger padding array.");
    static_assert(sizeof(Order) % 64 == 0,
                  "Order must be cache-line aligned/sized");
    static_assert(std::is_trivially_copyable_v<Order>,
                  "Order must be POD for by-value SPSC transport");
    static_assert(alignof(Order) == 64, "Order must be 64-byte aligned");

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

    // Returned by Matcher::match()/Matcher::replace() in place of a raw fill
    // count, so the benchmark harness (and any other caller) can report
    // "eligible orders examined/match" and "levels consumed/order" precisely
    // instead of approximating them from externally-observable fill events
    // alone -- see docs/BENCHMARK_METHODOLOGY.md for exactly what each field
    // counts and when (dry-run/FOK-availability traversals do NOT
    // contribute to these counters; only the real commit-pass sweep does).
    struct MatchStats
    {
        uint32_t fills_generated = 0;
        uint32_t levels_consumed = 0;
        // Every resting order visited during the sweep, whether eligible or
        // skipped as a self-trade. resting_orders_examined ==
        // eligible_orders_examined + self_trade_skips, always.
        uint32_t resting_orders_examined = 0;
        // Subset of resting_orders_examined that were NOT self-trade-skipped
        // (i.e. actually counted into eligible_total / could receive a fill).
        uint32_t eligible_orders_examined = 0;
        uint32_t self_trade_skips = 0;
        // Incoming quantity still unfilled when the sweep stopped (0 for a
        // fully-filled taker). Lets a caller classify partial vs. full
        // fills (fills_generated > 0 && remaining_qty > 0 == partial;
        // fills_generated > 0 && remaining_qty == 0 == full) without
        // needing to separately track the order's original quantity.
        uint32_t remaining_qty = 0;
    };
    static_assert(std::is_trivially_copyable_v<MatchStats>,
                  "MatchStats is a plain counters struct, no reason for it "
                  "not to be trivially copyable");

    static_assert(sizeof(FillEvent) == 40,
                  "FillEvent's reordered layout should total 36 bytes of fields "
                  "plus 4 bytes of trailing alignment padding; if this fails, "
                  "the member order/types above no longer match this comment "
                  "and one of them needs to be fixed to agree with the other");
    static_assert(std::is_trivially_copyable_v<FillEvent>,
                  "FillEvent must be POD for by-value pooled/queued transport");

} // namespace hydra