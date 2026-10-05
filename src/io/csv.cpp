#include "io/csv.hpp"

#include <cstddef>
#include <string>
#include <string_view>

#include "io/input_error.hpp"

namespace dorq {

void CsvParser::end_field() {
  // A CR before the LF of a CRLF line end is not part of an unquoted field.
  if (!field_was_quoted_ && !buffer_.empty() && buffer_.back() == '\r' &&
      (ends_.empty() || buffer_.size() > ends_.back())) {
    buffer_.pop_back();
  }
  ends_.push_back(buffer_.size());
  field_was_quoted_ = false;
}

void CsvParser::end_record(const RecordFn& on_record) {
  end_field();
  // A blank line is one empty, unquoted field: not a record.
  const bool blank = ends_.size() == 1 && ends_.front() == 0 && !record_has_content_;
  if (!blank) {
    views_.clear();
    const std::string_view text = buffer_;
    std::size_t start = 0;
    for (const std::size_t end : ends_) {
      views_.push_back(text.substr(start, end - start));
      start = end;
    }
    on_record(views_, record_line_);
  }
  buffer_.clear();
  ends_.clear();
  record_has_content_ = false;
  state_ = State::kFieldStart;
}

void CsvParser::feed(std::string_view chunk, const RecordFn& on_record) {
  for (const char ch : chunk) {
    switch (state_) {
      case State::kFieldStart:
        if (ch == '"') {
          state_ = State::kQuoted;
          field_was_quoted_ = true;
          record_has_content_ = true;
          break;
        }
        state_ = State::kUnquoted;
        [[fallthrough]];
      case State::kUnquoted:
        if (ch == delimiter_) {
          end_field();
          record_has_content_ = true;
          state_ = State::kFieldStart;
        } else if (ch == '\n') {
          end_record(on_record);
          ++line_;
          record_line_ = line_;
        } else {
          buffer_.push_back(ch);
          if (ch != '\r') {
            record_has_content_ = true;
          }
        }
        break;
      case State::kQuoted:
        if (ch == '"') {
          state_ = State::kQuoteInQuoted;
        } else {
          buffer_.push_back(ch);
          if (ch == '\n') {
            ++line_;
          }
        }
        break;
      case State::kQuoteInQuoted:
        if (ch == '"') {  // a doubled quote is a literal quote
          buffer_.push_back('"');
          state_ = State::kQuoted;
        } else if (ch == delimiter_) {
          end_field();
          state_ = State::kFieldStart;
        } else if (ch == '\n') {
          end_record(on_record);
          ++line_;
          record_line_ = line_;
        } else if (ch != '\r') {
          buffer_.push_back(ch);  // text after a closing quote: keep it
          state_ = State::kUnquoted;
          field_was_quoted_ = false;
        }
        break;
    }
  }
}

void CsvParser::finish(const RecordFn& on_record) {
  if (state_ == State::kQuoted) {
    throw InputError("unterminated quoted field starting on line " + std::to_string(record_line_));
  }
  if (state_ != State::kFieldStart || !buffer_.empty() || !ends_.empty()) {
    end_record(on_record);
  }
}

}  // namespace dorq
