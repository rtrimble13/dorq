// libFuzzer target: arbitrary bytes through the readers and every check.
//
// The first byte picks the input format, the grouping and the settings; the rest
// is the input.
// Anything may be rejected with an InputError -- that is the contract for bad
// input -- but nothing may crash, hang, leak or trip a sanitizer.
#include <cstddef>
#include <cstdint>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "checks/check.hpp"
#include "config/config.hpp"
#include "io/input_error.hpp"
#include "io/reader.hpp"
#include "price/model.hpp"
#include "statements/views.hpp"

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

  // Bit 0x40 runs the rates settings: bounds (DQ108) and the difference scale.
  const bool rates = (selector & 0x40U) != 0;
  dorq::IntegritySettings settings{.precision_min_segment = 2};
  dorq::PriceSettings price;
  if (rates) {
    settings.bounds = dorq::Bounds{-5.0, 25.0};
    price.transform = dorq::Transform::kDiff;
  }
  static const dorq::Calendar kCalendar;
  const dorq::CoverageSettings coverage;
  const dorq::SeverityThresholds thresholds;
  std::vector<dorq::Violation> found;
  dorq::SeriesAssembler assembler(grouping, [&](dorq::Series&& series) {
    // As the engine does: the bars' models for bars and point series, the views
    // for statements, and each check on the kinds it applies to.
    const bool statements = series.kind == dorq::SeriesKind::kStatements;
    const dorq::Frequency frequency =
        statements ? dorq::Frequency::kDaily : dorq::infer_frequency(series.date);
    std::optional<dorq::CoverageAnalysis> analysis;
    std::optional<dorq::PriceAnalysis> price_analysis;
    std::optional<dorq::StatementViews> views;
    if (statements) {
      views = dorq::build_statement_views(series, std::nullopt);
    } else {
      analysis = dorq::analyze_coverage(series, kCalendar, coverage, 0.95);
      price_analysis = dorq::analyze_prices(series, kCalendar, price, {.bounds = settings.bounds});
    }
    const dorq::SeriesContext context{.series = series,
                                      .integrity = settings,
                                      .coverage = coverage,
                                      .calendar = kCalendar,
                                      .thresholds = thresholds,
                                      .price = price,
                                      .frequency = frequency,
                                      .analysis = analysis ? &*analysis : nullptr,
                                      .price_analysis = price_analysis ? &*price_analysis : nullptr,
                                      .statements = views ? &*views : nullptr};
    for (const dorq::Check* check : dorq::all_checks()) {
      if (!dorq::applies_to(check->info().applies, series.kind)) {
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
