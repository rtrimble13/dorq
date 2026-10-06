#pragma once

#include <string>
#include <vector>

#include "checks/check.hpp"

namespace dorq {

// DQ201-DQ205 and DQ209: price action. One model scores every suspicious bar
// (src/price/model.cpp); each check reports the bars whose most probable
// explanation is its own. See doc/checks/DQ201.md.
class PriceCheck final : public Check {
 public:
  explicit PriceCheck(const CheckInfo& info) : info_(info) {}
  [[nodiscard]] const CheckInfo& info() const noexcept override { return info_; }
  void run(const SeriesContext& context, std::vector<Violation>& out) const override;

 private:
  const CheckInfo& info_;
};

[[nodiscard]] const CheckInfo& bad_print_info() noexcept;            // DQ201
[[nodiscard]] const CheckInfo& scale_shift_info() noexcept;          // DQ202
[[nodiscard]] const CheckInfo& unreported_split_info() noexcept;     // DQ203
[[nodiscard]] const CheckInfo& ohlc_close_mismatch_info() noexcept;  // DQ204
[[nodiscard]] const CheckInfo& history_segment_info() noexcept;      // DQ205
[[nodiscard]] const CheckInfo& large_move_info() noexcept;           // DQ209

// The DQ203 report of a finding as if an unreported split were its most probable
// error, without the "; P(error)" ending and with a provisional p_error and
// severity: for evidence found once every series is in (DQ601's siblings).
[[nodiscard]] Violation unreported_split_base(const SeriesContext& context,
                                              const PriceAnalysis& analysis,
                                              const PriceFinding& finding);

// A number to `digits` significant figures, for messages: 0.258512 -> "0.2585".
[[nodiscard]] std::string significant(double value, int digits);

}  // namespace dorq
