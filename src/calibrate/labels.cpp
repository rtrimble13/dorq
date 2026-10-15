#include "calibrate/labels.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numeric>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "core/text.hpp"
#include "io/input_error.hpp"
#include "io/json.hpp"

namespace dorq {
namespace {

[[noreturn]] void fail(const std::string& source, std::uint32_t line, const std::string& what) {
  throw InputError(source + " line " + std::to_string(line) + ": " + what);
}

Date date_of(const std::string& text, const std::string& source, std::uint32_t line,
             std::string_view key) {
  const auto date = parse_date(trim(text));
  if (!date) {
    fail(source, line, std::string{key} + " \"" + text + "\" is not a date");
  }
  return *date;
}

std::int64_t timestamp_of(const std::string& text, const std::string& source, std::uint32_t line,
                          std::string_view key) {
  const auto when = parse_timestamp(trim(text));
  if (!when) {
    fail(source, line, std::string{key} + " \"" + text + "\" is not an ISO 8601 timestamp");
  }
  return *when;
}

// Digits [pos, pos + n) of `text` as a number; nullopt when any is not a digit.
std::optional<int> digits(std::string_view text, std::size_t pos, std::size_t n) {
  if (pos + n > text.size()) {
    return std::nullopt;
  }
  int value = 0;
  for (std::size_t i = pos; i < pos + n; ++i) {
    if (text[i] < '0' || text[i] > '9') {
      return std::nullopt;
    }
    value = value * 10 + (text[i] - '0');
  }
  return value;
}

// The items of a comma-separated list, trimmed, with empty ones left out.
std::vector<std::string> list_items(const std::string& text) {
  std::vector<std::string> items;
  std::stringstream in(text);
  std::string item;
  while (std::getline(in, item, ',')) {
    if (const std::string_view trimmed = trim(item); !trimmed.empty()) {
      items.emplace_back(trimmed);
    }
  }
  return items;
}

LabelClass class_of(const std::string& text, const std::string& source, std::uint32_t line) {
  if (text == "data_error") {
    return LabelClass::kDataError;
  }
  if (text == "context_gap") {
    return LabelClass::kContextGap;
  }
  if (text == "market_fact") {
    return LabelClass::kMarketFact;
  }
  fail(source, line, "class \"" + text + "\" is not data_error, context_gap or market_fact");
}

}  // namespace

std::optional<std::int64_t> parse_timestamp(std::string_view text) {
  constexpr std::int64_t kDay = 86400;
  const auto date = parse_date(text.substr(0, std::min<std::size_t>(10, text.size())));
  if (!date) {
    return std::nullopt;
  }
  const Date epoch = Date::from_ymd(1970, 1, 1);
  std::int64_t seconds = (std::int64_t{date->days()} - epoch.days()) * kDay;
  std::size_t pos = 10;
  if (pos == text.size()) {
    return seconds;
  }
  if (text[pos] != 'T' && text[pos] != 't' && text[pos] != ' ') {
    return std::nullopt;
  }
  ++pos;
  const auto hour = digits(text, pos, 2);
  const auto minute =
      pos + 2 < text.size() && text[pos + 2] == ':' ? digits(text, pos + 3, 2) : std::nullopt;
  if (!hour || !minute || *hour > 23 || *minute > 59) {
    return std::nullopt;
  }
  seconds += *hour * 3600 + *minute * 60;
  pos += 5;
  if (pos < text.size() && text[pos] == ':') {
    const auto second = digits(text, pos + 1, 2);
    if (!second || *second > 60) {
      return std::nullopt;
    }
    seconds += *second;
    pos += 3;
    if (pos < text.size() && (text[pos] == '.' || text[pos] == ',')) {
      ++pos;  // a fraction of a second: dropped
      const std::size_t start = pos;
      while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') {
        ++pos;
      }
      if (pos == start) {
        return std::nullopt;
      }
    }
  }
  if (pos == text.size()) {
    return seconds;
  }
  if ((text[pos] == 'Z' || text[pos] == 'z') && pos + 1 == text.size()) {
    return seconds;
  }
  if ((text[pos] == '+' || text[pos] == '-') && pos + 6 == text.size() && text[pos + 3] == ':') {
    const auto oh = digits(text, pos + 1, 2);
    const auto om = digits(text, pos + 4, 2);
    if (!oh || !om || *oh > 23 || *om > 59) {
      return std::nullopt;
    }
    const std::int64_t offset = *oh * 3600 + *om * 60;
    return text[pos] == '+' ? seconds - offset : seconds + offset;
  }
  return std::nullopt;
}

std::string_view to_string(LabelClass label) noexcept {
  switch (label) {
    case LabelClass::kDataError:
      return "data_error";
    case LabelClass::kContextGap:
      return "context_gap";
    case LabelClass::kMarketFact:
      return "market_fact";
  }
  return "data_error";
}

std::vector<Label> read_labels(std::istream& in, const std::string& source) {
  std::vector<Label> labels;
  std::string text;
  std::uint32_t line = 0;
  while (std::getline(in, text)) {
    ++line;
    std::size_t pos = 0;
    skip_json_whitespace(text, pos);
    if (pos == text.size()) {
      continue;
    }
    const JsonRecord record =
        parse_json_record(text, pos, source + " line " + std::to_string(line));
    Label label;
    label.line = line;
    bool have_first = false;
    bool have_last = false;
    bool have_class = false;
    for (const JsonMember& m : record) {
      if (m.is_null) {
        continue;
      }
      if (m.key == "series") {
        label.series = m.text;
      } else if (m.key == "date" || m.key == "first") {
        label.first = date_of(m.text, source, line, m.key);
        have_first = true;
        if (m.key == "date") {
          label.last = label.first;
          have_last = true;
        }
      } else if (m.key == "last") {
        label.last = date_of(m.text, source, line, m.key);
        have_last = true;
      } else if (m.key == "class") {
        have_class = true;
        label.label_class = class_of(m.text, source, line);
      } else if (m.key == "expect") {
        label.expect = m.text;
      } else if (m.key == "codes") {
        label.codes = m.text;
      } else if (m.key == "kind") {
        label.kind = m.text;
      } else if (m.key == "source") {
        label.source = m.text;
      } else if (m.key == "note") {
        label.note = m.text;
      } else if (m.key == "remove") {
        for (const std::string& item : list_items(m.text)) {
          label.remove.push_back(date_of(item, source, line, "remove"));
        }
      } else if (m.key == "decided_at") {
        label.decided_at = timestamp_of(m.text, source, line, m.key);
      } else if (m.key == "applies_until") {
        label.applies_until = timestamp_of(m.text, source, line, m.key);
      } else if (m.key == "verified") {
        bool verified = false;
        if (!parse_bool(m.text, verified)) {
          fail(source, line, "verified \"" + m.text + "\" is not true or false");
        }
        label.verified = verified;
      } else if (m.key == "members") {
        label.members = list_items(m.text);
      }
      // Other keys are kept by the exporter for people, and ignored here.
    }
    if (label.series.empty() || !have_first || !have_class) {
      fail(source, line, "a label needs series, first (or date) and class");
    }
    if (!have_last) {
      label.last = label.first;
    }
    if (label.last < label.first) {
      fail(source, line, "last is before first");
    }
    if (label.label_class == LabelClass::kMarketFact && label.codes.empty()) {
      label.codes = "DQ2";
    }
    labels.push_back(std::move(label));
  }
  return labels;
}

Restorer::Restorer(const std::vector<Label>& labels, std::vector<Series> restored) {
  for (Series& series : restored) {
    std::string id = series.id;
    rows_.emplace(std::move(id), std::move(series));
  }
  for (const Label& label : labels) {
    auto& dates = removed_[label.series];
    dates.insert(dates.end(), label.remove.begin(), label.remove.end());
  }
  std::erase_if(removed_, [](const auto& entry) { return entry.second.empty(); });
}

void Restorer::apply(Series& series) const {
  const auto find = [&series](const auto& map) {
    auto it = map.find(series.id);
    if (it == map.end() && !series.label.empty()) {
      it = map.find(series.label);
    }
    return it;
  };
  const auto restore_it = find(rows_);
  const auto removed_it = find(removed_);
  const Series* restore = restore_it != rows_.end() ? &restore_it->second : nullptr;
  const std::vector<Date>* removed = removed_it != removed_.end() ? &removed_it->second : nullptr;
  if (restore == nullptr && removed == nullptr) {
    return;
  }
  const auto replaced = [&](Date day) {
    return (restore != nullptr &&
            std::find(restore->date.begin(), restore->date.end(), day) != restore->date.end()) ||
           (removed != nullptr &&
            std::find(removed->begin(), removed->end(), day) != removed->end());
  };
  // The rows that stay, then the restored ones, in date order.
  struct Source {
    const Series* from;
    std::size_t row;
  };
  std::vector<Source> rows;
  for (std::size_t i = 0; i < series.size(); ++i) {
    if (!replaced(series.date[i])) {
      rows.push_back({&series, i});
    }
  }
  if (restore != nullptr) {
    for (std::size_t i = 0; i < restore->size(); ++i) {
      rows.push_back({restore, i});
    }
  }
  std::stable_sort(rows.begin(), rows.end(), [](const Source& a, const Source& b) {
    return a.from->date[a.row] < b.from->date[b.row];
  });
  constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
  const auto column = [](const std::vector<double>& values, std::size_t row) {
    return row < values.size() ? values[row] : kNaN;
  };
  Series out;
  out.id = series.id;
  out.label = series.label;
  out.source = series.source;
  out.kind = series.kind;
  out.has_volume = series.has_volume;
  out.has_vwap = series.has_vwap;
  out.issues = series.issues;
  for (const Source& r : rows) {
    const Series& s = *r.from;
    out.date.push_back(s.date[r.row]);
    out.close.push_back(s.close[r.row]);
    out.close_decimals.push_back(s.close_decimals[r.row]);
    out.close_sig_figs.push_back(s.close_sig_figs[r.row]);
    out.line.push_back(r.from == &series ? s.line[r.row] : 0);
    if (out.kind == SeriesKind::kOhlcv) {
      out.open.push_back(column(s.open, r.row));
      out.high.push_back(column(s.high, r.row));
      out.low.push_back(column(s.low, r.row));
    }
    if (out.has_volume) {
      out.volume.push_back(s.has_volume ? s.volume[r.row] : kNaN);
    }
    if (out.has_vwap) {
      out.vwap.push_back(s.has_vwap ? s.vwap[r.row] : kNaN);
    }
  }
  series = std::move(out);
}

}  // namespace dorq
