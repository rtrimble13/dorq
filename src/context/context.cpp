#include "context/context.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "core/text.hpp"
#include "dorq/number.hpp"
#include "io/csv.hpp"
#include "io/input_error.hpp"

namespace dorq {
namespace {

using Row = std::span<const std::string_view>;

// An error that already names the file and line.
class TableError : public InputError {
 public:
  using InputError::InputError;
};
using RowFn = std::function<void(Row fields, std::uint32_t line)>;
using HeaderFn = std::function<void(const std::vector<std::string>& names, std::uint32_t line)>;

// Reads a CSV or TSV table: the delimiter is whichever of tab and comma the
// header line has more of.
void read_table(std::istream& in, const std::string& source, const HeaderFn& on_header,
                const RowFn& on_row) {
  std::ostringstream buffer;
  buffer << in.rdbuf();
  const std::string text = buffer.str();
  const std::string_view first_line = std::string_view{text}.substr(0, text.find('\n'));
  const char delimiter = std::count(first_line.begin(), first_line.end(), '\t') >
                                 std::count(first_line.begin(), first_line.end(), ',')
                             ? '\t'
                             : ',';
  bool have_header = false;
  CsvParser parser(delimiter);
  const CsvParser::RecordFn on_record = [&](Row fields, std::uint32_t line) {
    if (!have_header) {
      on_header(std::vector<std::string>(fields.begin(), fields.end()), line);
      have_header = true;
      return;
    }
    on_row(fields, line);
  };
  try {
    parser.feed(text, on_record);
    parser.finish(on_record);
  } catch (const TableError&) {
    throw;
  } catch (const InputError& error) {
    throw InputError(source + ": " + error.what());
  }
  if (!have_header) {
    throw InputError(source + " is empty");
  }
}

[[noreturn]] void fail(const std::string& source, std::uint32_t line, const std::string& what) {
  throw TableError(source + (line > 0 ? " line " + std::to_string(line) : "") + ": " + what);
}

std::string_view cell(Row fields, int col) {
  return col >= 0 && static_cast<std::size_t>(col) < fields.size()
             ? trim(fields[static_cast<std::size_t>(col)])
             : std::string_view{};
}

constexpr std::array<std::string_view, 5> kSeries = {"series", "securityid", "id", "symbol",
                                                     "ticker"};

// A field that should hold a positive number: its value (nullopt for an empty
// cell), or else what is wrong with it.
struct Positive {
  std::optional<double> value;
  std::string problem;  // empty when the field is usable
};

Positive positive(Row fields, int col, std::string_view what) {
  const std::string_view text = cell(fields, col);
  const ParsedNumber number = parse_number(text);
  if (number.status == ParsedNumber::Status::kMissing) {
    return {};
  }
  if (number.status != ParsedNumber::Status::kOk || !(number.value > 0.0)) {
    return {
        .value = std::nullopt,
        .problem = std::string{what} + " \"" + std::string{text} + "\" is not a positive number"};
  }
  return {.value = number.value, .problem = {}};
}

// What a ContextIssue names its file by (see there).
std::string file_name(const std::string& source) {
  return std::filesystem::path(source).filename().string();
}

}  // namespace

std::string SplitAction::ratio_text() const {
  return format_number(numerator) + ":" + format_number(denominator);
}

std::optional<double> MarketSeries::log_level_on(Date when) const {
  const auto it = std::upper_bound(date.begin(), date.end(), when);
  if (it == date.begin()) {
    return std::nullopt;
  }
  return log_level[static_cast<std::size_t>(it - date.begin()) - 1];
}

const SeriesActions* Context::actions_for(const Series& series) const {
  if (auto it = actions.find(series.id); it != actions.end()) {
    return &it->second;
  }
  if (!series.label.empty()) {
    if (auto it = actions.find(series.label); it != actions.end()) {
      return &it->second;
    }
  }
  return nullptr;
}

const SeriesMeta* Context::meta_for(const Series& series) const {
  if (auto it = meta.find(series.id); it != meta.end()) {
    return &it->second;
  }
  if (!series.label.empty()) {
    if (auto it = meta.find(series.label); it != meta.end()) {
      return &it->second;
    }
  }
  return nullptr;
}

void read_actions(std::istream& in, const std::string& source, Context& context) {
  constexpr std::array<std::string_view, 4> kDate = {"exdate", "date", "effectivedate",
                                                     "tradedate"};
  constexpr std::array<std::string_view, 3> kType = {"type", "actiontype", "kind"};
  constexpr std::array<std::string_view, 2> kNumerator = {"numerator", "splitnumerator"};
  constexpr std::array<std::string_view, 2> kDenominator = {"denominator", "splitdenominator"};
  constexpr std::array<std::string_view, 3> kAmount = {"amount", "dividendamount", "dividend"};
  int series_col = -1;
  int date_col = -1;
  int type_col = -1;
  int numerator_col = -1;
  int denominator_col = -1;
  int amount_col = -1;
  context.have_actions = true;
  const std::string name = file_name(source);
  read_table(
      in, source,
      [&](const std::vector<std::string>& names, std::uint32_t line) {
        series_col = find_column(names, kSeries);
        date_col = find_column(names, kDate);
        type_col = find_column(names, kType);
        numerator_col = find_column(names, kNumerator);
        denominator_col = find_column(names, kDenominator);
        amount_col = find_column(names, kAmount);
        if (series_col < 0 || date_col < 0 || type_col < 0) {
          fail(source, line,
               "an actions file needs series, ex_date and type columns (or security_id, "
               "ex_date and action_type)");
        }
      },
      [&](Row fields, std::uint32_t line) {
        // A row that cannot be used is skipped and reported (DQ109): one bad row
        // must not cost the run every other check.
        const std::string id{cell(fields, series_col)};
        const std::string_view date_text = cell(fields, date_col);
        const auto date = parse_date(date_text);
        const auto skip = [&](const std::string& what) {
          context.issues.push_back({.series = id,
                                    .source = name,
                                    .line = line,
                                    .date = date,
                                    .severity = Severity::kError,
                                    .message = what + "; the row is skipped"});
        };
        if (id.empty()) {
          skip("the series is empty");
          return;
        }
        if (!date) {
          skip("ex_date \"" + std::string{date_text} + "\" is not a date");
          return;
        }
        const std::string type = normalize_name(cell(fields, type_col));
        if (type == "split") {
          const Positive numerator = positive(fields, numerator_col, "numerator");
          const Positive denominator = positive(fields, denominator_col, "denominator");
          if (!numerator.problem.empty() || !denominator.problem.empty()) {
            skip(numerator.problem.empty() ? denominator.problem : numerator.problem);
          } else if (!numerator.value || !denominator.value) {
            skip("a split needs a numerator and a denominator");
          } else {
            context.actions[id].splits.push_back(
                {*date, *numerator.value, *denominator.value, line});
          }
        } else if (type == "dividend") {
          // Any number: a zero or negative amount is DQ705's to report, not an
          // unusable row. fafnir's core.corporate_action allows a zero.
          const std::string_view text = cell(fields, amount_col);
          const ParsedNumber amount = parse_number(text);
          if (amount.status == ParsedNumber::Status::kMissing) {
            skip("a dividend needs an amount");
          } else if (amount.status != ParsedNumber::Status::kOk) {
            skip("amount \"" + std::string{text} + "\" is not a number");
          } else {
            context.actions[id].dividends.push_back({*date, amount.value, line});
          }
        } else {
          skip("type \"" + std::string{cell(fields, type_col)} + "\" is not split or dividend");
        }
      });
  for (auto& [id, actions] : context.actions) {
    std::stable_sort(
        actions.splits.begin(), actions.splits.end(),
        [](const SplitAction& a, const SplitAction& b) { return a.ex_date < b.ex_date; });
    std::stable_sort(
        actions.dividends.begin(), actions.dividends.end(),
        [](const DividendAction& a, const DividendAction& b) { return a.ex_date < b.ex_date; });
  }
}

void read_meta(std::istream& in, const std::string& source, Context& context) {
  constexpr std::array<std::string_view, 2> kAssetType = {"assettype", "type"};
  constexpr std::array<std::string_view, 2> kNav = {"navpriced", "nav"};
  constexpr std::array<std::string_view, 2> kTick = {"ticksize", "tick"};
  constexpr std::array<std::string_view, 2> kPeer = {"peergroup", "family"};
  constexpr std::array<std::string_view, 3> kExchange = {"exchange", "exchangecode", "mic"};
  int series_col = -1;
  int asset_col = -1;
  int nav_col = -1;
  int tick_col = -1;
  int peer_col = -1;
  int exchange_col = -1;
  context.have_meta = true;
  const std::string name = file_name(source);
  read_table(
      in, source,
      [&](const std::vector<std::string>& names, std::uint32_t line) {
        series_col = find_column(names, kSeries);
        asset_col = find_column(names, kAssetType);
        nav_col = find_column(names, kNav);
        tick_col = find_column(names, kTick);
        peer_col = find_column(names, kPeer);
        exchange_col = find_column(names, kExchange);
        if (series_col < 0) {
          fail(source, line, "a metadata file needs a series column (or security_id)");
        }
      },
      [&](Row fields, std::uint32_t line) {
        // As for actions, what cannot be used is reported (DQ109) and the run
        // goes on: at warn, since a series loses no more than a profile match
        // or its tick.
        const std::string id{cell(fields, series_col)};
        const auto note = [&](const std::string& what) {
          context.issues.push_back({.series = id,
                                    .source = name,
                                    .line = line,
                                    .date = std::nullopt,
                                    .severity = Severity::kWarn,
                                    .message = what});
        };
        if (id.empty()) {
          note("the series is empty; the row is skipped");
          return;
        }
        if (context.meta.contains(id)) {
          note("series \"" + id + "\" appears twice; the row is skipped, and the first kept");
          return;
        }
        SeriesMeta meta;
        meta.asset_type = normalize_name(cell(fields, asset_col));
        if (const std::string_view nav = cell(fields, nav_col); !nav.empty()) {
          bool value = false;
          if (parse_bool(nav, value)) {
            meta.nav_priced = value;
          } else {
            note("nav_priced \"" + std::string{nav} + "\" is not true or false; it is left unset");
          }
        }
        const Positive tick = positive(fields, tick_col, "tick_size");
        if (tick.problem.empty()) {
          meta.tick_size = tick.value;
        } else {
          note(tick.problem + "; it is left unset");
        }
        meta.peer_group = std::string{cell(fields, peer_col)};
        meta.exchange = std::string{cell(fields, exchange_col)};
        context.meta.emplace(id, std::move(meta));
      });
}

MarketSeries make_market(const Series& series) {
  MarketSeries market;
  market.name = series.display_name();
  for (std::size_t i = 0; i < series.size(); ++i) {
    const double value = series.close[i];
    if (!std::isfinite(value) || value <= 0.0 ||
        (!market.date.empty() && market.date.back() == series.date[i])) {
      continue;
    }
    market.date.push_back(series.date[i]);
    market.log_level.push_back(std::log(value));
  }
  if (market.date.size() < 2) {
    throw InputError(series.source + ": the market series needs at least two positive values");
  }
  return market;
}

}  // namespace dorq
