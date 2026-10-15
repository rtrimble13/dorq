#pragma once

#include <string>
#include <vector>

#include "checks/check.hpp"
#include "context/context.hpp"

namespace dorq {

// DQ1xx: integrity. Deterministic checks of a row, or a series, on its own
// terms; each violation has p_error = 1. See doc/checks/DQ1xx.md.

// DQ101
class OhlcBounds final : public Check {
 public:
  [[nodiscard]] const CheckInfo& info() const noexcept override;
  void run(const SeriesContext& context, std::vector<Violation>& out) const override;
};

// DQ102
class NonPositive final : public Check {
 public:
  [[nodiscard]] const CheckInfo& info() const noexcept override;
  void run(const SeriesContext& context, std::vector<Violation>& out) const override;
};

// DQ103
class DuplicateDate final : public Check {
 public:
  [[nodiscard]] const CheckInfo& info() const noexcept override;
  void run(const SeriesContext& context, std::vector<Violation>& out) const override;
};

// DQ104
class MissingField final : public Check {
 public:
  [[nodiscard]] const CheckInfo& info() const noexcept override;
  void run(const SeriesContext& context, std::vector<Violation>& out) const override;
};

// DQ106
class PrecisionShift final : public Check {
 public:
  [[nodiscard]] const CheckInfo& info() const noexcept override;
  void run(const SeriesContext& context, std::vector<Violation>& out) const override;
};

// DQ107
class ZeroRangeWithVolume final : public Check {
 public:
  [[nodiscard]] const CheckInfo& info() const noexcept override;
  void run(const SeriesContext& context, std::vector<Violation>& out) const override;
};

// DQ107 and DQ501: a series that trades thinly, below [integrity]
// flat_bar_min_median_volume shares or flat_bar_min_median_dollar_volume dollars a
// day (the medians over days with trading). One or two prints make its day, so a
// flat bar or a repeated close is how its trading looks.
struct ThinSeries {
  bool thin = false;
  double median_dollar_volume = 0.0;
  std::string why;  // for a message: "a median 844 shares a day"
};

[[nodiscard]] ThinSeries thin_series(double median_volume, double median_dollar_volume,
                                     const IntegritySettings& settings);

// DQ108
class OutOfBounds final : public Check {
 public:
  [[nodiscard]] const CheckInfo& info() const noexcept override;
  void run(const SeriesContext& context, std::vector<Violation>& out) const override;
};

// DQ109: a row of a context file (--actions, --meta) that cannot be used, and was
// skipped. The engine reports Context::issues at the end of the run, each under
// the series and the file it names, whether or not that series is in the input.
class BadContextRow final : public Check {
 public:
  [[nodiscard]] const CheckInfo& info() const noexcept override;
  void run(const SeriesContext& /*context*/, std::vector<Violation>& /*out*/) const override {}
};

// The DQ109 report of one skipped row or field.
[[nodiscard]] Violation bad_context_row(const ContextIssue& issue);

}  // namespace dorq
