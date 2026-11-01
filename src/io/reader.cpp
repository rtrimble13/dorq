#include "io/reader.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <istream>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

#include "core/text.hpp"
#include "dorq/number.hpp"
#include "io/csv.hpp"
#include "io/input_error.hpp"
#include "io/json.hpp"

namespace dorq {
namespace {

constexpr std::size_t kChunkSize = std::size_t{1} << 20U;
constexpr std::size_t kIssueTextMax = 40;
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

// Reads `in` in chunks. The first chunk can be inspected before it is consumed,
// which is how the format is sniffed.
class ChunkSource {
 public:
  explicit ChunkSource(std::istream& in) : in_(in) {}

  // The next chunk, or empty at the end of input.
  std::string_view next() {
    if (pending_) {
      pending_ = false;
      return buffer_;
    }
    return read();
  }

  std::string_view peek() {
    if (!pending_) {
      read();
      pending_ = true;
    }
    return buffer_;
  }

 private:
  std::string_view read() {
    buffer_.resize(kChunkSize);
    in_.read(buffer_.data(), static_cast<std::streamsize>(buffer_.size()));
    buffer_.resize(static_cast<std::size_t>(in_.gcount()));
    if (in_.bad()) {
      throw InputError("read error");
    }
    if (first_) {
      first_ = false;
      if (buffer_.size() >= 3 && buffer_.compare(0, 3, "\xEF\xBB\xBF") == 0) {
        buffer_.erase(0, 3);  // UTF-8 byte-order mark
      }
    }
    return buffer_;
  }

  std::istream& in_;
  std::string buffer_;
  bool pending_ = false;
  bool first_ = true;
};

InputFormat sniff_format(std::string_view head) {
  std::size_t pos = 0;
  skip_json_whitespace(head, pos);
  if (pos < head.size() && head[pos] == '[') {
    return InputFormat::kJson;
  }
  if (pos < head.size() && head[pos] == '{') {
    return InputFormat::kJsonl;
  }
  const std::string_view first_line = head.substr(0, head.find('\n'));
  std::size_t tabs = 0;
  std::size_t commas = 0;
  for (const char ch : first_line) {
    tabs += ch == '\t' ? 1U : 0U;
    commas += ch == ',' ? 1U : 0U;
  }
  return tabs > commas ? InputFormat::kTsv : InputFormat::kCsv;
}

InputFormat format_from_extension(std::string_view extension) {
  if (iequals(extension, ".csv")) {
    return InputFormat::kCsv;
  }
  if (iequals(extension, ".tsv") || iequals(extension, ".tab")) {
    return InputFormat::kTsv;
  }
  if (iequals(extension, ".jsonl") || iequals(extension, ".ndjson")) {
    return InputFormat::kJsonl;
  }
  if (iequals(extension, ".json")) {
    return InputFormat::kJson;
  }
  return InputFormat::kAuto;
}

// A whole number as written ("3", "2025"), or nullopt.
std::optional<std::int32_t> parse_whole(std::string_view text) {
  std::int32_t value = 0;
  const char* end = text.data() + text.size();
  const auto [ptr, ec] = std::from_chars(text.data(), end, value);
  if (text.empty() || ec != std::errc{} || ptr != end) {
    return std::nullopt;
  }
  return value;
}

// The day of a date or of a timestamp ("2026-10-27 03:12:45.12+00"): when a row
// was loaded.
std::optional<Date> parse_day(std::string_view text) {
  if (const std::optional<Date> date = parse_date(text)) {
    return date;
  }
  constexpr std::size_t kDateLength = 10;
  if (text.size() > kDateLength && (text[kDateLength] == 'T' || text[kDateLength] == ' ')) {
    return parse_date(text.substr(0, kDateLength));
  }
  return std::nullopt;
}

bool is_blank(std::string_view text) {
  std::size_t pos = 0;
  skip_json_whitespace(text, pos);
  return pos == text.size();
}

void read_delimited(ChunkSource& source_chunks, char delimiter, const std::string& source,
                    const std::string& default_id, const ReadOptions& options,
                    SeriesAssembler& out) {
  CsvParser parser(delimiter);
  bool have_header = false;
  const CsvParser::RecordFn on_record = [&](std::span<const std::string_view> fields,
                                            std::uint32_t line) {
    if (!have_header) {
      const std::vector<std::string> names(fields.begin(), fields.end());
      out.begin_source(source, default_id,
                       map_columns(names, options.columns, options.kind, source,
                                   options.items ? &*options.items : nullptr));
      have_header = true;
      return;
    }
    out.add_row(fields, line);
  };
  for (std::string_view chunk = source_chunks.next(); !chunk.empty();
       chunk = source_chunks.next()) {
    parser.feed(chunk, on_record);
  }
  parser.finish(on_record);
  if (!have_header) {
    throw InputError(source + ": no header row");
  }
}

// Maps JSON records onto columns. The first record's keys are the columns; a key
// absent from a later record reads as missing, and a key new in a later record is
// ignored.
class JsonRows {
 public:
  JsonRows(const std::string& source, const std::string& default_id, const ReadOptions& options,
           SeriesAssembler& out)
      : source_(source), default_id_(default_id), options_(options), out_(out) {}

  void add(const JsonRecord& record, std::uint32_t line) {
    if (!started_) {
      std::vector<std::string> names;
      for (const auto& member : record) {
        if (column_.emplace(member.key, names.size()).second) {
          names.push_back(member.key);
        }
      }
      out_.begin_source(source_, default_id_,
                        map_columns(names, options_.columns, options_.kind, source_,
                                    options_.items ? &*options_.items : nullptr));
      cells_.resize(names.size());
      started_ = true;
    }
    std::fill(cells_.begin(), cells_.end(), std::string_view{});
    for (const auto& member : record) {
      const auto it = column_.find(member.key);
      if (it != column_.end() && !member.is_null) {
        cells_[it->second] = member.text;
      }
    }
    out_.add_row(cells_, line);
  }

 private:
  const std::string& source_;
  const std::string& default_id_;
  const ReadOptions& options_;
  SeriesAssembler& out_;
  bool started_ = false;
  std::unordered_map<std::string, std::size_t> column_;
  std::vector<std::string_view> cells_;
};

void read_jsonl(ChunkSource& chunks, const std::string& source, const std::string& default_id,
                const ReadOptions& options, SeriesAssembler& out) {
  JsonRows rows(source, default_id, options, out);
  std::string carry;
  std::uint32_t line = 0;
  const auto handle_line = [&](std::string_view text) {
    ++line;
    if (is_blank(text)) {
      return;
    }
    const std::string where = source + " line " + std::to_string(line);
    std::size_t pos = 0;
    const JsonRecord record = parse_json_record(text, pos, where);
    skip_json_whitespace(text, pos);
    if (pos != text.size()) {
      throw InputError("invalid JSON at " + where + ": text after the record");
    }
    rows.add(record, line);
  };
  for (std::string_view chunk = chunks.next(); !chunk.empty(); chunk = chunks.next()) {
    std::size_t start = 0;
    for (std::size_t nl = chunk.find('\n'); nl != std::string_view::npos;
         nl = chunk.find('\n', start)) {
      if (carry.empty()) {
        handle_line(chunk.substr(start, nl - start));
      } else {
        carry.append(chunk.substr(start, nl - start));
        handle_line(carry);
        carry.clear();
      }
      start = nl + 1;
    }
    carry.append(chunk.substr(start));
  }
  if (!carry.empty()) {
    handle_line(carry);
  }
}

void read_json_array(ChunkSource& chunks, const std::string& source, const std::string& default_id,
                     const ReadOptions& options, SeriesAssembler& out) {
  std::string text;
  for (std::string_view chunk = chunks.next(); !chunk.empty(); chunk = chunks.next()) {
    text.append(chunk);
  }
  JsonRows rows(source, default_id, options, out);
  std::size_t pos = 0;
  skip_json_whitespace(text, pos);
  if (pos >= text.size() || text[pos] != '[') {
    throw InputError("invalid JSON in " + source + ": expected an array of records");
  }
  ++pos;
  skip_json_whitespace(text, pos);
  std::uint32_t record_number = 0;
  if (pos < text.size() && text[pos] == ']') {
    ++pos;
  } else {
    while (true) {
      ++record_number;
      const std::string where = source + " record " + std::to_string(record_number);
      rows.add(parse_json_record(text, pos, where), record_number);
      skip_json_whitespace(text, pos);
      if (pos < text.size() && text[pos] == ',') {
        ++pos;
        continue;
      }
      if (pos < text.size() && text[pos] == ']') {
        ++pos;
        break;
      }
      throw InputError("invalid JSON at " + where + ": expected ',' or ']'");
    }
  }
  skip_json_whitespace(text, pos);
  if (pos != text.size()) {
    throw InputError("invalid JSON in " + source + ": text after the array");
  }
}

}  // namespace

std::optional<InputFormat> parse_input_format(std::string_view text) noexcept {
  if (iequals(text, "auto")) {
    return InputFormat::kAuto;
  }
  if (iequals(text, "csv")) {
    return InputFormat::kCsv;
  }
  if (iequals(text, "tsv")) {
    return InputFormat::kTsv;
  }
  if (iequals(text, "jsonl") || iequals(text, "ndjson")) {
    return InputFormat::kJsonl;
  }
  if (iequals(text, "json")) {
    return InputFormat::kJson;
  }
  return std::nullopt;
}

std::string_view to_string(InputFormat format) noexcept {
  switch (format) {
    case InputFormat::kAuto:
      return "auto";
    case InputFormat::kCsv:
      return "csv";
    case InputFormat::kTsv:
      return "tsv";
    case InputFormat::kJsonl:
      return "jsonl";
    case InputFormat::kJson:
      return "json";
  }
  return "auto";
}

SeriesAssembler::SeriesAssembler(Grouping grouping, SeriesFn on_series)
    : grouping_(grouping), on_series_(std::move(on_series)) {
  mapping_.column.fill(-1);
}

void SeriesAssembler::begin_source(const std::string& source, const std::string& default_id,
                                   const ColumnMapping& mapping) {
  source_ = source;
  default_id_ = default_id;
  mapping_ = mapping;
  slots_.clear();
}

void SeriesAssembler::emit(Series& series) {
  sort_by_date(series);
  ++series_count_;
  on_series_(std::move(series));
}

Series& SeriesAssembler::series_for(std::string_view id, std::uint32_t line) {
  if (grouping_ == Grouping::kStream) {
    if (current_ && current_->id == id) {
      return *current_;
    }
    if (current_) {
      emitted_.insert(current_->id);
      slots_.erase(current_->id);
      emit(*current_);
      current_.reset();
    }
    if (emitted_.contains(std::string{id})) {
      throw InputError(source_ + " line " + std::to_string(line) + ": series \"" + std::string{id} +
                       "\" appears again after other series. Large input is checked as it is "
                       "read, which needs every row of a series together: sort by series, then "
                       "date, or pass --buffer to read everything first");
    }
    current_.emplace();
    current_->id = std::string{id};
    current_->source = source_;
    current_->kind = mapping_.kind;
    return *current_;
  }
  const auto [it, inserted] = index_.try_emplace(std::string{id}, buffered_.size());
  if (inserted) {
    Series& series = buffered_.emplace_back();
    series.id = std::string{id};
    series.source = source_;
    series.kind = mapping_.kind;
  }
  return buffered_[it->second];
}

void SeriesAssembler::add_row(std::span<const std::string_view> cells, std::uint32_t line) {
  const auto cell = [&](Field field) -> std::string_view {
    const int index = mapping_.index(field);
    if (index < 0 || static_cast<std::size_t>(index) >= cells.size()) {
      return {};
    }
    return trim(cells[static_cast<std::size_t>(index)]);
  };

  ++rows_;
  const std::string_view id =
      mapping_.has(Field::kSeries) ? cell(Field::kSeries) : std::string_view{default_id_};
  Series& series = series_for(id, line);
  if (series.kind != mapping_.kind) {
    throw InputError(source_ + " line " + std::to_string(line) + ": series \"" + series.id +
                     "\" is " + std::string{to_string(series.kind)} + " elsewhere but " +
                     std::string{to_string(mapping_.kind)} + " here");
  }
  if (series.label.empty() && mapping_.has(Field::kLabel)) {
    series.label = std::string{cell(Field::kLabel)};
  }
  if (mapping_.kind == SeriesKind::kStatements) {
    add_statement_row(series, cells, line);
    return;
  }
  series.has_volume = series.has_volume || mapping_.has(Field::kVolume);
  series.has_vwap = series.has_vwap || mapping_.has(Field::kVwap);

  const std::string_view date_text = cell(Field::kDate);
  const std::optional<Date> date = parse_date(date_text);
  if (!date) {
    FieldIssue issue;
    issue.line = line;
    issue.field = Field::kDate;
    issue.kind = date_text.empty() ? FieldIssue::Kind::kMissing : FieldIssue::Kind::kInvalid;
    issue.text = abbreviate(date_text, kIssueTextMax);
    issue.skips_row = true;
    series.issues.push_back(std::move(issue));
    ++series.skipped_rows;
    return;
  }
  series.date.push_back(*date);
  series.line.push_back(line);

  const auto read = [&](Field field, std::vector<double>& column) -> ParsedNumber {
    if (!mapping_.has(field)) {
      column.push_back(kNaN);
      return {};
    }
    const std::string_view text = cell(field);
    ParsedNumber parsed = parse_number(text);
    if (parsed.status == ParsedNumber::Status::kOk) {
      column.push_back(parsed.value);
      return parsed;
    }
    column.push_back(kNaN);
    // An empty vwap is the norm, not a defect; nothing reports it.
    if (field != Field::kVwap || parsed.status == ParsedNumber::Status::kInvalid) {
      FieldIssue issue;
      issue.line = line;
      issue.field = field;
      issue.kind = parsed.status == ParsedNumber::Status::kMissing ? FieldIssue::Kind::kMissing
                                                                   : FieldIssue::Kind::kInvalid;
      issue.text = abbreviate(text, kIssueTextMax);
      issue.has_date = true;
      issue.date = *date;
      series.issues.push_back(std::move(issue));
    }
    return parsed;
  };

  if (mapping_.kind == SeriesKind::kOhlcv) {
    read(Field::kOpen, series.open);
    read(Field::kHigh, series.high);
    read(Field::kLow, series.low);
  }
  const ParsedNumber close = read(Field::kClose, series.close);
  series.close_decimals.push_back(close.decimals);
  series.close_sig_figs.push_back(close.sig_figs);
  read(Field::kVolume, series.volume);
  read(Field::kVwap, series.vwap);
}

const std::vector<std::size_t>& SeriesAssembler::item_slots(Series& series) {
  const auto [it, inserted] = slots_.try_emplace(series.id);
  if (inserted) {
    StatementRows& rows = series.statements;
    for (const auto& item : mapping_.items) {
      const std::string& name = item.second;
      const auto found = std::find(rows.item_names.begin(), rows.item_names.end(), name);
      const auto slot = static_cast<std::size_t>(found - rows.item_names.begin());
      if (found == rows.item_names.end()) {
        rows.item_names.push_back(name);
        rows.items.emplace_back(rows.size(), kNaN);  // NULL on the rows read so far
      }
      it->second.push_back(slot);
    }
  }
  return it->second;
}

void SeriesAssembler::add_statement_row(Series& series, std::span<const std::string_view> cells,
                                        std::uint32_t line) {
  const auto cell = [&](int index) -> std::string_view {
    if (index < 0 || static_cast<std::size_t>(index) >= cells.size()) {
      return {};
    }
    return trim(cells[static_cast<std::size_t>(index)]);
  };
  const auto field_cell = [&](Field field) { return cell(mapping_.index(field)); };
  std::vector<FieldIssue> issues;
  const auto report = [&](Field field, std::string_view text, bool skips_row) {
    FieldIssue& issue = issues.emplace_back();
    issue.line = line;
    issue.field = field;
    issue.kind = text.empty() ? FieldIssue::Kind::kMissing : FieldIssue::Kind::kInvalid;
    issue.text = abbreviate(text, kIssueTextMax);
    issue.skips_row = skips_row;
    return &issue;
  };

  // The key, and the day the version became known: a row without them is skipped.
  const std::string_view date_text = field_cell(Field::kDate);
  const std::optional<Date> fiscal_date = parse_date(date_text);
  if (!fiscal_date) {
    report(Field::kDate, date_text, true);
  }
  const std::string_view statement_text = field_cell(Field::kStatement);
  const std::optional<Statement> statement = parse_statement(statement_text);
  if (!statement) {
    report(Field::kStatement, statement_text, true);
  }
  const std::string_view type_text = field_cell(Field::kPeriodType);
  const std::optional<PeriodType> period_type = parse_period_type(type_text);
  if (!period_type) {
    report(Field::kPeriodType, type_text, true);
  }
  std::int32_t version = 1;
  bool version_read = true;
  if (mapping_.has(Field::kVersion)) {
    const std::string_view text = field_cell(Field::kVersion);
    const std::optional<std::int32_t> parsed = parse_whole(text);
    if (parsed && *parsed >= 1) {
      version = *parsed;
    } else {
      version_read = false;
      report(Field::kVersion, text, true);
    }
  }
  const std::string_view from_text = field_cell(Field::kKnownFrom);
  const std::optional<Date> known_from = parse_date(from_text);
  if (!known_from) {
    report(Field::kKnownFrom, from_text, true);
  }
  std::optional<Date> known_to;
  if (const std::string_view text = field_cell(Field::kKnownTo); !text.empty()) {
    known_to = parse_date(text);
    if (!known_to) {
      report(Field::kKnownTo, text, true);
    }
  }

  // The rest: what cannot be read is reported, and the row kept without it.
  std::int32_t fiscal_year = 0;
  if (mapping_.has(Field::kFiscalYear)) {
    const std::string_view text = field_cell(Field::kFiscalYear);
    if (const auto year = parse_whole(text); year && *year >= 1800 && *year <= 2200) {
      fiscal_year = *year;
    } else {
      report(Field::kFiscalYear, text, false);
    }
  }
  FiscalPeriod fiscal_period = FiscalPeriod::kUnknown;
  if (mapping_.has(Field::kFiscalPeriod)) {
    const std::string_view text = field_cell(Field::kFiscalPeriod);
    if (const auto period = parse_fiscal_period(text)) {
      fiscal_period = *period;
    } else {
      report(Field::kFiscalPeriod, text, false);
    }
  }
  KnowledgeSource knowledge_source = KnowledgeSource::kUnknown;
  if (const std::string_view text = field_cell(Field::kKnowledgeSource); !text.empty()) {
    if (const auto source = parse_knowledge_source(text)) {
      knowledge_source = *source;
    } else {
      report(Field::kKnowledgeSource, text, false);
    }
  }
  std::optional<Date> filing_date;
  if (const std::string_view text = field_cell(Field::kFilingDate); !text.empty()) {
    filing_date = parse_date(text);
    if (!filing_date) {
      report(Field::kFilingDate, text, false);
    }
  }
  std::optional<Date> loaded_on;
  if (const std::string_view text = field_cell(Field::kLoadedOn); !text.empty()) {
    loaded_on = parse_day(text);
    if (!loaded_on) {
      report(Field::kLoadedOn, text, false);
    }
  }
  ParsedNumber close;
  if (mapping_.has(Field::kClose)) {
    const std::string_view text = field_cell(Field::kClose);
    close = parse_number(text);
    if (close.status == ParsedNumber::Status::kInvalid) {
      report(Field::kClose, text, false);
    }
  }
  std::vector<double> values(mapping_.items.size(), kNaN);
  for (std::size_t k = 0; k < mapping_.items.size(); ++k) {
    const std::string_view text = cell(static_cast<int>(mapping_.items[k].first));
    const ParsedNumber parsed = parse_number(text);
    if (parsed.status == ParsedNumber::Status::kOk) {
      values[k] = parsed.value;
    } else if (parsed.status == ParsedNumber::Status::kInvalid) {
      report(Field::kItem, text, false)->column = mapping_.items[k].second;
    }
  }

  std::optional<Date> known = known_from;
  if (known && loaded_on && *loaded_on > *known) {
    known = loaded_on;
  }
  // A report on the row names its period as far as it could be read (DR-0902).
  std::optional<StatementPeriod> period;
  if (statement && period_type) {
    period = StatementPeriod{.statement = *statement,
                             .period_type = *period_type,
                             .fiscal_year = fiscal_year,
                             .fiscal_period = fiscal_period,
                             .version = version_read ? std::optional{version} : std::nullopt};
  }
  bool skipped = false;
  for (FieldIssue& issue : issues) {
    issue.has_date = fiscal_date.has_value();
    issue.date = fiscal_date.value_or(Date{});
    issue.known = known;
    issue.period = period;
    skipped = skipped || issue.skips_row;
    series.issues.push_back(std::move(issue));
  }
  // A row that nothing skipped has every key field.
  if (skipped || !fiscal_date || !statement || !period_type || !known_from) {
    ++series.skipped_rows;
    return;
  }

  const std::vector<std::size_t>& slots = item_slots(series);
  StatementRows& rows = series.statements;
  series.date.push_back(*fiscal_date);
  series.line.push_back(line);
  series.close.push_back(close.status == ParsedNumber::Status::kOk ? close.value : kNaN);
  series.close_decimals.push_back(close.decimals);
  series.close_sig_figs.push_back(close.sig_figs);
  // As a point series has them: no volume, no vwap.
  series.volume.push_back(kNaN);
  series.vwap.push_back(kNaN);
  rows.statement.push_back(*statement);
  rows.period_type.push_back(*period_type);
  rows.fiscal_year.push_back(fiscal_year);
  rows.fiscal_period.push_back(fiscal_period);
  rows.version.push_back(version);
  rows.known_from.push_back(*known_from);
  rows.known_to.push_back(known_to);
  rows.knowledge_source.push_back(knowledge_source);
  rows.filing_date.push_back(filing_date);
  rows.loaded_on.push_back(loaded_on);
  rows.currency.emplace_back(field_cell(Field::kCurrency));
  for (std::vector<double>& column : rows.items) {
    column.push_back(kNaN);
  }
  for (std::size_t k = 0; k < slots.size(); ++k) {
    rows.items[slots[k]].back() = values[k];
  }
}

void SeriesAssembler::finish() {
  if (grouping_ == Grouping::kStream) {
    if (current_) {
      emit(*current_);
      current_.reset();
    }
    return;
  }
  for (Series& series : buffered_) {
    emit(series);
  }
  buffered_.clear();
  index_.clear();
}

void read_input(std::istream& in, const std::string& source, const std::string& default_id,
                std::string_view extension, const ReadOptions& options, SeriesAssembler& out) {
  ChunkSource chunks(in);
  if (is_blank(chunks.peek())) {
    // Whitespace in the first megabyte and nothing after it: an empty input. A
    // pipeline whose export step failed looks exactly like this, so it is an error
    // rather than a clean run over nothing.
    std::string_view next = chunks.next();
    while (!next.empty() && is_blank(next)) {
      next = chunks.next();
    }
    if (next.empty()) {
      throw InputError(source + " is empty");
    }
    throw InputError(source + ": more than 1 MiB of leading whitespace");
  }
  InputFormat format = options.format;
  if (format == InputFormat::kAuto) {
    format = format_from_extension(extension);
  }
  if (format == InputFormat::kAuto) {
    format = sniff_format(chunks.peek());
  }
  switch (format) {
    case InputFormat::kCsv:
      read_delimited(chunks, ',', source, default_id, options, out);
      break;
    case InputFormat::kTsv:
      read_delimited(chunks, '\t', source, default_id, options, out);
      break;
    case InputFormat::kJsonl:
      read_jsonl(chunks, source, default_id, options, out);
      break;
    case InputFormat::kJson:
    case InputFormat::kAuto:
      read_json_array(chunks, source, default_id, options, out);
      break;
  }
}

}  // namespace dorq
