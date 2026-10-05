#pragma once

#include <vector>

#include "checks/check.hpp"

namespace dorq {

// DQ105 and DQ206 (calendar), and DQ3xx (coverage). See doc/checks/.

// DQ105
class NonSessionBar final : public Check {
 public:
  [[nodiscard]] const CheckInfo& info() const noexcept override;
  void run(const SeriesContext& context, std::vector<Violation>& out) const override;
};

// DQ206
class DateShift final : public Check {
 public:
  [[nodiscard]] const CheckInfo& info() const noexcept override;
  void run(const SeriesContext& context, std::vector<Violation>& out) const override;
};

// DQ301
class MissingRunCheck final : public Check {
 public:
  [[nodiscard]] const CheckInfo& info() const noexcept override;
  void run(const SeriesContext& context, std::vector<Violation>& out) const override;
};

// DQ302
class SparseSeries final : public Check {
 public:
  [[nodiscard]] const CheckInfo& info() const noexcept override;
  void run(const SeriesContext& context, std::vector<Violation>& out) const override;
};

// DQ303: judged across series by the engine (src/engine/cross_section.cpp).
class CohortGap final : public Check {
 public:
  [[nodiscard]] const CheckInfo& info() const noexcept override;
  void run(const SeriesContext& /*context*/, std::vector<Violation>& /*out*/) const override {}
};

// DQ304: judged at the end of a run, against the as-of date.
class StaleFeed final : public Check {
 public:
  [[nodiscard]] const CheckInfo& info() const noexcept override;
  void run(const SeriesContext& /*context*/, std::vector<Violation>& /*out*/) const override {}
};

// DQ305
class FrequencyGap final : public Check {
 public:
  [[nodiscard]] const CheckInfo& info() const noexcept override;
  void run(const SeriesContext& context, std::vector<Violation>& out) const override;
};

// "0.97", or ">0.99" when rounding would claim certainty.
[[nodiscard]] std::string format_probability(double p);

}  // namespace dorq
