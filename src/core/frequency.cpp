#include "dorq/frequency.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "core/text.hpp"

namespace dorq {

std::optional<Frequency> parse_frequency(std::string_view text) noexcept {
  for (const Frequency f :
       {Frequency::kAuto, Frequency::kDaily, Frequency::kWeekly, Frequency::kMonthly,
        Frequency::kQuarterly, Frequency::kAnnual, Frequency::kIrregular}) {
    if (iequals(text, to_string(f))) {
      return f;
    }
  }
  return std::nullopt;
}

std::string_view to_string(Frequency frequency) noexcept {
  switch (frequency) {
    case Frequency::kAuto:
      return "auto";
    case Frequency::kDaily:
      return "daily";
    case Frequency::kWeekly:
      return "weekly";
    case Frequency::kMonthly:
      return "monthly";
    case Frequency::kQuarterly:
      return "quarterly";
    case Frequency::kAnnual:
      return "annual";
    case Frequency::kIrregular:
      return "irregular";
  }
  return "irregular";
}

Frequency infer_frequency(std::span<const Date> sorted_dates) {
  std::vector<std::int32_t> gaps;
  for (std::size_t i = 1; i < sorted_dates.size(); ++i) {
    const std::int32_t gap = sorted_dates[i].days() - sorted_dates[i - 1].days();
    if (gap > 0) {
      gaps.push_back(gap);
    }
  }
  if (gaps.size() < 2) {
    return Frequency::kIrregular;
  }
  // The lower quartile, not the median: a daily series that trades on a third of
  // its sessions has a median gap of about five days, but a quarter of its gaps are
  // a day or a weekend. A weekly series has almost none that short.
  const auto quartile = gaps.begin() + static_cast<std::ptrdiff_t>(gaps.size() / 4);
  std::nth_element(gaps.begin(), quartile, gaps.end());
  const std::int32_t gap = *quartile;
  if (gap <= 4) {
    return Frequency::kDaily;
  }
  if (gap <= 10) {
    return Frequency::kWeekly;
  }
  if (gap >= 25 && gap <= 35) {
    return Frequency::kMonthly;
  }
  if (gap >= 80 && gap <= 100) {
    return Frequency::kQuarterly;
  }
  if (gap >= 350 && gap <= 380) {
    return Frequency::kAnnual;
  }
  return Frequency::kIrregular;
}

}  // namespace dorq
