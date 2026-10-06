#pragma once

#include <vector>

#include "checks/check.hpp"
#include "price/model.hpp"

namespace dorq {

// DQ7xx: corporate actions, with --actions. See doc/checks/DQ701.md to DQ705.md.

// DQ701-DQ704: a split on file that the bars contradict. One model judges each
// split (price/model.cpp, split_findings); each check reports the splits whose
// most probable error is its own.
class SplitOnFileCheck final : public Check {
 public:
  SplitOnFileCheck(const CheckInfo& info, SplitVerdict verdict) : info_(info), verdict_(verdict) {}
  [[nodiscard]] const CheckInfo& info() const noexcept override { return info_; }
  void run(const SeriesContext& context, std::vector<Violation>& out) const override;

 private:
  const CheckInfo& info_;
  SplitVerdict verdict_;
};

// DQ705: a dividend at or above the price, or an order of magnitude off the
// series' others.
class DividendImplausible final : public Check {
 public:
  [[nodiscard]] const CheckInfo& info() const noexcept override;
  void run(const SeriesContext& context, std::vector<Violation>& out) const override;
};

[[nodiscard]] const CheckInfo& split_misdated_info() noexcept;          // DQ701
[[nodiscard]] const CheckInfo& split_without_jump_info() noexcept;      // DQ702
[[nodiscard]] const CheckInfo& split_ratio_mismatch_info() noexcept;    // DQ703
[[nodiscard]] const CheckInfo& split_double_applied_info() noexcept;    // DQ704

}  // namespace dorq
