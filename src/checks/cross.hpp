#pragma once

#include <vector>

#include "checks/check.hpp"
#include "context/context.hpp"
#include "dorq/calendar.hpp"

namespace dorq {

// DQ6xx: cross-sectional evidence. Judged once every series is in
// (engine/cross_section.cpp); see doc/checks/DQ601.md and DQ602.md.

// DQ601: many series move by the same clean ratio on the same date.
class CohortMove final : public Check {
 public:
  [[nodiscard]] const CheckInfo& info() const noexcept override;
  void run(const SeriesContext& /*context*/, std::vector<Violation>& /*out*/) const override {}
};

// DQ602: info, a day the market reference moved far beyond its usual range, on
// which every series' ordinary move is wider.
class MarketDay final : public Check {
 public:
  [[nodiscard]] const CheckInfo& info() const noexcept override;
  void run(const SeriesContext& /*context*/, std::vector<Violation>& /*out*/) const override {}
};

// The DQ602 reports for a market reference: each day its move is at least
// `threshold` standard deviations of its own recent moves.
[[nodiscard]] std::vector<Violation> market_days(const MarketSeries& market,
                                                 const Calendar& calendar, double threshold = 5.0);

}  // namespace dorq
