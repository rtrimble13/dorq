#pragma once

#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "config/config.hpp"
#include "dorq/series.hpp"
#include "dorq/violation.hpp"

namespace dorq {

// What a check sees: one series, and the settings that apply to it.
struct SeriesContext {
  const Series& series;
  const IntegritySettings& integrity;
};

// A check reads one series and appends violations. Checks hold no state and are
// called concurrently on different series, so run() must not touch anything but
// its arguments.
class Check {
 public:
  Check() = default;
  Check(const Check&) = delete;
  Check& operator=(const Check&) = delete;
  Check(Check&&) = delete;
  Check& operator=(Check&&) = delete;
  virtual ~Check() = default;

  [[nodiscard]] virtual const CheckInfo& info() const noexcept = 0;
  virtual void run(const SeriesContext& context, std::vector<Violation>& out) const = 0;
};

// Every check, in code order.
[[nodiscard]] std::span<const Check* const> all_checks();

// A check by code ("DQ101", any case) or name ("ohlc-bounds"), or nullptr.
[[nodiscard]] const Check* find_check(std::string_view code_or_name);

// The page doc/checks/<code>.md, compiled in; empty if there is none.
[[nodiscard]] std::string_view check_doc(std::string_view code);

// Which checks run, from select / extend_select / ignore lists. Each entry is a
// code, a code prefix ("DQ1") or a name. As in flake8, the most specific entry
// wins: a check is enabled when its longest matching select entry is longer than
// its longest matching ignore entry. A name is the most specific match there is.
class Selection {
 public:
  Selection() = default;
  // Returns an error message for an entry that is neither a code prefix nor a
  // check name.
  [[nodiscard]] static std::string validate(std::span<const std::string> entries);

  Selection(std::span<const std::string> select, std::span<const std::string> ignore);

  [[nodiscard]] bool enabled(const CheckInfo& info) const;

 private:
  std::vector<std::string> select_;
  std::vector<std::string> ignore_;
};

}  // namespace dorq
