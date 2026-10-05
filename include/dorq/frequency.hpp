#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

#include "dorq/date.hpp"

namespace dorq {

// How often a series is observed.
enum class Frequency : std::uint8_t {
  kAuto,  // a setting only: infer it
  kDaily,
  kWeekly,
  kMonthly,
  kQuarterly,
  kAnnual,
  kIrregular,
};

[[nodiscard]] std::optional<Frequency> parse_frequency(std::string_view text) noexcept;
[[nodiscard]] std::string_view to_string(Frequency frequency) noexcept;

// Infers the frequency from sorted dates by the lower-quartile gap between
// distinct dates: up to 4 days is daily (weekends and holidays included, and
// thinly traded series too), 5-10 weekly, 25-35 monthly, 80-100 quarterly, 350-380
// annual; anything else, or fewer than 3 distinct dates, is irregular.
[[nodiscard]] Frequency infer_frequency(std::span<const Date> sorted_dates);

}  // namespace dorq
