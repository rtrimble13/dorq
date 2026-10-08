#pragma once

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
