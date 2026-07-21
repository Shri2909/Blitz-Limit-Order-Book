// tools/gen_dataset.cpp
//
// Standalone CLI tool wrapping DatasetGenerator -- the only way dataset
// files get produced, so benchmark.cpp never generates-and-writes a file
// as a side effect of a benchmark run. Generation and benchmarking stay
// decoupled: generate once, replay many times.

#include "hydra/config.hpp"
#include "hydra/dataset_generator.hpp"
#include "hydra/types.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <exception>
#include <stdexcept>
#include <string>
#include <vector>

// Same fallback convention as src/benchmark.cpp's HYDRA_LOB_VERSION -- set
// by CMakeLists.txt from `git rev-parse --short HEAD` at build time, so a
// dataset's manifest entry can record which generator binary (not just
// which flags) produced it.
#ifndef HYDRA_LOB_VERSION
#define HYDRA_LOB_VERSION "dev-build"
#endif

namespace
{

    // UTC, second precision, human-readable -- good enough for "which day
    // was this generated," not meant for sub-second provenance.
    [[nodiscard]] std::string utc_timestamp_now()
    {
        const std::time_t now = std::time(nullptr);
        std::tm utc{};
        gmtime_r(&now, &utc);
        char buf[32];
        std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &utc);
        return buf;
    }

    void print_usage(const char *prog)
    {
        std::fprintf(stderr,
                     "Usage: %s [--seed N] [--count N] [--mid-price N] [--spread N]\n"
                     "          [--min-qty N] [--max-qty N] [--cancel-ratio F]\n"
                     "          [--ioc-ratio F] [--fok-ratio F] [--replace-ratio F]\n"
                     "          [--client-id-count N] [--rate-hz N]\n"
                     "          --output PATH\n"
                     "\n"
                     "All flags except --output default to config.hpp's DEFAULT_*\n"
                     "constants when omitted (--replace-ratio defaults to 0.0,\n"
                     "--client-id-count defaults to 8).\n"
                     "\n"
                     "--client-id-count controls how many distinct synthetic client_ids\n"
                     "orders are drawn from; a smaller value deliberately raises the\n"
                     "self-trade collision rate -- use e.g. --client-id-count 2 for a\n"
                     "self-trade-heavy workload.\n"
                     "\n"
                     "Example:\n"
                     "  %s --seed 42 --count 110000 --output datasets/bench_110k.bin\n",
                     prog, prog);
    }

    bool match_flag(int argc, char **argv, int &i, const char *flag, std::string &out_value)
    {
        if (std::strcmp(argv[i], flag) != 0)
        {
            return false;
        }
        if (i + 1 >= argc)
        {
            std::fprintf(stderr, "error: %s requires a value\n", flag);
            std::exit(1);
        }
        out_value = argv[++i];
        return true;
    }

    unsigned long long parse_u64(const std::string &s, const char *flag)
    {
        try
        {
            std::size_t consumed = 0;
            const unsigned long long v = std::stoull(s, &consumed);
            if (consumed != s.size())
            {
                throw std::invalid_argument("trailing characters");
            }
            return v;
        }
        catch (const std::exception &)
        {
            std::fprintf(stderr, "error: %s expects a non-negative integer, got '%s'\n",
                         flag, s.c_str());
            std::exit(1);
        }
    }

    long long parse_i64(const std::string &s, const char *flag)
    {
        try
        {
            std::size_t consumed = 0;
            const long long v = std::stoll(s, &consumed);
            if (consumed != s.size())
            {
                throw std::invalid_argument("trailing characters");
            }
            return v;
        }
        catch (const std::exception &)
        {
            std::fprintf(stderr, "error: %s expects an integer, got '%s'\n", flag, s.c_str());
            std::exit(1);
        }
    }

    double parse_double(const std::string &s, const char *flag)
    {
        try
        {
            std::size_t consumed = 0;
            const double v = std::stod(s, &consumed);
            if (consumed != s.size())
            {
                throw std::invalid_argument("trailing characters");
            }
            return v;
        }
        catch (const std::exception &)
        {
            std::fprintf(stderr, "error: %s expects a number, got '%s'\n", flag, s.c_str());
            std::exit(1);
        }
    }

} // namespace

int main(int argc, char **argv)
{
    uint64_t seed = hydra::DEFAULT_DATASET_SEED;
    std::size_t count = hydra::DEFAULT_DATASET_ORDER_COUNT;
    int64_t mid_price = hydra::DEFAULT_MID_PRICE;
    int64_t spread = hydra::DEFAULT_PRICE_SPREAD_TICKS;
    uint32_t min_qty = hydra::DEFAULT_MIN_QTY;
    uint32_t max_qty = hydra::DEFAULT_MAX_QTY;
    double cancel_ratio = hydra::DEFAULT_CANCEL_RATIO;
    double ioc_ratio = hydra::DEFAULT_IOC_RATIO;
    double fok_ratio = hydra::DEFAULT_FOK_RATIO;
    double replace_ratio = 0.0;
    uint64_t client_id_count = 8;
    double rate_hz = hydra::DEFAULT_ARRIVAL_RATE_HZ;
    std::string output_path;

    for (int i = 1; i < argc; ++i)
    {
        std::string val;
        if (match_flag(argc, argv, i, "--seed", val))
        {
            seed = parse_u64(val, "--seed");
            continue;
        }
        if (match_flag(argc, argv, i, "--count", val))
        {
            count = static_cast<std::size_t>(parse_u64(val, "--count"));
            continue;
        }
        if (match_flag(argc, argv, i, "--mid-price", val))
        {
            mid_price = parse_i64(val, "--mid-price");
            continue;
        }
        if (match_flag(argc, argv, i, "--spread", val))
        {
            spread = parse_i64(val, "--spread");
            continue;
        }
        if (match_flag(argc, argv, i, "--min-qty", val))
        {
            min_qty = static_cast<uint32_t>(parse_u64(val, "--min-qty"));
            continue;
        }
        if (match_flag(argc, argv, i, "--max-qty", val))
        {
            max_qty = static_cast<uint32_t>(parse_u64(val, "--max-qty"));
            continue;
        }
        if (match_flag(argc, argv, i, "--cancel-ratio", val))
        {
            cancel_ratio = parse_double(val, "--cancel-ratio");
            continue;
        }
        if (match_flag(argc, argv, i, "--ioc-ratio", val))
        {
            ioc_ratio = parse_double(val, "--ioc-ratio");
            continue;
        }
        if (match_flag(argc, argv, i, "--fok-ratio", val))
        {
            fok_ratio = parse_double(val, "--fok-ratio");
            continue;
        }
        if (match_flag(argc, argv, i, "--replace-ratio", val))
        {
            replace_ratio = parse_double(val, "--replace-ratio");
            continue;
        }
        if (match_flag(argc, argv, i, "--client-id-count", val))
        {
            client_id_count = parse_u64(val, "--client-id-count");
            continue;
        }
        if (match_flag(argc, argv, i, "--rate-hz", val))
        {
            rate_hz = parse_double(val, "--rate-hz");
            continue;
        }
        if (match_flag(argc, argv, i, "--output", val))
        {
            output_path = val;
            continue;
        }
        if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0)
        {
            print_usage(argv[0]);
            return 0;
        }
        std::fprintf(stderr, "error: unknown flag '%s'\n", argv[i]);
        print_usage(argv[0]);
        return 1;
    }

    if (output_path.empty())
    {
        std::fprintf(stderr, "error: --output PATH is required\n");
        print_usage(argv[0]);
        return 1;
    }

    if (ioc_ratio + fok_ratio > 1.0)
    {
        std::fprintf(stderr,
                     "error: --ioc-ratio + --fok-ratio must be <= 1.0 (got %.6f + %.6f = %.6f)\n",
                     ioc_ratio, fok_ratio, ioc_ratio + fok_ratio);
        return 1;
    }

    hydra::DatasetConfig cfg{};
    cfg.seed = seed;
    cfg.order_count = count;
    cfg.mid_price = mid_price;
    cfg.price_spread_ticks = spread;
    cfg.min_qty = min_qty;
    cfg.max_qty = max_qty;
    cfg.cancel_ratio = cancel_ratio;
    cfg.arrival_rate_hz = rate_hz;
    cfg.ioc_ratio = ioc_ratio;
    cfg.fok_ratio = fok_ratio;
    cfg.replace_ratio = replace_ratio;
    cfg.client_id_count = client_id_count;

    const std::string generated_at = utc_timestamp_now();

    std::printf("blitz_gen_dataset: resolved config:\n");
    std::printf("  seed               = %llu\n", static_cast<unsigned long long>(cfg.seed));
    std::printf("  order_count        = %zu\n", cfg.order_count);
    std::printf("  mid_price          = %lld\n", static_cast<long long>(cfg.mid_price));
    std::printf("  price_spread_ticks = %lld\n", static_cast<long long>(cfg.price_spread_ticks));
    std::printf("  min_qty            = %u\n", cfg.min_qty);
    std::printf("  max_qty            = %u\n", cfg.max_qty);
    std::printf("  cancel_ratio       = %.6f\n", cfg.cancel_ratio);
    std::printf("  arrival_rate_hz    = %.6f\n", cfg.arrival_rate_hz);
    std::printf("  ioc_ratio          = %.6f\n", cfg.ioc_ratio);
    std::printf("  fok_ratio          = %.6f\n", cfg.fok_ratio);
    std::printf("  replace_ratio      = %.6f\n", cfg.replace_ratio);
    std::printf("  client_id_count    = %llu\n", static_cast<unsigned long long>(cfg.client_id_count));
    std::printf("  output             = %s\n", output_path.c_str());
    std::printf("  generated_utc      = %s\n", generated_at.c_str());
    std::printf("  generator_version  = %s\n", HYDRA_LOB_VERSION);

    try
    {
        hydra::DatasetGenerator gen(cfg);
        std::vector<hydra::DatasetEvent> events = gen.generate();
        gen.write_to_file(events, output_path);
    }
    catch (const std::exception &e)
    {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }

    std::printf("blitz_gen_dataset: wrote %zu events to '%s'\n", count, output_path.c_str());

    const std::size_t last_slash = output_path.find_last_of('/');
    const std::string basename =
        (last_slash == std::string::npos) ? output_path : output_path.substr(last_slash + 1);

    // Copy-pasteable datasets/manifest.txt entry -- the manifest is a
    // manually-maintained reproducibility record (this tool never writes
    // to it directly, matching the project's existing "flags in
    // manifest.txt, not the binary, are the source of truth" policy), but
    // previously recorded flags/seed only, with no generation timestamp or
    // generator identity -- a silent generator-*logic* change (not caught
    // by DatasetFileHeader's format_version check) would have been
    // undetectable from the manifest alone. Printed, not auto-appended:
    // whether/how to fold this into datasets/manifest.txt's prose entries
    // is still a human call.
    std::printf(
        "\n"
        "-- datasets/manifest.txt entry (copy/paste, add rationale by hand) --\n"
        "%s: --seed %llu --count %zu --mid-price %lld --spread %lld\n"
        "                --min-qty %u --max-qty %u --cancel-ratio %.6f\n"
        "                --ioc-ratio %.6f --fok-ratio %.6f --replace-ratio %.6f\n"
        "                --client-id-count %llu --rate-hz %.6f\n"
        "                (generated_utc=%s generator_version=%s)\n",
        basename.c_str(), static_cast<unsigned long long>(cfg.seed), cfg.order_count,
        static_cast<long long>(cfg.mid_price), static_cast<long long>(cfg.price_spread_ticks),
        cfg.min_qty, cfg.max_qty, cfg.cancel_ratio, cfg.ioc_ratio, cfg.fok_ratio,
        cfg.replace_ratio, static_cast<unsigned long long>(cfg.client_id_count),
        cfg.arrival_rate_hz, generated_at.c_str(), HYDRA_LOB_VERSION);

    return 0;
}
