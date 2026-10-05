// dorq-synth: synthetic market data with labelled faults, and a scorer for dorq's
// output against the labels. See tools/synth/README.md.
//
//   dorq-synth generate [--seed N] --out DIR     writes DIR/bars.csv, DIR/labels.csv
//   dorq-synth score --labels F --results F [--gates F] [--verbose]
//
// The generator draws from its own PRNG (xoshiro256**) and its own normal and t
// samplers, so a seed gives the same data with any C++ library.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <numbers>
#include <optional>
#include <set>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "dorq/calendar.hpp"
#include "dorq/date.hpp"
#include "dorq/number.hpp"
#include "io/csv.hpp"

namespace {

using dorq::Date;

// ---------------------------------------------------------------------------
// Random numbers

class Rng {
 public:
  explicit Rng(std::uint64_t seed) {
    for (auto& word : state_) {
      seed += 0x9e3779b97f4a7c15ULL;  // splitmix64
      std::uint64_t z = seed;
      z = (z ^ (z >> 30U)) * 0xbf58476d1ce4e5b9ULL;
      z = (z ^ (z >> 27U)) * 0x94d049bb133111ebULL;
      word = z ^ (z >> 31U);
    }
  }

  std::uint64_t next() {
    const std::uint64_t result = rotl(state_[1] * 5, 7) * 9;
    const std::uint64_t t = state_[1] << 17U;
    state_[2] ^= state_[0];
    state_[3] ^= state_[1];
    state_[1] ^= state_[2];
    state_[0] ^= state_[3];
    state_[2] ^= t;
    state_[3] = rotl(state_[3], 45);
    return result;
  }

  // Uniform on (0, 1).
  double uniform() { return (static_cast<double>(next() >> 11U) + 0.5) * 0x1.0p-53; }
  double uniform(double lo, double hi) { return lo + (hi - lo) * uniform(); }
  // Log-uniform on [lo, hi].
  double log_uniform(double lo, double hi) { return std::exp(uniform(std::log(lo), std::log(hi))); }
  int integer(int lo, int hi) {  // inclusive
    return lo + static_cast<int>(uniform() * (hi - lo + 1));
  }
  bool chance(double p) { return uniform() < p; }
  double normal() {  // Box-Muller, one of the pair
    return std::sqrt(-2.0 * std::log(uniform())) * std::cos(2.0 * std::numbers::pi * uniform());
  }
  // Student t with 4 degrees of freedom, scaled to unit variance.
  double t4() {
    const double chi2 = -2.0 * std::log(uniform() * uniform());  // chi-square, 4 dof
    return normal() / std::sqrt(chi2 / 4.0) / std::numbers::sqrt2;
  }

 private:
  static std::uint64_t rotl(std::uint64_t x, int k) {
    return (x << static_cast<unsigned>(k)) | (x >> static_cast<unsigned>(64 - k));
  }
  std::array<std::uint64_t, 4> state_{};
};

// ---------------------------------------------------------------------------
// Generation

enum class Class : std::uint8_t { kLiquid, kMid, kThin, kPenny, kSixteenths };

struct Bar {
  Date date;
  double open = 0, high = 0, low = 0, close = 0, volume = 0;
  bool present = true;
};

struct Label {
  std::string series;  // "*" for every series
  Date first;
  Date last;
  std::string kind;
  std::string expect;  // the code dorq should report; empty for a hard negative
  std::string codes;   // hard negatives: the code prefix a report would be wrong under
};

struct SeriesPlan {
  std::string id;
  Class cls = Class::kLiquid;
  double price = 50.0;
  double vol = 0.02;
  double volume = 1e6;
  double beta = 1.0;
  std::vector<Bar> bars;
};

double round_to(double value, double grid) { return std::round(value / grid) * grid; }

// A price as an exporter writes it: fixed decimals for its grid.
std::string price_text(double price, Class cls) {
  int decimals = price >= 1.0 ? 2 : 4;
  if (cls == Class::kPenny) {
    decimals = 2;
  }
  std::ostringstream out;
  out.setf(std::ios::fixed);
  out.precision(decimals);
  out << price;
  return out.str();
}

// The grid a class's prices are quoted on, at `price`.
double grid_of(double price, Class cls) {
  switch (cls) {
    case Class::kPenny:
      return 0.01;
    case Class::kSixteenths:
      return 0.0625;
    default:
      return price >= 1.0 ? 0.01 : 0.0001;
  }
}

double round_price(double price, Class cls) {
  switch (cls) {
    case Class::kPenny:
      return std::max(0.01, round_to(price, 0.01));
    case Class::kSixteenths:
      return std::max(0.0625, round_to(price, 0.0625));
    default:
      return price >= 1.0 ? round_to(price, 0.01) : round_to(price, 0.0001);
  }
}

using Pool = std::vector<SeriesPlan*>;

class Generator {
 public:
  explicit Generator(std::uint64_t seed) : rng_(seed), calendar_(dorq::CalendarKind::kXnys) {}

  void run(std::ostream& bars, std::ostream& labels);

 private:
  [[nodiscard]] std::vector<Date> sessions(Date from, int count) const {
    std::vector<Date> out;
    for (Date d = from; static_cast<int>(out.size()) < count; d = Date::from_days(d.days() + 1)) {
      if (calendar_.is_session(d)) {
        out.push_back(d);
      }
    }
    return out;
  }

  void simulate(SeriesPlan& plan, const std::vector<Date>& dates, const std::vector<double>& market,
                std::size_t crash_index);
  void inject(std::vector<SeriesPlan>& universe);
  void inject_bad_prints(Pool& tradable);
  void inject_newest(Pool& tradable);
  void inject_bad_closes(Pool& tradable);
  void inject_splits(Pool& tradable);
  void inject_scale_eras(Pool& tradable);
  void inject_segments(Pool& thin_or_mid);
  void inject_outages(Pool& liquid);
  void inject_flat_bars(Pool& liquid);
  void inject_failed_load(Pool& liquid);
  SeriesPlan& any(Pool& from);
  void label(const SeriesPlan& plan, std::size_t first, std::size_t last, const char* kind,
             const char* expect);
  std::size_t pick_bar(const SeriesPlan& plan, std::size_t margin);
  bool reserve(const std::string& id, std::size_t from, std::size_t to);

  Rng rng_;
  dorq::Calendar calendar_;
  std::vector<Label> labels_;
  std::map<std::string, std::vector<std::pair<std::size_t, std::size_t>>> used_;
};

void Generator::simulate(SeriesPlan& plan, const std::vector<Date>& dates,
                         const std::vector<double>& market, std::size_t crash_index) {
  const std::size_t n = dates.size();
  const double alpha = 0.06;
  const double garch_beta = 0.92;
  const double target = plan.vol * plan.vol;
  double variance = target;
  double log_price = std::log(plan.price);
  double prev_close = plan.price;
  double volume_state = 0.0;
  double surge = 0.0;  // extra log volume, decaying after an earnings day
  const bool thin = plan.cls == Class::kThin;
  // Earnings: two to four large reactions over the history.
  std::set<std::size_t> earnings;
  if (plan.cls == Class::kLiquid || plan.cls == Class::kMid) {
    const int count = rng_.integer(2, 4);
    for (int i = 0; i < count; ++i) {
      earnings.insert(static_cast<std::size_t>(rng_.integer(40, static_cast<int>(n) - 40)));
    }
  }
  plan.bars.clear();
  for (std::size_t t = 0; t < n; ++t) {
    const double z = rng_.t4();
    double r = std::sqrt(variance) * z;
    if (plan.cls != Class::kSixteenths) {
      r += plan.beta * market[t];
    }
    double gap_share = rng_.uniform();
    double day_surge = 0.0;
    if (earnings.contains(t)) {
      used_[plan.id].emplace_back(t, t);  // faults keep clear of earnings days
      const double size = rng_.uniform(0.08, plan.cls == Class::kLiquid ? 0.25 : 0.35);
      r += (rng_.chance(0.5) ? 1.0 : -1.0) * std::log1p(size);
      day_surge = std::log(rng_.uniform(3.0, 10.0));
      surge = 0.7;
      gap_share = rng_.uniform(0.7, 1.0);
      labels_.push_back({plan.id, dates[t], dates[t], "hn_earnings", "", "DQ2"});
    }
    if (t == crash_index) {
      day_surge = std::max(day_surge, std::log(rng_.uniform(2.5, 5.0)));
      surge = std::max(surge, 0.5);
    }
    const double eps = r - (plan.cls != Class::kSixteenths ? plan.beta * market[t] : 0.0);
    variance = target * (1.0 - alpha - garch_beta) + alpha * eps * eps + garch_beta * variance;
    variance = std::min(variance, 25.0 * target);
    log_price += r;
    volume_state = 0.7 * volume_state + 0.35 * rng_.normal();
    surge *= 0.8;
    const double log_volume = std::log(plan.volume) + volume_state +
                              0.3 * std::fabs(r) / std::sqrt(target) + surge + day_surge;

    Bar bar;
    bar.date = dates[t];
    // Thin names: many sessions without a bar, and carry bars with no trade.
    if (thin && rng_.chance(0.3)) {
      bar.present = false;
      plan.bars.push_back(bar);
      continue;
    }
    const double close = round_price(std::exp(log_price), plan.cls);
    if (thin && rng_.chance(0.15)) {
      bar.open = bar.high = bar.low = bar.close = prev_close;
      bar.volume = 0.0;
      plan.bars.push_back(bar);
      continue;
    }
    const double open = round_price(
        prev_close * std::exp(gap_share * (std::log(close) - std::log(prev_close))), plan.cls);
    const double spread = 0.5 * std::sqrt(variance);
    bar.close = close;
    bar.open = open;
    bar.high =
        round_price(std::max(open, close) * std::exp(std::fabs(rng_.normal()) * spread), plan.cls);
    bar.low =
        round_price(std::min(open, close) * std::exp(-std::fabs(rng_.normal()) * spread), plan.cls);
    bar.high = std::max({bar.high, open, close});
    bar.low = std::min({bar.low, open, close});
    // A traded day's prints land on both sides of the spread: the range spans a
    // tick most days, however quiet (it matters on a coarse grid).
    const double tick = grid_of(close, plan.cls);
    if (rng_.chance(0.7)) {
      bar.high = std::max(bar.high, round_price(std::max(open, close) + tick, plan.cls));
    }
    if (rng_.chance(0.7) && std::min(open, close) > tick) {
      bar.low = std::min(bar.low, round_price(std::min(open, close) - tick, plan.cls));
    }
    bar.volume = std::round(std::exp(log_volume));
    prev_close = close;
    plan.bars.push_back(bar);
  }
}

bool Generator::reserve(const std::string& id, std::size_t from, std::size_t to) {
  auto& ranges = used_[id];
  for (const auto& [a, b] : ranges) {
    if (from <= b + 60 && a <= to + 60) {
      return false;
    }
  }
  ranges.emplace_back(from, to);
  return true;
}

std::size_t Generator::pick_bar(const SeriesPlan& plan, std::size_t margin) {
  const auto n = static_cast<int>(plan.bars.size());
  return static_cast<std::size_t>(
      rng_.integer(static_cast<int>(margin), n - 1 - static_cast<int>(margin)));
}

// A multiplier for a bad value: a decimal slip, or a plausible-looking wrong price.
double bad_factor(Rng& rng) {
  if (rng.chance(0.5)) {
    static constexpr std::array<double, 4> kSlips = {10.0, 0.1, 100.0, 0.01};
    return kSlips.at(static_cast<std::size_t>(rng.integer(0, 3)));
  }
  const double size = rng.log_uniform(1.35, 2.5);
  return rng.chance(0.5) ? size : 1.0 / size;
}

// Every price of a bar multiplied by `factor`, back on the grid.
void scale_bar(Bar& bar, double factor, Class cls) {
  bar.open = round_price(bar.open * factor, cls);
  bar.high = round_price(bar.high * factor, cls);
  bar.low = round_price(bar.low * factor, cls);
  bar.close = round_price(bar.close * factor, cls);
}

SeriesPlan& Generator::any(Pool& from) {
  return *from.at(static_cast<std::size_t>(rng_.integer(0, static_cast<int>(from.size()) - 1)));
}

void Generator::label(const SeriesPlan& plan, std::size_t first, std::size_t last, const char* kind,
                      const char* expect) {
  labels_.push_back({plan.id, plan.bars[first].date, plan.bars[last].date, kind, expect, ""});
}

// Bad prints: one to three bars off, then back.
void Generator::inject_bad_prints(Pool& tradable) {
  for (int done = 0; done < 30;) {
    SeriesPlan& plan = any(tradable);
    const std::size_t t = pick_bar(plan, 50);
    std::size_t k = 1;
    if (!rng_.chance(0.6)) {
      k = rng_.chance(0.6) ? 2 : 3;
    }
    if (!reserve(plan.id, t, t + k)) {
      continue;
    }
    const double factor = bad_factor(rng_);
    for (std::size_t i = t; i < t + k; ++i) {
      scale_bar(plan.bars[i], factor, plan.cls);
      plan.bars[i].volume = std::round(plan.bars[i].volume * rng_.uniform(0.2, 1.0));
    }
    label(plan, t, t + k - 1, "bad_print", "DQ201");
    ++done;
  }
}

// The newest bar wrong: provisional.
void Generator::inject_newest(Pool& tradable) {
  for (int done = 0; done < 4;) {
    SeriesPlan& plan = any(tradable);
    const std::size_t t = plan.bars.size() - 1;
    if (!reserve(plan.id, t, t)) {
      continue;
    }
    Bar& bar = plan.bars[t];
    bar.close = round_price(bar.close * (rng_.chance(0.5) ? 100.0 : 0.01), plan.cls);
    bar.open = bar.high = bar.low = bar.close;
    // With no later bar, a bad print and a scale change look alike.
    label(plan, t, t, "bad_print_newest", "DQ201|DQ202");
    ++done;
  }
}

// Only the close wrong.
void Generator::inject_bad_closes(Pool& tradable) {
  for (int done = 0; done < 12;) {
    SeriesPlan& plan = any(tradable);
    const std::size_t t = pick_bar(plan, 50);
    if (!reserve(plan.id, t, t)) {
      continue;
    }
    plan.bars[t].close = round_price(plan.bars[t].close * bad_factor(rng_), plan.cls);
    label(plan, t, t, "bad_close", "DQ204");
    ++done;
  }
}

// Splits the data does not record: the raw price changes level, volume inversely.
void Generator::inject_splits(Pool& tradable) {
  struct Split {
    double after = 1.0;
    double before = 1.0;
  };
  // Forward splits bring a high price down; reverse splits lift a low one.
  static constexpr std::array<Split, 6> kForward = {
      {{2, 1}, {2, 1}, {2, 1}, {3, 1}, {3, 2}, {4, 1}}};
  static constexpr std::array<Split, 3> kReverse = {{{1, 10}, {1, 5}, {1, 20}}};
  for (int done = 0; done < 16;) {
    SeriesPlan& plan = any(tradable);
    const std::size_t t = pick_bar(plan, 60);
    const double price = plan.bars[t - 1].close;
    if ((price < 25.0 && price >= 8.0) || !plan.bars[t - 1].present ||
        !reserve(plan.id, t, plan.bars.size())) {
      continue;
    }
    const Split split = price >= 25.0 ? kForward.at(static_cast<std::size_t>(rng_.integer(0, 5)))
                                      : kReverse.at(static_cast<std::size_t>(rng_.integer(0, 2)));
    const double price_factor = split.before / split.after;
    for (std::size_t i = t; i < plan.bars.size(); ++i) {
      scale_bar(plan.bars[i], price_factor, plan.cls);
      plan.bars[i].volume = std::round(plan.bars[i].volume / price_factor);
    }
    label(plan, t, t, "unreported_split", "DQ203");
    ++done;
  }
}

// Eras stored at the wrong scale; and a scale change that runs to the end.
void Generator::inject_scale_eras(Pool& tradable) {
  static constexpr std::array<double, 4> kScales = {100.0, 0.01, 1000.0, 0.001};
  for (int done = 0; done < 12;) {
    SeriesPlan& plan = any(tradable);
    const bool to_end = done % 3 == 2;
    const std::size_t t = pick_bar(plan, 60);
    const std::size_t end = to_end ? plan.bars.size() - 1
                                   : std::min(plan.bars.size() - 30,
                                              t + static_cast<std::size_t>(rng_.integer(20, 250)));
    if (!reserve(plan.id, t, end)) {
      continue;
    }
    const double factor = kScales.at(static_cast<std::size_t>(rng_.integer(0, 3)));
    for (std::size_t i = t; i <= end; ++i) {
      scale_bar(plan.bars[i], factor, plan.cls);
    }
    label(plan, t, end, to_end ? "scale_to_end" : "scale_era", "DQ202");
    ++done;
  }
}

// A new security's history continuing an old one after a long gap.
void Generator::inject_segments(Pool& thin_or_mid) {
  static constexpr std::array<double, 3> kListing = {10.0, 20.0, 25.0};
  for (int done = 0; done < 6;) {
    SeriesPlan& plan = any(thin_or_mid);
    const std::size_t t = pick_bar(plan, 400);
    const auto gap = static_cast<std::size_t>(rng_.integer(100, 300));
    if (!reserve(plan.id, t, plan.bars.size())) {
      continue;
    }
    const double start = kListing.at(static_cast<std::size_t>(rng_.integer(0, 2)));
    std::size_t first = t + gap;  // the new history's first traded bar
    while (first + 1 < plan.bars.size() &&
           (!plan.bars[first].present || plan.bars[first].volume == 0.0)) {
      ++first;
    }
    for (std::size_t i = t; i < first; ++i) {
      plan.bars[i].present = false;
    }
    const double scale = start / plan.bars[first].close;
    for (std::size_t i = first; i < plan.bars.size(); ++i) {
      Bar& bar = plan.bars[i];
      if (!bar.present) {
        continue;
      }
      scale_bar(bar, scale, plan.cls);
      bar.volume = std::round(bar.volume * rng_.uniform(5.0, 50.0));
    }
    Bar& listing = plan.bars[first];
    listing.close = start;
    listing.open = start;
    listing.high = std::max(listing.high, start);
    listing.low = std::min(listing.low, start);
    label(plan, first, first, "history_segment", "DQ205");
    // The gap itself is a long run with no bar: DQ301 is right to report it.
    label(plan, t, first - 1, "history_gap", "DQ301");
    ++done;
  }
}

// Feed outages on liquid names.
void Generator::inject_outages(Pool& liquid) {
  for (int done = 0; done < 8;) {
    SeriesPlan& plan = any(liquid);
    const std::size_t t = pick_bar(plan, 60);
    const auto k = static_cast<std::size_t>(rng_.integer(1, 8));
    if (!reserve(plan.id, t, t + k)) {
      continue;
    }
    for (std::size_t i = t; i < t + k; ++i) {
      plan.bars[i].present = false;
    }
    label(plan, t, t + k - 1, "outage", "DQ301");
    ++done;
  }
}

// The range lost: the close copied into open, high and low on a busy day.
void Generator::inject_flat_bars(Pool& liquid) {
  for (int done = 0; done < 8;) {
    SeriesPlan& plan = any(liquid);
    const std::size_t t = pick_bar(plan, 60);
    if (!plan.bars[t].present || !reserve(plan.id, t, t)) {
      continue;
    }
    std::vector<double> volumes;
    for (const Bar& b : plan.bars) {
      if (b.present && b.volume > 0.0) {
        volumes.push_back(b.volume);
      }
    }
    const auto mid = volumes.begin() + static_cast<std::ptrdiff_t>(volumes.size() / 2);
    std::nth_element(volumes.begin(), mid, volumes.end());
    Bar& bar = plan.bars[t];
    bar.open = bar.high = bar.low = bar.close;
    bar.volume = std::round(*mid * rng_.uniform(1.2, 3.0));  // a busy day
    label(plan, t, t, "flat_bar", "DQ107");
    ++done;
  }
}

// One failed load: a date missing from most liquid names.
void Generator::inject_failed_load(Pool& liquid) {
  const std::size_t load = pick_bar(*liquid.front(), 100);
  for (SeriesPlan* plan : liquid) {
    if (rng_.chance(0.7)) {
      plan->bars[load].present = false;
    }
  }
  const Date date = liquid.front()->bars[load].date;
  labels_.push_back({"*", date, date, "failed_load", "DQ303", ""});
}

void Generator::inject(std::vector<SeriesPlan>& universe) {
  Pool liquid;
  Pool tradable;  // liquid and mid
  Pool thin_or_mid;
  for (SeriesPlan& plan : universe) {
    if (plan.cls == Class::kLiquid) {
      liquid.push_back(&plan);
    }
    if (plan.cls == Class::kLiquid || plan.cls == Class::kMid) {
      tradable.push_back(&plan);
    }
    if (plan.cls == Class::kThin || plan.cls == Class::kMid) {
      thin_or_mid.push_back(&plan);
    }
  }
  inject_bad_prints(tradable);
  inject_newest(tradable);
  inject_bad_closes(tradable);
  inject_splits(tradable);
  inject_scale_eras(tradable);
  inject_segments(thin_or_mid);
  inject_outages(liquid);
  inject_failed_load(liquid);
  inject_flat_bars(liquid);
}

void Generator::run(std::ostream& bars, std::ostream& labels) {
  const std::vector<Date> dates = sessions(Date::from_ymd(2015, 1, 2), 2000);
  const std::vector<Date> old_dates = sessions(Date::from_ymd(1998, 1, 2), 750);
  const std::size_t n = dates.size();
  // The market: a modest index, with one crash day and its aftermath.
  std::vector<double> market(n);
  double variance = 0.01 * 0.01;
  const auto crash = static_cast<std::size_t>(rng_.integer(300, static_cast<int>(n) - 300));
  for (std::size_t t = 0; t < n; ++t) {
    market[t] = std::sqrt(variance) * rng_.t4();
    if (t == crash) {
      market[t] = std::log(rng_.uniform(0.85, 0.9));
    }
    variance = 0.01 * 0.01 * 0.04 + 0.08 * market[t] * market[t] + 0.88 * variance;
  }
  labels_.push_back({"*", dates[crash], dates[crash], "hn_crash", "", "DQ2"});

  std::vector<SeriesPlan> universe;
  const auto add = [&](const char* prefix, int count, Class cls) {
    for (int i = 0; i < count; ++i) {
      SeriesPlan plan;
      std::ostringstream id;
      id << prefix << (i < 9 ? "0" : "") << (i + 1);
      plan.id = id.str();
      plan.cls = cls;
      switch (cls) {
        case Class::kLiquid:
          plan.price = rng_.log_uniform(20, 200);
          plan.vol = rng_.uniform(0.012, 0.025);
          plan.volume = rng_.log_uniform(5e5, 5e6);
          plan.beta = rng_.uniform(0.6, 1.3);
          break;
        case Class::kMid:
          plan.price = rng_.log_uniform(5, 40);
          plan.vol = rng_.uniform(0.025, 0.04);
          plan.volume = rng_.log_uniform(3e4, 3e5);
          plan.beta = rng_.uniform(0.6, 1.4);
          break;
        case Class::kThin:
          plan.price = rng_.log_uniform(1, 10);
          plan.vol = rng_.uniform(0.04, 0.06);
          plan.volume = rng_.log_uniform(300, 3000);
          plan.beta = rng_.uniform(0.3, 1.0);
          break;
        case Class::kPenny:
          plan.price = rng_.uniform(0.03, 0.09);
          plan.vol = rng_.uniform(0.03, 0.06);
          plan.volume = rng_.log_uniform(1e5, 1e6);
          plan.beta = 0.5;
          break;
        case Class::kSixteenths:
          plan.price = rng_.uniform(0.15, 0.5);
          plan.vol = rng_.uniform(0.03, 0.05);
          plan.volume = rng_.log_uniform(1e4, 1e5);
          plan.beta = 0.0;
          break;
      }
      universe.push_back(std::move(plan));
    }
  };
  add("LIQ", 60, Class::kLiquid);
  add("MID", 30, Class::kMid);
  add("THN", 20, Class::kThin);
  add("PNY", 10, Class::kPenny);
  add("SXT", 5, Class::kSixteenths);
  for (SeriesPlan& plan : universe) {
    if (plan.cls == Class::kSixteenths) {
      // Histories of different lengths: five that stopped together would be a
      // failed load (DQ303).
      const std::vector<Date> own(
          old_dates.begin(),
          old_dates.begin() + rng_.integer(600, static_cast<int>(old_dates.size())));
      simulate(plan, own, std::vector<double>(own.size(), 0.0), own.size());
      labels_.push_back({plan.id, own.front(), own.back(), "hn_tick", "", "DQ2"});
      // It stops long before the rest: stale as of the universe's last date.
      labels_.push_back({plan.id, own.back(), dates.back(), "series_ended", "DQ304", ""});
    } else {
      simulate(plan, dates, market, crash);
    }
    if (plan.cls == Class::kPenny) {
      labels_.push_back({plan.id, dates.front(), dates.back(), "hn_tick", "", "DQ2"});
    }
    if (plan.cls == Class::kThin) {
      labels_.push_back({plan.id, dates.front(), dates.back(), "hn_thin", "", "DQ20"});
    }
  }
  inject(universe);

  bars << "series,date,open,high,low,close,volume\n";
  for (const SeriesPlan& plan : universe) {
    for (const Bar& bar : plan.bars) {
      if (!bar.present) {
        continue;
      }
      bars << plan.id << ',' << bar.date.to_string() << ',' << price_text(bar.open, plan.cls) << ','
           << price_text(bar.high, plan.cls) << ',' << price_text(bar.low, plan.cls) << ','
           << price_text(bar.close, plan.cls) << ',' << dorq::format_number(bar.volume) << '\n';
    }
  }
  labels << "series,first,last,kind,expect,codes\n";
  for (const Label& l : labels_) {
    labels << l.series << ',' << l.first.to_string() << ',' << l.last.to_string() << ',' << l.kind
           << ',' << l.expect << ',' << l.codes << '\n';
  }
}

// ---------------------------------------------------------------------------
// Scoring

struct Result {
  std::string series;
  Date first;
  Date last;
  std::string code;
  std::string severity;
  std::string message;
};

std::vector<std::vector<std::string>> read_csv(const std::string& path) {
  const std::ifstream in(path, std::ios::binary);
  if (!in) {
    throw std::runtime_error(path + ": cannot open the file");
  }
  std::ostringstream text;
  text << in.rdbuf();
  std::vector<std::vector<std::string>> rows;
  dorq::CsvParser parser(',');
  const auto on_record = [&rows](std::span<const std::string_view> fields, std::uint32_t) {
    rows.emplace_back(fields.begin(), fields.end());
  };
  parser.feed(text.str(), on_record);
  parser.finish(on_record);
  return rows;
}

std::size_t column(const std::vector<std::string>& header, std::string_view name) {
  const auto it = std::find(header.begin(), header.end(), name);
  if (it == header.end()) {
    throw std::runtime_error("no column \"" + std::string{name} + "\"");
  }
  return static_cast<std::size_t>(it - header.begin());
}

Date date_of(const std::string& text) {
  const auto date = dorq::parse_date(text);
  if (!date) {
    throw std::runtime_error("bad date \"" + text + "\"");
  }
  return *date;
}

bool overlaps(const Result& r, const Label& l, int slack_days) {
  if (l.series != "*" && l.series != r.series) {
    return false;
  }
  if (l.series == "*" && !l.expect.empty() && !r.series.empty()) {
    return false;  // a cross-sectional label wants the cross-sectional row
  }
  return r.first.days() <= l.last.days() + slack_days &&
         l.first.days() <= r.last.days() + slack_days;
}

// A label's expected code, or one of several ("DQ201|DQ202").
bool expects(const Label& l, const std::string& code) {
  std::string_view rest = l.expect;
  while (!rest.empty()) {
    const auto bar = rest.find('|');
    if (rest.substr(0, bar) == code) {
      return true;
    }
    rest = bar == std::string_view::npos ? std::string_view{} : rest.substr(bar + 1);
  }
  return false;
}

struct Gate {
  std::string code;
  std::string metric;
  double bound = 0.0;
};

std::vector<Gate> read_gates(const std::string& path) {
  std::vector<Gate> gates;
  std::ifstream in(path);
  if (!in) {
    throw std::runtime_error(path + ": cannot open the file");
  }
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line.front() == '#') {
      continue;
    }
    std::istringstream words(line);
    Gate gate;
    words >> gate.code >> gate.metric >> gate.bound;
    gates.push_back(gate);
  }
  return gates;
}

std::vector<Label> read_labels(const std::string& path) {
  std::vector<Label> labels;
  const auto rows = read_csv(path);
  const auto& h = rows.at(0);
  for (std::size_t i = 1; i < rows.size(); ++i) {
    const auto& row = rows[i];
    labels.push_back({row.at(column(h, "series")), date_of(row.at(column(h, "first"))),
                      date_of(row.at(column(h, "last"))), row.at(column(h, "kind")),
                      row.at(column(h, "expect")), row.at(column(h, "codes"))});
  }
  return labels;
}

// dorq's --format csv output, at warn and above: precision and recall are
// measured there.
std::vector<Result> read_results(const std::string& path) {
  std::vector<Result> results;
  const auto rows = read_csv(path);
  if (rows.empty()) {
    return results;
  }
  const auto& h = rows.at(0);
  for (std::size_t i = 1; i < rows.size(); ++i) {
    const auto& row = rows[i];
    Result r;
    r.severity = row.at(column(h, "severity"));
    if (r.severity == "info") {
      continue;
    }
    r.series = row.at(column(h, "series"));
    r.first = date_of(row.at(column(h, "date")));
    const std::string& end = row.at(column(h, "end_date"));
    r.last = end.empty() ? r.first : date_of(end);
    r.code = row.at(column(h, "code"));
    r.message = row.at(column(h, "message"));
    results.push_back(std::move(r));
  }
  return results;
}

double ratio(int a, int b) { return b == 0 ? 1.0 : static_cast<double>(a) / b; }

std::string fixed3(double x) {
  std::ostringstream out;
  out.setf(std::ios::fixed);
  out.precision(3);
  out << x;
  return out.str();
}

constexpr int kSlack = 3;  // calendar days of slack in matching a date

// Results against labels.
class Tally {
 public:
  struct Counts {
    int reported = 0;
    int correct = 0;
    int other_fault = 0;
    int false_positive = 0;
    int labels = 0;
    int found = 0;
  };

  Tally(const std::vector<Label>& labels, const std::vector<Gate>& gates, bool verbose)
      : labels_(labels), gates_(gates), verbose_(verbose) {}

  void add(const Result& r) {
    Counts& c = by_code_[r.code];
    ++c.reported;
    bool correct = false;
    bool other = false;
    for (const Label& l : labels_) {
      if (!l.expect.empty() && overlaps(r, l, kSlack)) {
        (expects(l, r.code) ? correct : other) = true;
      }
    }
    if (correct) {
      ++c.correct;
    } else if (other) {
      ++c.other_fault;
    } else {
      ++c.false_positive;
      if (!hard_negative(r) && narrate(r.code)) {
        std::cout << "false positive: " << r.series << " " << r.first.to_string() << " " << r.code
                  << " " << r.message << "\n";
      }
    }
  }

  void recall(const std::vector<Result>& results) {
    for (const Label& l : labels_) {
      if (l.expect.empty()) {
        continue;
      }
      const std::string code = l.expect.substr(0, l.expect.find('|'));
      Counts& c = by_code_[code];
      ++c.labels;
      const bool found = std::any_of(results.begin(), results.end(), [&](const Result& r) {
        return expects(l, r.code) && overlaps(r, l, kSlack);
      });
      c.found += found ? 1 : 0;
      if (!found && narrate(code)) {
        std::cout << "missed (" << l.kind << "): " << l.series << " " << l.first.to_string() << ".."
                  << l.last.to_string() << " " << l.expect << "\n";
      }
    }
  }

  void print() {
    std::cout << "At warn and above. precision: the right code for an injected fault; fault\n"
                 "precision: any injected fault; recall: faults reported with the right code.\n\n"
                 "code    reported  right  other fault  false  precision  fault prec.  faults  "
                 "found  recall\n";
    for (const auto& [code, c] : by_code_) {
      std::string line = code;
      const auto cell = [&line](const std::string& text, std::size_t width) {
        line += std::string(width > text.size() ? width - text.size() : 1, ' ');
        line += text;
      };
      cell(std::to_string(c.reported), 11);
      cell(std::to_string(c.correct), 7);
      cell(std::to_string(c.other_fault), 13);
      cell(std::to_string(c.false_positive), 7);
      cell(fixed3(ratio(c.correct, c.reported)), 11);
      cell(fixed3(ratio(c.correct + c.other_fault, c.reported)), 13);
      cell(std::to_string(c.labels), 8);
      cell(std::to_string(c.found), 7);
      cell(fixed3(ratio(c.found, c.labels)), 8);
      std::cout << line << "\n";
    }
    std::cout << "\nhard negatives reported at warn or above:";
    std::set<std::string> kinds;
    for (const Label& l : labels_) {
      if (l.expect.empty()) {
        kinds.insert(l.kind);
      }
    }
    for (const std::string& kind : kinds) {
      std::cout << " " << kind << " " << hard_hits_[kind];
    }
    std::cout << "\n";
  }

  // The number of gates that fail, or -1 for a gate that names no metric dorq-synth knows.
  int failures() {
    int failed = 0;
    for (const Gate& gate : gates_) {
      const std::optional<double> value = metric(gate);
      if (!value) {
        std::cerr << "unknown metric \"" << gate.metric << "\" in the gates\n";
        return -1;
      }
      const bool ok = gate.metric == "max" ? *value <= gate.bound : *value >= gate.bound;
      if (!ok) {
        std::cout << "GATE FAILED: " << gate.code << " " << gate.metric << " "
                  << dorq::format_number(gate.bound) << " (got " << fixed3(*value) << ")\n";
        ++failed;
      }
    }
    return failed;
  }

 private:
  // With gates, the narration covers the codes they judge.
  [[nodiscard]] bool narrate(const std::string& code) const {
    return verbose_ &&
           (gates_.empty() || std::any_of(gates_.begin(), gates_.end(),
                                          [&](const Gate& g) { return g.code == code; }));
  }

  // Counts a false positive that overlaps a hard negative; true when one does.
  bool hard_negative(const Result& r) {
    bool hard = false;
    for (const Label& l : labels_) {
      if (!l.expect.empty() || !overlaps(r, l, 0) || !r.code.starts_with(l.codes)) {
        continue;
      }
      hard_hits_[l.kind] += hard ? 0 : 1;
      hard = true;
      if (verbose_) {
        std::cout << "hard negative (" << l.kind << "): " << r.series << " " << r.first.to_string()
                  << " " << r.code << " " << r.message << "\n";
      }
    }
    return hard;
  }

  std::optional<double> metric(const Gate& gate) {
    if (gate.metric == "max") {  // a hard-negative kind
      return hard_hits_[gate.code];
    }
    const Counts& c = by_code_[gate.code];
    if (gate.metric == "precision") {
      return ratio(c.correct, c.reported);
    }
    if (gate.metric == "fault_precision") {
      return ratio(c.correct + c.other_fault, c.reported);
    }
    if (gate.metric == "recall") {
      return ratio(c.found, c.labels);
    }
    return std::nullopt;
  }

  const std::vector<Label>& labels_;
  const std::vector<Gate>& gates_;
  bool verbose_ = false;
  std::map<std::string, Counts> by_code_;
  std::map<std::string, int> hard_hits_;  // by hard-negative kind
};

int score(const std::string& labels_path, const std::string& results_path,
          const std::string& gates_path, bool verbose) {
  const std::vector<Gate> gates = gates_path.empty() ? std::vector<Gate>{} : read_gates(gates_path);
  const std::vector<Label> labels = read_labels(labels_path);
  const std::vector<Result> results = read_results(results_path);
  Tally tally(labels, gates, verbose);
  for (const Result& r : results) {
    tally.add(r);
  }
  tally.recall(results);
  tally.print();
  const int failed = tally.failures();
  if (failed < 0) {
    return 2;
  }
  if (!gates.empty() && failed == 0) {
    std::cout << "all gates passed\n";
  }
  return failed == 0 ? 0 : 1;
}

int usage() {
  std::cerr << "usage: dorq-synth generate [--seed N] --out DIR\n"
               "       dorq-synth score --labels F --results F [--gates F] [--verbose]\n";
  return 2;
}

}  // namespace

int main(int argc, char** argv) {
  const std::span<char*> argument_span(argv, static_cast<std::size_t>(argc));
  const std::vector<std::string> args(argument_span.begin(), argument_span.end());
  if (args.size() < 2) {
    return usage();
  }
  const auto value = [&args](std::string_view flag) -> std::optional<std::string> {
    for (std::size_t i = 2; i + 1 < args.size(); ++i) {
      if (args[i] == flag) {
        return args[i + 1];
      }
    }
    return std::nullopt;
  };
  try {
    if (args[1] == "generate") {
      const auto out = value("--out");
      if (!out) {
        return usage();
      }
      const std::uint64_t seed = std::stoull(value("--seed").value_or("1"));
      std::filesystem::create_directories(*out);
      std::ofstream bars(std::filesystem::path(*out) / "bars.csv", std::ios::binary);
      std::ofstream labels(std::filesystem::path(*out) / "labels.csv", std::ios::binary);
      Generator(seed).run(bars, labels);
      return 0;
    }
    if (args[1] == "score") {
      const auto labels = value("--labels");
      const auto results = value("--results");
      if (!labels || !results) {
        return usage();
      }
      const bool verbose = std::find(args.begin(), args.end(), "--verbose") != args.end();
      return score(*labels, *results, value("--gates").value_or(""), verbose);
    }
  } catch (const std::exception& error) {
    std::cerr << "dorq-synth: " << error.what() << "\n";
    return 2;
  }
  return usage();
}
