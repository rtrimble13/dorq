#include <array>
#include <cmath>
#include <limits>
#include <vector>

#include <doctest/doctest.h>

#include "stats/hmm2.hpp"
#include "stats/proportion.hpp"
#include "stats/special.hpp"

namespace st = dorq::stats;
using doctest::Approx;

// Reference values from SciPy 1.x (scipy.special, scipy.stats).

TEST_CASE("log_gamma") {
  CHECK(st::log_gamma(1.0) == Approx(0.0).epsilon(1e-14));
  CHECK(st::log_gamma(0.5) == Approx(std::log(std::sqrt(3.141592653589793))).epsilon(1e-13));
  CHECK(st::log_gamma(10.0) == Approx(std::log(362880.0)).epsilon(1e-13));
  CHECK(st::log_gamma(1e-3) == Approx(6.907178885383853).epsilon(1e-12));
  CHECK(st::log_gamma(5000.0) == Approx(37582.62631568535).epsilon(1e-13));
}

TEST_CASE("incomplete_beta matches scipy.special.betainc") {
  CHECK(st::incomplete_beta(0.5, 0.5, 0.3) == Approx(0.36901011956554536).epsilon(1e-12));
  CHECK(st::incomplete_beta(2, 3, 0.4) == Approx(0.5248).epsilon(1e-12));
  CHECK(st::incomplete_beta(30, 5, 0.9) == Approx(0.7504082837878774).epsilon(1e-11));
  CHECK(st::incomplete_beta(100, 1e-3, 0.999) == Approx(0.0018260583349551315).epsilon(1e-9));
  CHECK(st::incomplete_beta(1, 1, 0.25) == Approx(0.25).epsilon(1e-14));
  CHECK(st::incomplete_beta(5000, 10, 0.998) == Approx(0.4555524944046158).epsilon(1e-9));
  CHECK(st::incomplete_beta(2, 3, 0.0) == 0.0);
  CHECK(st::incomplete_beta(2, 3, 1.0) == 1.0);
}

TEST_CASE("beta_quantile matches scipy.stats.beta.ppf") {
  CHECK(st::beta_quantile(2, 3, 0.05) == Approx(0.09761146288641434).epsilon(1e-10));
  CHECK(st::beta_quantile(2, 3, 0.95) == Approx(0.7513953742698181).epsilon(1e-10));
  CHECK(st::beta_quantile(120, 2, 0.05) == Approx(0.9613958452333438).epsilon(1e-10));
  CHECK(st::beta_quantile(31, 91, 0.95) == Approx(0.32092324850882525).epsilon(1e-10));
  CHECK(st::beta_quantile(1, 1, 0.5) == Approx(0.5).epsilon(1e-12));
}

TEST_CASE("incomplete_gamma and the Poisson tail match SciPy") {
  CHECK(st::incomplete_gamma(1, 0.5) == Approx(0.3934693402873665).epsilon(1e-13));
  CHECK(st::incomplete_gamma(3, 2.5) == Approx(0.45618688411667035).epsilon(1e-12));
  CHECK(st::incomplete_gamma(10, 0.04) == Approx(2.7864211031228174e-21).epsilon(1e-10));
  CHECK(st::incomplete_gamma(200, 150) == Approx(5.709688574208249e-05).epsilon(1e-9));
  CHECK(st::incomplete_gamma(0.5, 2) == Approx(0.9544997361036415).epsilon(1e-12));

  CHECK(st::poisson_upper_tail(3, 0.04) == Approx(1.0351730262003635e-05).epsilon(1e-10));
  CHECK(st::poisson_upper_tail(8, 0.5) == Approx(6.219690863728644e-08).epsilon(1e-10));
  CHECK(st::poisson_upper_tail(1, 2.0) == Approx(0.8646647167633873).epsilon(1e-12));
  CHECK(st::poisson_upper_tail(212, 10.0) == Approx(1.006312564739158e-195).epsilon(1e-8));
  CHECK(st::poisson_upper_tail(10, 10.0) == Approx(0.5420702855281478).epsilon(1e-11));
  CHECK(st::poisson_upper_tail(0, 3.0) == 1.0);
}

TEST_CASE("normal_cdf and log_add") {
  CHECK(st::normal_cdf(-13.8) == Approx(1.2742631455068554e-43).epsilon(1e-9));
  CHECK(st::normal_cdf(-3.07) == Approx(0.0010702938546789226).epsilon(1e-12));
  CHECK(st::normal_cdf(0) == 0.5);
  CHECK(st::normal_cdf(1.5) == Approx(0.9331927987311419).epsilon(1e-13));
  const double inf = std::numeric_limits<double>::infinity();
  CHECK(st::log_add(-inf, 2.0) == 2.0);
  CHECK(st::log_add(std::log(2.0), std::log(3.0)) == Approx(std::log(5.0)).epsilon(1e-14));
  CHECK(st::log_add(1000.0, 1000.0) == Approx(1000.0 + std::log(2.0)).epsilon(1e-14));
}

namespace {

// The same posterior by brute force: every path, weighted by its probability.
std::vector<double> brute_force(const st::Hmm2& model,
                                const std::vector<std::array<double, 2>>& emission) {
  const std::size_t n = emission.size();
  if (n == 0) {
    return {};
  }
  std::vector<double> mass1(n, 0.0);
  double total = 0.0;
  for (unsigned path = 0; path < (1U << n); ++path) {
    double log_p = 0.0;
    for (std::size_t t = 0; t < n; ++t) {
      const std::size_t s = (path >> t) & 1U;
      log_p += emission[t].at(s);
      if (t == 0) {
        log_p += model.initial.at(s);
      } else {
        const std::size_t previous = (path >> (t - 1)) & 1U;
        log_p += model.transition.at(previous).at(s);
      }
    }
    log_p += model.final.at((path >> (n - 1)) & 1U);
    const double p = std::exp(log_p);
    total += p;
    for (std::size_t t = 0; t < n; ++t) {
      if (((path >> t) & 1U) != 0) {
        mass1[t] += p;
      }
    }
  }
  for (double& m : mass1) {
    m /= total;
  }
  return mass1;
}

}  // namespace

TEST_CASE("forward-backward equals brute-force enumeration") {
  st::Hmm2 model;
  const double alpha = 0.01;
  const double beta = 0.2;
  model.initial = {std::log(1 - alpha), std::log(alpha)};
  model.transition = {
      {{std::log(1 - alpha), std::log(alpha)}, {std::log(beta), std::log(1 - beta)}}};
  model.final = {std::log(1 - alpha), std::log(beta)};
  // A run of misses on a series present 70% of the time; the outage state always
  // emits a miss.
  for (std::size_t n = 1; n <= 9; ++n) {
    CAPTURE(n);
    std::vector<std::array<double, 2>> emission(n, {std::log(0.3), 0.0});
    if (n > 3) {
      emission[2] = {std::log(0.7), std::log(1e-12)};  // a present day in the middle
    }
    const auto fast = st::posterior_state1(model, emission);
    const auto slow = brute_force(model, emission);
    REQUIRE(fast.size() == n);
    for (std::size_t t = 0; t < n; ++t) {
      CHECK(fast[t] == Approx(slow[t]).epsilon(1e-10));
    }
  }
  CHECK(st::posterior_state1(model, {}).empty());
}

// DE-01: weighted proportions and the paired comparison. Reference values from the
// formulas in tools/eval/README.md ("Intervals"), computed independently in Python.
TEST_CASE("wilson: the score interval, on an effective size") {
  const st::Interval eight = st::wilson(0.8, 10.0, st::kZTwoSided95);
  CHECK(eight.low == Approx(0.4901624691221323).epsilon(1e-12));
  CHECK(eight.high == Approx(0.9433178490622331).epsilon(1e-12));
  const st::Interval none = st::wilson(0.0, 5.0, st::kZTwoSided95);
  CHECK(none.low == 0.0);
  CHECK(none.high == Approx(0.43448246865939283).epsilon(1e-12));
  const st::Interval all = st::wilson(1.0, 5.0, st::kZTwoSided95);
  CHECK(all.low == Approx(0.5655175313406071).epsilon(1e-12));
  CHECK(all.high == 1.0);
  const st::Interval empty = st::wilson(0.5, 0.0, st::kZTwoSided95);
  CHECK(empty.low == 0.0);
  CHECK(empty.high == 1.0);
  // Weights 1, 1 and 4 on outcomes 1, 0 and 1: p = 5/6 on an effective size of 2.
  CHECK(st::effective_size(6.0, 18.0) == Approx(2.0));
  CHECK(st::effective_size(0.0, 0.0) == 0.0);
  const st::Interval weighted = st::wilson(5.0 / 6.0, 2.0, st::kZTwoSided95);
  CHECK(weighted.low == Approx(0.24078024007683524).epsilon(1e-12));
  CHECK(weighted.high == Approx(0.9874732425595976).epsilon(1e-12));
}

TEST_CASE("paired_difference_lower: Newcombe's method 10, one-sided") {
  const st::PairedSums s{20.0, 10.0, 2.0, 8.0, 40.0};
  CHECK(s.difference() == Approx(0.2));
  CHECK(st::paired_difference_lower(s, st::kZOneSided95) ==
        Approx(0.06479166431208222).epsilon(1e-10));
  // Both find every item: no difference, and none shown.
  const st::PairedSums tie{100.0, 0.0, 0.0, 0.0, 100.0};
  CHECK(tie.difference() == 0.0);
  CHECK(st::paired_difference_lower(tie, st::kZOneSided95) ==
        Approx(-0.02634273241732965).epsilon(1e-10));
  const st::PairedSums small{5.0, 3.0, 1.0, 1.0, 10.0};
  CHECK(st::paired_difference_lower(small, st::kZOneSided95) ==
        Approx(-0.11407546833538817).epsilon(1e-10));
  CHECK(st::paired_difference_lower(st::PairedSums{}, st::kZOneSided95) == 0.0);
}

TEST_CASE("mcnemar_one_sided: the exact binomial tail on discordant counts") {
  CHECK(st::mcnemar_one_sided(10, 2) == Approx(0.019287109375).epsilon(1e-12));
  CHECK(st::mcnemar_one_sided(3, 1) == Approx(0.3125).epsilon(1e-12));
  CHECK(st::mcnemar_one_sided(60, 40) == Approx(0.028443966820490395).epsilon(1e-10));
  CHECK(st::mcnemar_one_sided(0, 4) == 1.0);
  CHECK(st::mcnemar_one_sided(0, 0) == 1.0);
}
