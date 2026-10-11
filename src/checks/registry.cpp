#include <algorithm>
#include <array>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "checks/actions.hpp"
#include "checks/check.hpp"
#include "checks/coverage.hpp"
#include "checks/cross.hpp"
#include "checks/integrity.hpp"
#include "checks/price.hpp"
#include "checks/stale.hpp"
#include "checks/volume.hpp"
#include "core/text.hpp"

namespace dorq {
namespace {

constexpr std::size_t kNameMatch = 1000;  // longer than any code

bool is_code_prefix(std::string_view entry) {
  if (entry.size() < 2 || entry.size() > 5 || !iequals(entry.substr(0, 2), "DQ")) {
    return false;
  }
  return std::all_of(entry.begin() + 2, entry.end(),
                     [](char ch) { return ch >= '0' && ch <= '9'; });
}

// How specifically `entry` matches `info`: 0 for no match.
std::size_t match_length(std::string_view entry, const CheckInfo& info) {
  if (entry == info.name) {
    return kNameMatch;
  }
  if (is_code_prefix(entry) && entry.size() <= info.code.size() &&
      iequals(info.code.substr(0, entry.size()), entry)) {
    return entry.size();
  }
  return 0;
}

std::size_t longest_match(const std::vector<std::string>& entries, const CheckInfo& info) {
  std::size_t best = 0;
  for (const auto& entry : entries) {
    best = std::max(best, match_length(entry, info));
  }
  return best;
}

}  // namespace

std::span<const Check* const> all_checks() {
  static const OhlcBounds kOhlcBounds;
  static const NonPositive kNonPositive;
  static const DuplicateDate kDuplicateDate;
  static const MissingField kMissingField;
  static const PrecisionShift kPrecisionShift;
  static const NonSessionBar kNonSession;
  static const ZeroRangeWithVolume kZeroRange;
  static const OutOfBounds kOutOfBounds;
  static const BadContextRow kBadContextRow;
  static const PriceCheck kBadPrint(bad_print_info());
  static const PriceCheck kScaleShift(scale_shift_info());
  static const PriceCheck kUnreportedSplit(unreported_split_info());
  static const PriceCheck kCloseMismatch(ohlc_close_mismatch_info());
  static const PriceCheck kHistorySegment(history_segment_info());
  static const DateShift kDateShift;
  static const PriceCheck kLargeMove(large_move_info());
  static const MissingRunCheck kMissingRun;
  static const SparseSeries kSparse;
  static const CohortGap kCohort;
  static const StaleFeed kStale;
  static const FrequencyGap kFrequencyGap;
  static const VolumeScaleShift kVolumeShift;
  static const VolumeSpikeNoMove kVolumeSpike;
  static const MoveOnZeroVolume kZeroVolumeMove;
  static const RepeatedPrice kRepeatedPrice;
  static const CarryBar kCarryBar;
  static const SplitOnFileCheck kSplitMisdated(split_misdated_info(), SplitVerdict::kMisdated);
  static const SplitOnFileCheck kSplitNoJump(split_without_jump_info(), SplitVerdict::kNoJump);
  static const SplitOnFileCheck kSplitRatio(split_ratio_mismatch_info(),
                                            SplitVerdict::kRatioMismatch);
  static const SplitOnFileCheck kSplitDouble(split_double_applied_info(),
                                             SplitVerdict::kDoubleApplied);
  static const DividendImplausible kDividend;
  static const CohortMove kCohortMove;
  static const MarketDay kMarketDay;
  static const std::array<const Check*, 33> kChecks = {
      &kOhlcBounds,     &kNonPositive,     &kDuplicateDate, &kMissingField,   &kNonSession,
      &kPrecisionShift, &kZeroRange,       &kOutOfBounds,   &kBadContextRow,  &kBadPrint,
      &kScaleShift,     &kUnreportedSplit, &kCloseMismatch, &kHistorySegment, &kDateShift,
      &kLargeMove,      &kMissingRun,      &kSparse,        &kCohort,         &kStale,
      &kFrequencyGap,   &kVolumeShift,     &kVolumeSpike,   &kZeroVolumeMove, &kRepeatedPrice,
      &kCarryBar,       &kCohortMove,      &kMarketDay,     &kSplitMisdated,  &kSplitNoJump,
      &kSplitRatio,     &kSplitDouble,     &kDividend};
  return kChecks;
}

const Check* find_check(std::string_view code_or_name) {
  for (const Check* check : all_checks()) {
    if (iequals(check->info().code, code_or_name) || check->info().name == code_or_name) {
      return check;
    }
  }
  return nullptr;
}

std::string Selection::validate(std::span<const std::string> entries) {
  for (const auto& entry : entries) {
    if (is_code_prefix(entry) || find_check(entry) != nullptr) {
      continue;
    }
    return "unknown check \"" + entry +
           "\": expected a code (DQ101), a code prefix (DQ1) or a check name (see dorq "
           "list-checks)";
  }
  return {};
}

Selection::Selection(std::span<const std::string> select, std::span<const std::string> ignore)
    : select_(select.begin(), select.end()), ignore_(ignore.begin(), ignore.end()) {}

bool Selection::enabled(const CheckInfo& info) const {
  const std::size_t selected = longest_match(select_, info);
  return selected > 0 && selected > longest_match(ignore_, info);
}

}  // namespace dorq
