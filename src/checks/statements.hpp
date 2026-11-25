#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "checks/check.hpp"
#include "dorq/violation.hpp"

namespace dorq {

// The statement checks (DQ9xx, ADR 0005; doc/statements.md). They read a statements
// series' rows and views, and the [statements] settings.

// DQ901
class StatementKey final : public Check {
 public:
  [[nodiscard]] const CheckInfo& info() const noexcept override;
  void run(const SeriesContext& context, std::vector<Violation>& out) const override;
};

// DQ902
class ImpossibleValue final : public Check {
 public:
  [[nodiscard]] const CheckInfo& info() const noexcept override;
  void run(const SeriesContext& context, std::vector<Violation>& out) const override;
};

// DQ903 (identities.cpp)
class IdentityBreak final : public Check {
 public:
  [[nodiscard]] const CheckInfo& info() const noexcept override;
  void run(const SeriesContext& context, std::vector<Violation>& out) const override;
};

// DQ904 (identities.cpp)
class PeriodTie final : public Check {
 public:
  [[nodiscard]] const CheckInfo& info() const noexcept override;
  void run(const SeriesContext& context, std::vector<Violation>& out) const override;
};

// How often each identity and tie breaks on a statements series' periods in force
// (identities.cpp): by identity name, "cash" and "quarter_sum", the periods judged
// and those that break. What `dorq calibrate` pools across series into
// [statements.break_rates], the cross-section's rates (DE-11).
struct BreakCount {
  std::string name;
  std::size_t breaks = 0;
  std::size_t periods = 0;
};
[[nodiscard]] std::vector<BreakCount> count_breaks(const SeriesContext& context);

// DQ911, DQ912, DQ913 and DQ919 (values.cpp): the statement value model's findings.
class BadValue final : public Check {
 public:
  [[nodiscard]] const CheckInfo& info() const noexcept override;
  void run(const SeriesContext& context, std::vector<Violation>& out) const override;
};

class UnitError final : public Check {
 public:
  [[nodiscard]] const CheckInfo& info() const noexcept override;
  void run(const SeriesContext& context, std::vector<Violation>& out) const override;
};

class SignError final : public Check {
 public:
  [[nodiscard]] const CheckInfo& info() const noexcept override;
  void run(const SeriesContext& context, std::vector<Violation>& out) const override;
};

class CopiedPeriod final : public Check {
 public:
  [[nodiscard]] const CheckInfo& info() const noexcept override;
  void run(const SeriesContext& context, std::vector<Violation>& out) const override;
};

class AbsentAsZero final : public Check {
 public:
  [[nodiscard]] const CheckInfo& info() const noexcept override;
  void run(const SeriesContext& context, std::vector<Violation>& out) const override;
};

class CumulativeQuarters final : public Check {
 public:
  [[nodiscard]] const CheckInfo& info() const noexcept override;
  void run(const SeriesContext& context, std::vector<Violation>& out) const override;
};

class ShareTermsStep final : public Check {
 public:
  [[nodiscard]] const CheckInfo& info() const noexcept override;
  void run(const SeriesContext& context, std::vector<Violation>& out) const override;
};

class ShareCountMismatch final : public Check {
 public:
  [[nodiscard]] const CheckInfo& info() const noexcept override;
  void run(const SeriesContext& context, std::vector<Violation>& out) const override;
};

class FilingDateImplausible final : public Check {
 public:
  [[nodiscard]] const CheckInfo& info() const noexcept override;
  void run(const SeriesContext& context, std::vector<Violation>& out) const override;
};

class RestatementImplausible final : public Check {
 public:
  [[nodiscard]] const CheckInfo& info() const noexcept override;
  void run(const SeriesContext& context, std::vector<Violation>& out) const override;
};

class PeriodGap final : public Check {
 public:
  [[nodiscard]] const CheckInfo& info() const noexcept override;
  void run(const SeriesContext& context, std::vector<Violation>& out) const override;
};

class StatementsStale final : public Check {
 public:
  [[nodiscard]] const CheckInfo& info() const noexcept override;
  void run(const SeriesContext& context, std::vector<Violation>& out) const override;
};

class StatementsMissing final : public Check {
 public:
  [[nodiscard]] const CheckInfo& info() const noexcept override;
  void run(const SeriesContext& context, std::vector<Violation>& out) const override;
};

class ReportingCurrency final : public Check {
 public:
  [[nodiscard]] const CheckInfo& info() const noexcept override;
  void run(const SeriesContext& context, std::vector<Violation>& out) const override;
};

class LargeChange final : public Check {
 public:
  [[nodiscard]] const CheckInfo& info() const noexcept override;
  void run(const SeriesContext& context, std::vector<Violation>& out) const override;
};

}  // namespace dorq
