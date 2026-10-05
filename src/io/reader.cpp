#include "io/reader.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <istream>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
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
                       map_columns(names, options.columns, options.kind, source));
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
                        map_columns(names, options_.columns, options_.kind, source_));
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
    series.issues.push_back(std::move(issue));
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
