// libFuzzer target: arbitrary bytes through the readers and every check.
//
// The first byte picks the input format and the grouping; the rest is the input.
// Anything may be rejected with an InputError -- that is the contract for bad
// input -- but nothing may crash, hang, leak or trip a sanitizer.
#include <cstddef>
#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

#include "checks/check.hpp"
#include "config/config.hpp"
#include "io/input_error.hpp"
#include "io/reader.hpp"
#include "price/model.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  if (size == 0) {
    return 0;
  }
  const std::uint8_t selector = data[0];
  dorq::ReadOptions options;
  constexpr std::array<dorq::InputFormat, 5> kFormats = {
      dorq::InputFormat::kAuto, dorq::InputFormat::kCsv, dorq::InputFormat::kTsv,
      dorq::InputFormat::kJsonl, dorq::InputFormat::kJson};
  options.format = kFormats.at(selector % kFormats.size());
  const auto grouping = (selector & 0x80U) != 0 ? dorq::Grouping::kStream : dorq::Grouping::kBuffer;

  const dorq::IntegritySettings settings{.precision_min_segment = 2};
  static const dorq::Calendar kCalendar;
  const dorq::CoverageSettings coverage;
  const dorq::SeverityThresholds thresholds;
  const dorq::PriceSettings price;
  std::vector<dorq::Violation> found;
  dorq::SeriesAssembler assembler(grouping, [&](dorq::Series&& series) {
    const dorq::Frequency frequency = dorq::infer_frequency(series.date);
    const auto analysis = dorq::analyze_coverage(series, kCalendar, coverage, 0.95);
    const auto price_analysis = dorq::analyze_prices(series, kCalendar, price);
    const dorq::SeriesContext context{.series = series,
                                      .integrity = settings,
                                      .coverage = coverage,
                                      .calendar = kCalendar,
                                      .thresholds = thresholds,
                                      .price = price,
                                      .frequency = frequency,
                                      .analysis = &analysis,
                                      .price_analysis = &price_analysis};
    for (const dorq::Check* check : dorq::all_checks()) {
      if (check->info().applies == dorq::Applies::kOhlcvOnly &&
          series.kind != dorq::SeriesKind::kOhlcv) {
        continue;
      }
      check->run(context, found);
    }
  });
  std::istringstream in(std::string(reinterpret_cast<const char*>(data + 1), size - 1));
  try {
    dorq::read_input(in, "fuzz", "fuzz", "", options, assembler);
    assembler.finish();
  } catch (const dorq::InputError&) {
  }
  return 0;
}
