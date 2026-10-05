#pragma once

#include <vector>

#include "checks/check.hpp"

namespace dorq {

// DQ5xx: stale values. See doc/checks/DQ501.md and DQ502.md.

// DQ501: a run of repeated closes on traded bars that the series' own volatility
// and price grid make improbable (the price model's stale runs).
class RepeatedPrice final : public Check {
 public:
  [[nodiscard]] const CheckInfo& info() const noexcept override;
  void run(const SeriesContext& context, std::vector<Violation>& out) const override;
};

// DQ502: info, a run of bars with no volume that repeat the last close.
class CarryBar final : public Check {
 public:
  [[nodiscard]] const CheckInfo& info() const noexcept override;
  void run(const SeriesContext& context, std::vector<Violation>& out) const override;
};

}  // namespace dorq
