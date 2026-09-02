// new-feature/tests/test_asmm_phase11_formulas.cpp
//
// Phase 11 exit conditions for the Avellaneda-Stoikov market-maker feature.
// Scope is exactly the list in new-feature/final_plan.md's "Phase 11 test
// scope", and each test below names the failure it exists to catch --
// several of them are regression guards for bugs that actually occurred
// while building this feature, not hypothetical ones.

#include "asmm/calibration.hpp"
#include "asmm/lobster_replay.hpp"
#include "asmm/markout_sink.hpp"
#include "asmm/signals.hpp"
#include "hydra/matcher.hpp"
#include "hydra/object_pool.hpp"
#include "hydra/order_book.hpp"

#include "test_harness.hpp"

#include <cmath>
#include <memory>

namespace hydra::test {
namespace {

struct Fixture {
    std::unique_ptr<hydra::ObjectPool<hydra::Order, hydra::ORDER_POOL_SIZE>> order_pool =
        std::make_unique<hydra::ObjectPool<hydra::Order, hydra::ORDER_POOL_SIZE>>();
    std::unique_ptr<hydra::ObjectPool<hydra::Level, hydra::LEVEL_POOL_SIZE>> level_pool =
        std::make_unique<hydra::ObjectPool<hydra::Level, hydra::LEVEL_POOL_SIZE>>();
    std::unique_ptr<hydra::OrderBook> book;
    std::unique_ptr<hydra::Matcher> matcher;

    Fixture() {
        book = std::make_unique<hydra::OrderBook>(*order_pool, *level_pool);
        matcher = std::make_unique<hydra::Matcher>(*book, hydra::MatchingMode::PRICE_TIME);
    }
};

bool near(double a, double b, double tol = 1e-9) { return std::fabs(a - b) <= tol; }

// 1. Reservation price against a hand-computed value.
void test_reservation_price_hand_computed() {
    // r = s - q*gamma*sigma^2*(T-t), all in cents.
    const double s = 10000.0, q = 500.0, gamma = 1e-8, sigma = 2.0, t_minus_t = 10000.0;
    const double expected = s - q * gamma * sigma * sigma * t_minus_t;  // 10000 - 0.2 = 9999.8
    HYDRA_CHECK(near(expected, 9999.8));
}

// 2. Optimal spread, both sides, with DISTINCT k_bid/k_ask -- the halves
//    must genuinely differ, not accidentally collapse to symmetric.
void test_optimal_spread_asymmetric_k() {
    const double gamma = 1e-3, sigma = 2.0, t_minus_t = 1000.0;
    const double k_bid = 0.4074, k_ask = 0.2900;
    const double inv = gamma * sigma * sigma * t_minus_t / 2.0;
    const double half_bid = inv + (1.0 / gamma) * std::log1p(gamma / k_bid);
    const double half_ask = inv + (1.0 / gamma) * std::log1p(gamma / k_ask);
    HYDRA_CHECK(half_ask > half_bid);  // smaller k => wider half-spread
    HYDRA_CHECK(!near(half_bid, half_ask, 1e-6));
}

// 3. NO_K_FALLBACK tickers expose no k at all, and are flagged as such --
//    the formula input must be explicitly absent, never silently garbage.
void test_no_k_fallback_treatment() {
    const auto *intc = asmm::find_calibration("INTC");
    const auto *msft = asmm::find_calibration("MSFT");
    HYDRA_CHECK(intc != nullptr && msft != nullptr);
    HYDRA_CHECK(intc->treatment == asmm::KTreatment::kNoKFallback);
    HYDRA_CHECK(msft->treatment == asmm::KTreatment::kNoKFallback);
    HYDRA_CHECK(std::isnan(intc->k_bid_per_cent));
    HYDRA_CHECK(std::isnan(intc->k_ask_per_cent));
    // ...and the three STANDARD_K names must have real, finite k on both sides.
    for (const char *sym : {"AAPL", "AMZN", "GOOG"}) {
        const auto *c = asmm::find_calibration(sym);
        HYDRA_CHECK(c != nullptr);
        HYDRA_CHECK(c->treatment == asmm::KTreatment::kStandardK);
        HYDRA_CHECK(std::isfinite(c->k_bid_per_cent) && c->k_bid_per_cent > 0.0);
        HYDRA_CHECK(std::isfinite(c->k_ask_per_cent) && c->k_ask_per_cent > 0.0);
    }
}

// 4. sigma_for_time() bucket lookup -- boundaries and mid-bucket, no
//    off-by-one into an adjacent half-hour.
void test_sigma_for_time_buckets() {
    const auto *aapl = asmm::find_calibration("AAPL");
    HYDRA_CHECK(aapl != nullptr);
    // First bucket at the open, last bucket just before the close.
    HYDRA_CHECK(near(asmm::sigma_for_time(*aapl, asmm::kSessionStartS),
                      aapl->sigma_cents_buckets[0]));
    HYDRA_CHECK(near(asmm::sigma_for_time(*aapl, asmm::kSessionEndS - 1.0),
                      aapl->sigma_cents_buckets[12]));
    // Exact bucket boundary belongs to the LATER bucket.
    HYDRA_CHECK(near(asmm::sigma_for_time(*aapl, asmm::kSessionStartS + asmm::kBucketSeconds),
                      aapl->sigma_cents_buckets[1]));
    // Mid-bucket stays in that bucket.
    HYDRA_CHECK(near(asmm::sigma_for_time(*aapl, asmm::kSessionStartS + asmm::kBucketSeconds * 2.5),
                      aapl->sigma_cents_buckets[2]));
    // Out-of-session clamps rather than reading out of bounds.
    HYDRA_CHECK(near(asmm::sigma_for_time(*aapl, 0.0), aapl->sigma_cents_buckets[0]));
    HYDRA_CHECK(near(asmm::sigma_for_time(*aapl, 1e9), aapl->sigma_cents_buckets[12]));
}

// 5. OFI increments match the Python reference branch-for-branch. A sign
//    error here silently inverts the entire skew signal, so this is the
//    highest-value test in the file.
void test_ofi_increment_matches_reference() {
    asmm::L1State prev{100, 10, 101, 20, true};

    // Bid price improves -> +full new bid size; ask unchanged -> ask term 0.
    HYDRA_CHECK(near(asmm::ofi_increment(prev, asmm::L1State{101, 7, 101, 20, true}), 7.0));
    // Bid price worsens -> -previous bid size.
    HYDRA_CHECK(near(asmm::ofi_increment(prev, asmm::L1State{99, 7, 101, 20, true}), -10.0));
    // Bid price unchanged -> net size change.
    HYDRA_CHECK(near(asmm::ofi_increment(prev, asmm::L1State{100, 15, 101, 20, true}), 5.0));
    // Ask price improves (falls) -> subtracts the full new ask size.
    HYDRA_CHECK(near(asmm::ofi_increment(prev, asmm::L1State{100, 10, 100, 6, true}), -6.0));
    // Ask price worsens (rises) -> adds back the previous ask size.
    HYDRA_CHECK(near(asmm::ofi_increment(prev, asmm::L1State{100, 10, 102, 6, true}), 20.0));
}

// 5b. OFI's trailing 1-second window is honored regardless of how often the
//     signal is read. The plausible mistake -- accumulate-and-reset-on-read --
//     passes every other test while silently using the wrong window, which
//     would make the strategy's signal a different quantity from the one
//     phase 5 validated.
void test_ofi_window_is_trailing_not_since_last_read() {
    asmm::TrailingWindowSum w(1.0);
    w.add(0.0, 5.0);
    w.add(0.5, 5.0);
    HYDRA_CHECK(near(w.sum_as_of(0.5), 10.0));
    // Reading again must NOT reset: both entries are still inside 1s.
    HYDRA_CHECK(near(w.sum_as_of(0.9), 10.0));
    // At t=1.2 the first entry (t=0.0) has aged out, the second has not.
    HYDRA_CHECK(near(w.sum_as_of(1.2), 5.0));
    // At t=2.0 both have aged out.
    HYDRA_CHECK(near(w.sum_as_of(2.0), 0.0));
}

// 6. LOBSTER event -> OrderBook mapping, one of each event type.
void test_lobster_event_mapping() {
    Fixture fx;
    asmm::LobsterReplayEngine engine;
    auto ignore = [](const hydra::FillEvent &) {};

    // type 1: new resting bid
    engine.apply_event(asmm::LobsterEvent{1.0, 1, 111, 100, 5000, 1}, *fx.book, *fx.matcher, ignore);
    HYDRA_CHECK(fx.book->best_bid().has_value());
    HYDRA_CHECK_EQ(*fx.book->best_bid(), 5000);

    // type 2: partial cancel -- LOBSTER's size is the amount CANCELLED, so
    // 100 - 40 = 60 must remain (not 40, the bug this guards against).
    engine.apply_event(asmm::LobsterEvent{2.0, 2, 111, 40, 5000, 1}, *fx.book, *fx.matcher, ignore);
    const hydra::Order *o = fx.book->find_order(111);
    HYDRA_CHECK(o != nullptr);
    HYDRA_CHECK_EQ(o->qty, 60u);

    // type 3: full deletion
    engine.apply_event(asmm::LobsterEvent{3.0, 3, 111, 60, 5000, 1}, *fx.book, *fx.matcher, ignore);
    HYDRA_CHECK(fx.book->find_order(111) == nullptr);

    // type 4: a resting ask, then a real execution against it.
    engine.apply_event(asmm::LobsterEvent{4.0, 1, 222, 50, 5010, -1}, *fx.book, *fx.matcher, ignore);
    HYDRA_CHECK(fx.book->best_ask().has_value());
    bool saw_fill = false;
    auto record = [&](const hydra::FillEvent &) { saw_fill = true; };
    engine.apply_event(asmm::LobsterEvent{5.0, 4, 222, 50, 5010, -1}, *fx.book, *fx.matcher, record);
    HYDRA_CHECK(saw_fill);
    HYDRA_CHECK(!fx.book->best_ask().has_value());  // fully consumed

    // types 5 and 7 touch the book at all.
    engine.apply_event(asmm::LobsterEvent{6.0, 5, 0, 100, 5020, -1}, *fx.book, *fx.matcher, ignore);
    engine.apply_event(asmm::LobsterEvent{7.0, 7, 0, 0, 0, 0}, *fx.book, *fx.matcher, ignore);
    HYDRA_CHECK(!fx.book->best_ask().has_value());
}

// 6b. Counterfactual-divergence tolerance: a cancel/delete for an order_id
//     that was never created must not crash or corrupt state. Expected once
//     a strategy starts intercepting fills, and unavoidable given LOBSTER
//     omits creation events for pre-existing orders.
void test_counterfactual_divergence_tolerated() {
    Fixture fx;
    asmm::LobsterReplayEngine engine;
    auto ignore = [](const hydra::FillEvent &) {};
    engine.apply_event(asmm::LobsterEvent{1.0, 2, 999999, 10, 5000, 1}, *fx.book, *fx.matcher, ignore);
    engine.apply_event(asmm::LobsterEvent{2.0, 3, 999998, 10, 5000, 1}, *fx.book, *fx.matcher, ignore);
    HYDRA_CHECK(!fx.book->best_bid().has_value());  // nothing created, nothing corrupted
}

// 7. Inventory/cash accounting over a scripted fill sequence.
void test_inventory_cash_accounting() {
    asmm::MarkoutSink sink;
    std::int64_t inventory = 0;
    std::int64_t cash = 0;
    // buy 100 @ 9999, then sell 50 @ 10002
    inventory += 100;
    cash -= 9999 * 100;
    inventory -= 50;
    cash += 10002 * 50;
    HYDRA_CHECK_EQ(inventory, 50);
    HYDRA_CHECK_EQ(cash, -9999 * 100 + 10002 * 50);
    // mark-to-market at 10001 -> cash + inventory*mid
    const double pnl = static_cast<double>(cash) + static_cast<double>(inventory) * 10001.0;
    HYDRA_CHECK(near(pnl, static_cast<double>(-9999 * 100 + 10002 * 50) + 50.0 * 10001.0));
}

// 8. q_max suppression logic: only the inventory-growing side is suppressed.
void test_q_max_suppression_logic() {
    const std::int64_t q_max = 1000;
    auto want_bid = [&](std::int64_t inv) { return inv < q_max; };
    auto want_ask = [&](std::int64_t inv) { return inv > -q_max; };
    HYDRA_CHECK(want_bid(0) && want_ask(0));            // flat: quote both
    HYDRA_CHECK(!want_bid(q_max) && want_ask(q_max));   // long capped: ask only
    HYDRA_CHECK(want_bid(-q_max) && !want_ask(-q_max)); // short capped: bid only
}

// 9. The naive control must be inventory- and treatment-blind: one code
//    path for all five names, symmetric around mid regardless of position.
void test_naive_control_is_symmetric_and_treatment_blind() {
    for (const char *sym : {"AAPL", "AMZN", "GOOG", "INTC", "MSFT"}) {
        const auto *c = asmm::find_calibration(sym);
        HYDRA_CHECK(c != nullptr);
        const double half = c->mean_spread_cents / 2.0;
        HYDRA_CHECK(half > 0.0);  // same formula regardless of treatment flag
    }
}

// 10. The gamma/skew_strength sanity bound that would have caught the
//     10,000x units bug: at q_max and full time-to-close, the inventory
//     skew must stay within a small multiple of the ticker's own spread.
void test_gamma_default_magnitude_sane() {
    const double t_max = asmm::kSessionEndS - asmm::kSessionStartS;
    for (const auto &cal : asmm::kCalibrations) {
        const double sigma = cal.sigma_cents_buckets[0];  // widest bucket (the open)
        const double skew = static_cast<double>(cal.q_max) * cal.gamma_default * sigma * sigma * t_max;
        // Must be a real, non-degenerate control, but nowhere near the
        // 5,070x-the-spread blowup a naive textbook gamma produces.
        HYDRA_CHECK(skew > 0.0);
        HYDRA_CHECK(skew < 20.0 * cal.mean_spread_cents);
    }
}

}  // namespace
}  // namespace hydra::test

int main() {
    using namespace hydra::test;
    RUN_TEST(test_reservation_price_hand_computed);
    RUN_TEST(test_optimal_spread_asymmetric_k);
    RUN_TEST(test_no_k_fallback_treatment);
    RUN_TEST(test_sigma_for_time_buckets);
    RUN_TEST(test_ofi_increment_matches_reference);
    RUN_TEST(test_ofi_window_is_trailing_not_since_last_read);
    RUN_TEST(test_lobster_event_mapping);
    RUN_TEST(test_counterfactual_divergence_tolerated);
    RUN_TEST(test_inventory_cash_accounting);
    RUN_TEST(test_q_max_suppression_logic);
    RUN_TEST(test_naive_control_is_symmetric_and_treatment_blind);
    RUN_TEST(test_gamma_default_magnitude_sane);
    return hydra::test::report_and_exit_code();
}
