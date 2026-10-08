#pragma once

#include <cstdint>
#include <istream>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "dorq/date.hpp"
#include "dorq/series.hpp"
#include "dorq/violation.hpp"

namespace dorq {

// Context inputs (plan section 2.2): corporate actions (--actions), per-series
// metadata (--meta) and a market reference series (--market). Each is optional;
// each makes some checks possible or sharper. See doc/context.md.

// A split on file: `numerator` new shares for `denominator` old ones (a 2-for-1
// split is 2 and 1, as fafnir's core.corporate_action stores it). Prices before
// the ex-date are `denominator / numerator` times those after.
struct SplitAction {
  Date ex_date;
  double numerator = 1.0;
  double denominator = 1.0;
  std::uint32_t line = 0;  // in the actions file

  [[nodiscard]] double price_factor() const noexcept { return denominator / numerator; }
  // "2:1", "1:10", "3:2".
  [[nodiscard]] std::string ratio_text() const;
};

// A cash dividend on file, per share.
struct DividendAction {
  Date ex_date;
  double amount = 0.0;
  std::uint32_t line = 0;
};

// One series' actions, each list sorted by ex-date.
struct SeriesActions {
  std::vector<SplitAction> splits;
  std::vector<DividendAction> dividends;
};

// One series' metadata. Every field is optional.
struct SeriesMeta {
  std::string asset_type;  // equity, etf, fund, rate, ... (lower case)
  std::optional<bool> nav_priced;
  std::optional<double> tick_size;
  std::string peer_group;
  std::string exchange;
};

// The market reference: the log of its level by date.
struct MarketSeries {
  std::string name;
  std::vector<Date> date;  // sorted, unique
  std::vector<double> log_level;

  // The log level on the latest date on or before `when`, or nullopt before the
  // first.
  [[nodiscard]] std::optional<double> log_level_on(Date when) const;
};

// A row of a context file, or one field of it, that dorq could not use and
// skipped. Reported as DQ109; the run goes on without it.
struct ContextIssue {
  std::string series;  // as the row names it; empty when it names none
  // The file's name without its directory: fafnir writes its exports to a new
  // temporary directory each run, and the output must not change with it.
  std::string source;
  std::uint32_t line = 0;
  std::optional<Date> date;  // the row's ex-date, when it has a usable one
  Severity severity = Severity::kError;
  std::string message;  // "numerator \"0\" is not a positive number; the row is skipped"
};

struct Context {
  bool have_actions = false;
  bool have_meta = false;
  std::unordered_map<std::string, SeriesActions> actions;  // by series id
  std::unordered_map<std::string, SeriesMeta> meta;        // by series id
  std::optional<MarketSeries> market;
  std::vector<ContextIssue> issues;  // in the order the files were read

  // A series' entries, by its id or else its label (a ticker); nullptr if none.
  [[nodiscard]] const SeriesActions* actions_for(const Series& series) const;
  [[nodiscard]] const SeriesMeta* meta_for(const Series& series) const;
};

// Read a CSV or TSV actions file into `context`: columns series, ex_date, type
// (split or dividend), numerator, denominator, amount, under the names fafnir's
// core.corporate_action uses as well (security_id, action_type, split_numerator,
// split_denominator, dividend_amount). A row that cannot be used (no date, an
// unknown type, a split without its ratio) is skipped and added to
// `context.issues`. Throws InputError, naming the file, only when the file cannot
// be read as a whole: empty, no series, ex_date or type column, or an unterminated
// quote.
void read_actions(std::istream& in, const std::string& source, Context& context);

// Read a CSV or TSV metadata file: series, asset_type, nav_priced, tick_size,
// peer_group, exchange (or exchange_code). A series listed twice keeps its first
// row; a nav_priced or tick_size that cannot be read is left unset. Each goes to
// `context.issues`. Throws InputError when the file cannot be read as a whole.
void read_meta(std::istream& in, const std::string& source, Context& context);

// The market reference from a series' dates and closes (the first of each date;
// values at or below zero are skipped). Throws InputError when fewer than two
// dates remain.
[[nodiscard]] MarketSeries make_market(const Series& series);

}  // namespace dorq
