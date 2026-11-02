#pragma once

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
