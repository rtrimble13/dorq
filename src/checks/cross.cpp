#include "checks/cross.hpp"

#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

#include "checks/price.hpp"
#include "stats/nig.hpp"
#include "stats/robust.hpp"

namespace dorq {

const CheckInfo& CohortMove::info() const noexcept {
  static const CheckInfo kInfo{
      .code = "DQ601",
      .name = "cohort-move",
      .summary = "several series move by the same clean ratio on the same date",
      .default_severity = Severity::kWarn,
      .applies = Applies::kOhlcvOnly,
      .cross_sectional = true,
  };
  return kInfo;
}

const CheckInfo& MarketDay::info() const noexcept {
  static const CheckInfo kInfo{
      .code = "DQ602",
      .name = "market-day",
      .summary = "info: the market reference moved far beyond its usual range (with --market)",
      .default_severity = Severity::kInfo,
      .applies = Applies::kAny,
      .cross_sectional = true,
  };
  return kInfo;
}

std::vector<Violation> market_days(const MarketSeries& market, const Calendar& calendar,
                                   double threshold) {
  // The market's own volatility per session, from the moves before each day: a
  // discounted Normal-Inverse-Gamma estimate, as the price model keeps for every
  // series, started from the robust scale of all its moves.
  constexpr double kDiscount = 0.97;
  constexpr double kClip = 4.0;
  std::vector<Violation> out;
  const std::size_t n = market.date.size();
  std::vector<double> scaled;
  std::vector<int> sessions(n, 1);
  for (std::size_t i = 1; i < n; ++i) {
    sessions[i] = std::max(
        1, calendar.sessions_between(Date::from_days(market.date[i - 1].days() + 1), market.date[i]));
    scaled.push_back((market.log_level[i] - market.log_level[i - 1]) /
                     std::sqrt(static_cast<double>(sessions[i])));
  }
  const double scale = stats::mad(scaled);
  if (!(scale > 0.0)) {
    return out;
  }
  static const MarketDay kCheck;
  stats::DiscountedNig state = stats::DiscountedNig::prior(scale * scale, 10.0);
  for (std::size_t i = 1; i < n; ++i) {
    const double move = scaled[i - 1];
    const double sd = std::sqrt(state.variance());
    const double z = move / sd;
    state.update(move, kDiscount, kClip);
    if (std::fabs(z) < threshold) {
      continue;
    }
    const double change = std::expm1(market.log_level[i] - market.log_level[i - 1]);
    Violation v;
    v.check = &kCheck.info();
    v.severity = Severity::kInfo;
    v.classification = Classification::kMarketFact;
    v.p_error = 0.0;
    v.date = market.date[i];
    v.message = "the market (" + market.name + ") moved " +
                (change >= 0.0 ? "+" : "−") + significant(std::fabs(change) * 100.0, 3) + "% (" +
                significant(std::fabs(z), 2) +
                " standard deviations of its recent moves): every series' ordinary move is "
                "wider today";
    v.detail = {{"market", market.name}, {"change", change}, {"z", z}};
    out.push_back(std::move(v));
  }
  return out;
}

}  // namespace dorq
