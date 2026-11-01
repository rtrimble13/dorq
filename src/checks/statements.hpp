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

}  // namespace dorq
