#include "calibrate/statements.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "calibrate/calibrate.hpp"
#include "dorq/number.hpp"
#include "dorq/version.hpp"
#include "engine/engine.hpp"
#include "stats/special.hpp"

namespace dorq {
namespace {

constexpr double kNegInf = -std::numeric_limits<double>::infinity();
// As for the price model, a Gaussian penalty keeps the fitted log priors within
// about `spread` e-folds of the configured ones, so a few labels cannot drive a
// prior to zero. With held-out series, the spread is chosen among these by the
// held-out log-likelihood: a statements fit is easily pulled by one universe's mix
// of faults. Without enough of them it is the price model's 1.5.
constexpr std::array<double, 5> kSpreads{0.25, 0.5, 1.0, 1.5, 3.0};
constexpr double kDefaultSpread = 1.5;
// The fewest held-out observations a spread is chosen by.
constexpr std::size_t kMinHeldOut = 30;
// A label covers a candidate whose fiscal dates are within this many calendar days
// of its own: dorq-eval's slack.
constexpr int kSlackDays = 3;
// A sub-family is mapped only with this many training observations, and at least
// this many errors and this many innocent ones among them.
constexpr std::size_t kMinMapObservations = 20;
constexpr std::size_t kMinMapClass = 2;

// The three models whose priors are fitted: their codes and hypotheses, in the
// order the checks report them; which of them are errors.
struct ModelSpec {
  const char* name;
  std::array<std::string_view, 4> codes;  // the codes whose candidates it scores
  std::array<std::string_view, 4> hypotheses;
  std::array<bool, 4> error;
  std::array<std::string_view, 4> expect;  // a fault label's code naming each hypothesis
};

constexpr std::array<ModelSpec, 3> kModels{{
    {"values",
     {"DQ911", "DQ912", "DQ913", "DQ919"},
     {"real_change", "bad_value", "unit_error", "sign_error"},
     {false, true, true, true},
     {"", "DQ911", "DQ912", "DQ913"}},
    {"identities",
     {"DQ903", "", "", ""},
     {"structural", "reclassification", "field_error", "unexplained"},
     {false, false, true, true},
     {"", "", "DQ903", "DQ903"}},
    {"filings",
     {"DQ931", "", "", ""},
     {"ordinary", "late_filing", "misdated_early", "misdated_amendment"},
     {false, false, true, true},
     {"", "", "DQ931", "DQ931"}},
}};

using Vec = std::array<double, 4>;

Vec priors_of(std::size_t model, const StatementPriors& p) {
  switch (model) {
    case 0:
      return {1.0 - p.bad_value - p.unit_error - p.sign_error, p.bad_value, p.unit_error,
              p.sign_error};
    case 1: {
      const double sum = p.identity_structural + p.identity_reclassification +
                         p.identity_field_error + p.identity_unexplained;
      return {p.identity_structural / sum, p.identity_reclassification / sum,
              p.identity_field_error / sum, p.identity_unexplained / sum};
    }
    default:
      return {1.0 - p.filing_late - p.filing_early - p.filing_amendment, p.filing_late,
              p.filing_early, p.filing_amendment};
  }
}

// The fitted probabilities into the priors, within the bounds the config reader
// takes: an error prior of at least 1e-6 and at most 0.5, the value model's three
// and the filing model's three summing to less than 0.95.
void set_priors(std::size_t model, const Vec& q, StatementPriors& p) {
  const auto bounded = [](double x) { return std::clamp(x, 1e-6, 0.5); };
  if (model == 0 || model == 2) {
    Vec e{0.0, bounded(q[1]), bounded(q[2]), bounded(q[3])};
    const double sum = e[1] + e[2] + e[3];
    if (sum >= 0.94) {
      for (double& x : e) {
        x *= 0.94 / sum;
      }
    }
    if (model == 0) {
      p.bad_value = e[1];
      p.unit_error = e[2];
      p.sign_error = e[3];
    } else {
      p.filing_late = e[1];
      p.filing_early = e[2];
      p.filing_amendment = e[3];
    }
    return;
  }
  const double top = *std::max_element(q.begin(), q.end());
  const auto weight = [&](double x) { return std::clamp(x / top, 1e-6, 1.0); };
  p.identity_structural = weight(q[0]);
  p.identity_reclassification = weight(q[1]);
  p.identity_field_error = weight(q[2]);
  p.identity_unexplained = weight(q[3]);
}

std::uint64_t fnv(std::string_view text) {
  std::uint64_t hash = 0xcbf29ce484222325ULL;
  for (const char ch : text) {
    hash ^= static_cast<unsigned char>(ch);
    hash *= 0x100000001b3ULL;
  }
  return hash;
}

// One candidate a check reported, with every candidate shown.
struct Candidate {
  std::string series;
  std::string label;
  std::string code;
  Date first;
  Date last;
  std::optional<StatementPeriod> period;
  std::string item;
  std::string scope;     // a value finding's row or era (DQ912), or empty
  double p_error = 0.0;  // the model's, before any map
  std::vector<HypothesisProbability> hypotheses;
};

class Collect final : public ResultSink {
 public:
  explicit Collect(std::vector<Candidate>& out) : out_(out) {}
  void series_done(SeriesResult&& result) override {
    if (result.kind != SeriesKind::kStatements || result.from_context) {
      return;
    }
    for (Violation& v : result.violations) {
      if (v.check == nullptr || !v.date || !StatementCalibration::family(v.check->code)) {
        continue;
      }
      Candidate c;
      c.series = result.id;
      c.label = result.label;
      c.code = std::string{v.check->code};
      c.first = *v.date;
      c.last = v.end_date.value_or(*v.date);
      c.period = v.period;
      for (const DetailField& k : v.key) {
        if (const auto* text = std::get_if<std::string>(&k.value)) {
          if (k.key == "item") {
            c.item = *text;
          } else if (k.key == "scope") {
            c.scope = *text;
          }
        }
      }
      c.p_error = v.p_error;
      c.hypotheses = std::move(v.hypotheses);
      out_.push_back(std::move(c));
    }
  }

 private:
  std::vector<Candidate>& out_;
};

bool is_statements_label(const Label& l) {
  if (l.about_statements()) {
    return true;
  }
  if (l.codes.starts_with("DQ9")) {
    return true;
  }
  std::string_view rest = l.expect;
  while (!rest.empty()) {
    const std::size_t bar = rest.find('|');
    if (rest.substr(0, bar).starts_with("DQ9")) {
      return true;
    }
    rest = bar == std::string_view::npos ? std::string_view{} : rest.substr(bar + 1);
  }
  return false;
}

bool expects(const Label& l, std::string_view code) {
  if (l.expect.empty()) {
    return true;
  }
  std::string_view rest = l.expect;
  while (!rest.empty()) {
    const std::size_t bar = rest.find('|');
    if (rest.substr(0, bar) == code) {
      return true;
    }
    rest = bar == std::string_view::npos ? std::string_view{} : rest.substr(bar + 1);
  }
  return false;
}

// Whether label `l` covers candidate `c`, as dorq-eval matches a statements label.
bool covers(const Label& l, const Candidate& c, int slack) {
  if (l.series != c.series && (c.label.empty() || l.series != c.label)) {
    return false;
  }
  if (c.period) {
    if (l.statement && *l.statement != c.period->statement) {
      return false;
    }
    if (l.period_type && *l.period_type != c.period->period_type) {
      return false;
    }
    if (l.version && c.period->version && *l.version != *c.period->version) {
      return false;
    }
  }
  if (!l.item.empty() && !c.item.empty() && l.item != c.item) {
    return false;
  }
  return c.first.days() <= l.last.days() + slack && l.first.days() <= c.last.days() + slack;
}

// What the labels say of one candidate: whether it is an error (for the maps), and
// for a model's candidate the hypotheses they allow (empty: no observation).
struct Judgement {
  bool labelled = false;
  bool error = false;  // a fault label naming its code covers it
  // Only fault labels naming other codes cover it: the fault is real, but whether
  // it is the thing reported is not known, so it is left out of the maps, as
  // dorq-eval leaves it out of its reliability diagram.
  bool other = false;
  std::array<bool, 4> allowed{};
};

Judgement judge(const Candidate& c, const std::vector<const Label*>& labels,
                const ModelSpec* model) {
  Judgement out;
  bool fault = false;
  bool own = false;  // a fault label naming the candidate's code
  bool fact = false;
  std::array<bool, 4> named{};  // the error hypotheses the fault labels name
  bool any_error = false;       // a fault label naming no code of the model
  for (const Label* l : labels) {
    if (l->label_class == LabelClass::kMarketFact) {
      if (covers(*l, c, 0) && c.code.starts_with(l->codes)) {
        fact = true;
      }
      continue;
    }
    if (!covers(*l, c, kSlackDays)) {
      continue;
    }
    fault = true;
    own = own || expects(*l, c.code);
    if (model == nullptr) {
      continue;
    }
    if (l->expect.empty()) {
      any_error = true;
    }
    for (std::size_t h = 0; h < 4; ++h) {
      if (model->error.at(h) && !model->expect.at(h).empty() && expects(*l, model->expect.at(h))) {
        named.at(h) = true;
      }
    }
  }
  out.labelled = fault || fact;
  out.error = own;
  out.other = fault && !own;
  if (model == nullptr) {
    return out;
  }
  if (fault) {
    for (std::size_t h = 0; h < 4; ++h) {
      out.allowed.at(h) = model->error.at(h) && (any_error || named.at(h));
    }
  } else if (fact) {
    for (std::size_t h = 0; h < 4; ++h) {
      out.allowed.at(h) = !model->error.at(h);
    }
  }
  return out;
}

bool fault_of(const Judgement& j, const ModelSpec& model) {
  for (std::size_t h = 0; h < 4; ++h) {
    if (model.error.at(h) && j.allowed.at(h)) {
      return true;
    }
  }
  return false;
}

// One labelled candidate of a fitted model: the evidence under each hypothesis
// (its log-likelihood, up to a constant) and the hypotheses the label allows.
struct ModelObservation {
  Vec log_likelihood{};
  std::array<bool, 4> considered{};
  std::array<bool, 4> allowed{};
  bool error = false;
  bool test = false;
};

double log_sum(const ModelObservation& o, const Vec& w, const std::array<bool, 4>& mask) {
  double total = kNegInf;
  for (std::size_t h = 0; h < 4; ++h) {
    if (mask.at(h) && o.considered.at(h)) {
      total = stats::log_add(total, w.at(h) + o.log_likelihood.at(h));
    }
  }
  return total;
}

double label_log_lik(const ModelObservation& o, const Vec& w, Vec* gradient) {
  const double allowed = log_sum(o, w, o.allowed);
  const double all = log_sum(o, w, o.considered);
  if (gradient != nullptr && allowed > kNegInf) {
    for (std::size_t h = 0; h < 4; ++h) {
      if (!o.considered.at(h)) {
        continue;
      }
      const double share = std::exp(w.at(h) + o.log_likelihood.at(h) - all);
      const double in =
          o.allowed.at(h) ? std::exp(w.at(h) + o.log_likelihood.at(h) - allowed) : 0.0;
      gradient->at(h) += in - share;
    }
  }
  return allowed - all;
}

struct Objective {
  double value = 0.0;
  double log_lik = 0.0;
  std::size_t count = 0;
};

Objective objective(const std::vector<ModelObservation>& obs, const Vec& w, const Vec& w0,
                    bool test, Vec* gradient, double spread = kDefaultSpread) {
  Objective out;
  for (const ModelObservation& o : obs) {
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
  for (std::size_t h = 0; h < 4; ++h) {
    const double d = w.at(h) - w0.at(h);
    out.value -= d * d / (2.0 * spread * spread);
    if (gradient != nullptr) {
      gradient->at(h) -= d / (spread * spread);
    }
  }
  return out;
}

// Gradient ascent with backtracking from w0, on the training observations.
Vec fit_weights(const std::vector<ModelObservation>& obs, const Vec& w0, double spread) {
  constexpr int kIterations = 400;
  Vec w = w0;
  double step = 0.5;
  double current = objective(obs, w, w0, false, nullptr, spread).value;
  for (int it = 0; it < kIterations && step > 1e-8; ++it) {
    Vec gradient{};
    static_cast<void>(objective(obs, w, w0, false, &gradient, spread));
    double norm = 0.0;
    for (const double g : gradient) {
      norm += g * g;
    }
    norm = std::sqrt(norm);
    if (norm < 1e-9) {
      break;
    }
    Vec trial = w;
    for (std::size_t h = 0; h < 4; ++h) {
      trial.at(h) += step * gradient.at(h) / norm;
    }
    const double value = objective(obs, trial, w0, false, nullptr, spread).value;
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

Vec log_of(const Vec& q) {
  Vec out{};
  for (std::size_t h = 0; h < 4; ++h) {
    out.at(h) = std::log(q.at(h));
  }
  return out;
}

Vec normalized(const Vec& w) {
  const double top = *std::max_element(w.begin(), w.end());
  Vec out{};
  double sum = 0.0;
  for (std::size_t h = 0; h < 4; ++h) {
    out.at(h) = std::exp(w.at(h) - top);
    sum += out.at(h);
  }
  for (double& x : out) {
    x /= sum;
  }
  return out;
}

// P(an error | the evidence) under log priors w.
double p_error_of(const ModelObservation& o, const ModelSpec& model, const Vec& w) {
  std::array<bool, 4> errors{};
  for (std::size_t h = 0; h < 4; ++h) {
    errors.at(h) = model.error.at(h);
  }
  const double e = log_sum(o, w, errors);
  return e == kNegInf ? 0.0 : std::exp(e - log_sum(o, w, o.considered));
}

// Isotonic regression of y on x by pool adjacent violators over the points
// themselves, not over equal-count bins: a statements fit sees thousands of
// innocent candidates near p = 0 and a few faults near 1, and an equal-count top
// bin would pool them, holding every high p_error at the bin's rate. Each block is
// a knot at its mean x, as the bars fit's isotonic_map writes it, so the map
// interpolates between blocks rather than stepping at their edges: where the
// training points are sparse, a step would carry a block's rate across a range it
// barely saw. A block's rate is shrunk toward its mean x by kPseudoCount
// observations, so that a few labels cannot pin it at 0 or 1. Beyond kMaxKnots,
// the adjacent pair of blocks with the fewest points is merged first.
std::vector<std::pair<double, double>> pav_map(std::vector<std::pair<double, double>> points) {
  constexpr std::size_t kMaxKnots = 40;
  constexpr double kPseudoCount = 2.0;
  std::sort(points.begin(), points.end());
  struct Block {
    double sx = 0.0;
    double sy = 0.0;
    double n = 0.0;
    [[nodiscard]] double x() const { return sx / n; }
    [[nodiscard]] double y() const { return sy / n; }
    [[nodiscard]] double shrunk() const { return (sy + kPseudoCount * x()) / (n + kPseudoCount); }
    void absorb(const Block& next) {
      sx += next.sx;
      sy += next.sy;
      n += next.n;
    }
  };
  std::vector<Block> blocks;
  for (const auto& [x, y] : points) {
    blocks.push_back({x, y, 1.0});
    while (blocks.size() > 1 && blocks[blocks.size() - 2].y() >= blocks.back().y()) {
      const Block last = blocks.back();
      blocks.pop_back();
      blocks.back().absorb(last);
    }
  }
  while (blocks.size() > kMaxKnots) {
    std::size_t best = 0;
    double fewest = std::numeric_limits<double>::infinity();
    for (std::size_t i = 0; i + 1 < blocks.size(); ++i) {
      if (blocks[i].n + blocks[i + 1].n < fewest) {
        fewest = blocks[i].n + blocks[i + 1].n;
        best = i;
      }
    }
    blocks[best].absorb(blocks[best + 1]);
    blocks.erase(blocks.begin() + static_cast<std::ptrdiff_t>(best) + 1);
  }
  // Knots at six decimals, as the file holds them; knots that coincide there
  // become one, at the higher rate, and the rates never fall.
  std::vector<std::pair<double, double>> knots;
  for (const Block& b : blocks) {
    const double x = std::round(b.x() * 1e6) / 1e6;
    const double y =
        std::max(std::round(b.shrunk() * 1e6) / 1e6, knots.empty() ? 0.0 : knots.back().second);
    if (!knots.empty() && x <= knots.back().first) {
      knots.back().second = std::max(knots.back().second, y);
      continue;
    }
    knots.emplace_back(x, y);
  }
  return knots;
}

std::string fixed(double value, int decimals) {
  if (!std::isfinite(value)) {
    return "nan";
  }
  char buffer[64];
  std::snprintf(buffer, sizeof buffer, "%.*f", decimals, value);
  return buffer;
}

// A prior to four significant figures.
std::string weight(double value) {
  char buffer[64];
  std::snprintf(buffer, sizeof buffer, "%.4g", value);
  return buffer;
}

}  // namespace

StatementCalibrationReport calibrate_statements(const std::vector<Series>& series,
                                                const std::vector<Label>& labels,
                                                const Config& config, const Calendar& calendar,
                                                const Context& context,
                                                const StatementCalibrateOptions& options) {
  StatementCalibrationReport report;
  report.labels = labels.size();
  report.version = options.version;
  report.priors = config.statements.priors;
  std::vector<const Label*> statement_labels;
  for (const Label& l : labels) {
    if (is_statements_label(l)) {
      statement_labels.push_back(&l);
    }
  }
  report.statement_labels = statement_labels.size();

  // Every candidate, as the model scores it: no threshold, the innocent shown,
  // no map applied.
  Config run = config;
  run.severity.info = 0.0;
  run.min_severity = Severity::kInfo;
  run.statements.report_all = true;
  run.statements.calibration = {};
  std::vector<Candidate> candidates;
  {
    Collect sink(candidates);
    Engine engine(run,
                  {.threads = options.threads,
                   .min_severity = Severity::kInfo,
                   .since = std::nullopt,
                   .as_of = options.as_of,
                   .calendar = &calendar,
                   .context = &context},
                  sink);
    for (const Series& s : series) {
      if (s.kind == SeriesKind::kStatements) {
        engine.submit(Series{s});
      }
    }
    engine.finish();
  }
  report.candidates = candidates.size();

  // Labels by series, so each candidate meets only its own.
  std::vector<std::pair<std::string, const Label*>> by_series;
  for (const Label* l : statement_labels) {
    by_series.emplace_back(l->series, l);
  }
  std::sort(by_series.begin(), by_series.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });
  const auto labels_of = [&by_series](const std::string& id) {
    std::vector<const Label*> out;
    auto it = std::lower_bound(
        by_series.begin(), by_series.end(), id,
        [](const auto& entry, const std::string& key) { return entry.first < key; });
    for (; it != by_series.end() && it->first == id; ++it) {
      out.push_back(it->second);
    }
    return out;
  };

  std::array<std::vector<ModelObservation>, 3> obs;
  // Per family: (the model's p_error, error), with the candidate's model and
  // observation where its p_error is refitted.
  struct Point {
    double p = 0.0;
    bool error = false;
    bool test = false;
    int model = -1;
    std::size_t index = 0;
  };
  std::array<std::vector<Point>, StatementCalibration::kFamilies> points;
  std::vector<const Label*> matched;
  for (const Candidate& c : candidates) {
    const bool test = options.holdout > 0.0 &&
                      static_cast<double>(fnv(c.series) % 1000) < options.holdout * 1000.0;
    std::vector<const Label*> own = labels_of(c.series);
    if (!c.label.empty() && c.label != c.series) {
      const std::vector<const Label*> by_label = labels_of(c.label);
      own.insert(own.end(), by_label.begin(), by_label.end());
    }
    int model = -1;
    for (std::size_t m = 0; m < kModels.size(); ++m) {
      if (std::find(kModels.at(m).codes.begin(), kModels.at(m).codes.end(), c.code) !=
          kModels.at(m).codes.end()) {
        model = static_cast<int>(m);
      }
    }
    if (!c.scope.empty()) {
      model = -1;  // a row's or an era's posterior is not the value model's priors'
    }
    const ModelSpec* spec = model >= 0 ? &kModels.at(static_cast<std::size_t>(model)) : nullptr;
    Judgement j = judge(c, own, spec);
    for (const Label* l : own) {
      if (covers(*l, c, kSlackDays) &&
          std::find(matched.begin(), matched.end(), l) == matched.end()) {
        matched.push_back(l);
      }
    }
    if (!j.labelled) {
      if (!options.clean_unlabelled) {
        continue;
      }
      j.error = false;
      if (spec != nullptr) {
        for (std::size_t h = 0; h < 4; ++h) {
          j.allowed.at(h) = !spec->error.at(h);
        }
      }
    }
    Point point{c.p_error, j.error, test, -1, 0};
    if (spec != nullptr) {
      // The evidence under each hypothesis: its posterior over its prior.
      const Vec prior = priors_of(static_cast<std::size_t>(model), config.statements.priors);
      ModelObservation o;
      // An error for the model: a fault label naming one of its error hypotheses
      // (a DQ912 label on a DQ911 candidate is the value model's unit_error).
      o.error = fault_of(j, *spec);
      o.test = test;
      bool any = false;
      for (std::size_t h = 0; h < 4; ++h) {
        const auto it = std::find_if(c.hypotheses.begin(), c.hypotheses.end(), [&](const auto& x) {
          return x.name == spec->hypotheses.at(h);
        });
        if (it != c.hypotheses.end() && it->probability > 0.0 && prior.at(h) > 0.0) {
          o.considered.at(h) = true;
          o.log_likelihood.at(h) = std::log(it->probability) - std::log(prior.at(h));
        }
        o.allowed.at(h) = j.allowed.at(h) && o.considered.at(h);
        any = any || o.allowed.at(h);
      }
      if (any) {
        point.model = model;
        point.index = obs.at(static_cast<std::size_t>(model)).size();
        obs.at(static_cast<std::size_t>(model)).push_back(o);
      }
    }
    // A map is fitted on the codes that apply it.
    if (const auto f = StatementCalibration::family(c.code);
        f && StatementCalibration::mapped(c.code) && !j.other) {
      points.at(*f).push_back(point);
    }
  }
  report.matched_labels = matched.size();

  // The priors.
  std::array<Vec, 3> w_after{};
  for (std::size_t m = 0; m < kModels.size(); ++m) {
    const ModelSpec& spec = kModels.at(m);
    StatementModelFit& fit = report.models.at(m);
    fit.name = spec.name;
    for (const std::string_view h : spec.hypotheses) {
      fit.hypotheses.emplace_back(h);
    }
    const Vec before = priors_of(m, config.statements.priors);
    const Vec w0 = log_of(before);
    const std::vector<ModelObservation>& o = obs.at(m);
    fit.observations = o.size();
    fit.errors = static_cast<std::size_t>(
        std::count_if(o.begin(), o.end(), [](const ModelObservation& x) { return x.error; }));
    fit.test_observations = static_cast<std::size_t>(
        std::count_if(o.begin(), o.end(), [](const ModelObservation& x) { return x.test; }));
    // The spread: by the held-out log-likelihood when there are held-out
    // observations, the default otherwise.
    Vec w = w0;
    fit.spread = kDefaultSpread;
    if (!o.empty()) {
      w = fit_weights(o, w0, kDefaultSpread);
      if (fit.test_observations >= kMinHeldOut) {
        double best = objective(o, w, w0, true, nullptr).log_lik;
        for (const double spread : kSpreads) {
          const Vec trial = fit_weights(o, w0, spread);
          const double held = objective(o, trial, w0, true, nullptr).log_lik;
          if (held > best) {
            best = held;
            w = trial;
            fit.spread = spread;
          }
        }
      }
    }
    w_after.at(m) = w;
    fit.before.assign(before.begin(), before.end());
    const Vec after = normalized(w);
    fit.after.assign(after.begin(), after.end());
    const auto mean = [&](const Vec& weights, bool test) {
      const Objective x = objective(o, weights, w0, test, nullptr);
      return x.count > 0 ? x.log_lik / static_cast<double>(x.count) : 0.0;
    };
    fit.mean_log_lik_before = mean(w0, false);
    fit.mean_log_lik_after = mean(w, false);
    fit.test_mean_log_lik_before = mean(w0, true);
    fit.test_mean_log_lik_after = mean(w, true);
    if (!o.empty()) {
      set_priors(m, after, report.priors);
    }
  }

  // The maps: each family's p_error under the fitted priors, against the labels.
  const bool held_out = std::any_of(points.begin(), points.end(), [](const auto& family) {
    return std::any_of(family.begin(), family.end(), [](const Point& p) { return p.test; });
  });
  std::vector<std::pair<double, double>> all_before;
  std::vector<std::pair<double, double>> all_after;
  for (std::size_t f = 0; f < StatementCalibration::kFamilies; ++f) {
    StatementFamilyFit& fam = report.families.at(f);
    std::vector<std::pair<double, double>> train;
    std::vector<std::pair<double, double>> check_before;
    std::vector<std::pair<double, double>> check_after;
    for (const Point& p : points.at(f)) {
      double refit = p.p;
      if (p.model >= 0) {
        const auto m = static_cast<std::size_t>(p.model);
        refit = p_error_of(obs.at(m).at(p.index), kModels.at(m), w_after.at(m));
      }
      ++fam.observations;
      fam.errors += p.error ? 1 : 0;
      const double y = p.error ? 1.0 : 0.0;
      if (!p.test) {
        train.emplace_back(refit, y);
      }
      if (p.test == held_out) {
        check_before.emplace_back(config.statements.calibration.apply_family(f, p.p), y);
        check_after.emplace_back(refit, y);
      }
    }
    const std::size_t errors = static_cast<std::size_t>(
        std::count_if(train.begin(), train.end(), [](const auto& x) { return x.second > 0.5; }));
    if (options.isotonic && train.size() >= kMinMapObservations && errors >= kMinMapClass &&
        train.size() - errors >= kMinMapClass) {
      fam.map = pav_map(train);
    }
    StatementCalibration only;
    only.maps.at(f) = fam.map;
    for (auto& [p, y] : check_after) {
      p = only.apply_family(f, p);
    }
    fam.ece_before = expected_calibration_error(check_before);
    fam.ece_after = expected_calibration_error(check_after);
    all_before.insert(all_before.end(), check_before.begin(), check_before.end());
    all_after.insert(all_after.end(), check_after.begin(), check_after.end());
  }
  report.ece_before = expected_calibration_error(all_before);
  report.ece_after = expected_calibration_error(all_after);
  return report;
}

std::string statements_toml(const StatementCalibrationReport& r) {
  std::string out;
  out += "# Written by `dorq calibrate` (dorq " + std::string{kVersion} +
         ") on statements labels.\n# Include it from a config file: include = \"...\". See "
         "doc/calibration.md.\n#\n";
  out += "# labels: " + std::to_string(r.labels) + ", of which " +
         std::to_string(r.statement_labels) + " about statements and " +
         std::to_string(r.matched_labels) + " matching a candidate\n";
  out += "# candidates: " + std::to_string(r.candidates) + "\n";
  for (const StatementModelFit& m : r.models) {
    out += "# " + m.name + ": " + std::to_string(m.observations) + " observations (" +
           std::to_string(m.errors) + " errors, " + std::to_string(m.test_observations) +
           " held out), spread " + format_number(m.spread) + "; mean log P(label | evidence) " +
           fixed(m.mean_log_lik_before, 4) + " before, " + fixed(m.mean_log_lik_after, 4) +
           " after";
    if (m.test_observations > 0) {
      out += "; held out " + fixed(m.test_mean_log_lik_before, 4) + " before, " +
             fixed(m.test_mean_log_lik_after, 4) + " after";
    }
    out += "\n";
  }
  for (std::size_t f = 0; f < r.families.size(); ++f) {
    const StatementFamilyFit& fam = r.families.at(f);
    if (fam.observations == 0) {
      continue;
    }
    out += "# DQ9" + std::to_string(f) + "x: " + std::to_string(fam.observations) +
           " observations (" + std::to_string(fam.errors) +
           " errors); expected calibration error " + fixed(fam.ece_before, 4) + " before, " +
           fixed(fam.ece_after, 4) + " after" + (fam.map.empty() ? " (no map: too few)" : "") +
           "\n";
  }
  out += "# expected calibration error, every family: " + fixed(r.ece_before, 4) + " before, " +
         fixed(r.ece_after, 4) + " after\n";
  const StatementPriors& p = r.priors;
  out += "\n[statements.priors]\n";
  out += "bad_value = " + weight(p.bad_value) + "\n";
  out += "unit_error = " + weight(p.unit_error) + "\n";
  out += "sign_error = " + weight(p.sign_error) + "\n";
  out += "identity_structural = " + weight(p.identity_structural) + "\n";
  out += "identity_reclassification = " + weight(p.identity_reclassification) + "\n";
  out += "identity_field_error = " + weight(p.identity_field_error) + "\n";
  out += "identity_unexplained = " + weight(p.identity_unexplained) + "\n";
  out += "filing_late = " + weight(p.filing_late) + "\n";
  out += "filing_early = " + weight(p.filing_early) + "\n";
  out += "filing_amendment = " + weight(p.filing_amendment) + "\n";
  out += "\n[statements.calibration]\nversion = \"" + r.version + "\"\n";
  for (std::size_t f = 0; f < r.families.size(); ++f) {
    const auto& map = r.families.at(f).map;
    out += "dq9" + std::to_string(f) + " = [";
    for (std::size_t i = 0; i < map.size(); ++i) {
      out += (i == 0 ? "\n  [" : ",\n  [") + fixed(map[i].first, 6) + ", " +
             fixed(map[i].second, 6) + "]";
    }
    out += map.empty() ? "]\n" : ",\n]\n";
  }
  return out;
}

std::string statements_summary(const StatementCalibrationReport& r) {
  std::string out;
  out += "labels: " + std::to_string(r.labels) + " (" + std::to_string(r.statement_labels) +
         " about statements, " + std::to_string(r.matched_labels) + " matched); " +
         std::to_string(r.candidates) + " candidates\n";
  for (const StatementModelFit& m : r.models) {
    out += m.name + ": " + std::to_string(m.observations) + " observations (" +
           std::to_string(m.errors) + " errors); mean log P(label | evidence) " +
           fixed(m.mean_log_lik_before, 4) + " -> " + fixed(m.mean_log_lik_after, 4) + "; priors";
    for (std::size_t h = 0; h < m.hypotheses.size(); ++h) {
      out += " " + m.hypotheses[h] + " " + weight(m.before.at(h)) + "->" + weight(m.after.at(h));
    }
    out += "\n";
  }
  out += "expected calibration error: " + fixed(r.ece_before, 4) + " -> " + fixed(r.ece_after, 4) +
         "\n";
  return out;
}

}  // namespace dorq
