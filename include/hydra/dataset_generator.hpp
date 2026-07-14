#pragma once

// include/hydra/dataset_generator.hpp
//
// Deterministic, seeded synthetic order dataset generation. Decouples *what*
// orders get sent from *how* they get sent, so the exact same order
// sequence can be replayed across benchmark runs, across machines, or fed
// into regression tests, instead of regenerating a fresh random sequence
// inline every time benchmark.cpp runs.

#include "hydra/types.hpp"

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <random>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace hydra
{

    struct DatasetConfig
    {
        uint64_t seed;
        std::size_t order_count;
        int64_t mid_price;
        int64_t price_spread_ticks;
        uint32_t min_qty;
        uint32_t max_qty;
        double cancel_ratio;
        double arrival_rate_hz;
        double ioc_ratio;
        double fok_ratio;
    };
    static_assert(sizeof(DatasetConfig) == 72, "DatasetConfig layout drifted from the computed byte budget");
    static_assert(std::is_trivially_copyable_v<DatasetConfig>,
                  "DatasetConfig is written verbatim into the file header; it "
                  "must be trivially copyable for that memcpy/fwrite to be safe");

    enum class EventType : uint8_t
    {
        NEW_ORDER,
        CANCEL
    };

    struct DatasetEvent
    {
        EventType type;
        Order order;
        uint64_t cancel_order_id;
        uint64_t arrival_offset_ns;
    };

    struct DatasetFileHeader
    {
        char magic[8];
        uint32_t format_version;
        uint32_t _reserved0;
        uint64_t event_count;
        DatasetConfig config;
    };
    static_assert(sizeof(DatasetFileHeader) == 96, "DatasetFileHeader layout drifted from the computed byte budget");
    static_assert(std::is_trivially_copyable_v<DatasetFileHeader>,
                  "DatasetFileHeader must be trivially copyable to read/write "
                  "with a single memcpy/fread/fwrite of raw bytes");

    struct DatasetRecord
    {
        uint8_t event_type;
        uint8_t _reserved0[63];
        Order order;
        uint64_t cancel_order_id;
        uint64_t arrival_offset_ns;
    };
    static_assert(sizeof(DatasetRecord) == 256, "DatasetRecord layout drifted from the computed byte budget");
    static_assert(std::is_trivially_copyable_v<DatasetRecord>,
                  "DatasetRecord must be trivially copyable to read/write with "
                  "a single memcpy/fread/fwrite of raw bytes");

    class DatasetGenerator
    {
    public:
        explicit DatasetGenerator(const DatasetConfig &cfg)
            : cfg_(cfg), rng_(cfg.seed)
        {
            if (cfg_.ioc_ratio + cfg_.fok_ratio > 1.0)
            {
                throw std::invalid_argument(
                    "DatasetConfig: ioc_ratio + fok_ratio must be <= 1.0 (got " +
                    std::to_string(cfg_.ioc_ratio) + " + " +
                    std::to_string(cfg_.fok_ratio) + ")");
            }
            if (cfg_.ioc_ratio < 0.0 || cfg_.fok_ratio < 0.0 || cfg_.cancel_ratio < 0.0 ||
                cfg_.cancel_ratio > 1.0)
            {
                throw std::invalid_argument(
                    "DatasetConfig: ioc_ratio/fok_ratio/cancel_ratio must be "
                    "non-negative, and cancel_ratio must additionally be <= 1.0");
            }
            if (cfg_.min_qty == 0 || cfg_.min_qty > cfg_.max_qty)
            {
                throw std::invalid_argument(
                    "DatasetConfig: require 0 < min_qty <= max_qty");
            }
            if (cfg_.arrival_rate_hz <= 0.0)
            {
                throw std::invalid_argument("DatasetConfig: arrival_rate_hz must be > 0");
            }
        }

        [[nodiscard]] std::vector<DatasetEvent> generate()
        {
            std::vector<DatasetEvent> events;
            events.reserve(cfg_.order_count);

            std::uniform_int_distribution<int64_t> price_offset_dist(
                -cfg_.price_spread_ticks, cfg_.price_spread_ticks);
            std::uniform_int_distribution<uint32_t> qty_dist(cfg_.min_qty, cfg_.max_qty);
            std::uniform_real_distribution<double> unit_dist(0.0, 1.0);
            std::exponential_distribution<double> inter_arrival_dist(cfg_.arrival_rate_hz);

            uint64_t arrival_offset_ns = 0;

            for (std::size_t i = 0; i < cfg_.order_count; ++i)
            {
                const double dt_seconds = inter_arrival_dist(rng_);
                arrival_offset_ns += static_cast<uint64_t>(dt_seconds * 1e9);

                const bool want_cancel =
                    !resting_ids_.empty() && unit_dist(rng_) < cfg_.cancel_ratio;

                if (want_cancel)
                {
                    const std::size_t idx =
                        std::uniform_int_distribution<std::size_t>(0, resting_ids_.size() - 1)(rng_);
                    const uint64_t cancel_id = resting_ids_[idx];
                    remove_resting(cancel_id, idx);

                    DatasetEvent ev{};
                    ev.type = EventType::CANCEL;
                    ev.cancel_order_id = cancel_id;
                    ev.arrival_offset_ns = arrival_offset_ns;
                    events.push_back(ev);
                }
                else
                {
                    Order o;
                    o.order_id = next_order_id_++;
                    o.side = (unit_dist(rng_) < 0.5) ? Side::BUY : Side::SELL;
                    o.price = cfg_.mid_price + price_offset_dist(rng_);
                    o.qty = qty_dist(rng_);
                    o.timestamp_ns = arrival_offset_ns;
                    o.client_id = 1 + (o.order_id % kSyntheticClientCount);

                    const double u = unit_dist(rng_);
                    if (u < cfg_.ioc_ratio)
                    {
                        o.tif = TimeInForce::IOC;
                    }
                    else if (u < cfg_.ioc_ratio + cfg_.fok_ratio)
                    {
                        o.tif = TimeInForce::FOK;
                    }
                    else
                    {
                        o.tif = TimeInForce::GTC;
                        add_resting(o.order_id);
                    }

                    DatasetEvent ev{};
                    ev.type = EventType::NEW_ORDER;
                    ev.order = o;
                    ev.arrival_offset_ns = arrival_offset_ns;
                    events.push_back(ev);
                }
            }

            return events;
        }

        void write_to_file(const std::vector<DatasetEvent> &events, const std::string &path) const
        {
            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            if (!out)
            {
                throw std::runtime_error(
                    "DatasetGenerator::write_to_file: cannot open '" + path + "' for writing");
            }

            DatasetFileHeader header{};
            std::memcpy(header.magic, kMagic, sizeof(header.magic));
            header.format_version = kFormatVersion;
            header._reserved0 = 0;
            header.event_count = events.size();
            header.config = cfg_;

            out.write(reinterpret_cast<const char *>(&header), sizeof(header));

            for (const DatasetEvent &ev : events)
            {
                DatasetRecord rec{};
                rec.event_type = static_cast<uint8_t>(ev.type);
                rec.order = ev.order;
                rec.cancel_order_id = ev.cancel_order_id;
                rec.arrival_offset_ns = ev.arrival_offset_ns;
                out.write(reinterpret_cast<const char *>(&rec), sizeof(rec));
            }

            if (!out)
            {
                throw std::runtime_error(
                    "DatasetGenerator::write_to_file: write failed for '" + path + "'");
            }
        }

        [[nodiscard]] static std::vector<DatasetEvent> read_from_file(const std::string &path)
        {
            std::ifstream in(path, std::ios::binary);
            if (!in)
            {
                throw std::runtime_error("DatasetGenerator::read_from_file: cannot open '" + path + "'");
            }

            DatasetFileHeader header{};
            in.read(reinterpret_cast<char *>(&header), sizeof(header));
            if (!in || in.gcount() != static_cast<std::streamsize>(sizeof(header)))
            {
                throw std::runtime_error(
                    "DatasetGenerator::read_from_file: truncated header in '" + path + "'");
            }
            if (std::memcmp(header.magic, kMagic, sizeof(header.magic)) != 0)
            {
                throw std::runtime_error(
                    "DatasetGenerator::read_from_file: bad magic in '" + path +
                    "' (not a HYDRA-LOB dataset file)");
            }
            if (header.format_version != kFormatVersion)
            {
                throw std::runtime_error(
                    "DatasetGenerator::read_from_file: format_version mismatch in '" + path +
                    "' (file=" + std::to_string(header.format_version) +
                    ", reader=" + std::to_string(kFormatVersion) + ")");
            }

            std::vector<DatasetEvent> events;
            events.reserve(header.event_count);
            for (uint64_t i = 0; i < header.event_count; ++i)
            {
                DatasetRecord rec{};
                in.read(reinterpret_cast<char *>(&rec), sizeof(rec));
                if (!in || in.gcount() != static_cast<std::streamsize>(sizeof(rec)))
                {
                    throw std::runtime_error(
                        "DatasetGenerator::read_from_file: truncated record " +
                        std::to_string(i) + " in '" + path + "'");
                }
                DatasetEvent ev{};
                ev.type = static_cast<EventType>(rec.event_type);
                ev.order = rec.order;
                ev.cancel_order_id = rec.cancel_order_id;
                ev.arrival_offset_ns = rec.arrival_offset_ns;
                events.push_back(ev);
            }
            return events;
        }

    private:
        static constexpr char kMagic[8] = {'H', 'Y', 'D', 'R', 'A', 'L', 'O', 'B'};
        static constexpr uint32_t kFormatVersion = 2;
        static constexpr uint64_t kSyntheticClientCount = 8;

        DatasetConfig cfg_;
        std::mt19937_64 rng_;
        uint64_t next_order_id_ = 1;

        std::vector<uint64_t> resting_ids_;
        std::unordered_map<uint64_t, std::size_t> resting_index_;

        void add_resting(uint64_t id)
        {
            resting_index_[id] = resting_ids_.size();
            resting_ids_.push_back(id);
        }

        void remove_resting(uint64_t id, std::size_t idx)
        {
            const std::size_t last = resting_ids_.size() - 1;
            if (idx != last)
            {
                resting_ids_[idx] = resting_ids_[last];
                resting_index_[resting_ids_[idx]] = idx;
            }
            resting_ids_.pop_back();
            resting_index_.erase(id);
        }
    };

} // namespace hydra