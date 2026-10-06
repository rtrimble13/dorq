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

struct Context {
  bool have_actions = false;
  bool have_meta = false;
  std::unordered_map<std::string, SeriesActions> actions;  // by series id
  std::unordered_map<std::string, SeriesMeta> meta;        // by series id
  std::optional<MarketSeries> market;

  // A series' entries, by its id or else its label (a ticker); nullptr if none.
  [[nodiscard]] const SeriesActions* actions_for(const Series& series) const;
  [[nodiscard]] const SeriesMeta* meta_for(const Series& series) const;
};

// Read a CSV or TSV actions file into `context`: columns series, ex_date, type
// (split or dividend), numerator, denominator, amount, under the names fafnir's
// core.corporate_action uses as well (security_id, action_type, split_numerator,
// split_denominator, dividend_amount). Throws InputError naming the file and line.
void read_actions(std::istream& in, const std::string& source, Context& context);

// Read a CSV or TSV metadata file: series, asset_type, nav_priced, tick_size,
// peer_group, exchange (or exchange_code). Throws InputError.
void read_meta(std::istream& in, const std::string& source, Context& context);

// The market reference from a series' dates and closes (the first of each date;
// values at or below zero are skipped). Throws InputError when fewer than two
// dates remain.
[[nodiscard]] MarketSeries make_market(const Series& series);

}  // namespace dorq
