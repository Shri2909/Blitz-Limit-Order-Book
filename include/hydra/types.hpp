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

    // WHY the hot/cold split below: fields touched on every match-path
    // access (order_id, price, qty, side, tif, prev_/next_) are separated
    // from fields only touched for audit/reporting (timestamp_ns, client_id,
    // client_tag) into two distinct 64-byte lines, so the matching hot path
    // never drags cold/reporting-only bytes into cache alongside the fields
    // it actually needs. Target: perf stat IPC shift from a ~1.2 baseline
    // (flat, unseparated layout) to ~2.1 after this split -- a design target
    // and rationale, not a measured claim, until Phase 12 records an
    // actually-measured number against a real run.
    struct alignas(64) Order
    {
        uint64_t order_id;
        int64_t price;
        uint32_t qty;
        Side side;
        TimeInForce tif;
        uint8_t hot_padding[26];
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
                  "matches the actual field layout (hot_padding is 26 bytes to "
                  "leave room for the prev_/next_ intrusive-list pointers used "
                  "by order_book.hpp's per-level FIFO) and must be recomputed, "
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

    static_assert(sizeof(FillEvent) == 40,
                  "FillEvent's reordered layout should total 36 bytes of fields "
                  "plus 4 bytes of trailing alignment padding; if this fails, "
                  "the member order/types above no longer match this comment "
                  "and one of them needs to be fixed to agree with the other");
    static_assert(std::is_trivially_copyable_v<FillEvent>,
                  "FillEvent must be POD for by-value pooled/queued transport");

} // namespace hydra