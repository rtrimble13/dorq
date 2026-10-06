#include "calibrate/calibrate.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "dorq/number.hpp"
#include "dorq/version.hpp"
#include "stats/special.hpp"

namespace dorq {
namespace {

using H = PriceHypothesis;
constexpr std::size_t kCount = kPriceHypotheses;
using Weights = std::array<double, kCount>;
constexpr double kNegInf = -std::numeric_limits<double>::infinity();

constexpr std::size_t idx(H h) noexcept { return static_cast<std::size_t>(h); }

// The fitted log prior weights stay within about this many e-folds of the
// configured ones: a Gaussian penalty, so a few labels cannot drive a prior to
// zero.
constexpr double kPriorSpread = 1.5;
// A label covers scored moves within this many calendar days of its span.
constexpr int kSlackDays = 3;

Weights weights_of(const PricePriors& p) {
  return {p.market_move,      p.tick_move,   p.bad_print,       p.bad_close,
          p.unreported_split, p.scale_error, p.history_segment, p.explained_split};
}

PricePriors priors_of(const Weights& w, const PricePriors& base) {
  PricePriors p = base;
  p.market_move = w.at(idx(H::kMarketMove));
  p.tick_move = w.at(idx(H::kTickMove));
  p.bad_print = w.at(idx(H::kBadPrint));
  p.bad_close = w.at(idx(H::kBadClose));
  p.unreported_split = w.at(idx(H::kUnreportedSplit));
  p.scale_error = w.at(idx(H::kScaleError));
  p.history_segment = w.at(idx(H::kHistorySegment));
  p.explained_split = w.at(idx(H::kExplainedSplit));
  return p;
}

// The hypotheses a label allows, or nullopt for a label about something the
// price model does not judge (a gap, a volume step, a split on file).
std::optional<std::array<bool, kCount>> allowed_for(const Label& label) {
  std::array<bool, kCount> a{};
  const auto set = [&a](H h) { a.at(idx(h)) = true; };
  if (label.label_class == LabelClass::kMarketFact) {
    if (label.codes != "DQ" && !label.codes.starts_with("DQ2")) {
      return std::nullopt;
    }
    set(H::kMarketMove);
    set(H::kTickMove);
    set(H::kExplainedSplit);
    return a;
  }
  const std::string& code = label.expect;
  if (code.empty()) {
    if (label.label_class == LabelClass::kContextGap) {
      set(H::kUnreportedSplit);
    } else {
      for (std::size_t h = 0; h < kCount; ++h) {
        a.at(h) = is_error(static_cast<H>(h));
      }
    }
  } else if (code == "DQ201") {
    set(H::kBadPrint);
  } else if (code == "DQ202") {
    set(H::kScaleError);
  } else if (code == "DQ203" || code == "DQ601") {
    set(H::kUnreportedSplit);
  } else if (code == "DQ204") {
    set(H::kBadClose);
  } else if (code == "DQ205") {
    set(H::kHistorySegment);
  } else {
    return std::nullopt;
  }
  return a;
}

std::uint64_t fnv(std::string_view text) {
  std::uint64_t hash = 0xcbf29ce484222325ULL;
  for (const char ch : text) {
    hash ^= static_cast<unsigned char>(ch);
    hash *= 0x100000001b3ULL;
  }
  return hash;
}

// The price settings and bounds the engine would use for a series: the global
// ones, with the matching profiles' patches over them.
struct SeriesSettings {
  PriceSettings price;
  std::optional<Bounds> bounds;
};

SeriesSettings settings_for(const Config& config, const Series& series, const SeriesMeta* meta) {
  SeriesSettings out{config.price, config.integrity.bounds};
  for (const Profile& profile : config.profiles) {
    if (profile.matches(series, meta)) {
      profile.price.apply_to(out.price);
      if (profile.integrity.bounds) {
        out.bounds = profile.integrity.bounds;
      }
    }
  }
  return out;
}

struct Hyper {
  double jump_prob = 0.03;
  double jump_scale = 6.0;
  double ratio_tolerance = 0.01;
};

class Collector {
 public:
  Collector(const std::vector<Series>& series, const std::vector<Label>& labels,
            const Config& config, const Calendar& calendar, const Context& context,
            const CalibrateOptions& options)
      : series_(series),
        config_(config),
        calendar_(calendar),
        context_(context),
        options_(options) {
    for (std::size_t i = 0; i < labels.size(); ++i) {
      by_series_[labels[i].series].push_back(&labels[i]);
    }
    matched_.assign(labels.size(), 0);
    labels_ = &labels;
  }

  // Every labelled scored move under one setting.
  std::vector<Observation> collect(const Hyper& hyper, const PricePriors& priors) {
    std::vector<std::vector<Observation>> per_series(series_.size());
    std::vector<std::vector<std::uint8_t>> per_matched(series_.size());
    std::atomic<std::size_t> next{0};
    const auto work = [&] {
      for (std::size_t i = next++; i < series_.size(); i = next++) {
        per_matched[i].assign(labels_->size(), 0);
        per_series[i] = observe(series_[i], hyper, priors, per_matched[i]);
      }
    };
    std::vector<std::thread> threads;
    for (unsigned t = 1; t < std::max(1U, options_.threads); ++t) {
      threads.emplace_back(work);
    }
    work();
    for (std::thread& thread : threads) {
      thread.join();
    }
    std::vector<Observation> out;
    std::fill(matched_.begin(), matched_.end(), 0);
    for (std::size_t i = 0; i < series_.size(); ++i) {
      out.insert(out.end(), per_series[i].begin(), per_series[i].end());
      for (std::size_t l = 0; l < matched_.size(); ++l) {
        matched_[l] = static_cast<std::uint8_t>(matched_[l] | per_matched[i][l]);
      }
    }
    return out;
  }

  [[nodiscard]] std::size_t matched_labels() const {
    return static_cast<std::size_t>(std::count(matched_.begin(), matched_.end(), 1));
  }

 private:
  std::vector<Observation> observe(const Series& s, const Hyper& hyper, const PricePriors& priors,
                                   std::vector<std::uint8_t>& matched) const {
    std::vector<const Label*> labels;
    for (const std::string* key : {&s.id, &s.label}) {
      if (const auto it = by_series_.find(*key); !key->empty() && it != by_series_.end()) {
        labels.insert(labels.end(), it->second.begin(), it->second.end());
      }
    }
    if (const auto it = by_series_.find("*"); it != by_series_.end()) {
      labels.insert(labels.end(), it->second.begin(), it->second.end());
    }
    std::vector<Observation> out;
    if (labels.empty() && !options_.clean_unlabelled) {
      return out;
    }
    const SeriesMeta* meta = context_.meta_for(s);
    SeriesSettings settings = settings_for(config_, s, meta);
    settings.price.jump_prob = hyper.jump_prob;
    settings.price.jump_scale = hyper.jump_scale;
    settings.price.ratio_tolerance = hyper.ratio_tolerance;
    settings.price.priors = priors;
    settings.price.p_error_map.clear();
    const PriceAnalysis analysis = analyze_prices(
        s, calendar_, settings.price,
        {.bounds = settings.bounds,
         .have_actions = context_.have_actions,
         .actions = context_.actions_for(s),
         .tick_size = meta != nullptr ? meta->tick_size : std::nullopt,
         .market = context_.market ? &context_.market.value() : nullptr});
    const bool test = options_.holdout > 0.0 &&
                      static_cast<double>(fnv(s.id) % 1000) < options_.holdout * 1000.0;
    for (const PriceFinding& finding : analysis.findings) {
      if (finding.claimed ||
          std::none_of(finding.considered.begin(), finding.considered.end(),
                       [](bool on) { return on; })) {
        continue;
      }
      const Date day = s.date[analysis.features.row[finding.bar]];
      // The label: a fault's before a market fact's.
      const Label* best = nullptr;
      std::optional<std::array<bool, kCount>> allowed;
      for (const Label* label : labels) {
        if (day.days() < label->first.days() - kSlackDays ||
            day.days() > label->last.days() + kSlackDays) {
          continue;
        }
        const auto a = allowed_for(*label);
        if (!a) {
          continue;
        }
        if (best == nullptr || (best->label_class == LabelClass::kMarketFact &&
                                label->label_class != LabelClass::kMarketFact)) {
          best = label;
          allowed = a;
        }
      }
      Observation o;
      o.log_likelihood = finding.log_likelihood;
      o.considered = finding.considered;
      o.test = test;
      if (best != nullptr) {
        o.allowed = *allowed;
        o.error = best->label_class != LabelClass::kMarketFact;
        matched.at(static_cast<std::size_t>(best - labels_->data())) = 1;
      } else if (options_.clean_unlabelled) {
        o.allowed.at(idx(H::kMarketMove)) = true;
        o.allowed.at(idx(H::kTickMove)) = true;
        o.allowed.at(idx(H::kExplainedSplit)) = true;
      } else {
        continue;
      }
      // A label the model cannot express here (a split on a point series) says
      // nothing about the priors.
      bool possible = false;
      for (std::size_t h = 0; h < kCount; ++h) {
        possible = possible || (o.allowed.at(h) && o.considered.at(h));
      }
      if (possible) {
        out.push_back(o);
      }
    }
    return out;
  }

  const std::vector<Series>& series_;
  const Config& config_;
  const Calendar& calendar_;
  const Context& context_;
  const CalibrateOptions& options_;
  const std::vector<Label>* labels_ = nullptr;
  std::unordered_map<std::string, std::vector<const Label*>> by_series_;
  std::vector<std::uint8_t> matched_;
};

// log sum over hypotheses in `mask` of exp(w + ll).
double log_sum(const Observation& o, const Weights& w, const std::array<bool, kCount>& mask) {
  double total = kNegInf;
  for (std::size_t h = 0; h < kCount; ++h) {
    if (mask.at(h) && o.considered.at(h) && w.at(h) > kNegInf) {
      total = stats::log_add(total, w.at(h) + o.log_likelihood.at(h));
    }
  }
  return total;
}

// log P(the label | the evidence), and its gradient in the log weights.
double label_log_lik(const Observation& o, const Weights& w, Weights* gradient) {
  const double allowed = log_sum(o, w, o.allowed);
  const double all = log_sum(o, w, o.considered);
  if (gradient != nullptr && allowed > kNegInf) {
    for (std::size_t h = 0; h < kCount; ++h) {
      if (!o.considered.at(h) || w.at(h) == kNegInf) {
        continue;
      }
      const double share = std::exp(w.at(h) + o.log_likelihood.at(h) - all);
      const double in = o.allowed.at(h) ? std::exp(w.at(h) + o.log_likelihood.at(h) - allowed) : 0.0;
      gradient->at(h) += in - share;
    }
  }
  return allowed - all;
}

// P(an error | the evidence) under log weights w.
double p_error(const Observation& o, const Weights& w) {
  std::array<bool, kCount> errors{};
  for (std::size_t h = 0; h < kCount; ++h) {
    errors.at(h) = is_error(static_cast<H>(h));
  }
  const double e = log_sum(o, w, errors);
  return e == kNegInf ? 0.0 : std::exp(e - log_sum(o, w, o.considered));
}

struct Objective {
  double value = 0.0;
  double log_lik = 0.0;
  std::size_t count = 0;
};

Objective objective(const std::vector<Observation>& obs, const Weights& w, const Weights& w0,
                    bool test, Weights* gradient) {
  Objective out;
  for (const Observation& o : obs) {
    if (o.test != test) {
      continue;
    }
    const double ll = label_log_lik(o, w, gradient);
    if (ll > kNegInf) {
      out.log_lik += ll;
      ++out.count;
    }
  }
  out.value = out.log_lik;
  for (std::size_t h = 0; h < kCount; ++h) {
    if (w0.at(h) > kNegInf) {
      const double d = w.at(h) - w0.at(h);
      out.value -= d * d / (2.0 * kPriorSpread * kPriorSpread);
      if (gradient != nullptr) {
        gradient->at(h) -= d / (kPriorSpread * kPriorSpread);
      }
    }
  }
  return out;
}

Weights log_weights(const PricePriors& priors) {
  Weights w = weights_of(priors);
  for (double& x : w) {
    x = x > 0.0 ? std::log(x) : kNegInf;
  }
  return w;
}

// Gradient ascent with backtracking from w0, on the training observations.
Weights fit_weights(const std::vector<Observation>& obs, const Weights& w0) {
  constexpr int kIterations = 400;
  Weights w = w0;
  double step = 0.5;
  double current = objective(obs, w, w0, false, nullptr).value;
  for (int it = 0; it < kIterations && step > 1e-8; ++it) {
    Weights gradient{};
    static_cast<void>(objective(obs, w, w0, false, &gradient));
    double norm = 0.0;
    for (const double g : gradient) {
      norm += g * g;
    }
    norm = std::sqrt(norm);
    if (norm < 1e-9) {
      break;
    }
    Weights trial = w;
    for (std::size_t h = 0; h < kCount; ++h) {
      if (w.at(h) > kNegInf) {
        trial.at(h) += step * gradient.at(h) / norm;
      }
    }
    const double value = objective(obs, trial, w0, false, nullptr).value;
    if (value > current) {
      w = trial;
      current = value;
      step *= 1.2;
    } else {
      step *= 0.5;
    }
  }
  return w;
}

Fit evaluate(const std::vector<Observation>& obs, const Weights& w, const Weights& w0,
             const Hyper& hyper, const PricePriors& base) {
  Fit fit;
  // Priors as weights summing to 1 over the seven hypotheses every bar may have;
  // explained_split on the same scale.
  Weights normalized = w;
  double total = 0.0;
  for (std::size_t h = 0; h < kCount; ++h) {
    if (h != idx(H::kExplainedSplit) && w.at(h) > kNegInf) {
      total += std::exp(w.at(h));
    }
  }
  for (double& x : normalized) {
    x = x > kNegInf ? std::exp(x) / total : 0.0;
  }
  fit.priors = priors_of(normalized, base);
  fit.jump_prob = hyper.jump_prob;
  fit.jump_scale = hyper.jump_scale;
  fit.ratio_tolerance = hyper.ratio_tolerance;
  const Objective train = objective(obs, w, w0, false, nullptr);
  const Objective test = objective(obs, w, w0, true, nullptr);
  fit.objective = train.value;
  fit.mean_log_lik = train.count > 0 ? train.log_lik / static_cast<double>(train.count) : 0.0;
  fit.test_mean_log_lik = test.count > 0 ? test.log_lik / static_cast<double>(test.count) : 0.0;
  return fit;
}

std::string fixed(double value, int decimals) {
  std::ostringstream out;
  out.setf(std::ios::fixed);
  out.precision(decimals);
  out << value;
  return out.str();
}

// A prior weight to four significant figures.
std::string weight(double value) {
  if (value <= 0.0) {
    return "0";
  }
  const double magnitude = std::floor(std::log10(value));
  const double scale = std::pow(10.0, 3.0 - magnitude);
  return format_number(std::round(value * scale) / scale);
}

}  // namespace

std::vector<std::pair<double, double>> isotonic_map(std::vector<std::pair<double, double>> points,
                                                    std::size_t bins) {
  std::vector<std::pair<double, double>> out;
  if (points.empty() || bins == 0) {
    return out;
  }
  std::stable_sort(points.begin(), points.end(),
                   [](const auto& a, const auto& b) { return a.first < b.first; });
  // Equal-count bins: (mean x, mean y, count).
  struct Block {
    double x = 0.0;
    double y = 0.0;
    double n = 0.0;
  };
  std::vector<Block> blocks;
  const std::size_t per = std::max<std::size_t>(1, (points.size() + bins - 1) / bins);
  for (std::size_t i = 0; i < points.size(); i += per) {
    Block b;
    for (std::size_t j = i; j < std::min(points.size(), i + per); ++j) {
      b.x += points[j].first;
      b.y += points[j].second;
      b.n += 1.0;
    }
    b.x /= b.n;
    b.y /= b.n;
    blocks.push_back(b);
  }
  // Pool adjacent violators.
  std::vector<Block> pooled;
  for (const Block& b : blocks) {
    pooled.push_back(b);
    while (pooled.size() > 1 && pooled[pooled.size() - 2].y > pooled.back().y) {
      const Block top = pooled.back();
      pooled.pop_back();
      Block& prev = pooled.back();
      const double n = prev.n + top.n;
      prev.x = (prev.x * prev.n + top.x * top.n) / n;
      prev.y = (prev.y * prev.n + top.y * top.n) / n;
      prev.n = n;
    }
  }
  // Knots to six decimals, as priors_toml writes them; knots that coincide there
  // become one.
  const auto round6 = [](double v) { return std::round(v * 1e6) / 1e6; };
  for (const Block& b : pooled) {
    const double x = round6(b.x);
    const double y = round6(b.y);
    if (!out.empty() && x <= out.back().first) {
      out.back().second = std::max(out.back().second, y);
      continue;
    }
    out.emplace_back(x, y);
  }
  return out;
}

double expected_calibration_error(const std::vector<std::pair<double, double>>& points,
                                  std::size_t bins) {
  if (points.empty() || bins == 0) {
    return 0.0;
  }
  std::vector<double> p(bins, 0.0);
  std::vector<double> y(bins, 0.0);
  std::vector<double> n(bins, 0.0);
  for (const auto& [x, outcome] : points) {
    const auto b = std::min(bins - 1, static_cast<std::size_t>(x * static_cast<double>(bins)));
    p[b] += x;
    y[b] += outcome;
    n[b] += 1.0;
  }
  double ece = 0.0;
  for (std::size_t b = 0; b < bins; ++b) {
    if (n[b] > 0.0) {
      ece += std::fabs(p[b] - y[b]) / static_cast<double>(points.size());
    }
  }
  return ece;
}

CalibrationReport calibrate(const std::vector<Series>& series, const std::vector<Label>& labels,
                            const Config& config, const Calendar& calendar, const Context& context,
                            const CalibrateOptions& options) {
  CalibrationReport report;
  report.labels = labels.size();
  report.price_labels = static_cast<std::size_t>(std::count_if(
      labels.begin(), labels.end(), [](const Label& l) { return allowed_for(l).has_value(); }));
  report.version = options.version;
  Collector collector(series, labels, config, calendar, context, options);
  const PricePriors& base = config.price.priors;
  const Weights w0 = log_weights(base);
  const Hyper given{config.price.jump_prob, config.price.jump_scale, config.price.ratio_tolerance};

  // As configured.
  const std::vector<Observation> before_obs = collector.collect(given, base);
  report.before = evaluate(before_obs, w0, w0, given, base);
  report.matched_labels = collector.matched_labels();

  // Each setting of the grid, the priors fitted to it; the best by the mean
  // penalized log-likelihood of the training observations (the grid's settings
  // can score slightly different sets of moves).
  std::vector<Hyper> grid = {given};
  if (options.grid) {
    for (const double jp : {0.01, 0.02, 0.03, 0.05}) {
      for (const double js : {4.0, 6.0, 8.0}) {
        for (const double rt : {0.005, 0.01, 0.02}) {
          grid.push_back({jp, js, rt});
        }
      }
    }
  }
  std::vector<Observation> best_obs;
  Weights best_w = w0;
  double best_score = kNegInf;
  for (const Hyper& hyper : grid) {
    std::vector<Observation> obs = collector.collect(hyper, base);
    const Weights w = fit_weights(obs, w0);
    const Objective o = objective(obs, w, w0, false, nullptr);
    const double score = o.count > 0 ? o.value / static_cast<double>(o.count) : kNegInf;
    ++report.settings_tried;
    if (score > best_score) {
      best_score = score;
      best_obs = std::move(obs);
      best_w = w;
      report.after = evaluate(best_obs, w, w0, hyper, base);
    }
  }
  report.observations = best_obs.size();
  report.errors = static_cast<std::size_t>(
      std::count_if(best_obs.begin(), best_obs.end(), [](const Observation& o) { return o.error; }));
  report.test_observations = static_cast<std::size_t>(
      std::count_if(best_obs.begin(), best_obs.end(), [](const Observation& o) { return o.test; }));

  // The p_error map, from the training observations; calibration error on the
  // held-out ones (all, without a hold-out).
  const bool held_out = report.test_observations > 0;
  std::vector<std::pair<double, double>> train;
  std::vector<std::pair<double, double>> check_before;
  std::vector<std::pair<double, double>> check_raw;
  for (const Observation& o : best_obs) {
    if (!o.test) {
      train.emplace_back(p_error(o, best_w), o.error ? 1.0 : 0.0);
    }
    if (o.test == held_out) {
      check_raw.emplace_back(p_error(o, best_w), o.error ? 1.0 : 0.0);
    }
  }
  for (const Observation& o : before_obs) {
    if (o.test == held_out) {
      check_before.emplace_back(calibrated(config.price.p_error_map, p_error(o, w0)),
                                o.error ? 1.0 : 0.0);
    }
  }
  if (options.isotonic) {
    report.p_error_map = isotonic_map(train);
  }
  for (auto& [p, y] : check_raw) {
    p = calibrated(report.p_error_map, p);
  }
  report.ece_before = expected_calibration_error(check_before);
  report.ece_after = expected_calibration_error(check_raw);
  return report;
}

std::string priors_toml(const CalibrationReport& r) {
  const PricePriors& p = r.after.priors;
  std::string out;
  out += "# Written by `dorq calibrate` (dorq " + std::string{kVersion} + "). Include it from a\n";
  out += "# config file: include = \"priors.toml\". See doc/calibration.md.\n#\n";
  out += "# labels: " + std::to_string(r.labels) + ", of which " + std::to_string(r.price_labels) +
         " about price moves and " + std::to_string(r.matched_labels) +
         " matching a scored move\n";
  out += "# observations: " + std::to_string(r.observations) + " (" + std::to_string(r.errors) +
         " errors), " + std::to_string(r.test_observations) + " held out\n";
  out += "# settings tried: " + std::to_string(r.settings_tried) + "\n";
  out += "# mean log P(label | evidence), training: " + fixed(r.before.mean_log_lik, 4) +
         " before, " + fixed(r.after.mean_log_lik, 4) + " after\n";
  if (r.test_observations > 0) {
    out += "# mean log P(label | evidence), held out: " + fixed(r.before.test_mean_log_lik, 4) +
           " before, " + fixed(r.after.test_mean_log_lik, 4) + " after\n";
  }
  out += "# expected calibration error of p_error: " + fixed(r.ece_before, 4) + " before, " +
         fixed(r.ece_after, 4) + " after\n";
  out += "\n[price]\n";
  out += "jump_prob = " + format_number(r.after.jump_prob) + "\n";
  out += "jump_scale = " + format_number(r.after.jump_scale) + "\n";
  out += "ratio_tolerance = " + format_number(r.after.ratio_tolerance) + "\n";
  out += "\n[priors]\n";
  out += "market_move = " + weight(p.market_move) + "\n";
  out += "tick_move = " + weight(p.tick_move) + "\n";
  out += "bad_print = " + weight(p.bad_print) + "\n";
  out += "bad_close = " + weight(p.bad_close) + "\n";
  out += "unreported_split = " + weight(p.unreported_split) + "\n";
  out += "scale_error = " + weight(p.scale_error) + "\n";
  out += "history_segment = " + weight(p.history_segment) + "\n";
  out += "explained_split = " + weight(p.explained_split) + "\n";
  out += "\n[calibration]\nversion = \"" + r.version + "\"\np_error_map = [";
  for (std::size_t i = 0; i < r.p_error_map.size(); ++i) {
    out += (i == 0 ? "\n  [" : ",\n  [") + fixed(r.p_error_map[i].first, 6) + ", " +
           fixed(r.p_error_map[i].second, 6) + "]";
  }
  out += r.p_error_map.empty() ? "]\n" : ",\n]\n";
  return out;
}

std::string calibration_summary(const CalibrationReport& r) {
  std::string out;
  out += "labels: " + std::to_string(r.labels) + " (" + std::to_string(r.price_labels) +
         " about price moves, " + std::to_string(r.matched_labels) + " matched)\n";
  out += "observations: " + std::to_string(r.observations) + " (" + std::to_string(r.errors) +
         " errors, " + std::to_string(r.test_observations) + " held out)\n";
  out += "settings tried: " + std::to_string(r.settings_tried) + "; chosen jump_prob " +
         format_number(r.after.jump_prob) + ", jump_scale " + format_number(r.after.jump_scale) +
         ", ratio_tolerance " + format_number(r.after.ratio_tolerance) + "\n";
  out += "mean log P(label | evidence): " + fixed(r.before.mean_log_lik, 4) + " -> " +
         fixed(r.after.mean_log_lik, 4);
  if (r.test_observations > 0) {
    out += " (held out: " + fixed(r.before.test_mean_log_lik, 4) + " -> " +
           fixed(r.after.test_mean_log_lik, 4) + ")";
  }
  out += "\nexpected calibration error: " + fixed(r.ece_before, 4) + " -> " +
         fixed(r.ece_after, 4) + "\n";
  return out;
}

}  // namespace dorq
