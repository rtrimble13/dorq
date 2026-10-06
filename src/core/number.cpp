#include "dorq/number.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <system_error>

#include <fast_float/fast_float.h>

#include "core/text.hpp"

namespace dorq {
namespace {

bool is_missing_marker(std::string_view text) noexcept {
  // Every marker starts with a letter, '.' or '#', or is empty: a number seldom does.
  if (!text.empty() && ((text.front() >= '0' && text.front() <= '9') || text.front() == '-')) {
    return false;
  }
  constexpr std::array<std::string_view, 8> kMarkers = {"",     "na",   "n/a", "nan",
                                                        "null", "none", ".",   "#n/a"};
  return std::any_of(kMarkers.begin(), kMarkers.end(),
                     [text](std::string_view marker) { return iequals(text, marker); });
}

std::uint8_t clamp_u8(long value) noexcept {
  return static_cast<std::uint8_t>(std::clamp(value, 0L, 255L));
}

// Counts decimals and significant figures in an already-validated number.
void measure_precision(std::string_view text, ParsedNumber& out) noexcept {
  if (!text.empty() && (text.front() == '-' || text.front() == '+')) {
    text.remove_prefix(1);
  }
  // One pass for the decimal point and the exponent.
  std::size_t dot = std::string_view::npos;
  std::size_t e = std::string_view::npos;
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '.') {
      dot = i;
    } else if (text[i] == 'e' || text[i] == 'E') {
      e = i;
      break;
    }
  }
  long exponent = 0;
  if (e != std::string_view::npos) {
    long parsed = 0;
    std::string_view exp_text = text.substr(e + 1);
    if (!exp_text.empty() && exp_text.front() == '+') {
      exp_text.remove_prefix(1);
    }
    const auto [ptr, ec] =
        std::from_chars(exp_text.data(), exp_text.data() + exp_text.size(), parsed);
    exponent = ec == std::errc{} ? parsed : 0;
    text = text.substr(0, e);
  }
  std::string_view integer = text;
  std::string_view fraction;
  if (dot != std::string_view::npos) {
    integer = text.substr(0, dot);
    fraction = text.substr(dot + 1);
  }
  while (!fraction.empty() && fraction.back() == '0') {
    fraction.remove_suffix(1);
  }
  out.decimals = clamp_u8(static_cast<long>(fraction.size()) - exponent);

  // Significant figures: from the first non-zero digit to the last non-zero one,
  // over the integer digits followed by the fraction's.
  const auto lead = integer.find_first_not_of('0');
  std::size_t first = 0;
  if (lead != std::string_view::npos) {
    first = lead;
  } else if (const auto in_fraction = fraction.find_first_not_of('0');
             in_fraction != std::string_view::npos) {
    first = integer.size() + in_fraction;
  } else {
    out.sig_figs = 0;
    return;
  }
  // The fraction has no trailing zeros, so it ends the digits when it has any.
  const std::size_t last =
      fraction.empty() ? integer.find_last_not_of('0') : integer.size() + fraction.size() - 1;
  out.sig_figs = clamp_u8(static_cast<long>(last - first + 1));
}

}  // namespace

ParsedNumber parse_number(std::string_view text) noexcept {
  text = trim(text);
  ParsedNumber out;
  if (is_missing_marker(text)) {
    out.status = ParsedNumber::Status::kMissing;
    return out;
  }
  std::string_view body = text;
  if (!body.empty() && body.front() == '+') {
    body.remove_prefix(1);
    if (!body.empty() && (body.front() == '-' || body.front() == '+')) {
      out.status = ParsedNumber::Status::kInvalid;
      return out;
    }
  }
  double value = 0.0;
  const auto result = fast_float::from_chars(body.data(), body.data() + body.size(), value);
  if (result.ec != std::errc{} || result.ptr != body.data() + body.size() ||
      !std::isfinite(value)) {
    out.status = ParsedNumber::Status::kInvalid;
    return out;
  }
  out.status = ParsedNumber::Status::kOk;
  out.value = value;
  measure_precision(body, out);
  return out;
}

std::string format_number(double value) {
  if (std::isnan(value)) {
    return "nan";
  }
  if (std::isinf(value)) {
    return value < 0 ? "-inf" : "inf";
  }
  std::array<char, 32> buffer{};
  const auto [ptr, ec] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
  if (ec != std::errc{}) {
    return "nan";  // unreachable: 32 bytes holds any shortest double
  }
  return {buffer.data(), ptr};
}

}  // namespace dorq
