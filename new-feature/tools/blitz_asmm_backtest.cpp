// new-feature/tools/blitz_asmm_backtest.cpp
//
// CLI driver for the Avellaneda-Stoikov market-maker backtest against real
// LOBSTER order-book replay. Flag parsing follows tools/gen_dataset.cpp's
// hand-rolled style rather than pulling in a CLI library, matching this
// project's convention for small standalone tools.

#include "asmm/backtest_engine.hpp"
#include "asmm/calibration.hpp"
#include "asmm/markout_sink.hpp"
#include "asmm/strategy.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

#ifndef HYDRA_LOB_VERSION
#define HYDRA_LOB_VERSION "dev-build"
#endif

namespace {

void usage(const char *prog) {
    std::printf(
        "usage: %s --ticker {AAPL|AMZN|GOOG|INTC|MSFT} [options]\n"
        "\n"
        "  --data-dir PATH           directory holding the LOBSTER CSVs\n"
        "                            (default: new-feature/datasets)\n"
        "  --strategy {as|naive|both}  default: both (paired comparison)\n"
        "  --duration-seconds N      truncate replay (dev convenience; default: full session)\n"
        "  --requote-interval-ms N   default: 250\n"
        "  --gamma F                 override the per-ticker calibrated default\n"
        "  --skew-strength F         override the per-ticker calibrated default\n"
        "  --output-dir PATH         where per-fill CSVs are written (default: none)\n"
        "  --help\n",
        prog);
}

bool match_flag(int argc, char **argv, int &i, const char *flag, std::string &out) {
    if (std::strcmp(argv[i], flag) != 0) return false;
    if (i + 1 >= argc) {
        std::fprintf(stderr, "error: %s expects a value\n", flag);
        std::exit(1);
    }
    out = argv[++i];
    return true;
}

void print_result(const char *label, const asmm::BacktestResult &r) {
    std::printf("  %-6s fills=%-7llu inventory=%-8lld cash=%12.2f$  totalP&L=%10.2f$  "
                 "markout(1s/5s/30s)= %+.3f / %+.3f / %+.3f cents\n",
                 label, (unsigned long long)r.fill_count, (long long)r.final_inventory,
                 static_cast<double>(r.final_cash_cents) / 100.0, r.total_pnl_cents / 100.0,
                 r.mean_markout_1s, r.mean_markout_5s, r.mean_markout_30s);
}

}  // namespace

int main(int argc, char **argv) {
    std::string ticker;
    std::string data_dir = "new-feature/datasets";
    std::string strategy_sel = "both";
    std::string output_dir;
    std::string tmp;
    double duration_seconds = 0.0;
    double requote_interval_ms = 250.0;
    double gamma_override = 0.0;
    double skew_override = -1.0;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            usage(argv[0]);
            return 0;
        }
        if (match_flag(argc, argv, i, "--ticker", ticker)) continue;
        if (match_flag(argc, argv, i, "--data-dir", data_dir)) continue;
        if (match_flag(argc, argv, i, "--strategy", strategy_sel)) continue;
        if (match_flag(argc, argv, i, "--output-dir", output_dir)) continue;
        if (match_flag(argc, argv, i, "--duration-seconds", tmp)) {
            duration_seconds = std::stod(tmp);
            continue;
        }
        if (match_flag(argc, argv, i, "--requote-interval-ms", tmp)) {
            requote_interval_ms = std::stod(tmp);
            continue;
        }
        if (match_flag(argc, argv, i, "--gamma", tmp)) {
            gamma_override = std::stod(tmp);
            continue;
        }
        if (match_flag(argc, argv, i, "--skew-strength", tmp)) {
            skew_override = std::stod(tmp);
            continue;
        }
        std::fprintf(stderr, "error: unknown flag %s\n", argv[i]);
        usage(argv[0]);
        return 1;
    }

    if (ticker.empty()) {
        std::fprintf(stderr, "error: --ticker is required\n");
        usage(argv[0]);
        return 1;
    }
    const asmm::TickerCalibration *cal = asmm::find_calibration(ticker);
    if (cal == nullptr) {
        std::fprintf(stderr, "error: no calibration for ticker '%s'\n", ticker.c_str());
        return 1;
    }

    asmm::BacktestConfig cfg;
    cfg.message_csv = data_dir + "/" + ticker + "_2012-06-21_message_10.csv";
    cfg.orderbook_csv = data_dir + "/" + ticker + "_2012-06-21_orderbook_10.csv";
    cfg.requote_interval_s = requote_interval_ms / 1000.0;
    cfg.duration_seconds = duration_seconds;

    asmm::StrategyParams params;
    params.gamma = gamma_override;
    params.skew_strength = skew_override;

    const bool run_as = (strategy_sel == "as" || strategy_sel == "both");
    const bool run_naive = (strategy_sel == "naive" || strategy_sel == "both");
    if (!run_as && !run_naive) {
        std::fprintf(stderr, "error: --strategy must be as, naive, or both\n");
        return 1;
    }

    std::printf("blitz_asmm_backtest (%s)\n", HYDRA_LOB_VERSION);
    std::printf("  ticker=%s  treatment=%s  requote=%.0fms  duration=%s\n", ticker.c_str(),
                 cal->treatment == asmm::KTreatment::kStandardK ? "STANDARD_K" : "NO_K_FALLBACK",
                 requote_interval_ms,
                 duration_seconds > 0 ? (std::to_string(duration_seconds) + "s").c_str() : "full session");
    std::printf("  gamma=%.6e  skew_strength=%.6e  q_max=%lld  quote_size=%u\n",
                 params.gamma > 0 ? params.gamma : cal->gamma_default,
                 params.skew_strength >= 0 ? params.skew_strength : cal->skew_strength_default,
                 (long long)cal->q_max, cal->quote_size);
    std::printf("  P&L convention: cash + inventory marked to the final real mid\n\n");

    asmm::BacktestResult as_result{};
    asmm::BacktestResult naive_result{};

    if (run_as) {
        asmm::MarkoutSink sink;
        as_result = asmm::run_backtest(
            *cal, cfg, params,
            [](const asmm::TickerCalibration &c, hydra::OrderBook &b, hydra::Matcher &m,
                asmm::MarkoutSink &s, asmm::StrategyParams p) {
                return std::make_unique<asmm::AvellanedaStoikovStrategy>(c, b, m, s, p);
            },
            sink);
        print_result("A-S", as_result);
        if (!output_dir.empty()) {
            sink.write_csv(output_dir + "/asmm_" + ticker + "_as.csv");
        }
    }
    if (run_naive) {
        asmm::MarkoutSink sink;
        asmm::StrategyParams naive_params = params;
        naive_params.enable_skew = false;
        naive_result = asmm::run_backtest(
            *cal, cfg, naive_params,
            [](const asmm::TickerCalibration &c, hydra::OrderBook &b, hydra::Matcher &m,
                asmm::MarkoutSink &s, asmm::StrategyParams p) {
                return std::make_unique<asmm::NaiveFixedSpreadStrategy>(c, b, m, s, p);
            },
            sink);
        print_result("naive", naive_result);
        if (!output_dir.empty()) {
            sink.write_csv(output_dir + "/asmm_" + ticker + "_naive.csv");
        }
    }

    if (run_as && run_naive) {
        std::printf("\n  paired comparison (A-S minus naive):\n");
        std::printf("    total P&L delta : %+.2f$\n",
                     (as_result.total_pnl_cents - naive_result.total_pnl_cents) / 100.0);
        std::printf("    markout@5s delta: %+.3f cents\n",
                     as_result.mean_markout_5s - naive_result.mean_markout_5s);
        std::printf("    fills           : A-S %llu vs naive %llu\n",
                     (unsigned long long)as_result.fill_count,
                     (unsigned long long)naive_result.fill_count);
    }
    return 0;
}
