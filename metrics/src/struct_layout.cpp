// metrics/src/struct_layout.cpp
//
// Metric C1: cache-line layout facts for Order/Level/FillEvent.
//
// No measurement here -- these are compile-time facts (sizeof/alignof/
// offsetof) about the actual current struct definitions in types.hpp,
// printed so the report's cache-line diagram is always generated from the
// real, current layout rather than a hand-drawn picture that can silently
// drift out of sync with the code.
//
// Output: a small line-oriented "key=value" format to stdout (simpler to
// parse from Python than JSON without pulling in a JSON library on the
// C++ side).

#include "hydra/types.hpp"

#include <cstddef>
#include <cstdio>

using namespace hydra;

int main()
{
    std::printf("order_size=%zu\n", sizeof(Order));
    std::printf("order_align=%zu\n", alignof(Order));
    std::printf("order_field=order_id,%zu,%zu\n", offsetof(Order, order_id), sizeof(Order::order_id));
    std::printf("order_field=price,%zu,%zu\n", offsetof(Order, price), sizeof(Order::price));
    std::printf("order_field=qty,%zu,%zu\n", offsetof(Order, qty), sizeof(Order::qty));
    std::printf("order_field=side,%zu,%zu\n", offsetof(Order, side), sizeof(Order::side));
    std::printf("order_field=tif,%zu,%zu\n", offsetof(Order, tif), sizeof(Order::tif));
    std::printf("order_field=hot_padding,%zu,%zu\n", offsetof(Order, hot_padding), sizeof(Order::hot_padding));
    std::printf("order_field=prev_,%zu,%zu\n", offsetof(Order, prev_), sizeof(Order::prev_));
    std::printf("order_field=next_,%zu,%zu\n", offsetof(Order, next_), sizeof(Order::next_));
    std::printf("order_field=timestamp_ns,%zu,%zu\n", offsetof(Order, timestamp_ns), sizeof(Order::timestamp_ns));
    std::printf("order_field=client_id,%zu,%zu\n", offsetof(Order, client_id), sizeof(Order::client_id));
    std::printf("order_field=client_tag,%zu,%zu\n", offsetof(Order, client_tag), sizeof(Order::client_tag));
    std::printf("order_field=cold_padding,%zu,%zu\n", offsetof(Order, cold_padding), sizeof(Order::cold_padding));

    std::printf("level_size=%zu\n", sizeof(Level));
    std::printf("level_align=%zu\n", alignof(Level));
    std::printf("level_field=price,%zu,%zu\n", offsetof(Level, price), sizeof(Level::price));
    std::printf("level_field=total_qty,%zu,%zu\n", offsetof(Level, total_qty), sizeof(Level::total_qty));
    std::printf("level_field=order_count,%zu,%zu\n", offsetof(Level, order_count), sizeof(Level::order_count));
    std::printf("level_field=head_,%zu,%zu\n", offsetof(Level, head_), sizeof(Level::head_));
    std::printf("level_field=tail_,%zu,%zu\n", offsetof(Level, tail_), sizeof(Level::tail_));

    std::printf("fillevent_size=%zu\n", sizeof(FillEvent));
    std::printf("fillevent_field=maker_order_id,%zu,%zu\n", offsetof(FillEvent, maker_order_id), sizeof(FillEvent::maker_order_id));
    std::printf("fillevent_field=taker_order_id,%zu,%zu\n", offsetof(FillEvent, taker_order_id), sizeof(FillEvent::taker_order_id));
    std::printf("fillevent_field=price,%zu,%zu\n", offsetof(FillEvent, price), sizeof(FillEvent::price));
    std::printf("fillevent_field=timestamp_ns,%zu,%zu\n", offsetof(FillEvent, timestamp_ns), sizeof(FillEvent::timestamp_ns));
    std::printf("fillevent_field=qty,%zu,%zu\n", offsetof(FillEvent, qty), sizeof(FillEvent::qty));

    return 0;
}
