// libFuzzer target: arbitrary bytes as an actions file, a metadata file or a
// labels file (the first byte chooses). A bad file must be an InputError and
// nothing else. The actions and metadata of a good one then go through the price
// model and every check, on a fixed series; the labels' removed dates go through
// --restore. Nothing may crash or trip a sanitizer.
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

#include "calibrate/labels.hpp"
#include "checks/check.hpp"
#include "config/config.hpp"
#include "context/context.hpp"
#include "io/input_error.hpp"
#include "price/model.hpp"

namespace {

// 120 daily bars from 2020-01-02 with a 2:1 split at bar 60: series "A".
dorq::Series make_series() {
  dorq::Series s;
  s.id = "A";
  s.kind = dorq::SeriesKind::kOhlcv;
  s.has_volume = true;
  static const dorq::Calendar kCalendar;
  dorq::Date day = dorq::Date::from_ymd(2020, 1, 2);
  double price = 50.0;
  std::uint32_t state = 1;
  while (s.size() < 120) {
    if (!kCalendar.is_session(day)) {
      day = dorq::Date::from_days(day.days() + 1);
      continue;
    }
    state = state * 1664525U + 1013904223U;
    price *= std::exp((static_cast<double>(state >> 8U) / 16777216.0 - 0.5) * 0.04);
    const double close = s.size() >= 60 ? price / 2.0 : price;
    s.date.push_back(day);
    s.open.push_back(close);
    s.high.push_back(close * 1.01);
    s.low.push_back(close * 0.99);
    s.close.push_back(close);
    s.volume.push_back(s.size() >= 60 ? 2e6 : 1e6);
    s.close_decimals.push_back(2);
    s.close_sig_figs.push_back(4);
    s.line.push_back(static_cast<std::uint32_t>(s.size() + 1));
    day = dorq::Date::from_days(day.days() + 1);
  }
  return s;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  if (size == 0) {
    return 0;
  }
  static const dorq::Series kSeries = make_series();
  static const dorq::Calendar kCalendar;
  const unsigned mode = data[0] % 3U;
  const bool actions = mode == 1;
  std::istringstream in(std::string(reinterpret_cast<const char*>(data + 1), size - 1));
  if (mode == 2) {
    try {
      const std::vector<dorq::Label> labels = dorq::read_labels(in, "fuzz");
      const dorq::Restorer restorer(labels, {kSeries});
      dorq::Series series = kSeries;
      restorer.apply(series);
    } catch (const dorq::InputError&) {
    }
    return 0;
  }
  dorq::Context context;
  try {
    if (actions) {
      dorq::read_actions(in, "fuzz", context);
    } else {
      dorq::read_meta(in, "fuzz", context);
    }
  } catch (const dorq::InputError&) {
    return 0;
  }
  const dorq::SeriesActions* series_actions = context.actions_for(kSeries);
  const dorq::SeriesMeta* meta = context.meta_for(kSeries);
  const dorq::PriceSettings price;
  const dorq::IntegritySettings integrity;
  const dorq::CoverageSettings coverage;
  const dorq::SeverityThresholds thresholds;
  const dorq::PriceAnalysis analysis =
      dorq::analyze_prices(kSeries, kCalendar, price,
                           {.have_actions = context.have_actions,
                            .actions = series_actions,
                            .tick_size = meta != nullptr ? meta->tick_size : std::nullopt});
  const dorq::SeriesContext series_context{.series = kSeries,
                                           .integrity = integrity,
                                           .coverage = coverage,
                                           .calendar = kCalendar,
                                           .thresholds = thresholds,
                                           .price = price,
                                           .price_analysis = &analysis,
                                           .have_actions = context.have_actions,
                                           .actions = series_actions,
                                           .meta = meta};
  std::vector<dorq::Violation> found;
  for (const dorq::Check* check : dorq::all_checks()) {
    if (!check->info().cross_sectional && check->info().code[2] != '3') {
      check->run(series_context, found);
    }
  }
  return 0;
}
