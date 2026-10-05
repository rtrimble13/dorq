#include "dorq/date.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace dorq {
namespace {

// Howard Hinnant's civil-date algorithms (public domain), for the proleptic
// Gregorian calendar: http://howardhinnant.github.io/date_algorithms.html
constexpr std::int32_t days_from_civil(int year, int month, int day) noexcept {
  const int y = month <= 2 ? year - 1 : year;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const int yoe = y - era * 400;
  const int mp = (month + 9) % 12;
  const int doy = (153 * mp + 2) / 5 + day - 1;
  const int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}

struct Ymd {
  int year;
  int month;
  int day;
};

constexpr Ymd civil_from_days(std::int32_t days) noexcept {
  const int z = days + 719468;
  const int era = (z >= 0 ? z : z - 146096) / 146097;
  const int doe = z - era * 146097;
  const int yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const int doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const int mp = (5 * doy + 2) / 153;
  const int day = doy - (153 * mp + 2) / 5 + 1;
  const int month = mp < 10 ? mp + 3 : mp - 9;
  return {month <= 2 ? yoe + era * 400 + 1 : yoe + era * 400, month, day};
}

constexpr bool is_leap(int year) noexcept {
  return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

constexpr int days_in_month(int year, int month) noexcept {
  constexpr std::array<int, 12> kDays = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  return month == 2 && is_leap(year) ? 29 : kDays.at(static_cast<std::size_t>(month - 1));
}

bool is_digit(char ch) noexcept { return ch >= '0' && ch <= '9'; }

// Reads `count` digits at `pos`, or returns -1.
int read_digits(std::string_view text, std::size_t pos, std::size_t count) noexcept {
  if (pos + count > text.size()) {
    return -1;
  }
  int value = 0;
  for (std::size_t i = pos; i < pos + count; ++i) {
    if (!is_digit(text[i])) {
      return -1;
    }
    value = value * 10 + (text[i] - '0');
  }
  return value;
}

// Accepts the rest of a timestamp after the date only if it says midnight.
bool is_midnight_suffix(std::string_view rest) noexcept {
  if (rest.empty()) {
    return true;
  }
  if (rest.front() != 'T' && rest.front() != ' ') {
    return false;
  }
  rest.remove_prefix(1);
  // HH:MM, then optional :SS, then optional .fff
  if (rest.size() < 5 || rest.substr(0, 5) != "00:00") {
    return false;
  }
  rest.remove_prefix(5);
  if (rest.size() >= 3 && rest[0] == ':') {
    if (rest.substr(0, 3) != ":00") {
      return false;
    }
    rest.remove_prefix(3);
    if (!rest.empty() && rest[0] == '.') {
      rest.remove_prefix(1);
      while (!rest.empty() && rest[0] == '0') {
        rest.remove_prefix(1);
      }
    }
  }
  // Optional zone: Z, +HH, +HH:MM, +HHMM (and the same with '-').
  if (rest.empty() || rest == "Z") {
    return true;
  }
  if (rest[0] != '+' && rest[0] != '-') {
    return false;
  }
  rest.remove_prefix(1);
  for (const char ch : rest) {
    if (!is_digit(ch) && ch != ':') {
      return false;
    }
  }
  return rest.size() >= 2 && rest.size() <= 5;
}

}  // namespace

Date Date::from_ymd(int year, int month, int day) noexcept {
  return from_days(days_from_civil(year, month, day));
}

int Date::year() const noexcept { return civil_from_days(days_).year; }

int Date::weekday() const noexcept {
  // 1970-01-01 was a Thursday (3, counting Monday as 0).
  const int w = (days_ + 3) % 7;
  return w < 0 ? w + 7 : w;
}

std::string Date::to_string() const {
  const Ymd ymd = civil_from_days(days_);
  std::string out(10, '0');
  int year = ymd.year;
  for (int i = 3; i >= 0; --i) {
    out[static_cast<std::size_t>(i)] = static_cast<char>('0' + year % 10);
    year /= 10;
  }
  out[4] = '-';
  out[5] = static_cast<char>('0' + ymd.month / 10);
  out[6] = static_cast<char>('0' + ymd.month % 10);
  out[7] = '-';
  out[8] = static_cast<char>('0' + ymd.day / 10);
  out[9] = static_cast<char>('0' + ymd.day % 10);
  return out;
}

std::optional<Date> parse_date(std::string_view text) noexcept {
  int year = 0;
  int month = 0;
  int day = 0;
  std::string_view rest;
  if (text.size() >= 10 && (text[4] == '-' || text[4] == '/') && text[7] == text[4]) {
    year = read_digits(text, 0, 4);
    month = read_digits(text, 5, 2);
    day = read_digits(text, 8, 2);
    rest = text.substr(10);
  } else if (text.size() == 8) {
    year = read_digits(text, 0, 4);
    month = read_digits(text, 4, 2);
    day = read_digits(text, 6, 2);
  } else {
    return std::nullopt;
  }
  if (year < 1800 || year > 2200 || month < 1 || month > 12 || day < 1 ||
      day > days_in_month(year, month) || !is_midnight_suffix(rest)) {
    return std::nullopt;
  }
  return Date::from_ymd(year, month, day);
}

}  // namespace dorq
