#include "stats/hmm2.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <span>
#include <vector>

#include "stats/special.hpp"

namespace dorq::stats {

std::vector<double> posterior_state1(const Hmm2& model,
                                     std::span<const std::array<double, 2>> emission) {
  const std::size_t n = emission.size();
  if (n == 0) {
    return {};
  }
  std::vector<std::array<double, 2>> forward(n);
  std::vector<std::array<double, 2>> backward(n);
  for (std::size_t s = 0; s < 2; ++s) {
    forward[0].at(s) = model.initial.at(s) + emission[0].at(s);
  }
  for (std::size_t t = 1; t < n; ++t) {
    for (std::size_t s = 0; s < 2; ++s) {
      forward[t].at(s) = log_add(forward[t - 1][0] + model.transition[0].at(s),
                                 forward[t - 1][1] + model.transition[1].at(s)) +
                         emission[t].at(s);
    }
  }
  backward[n - 1] = model.final;
  for (std::size_t t = n - 1; t > 0; --t) {
    for (std::size_t r = 0; r < 2; ++r) {
      backward[t - 1].at(r) = log_add(model.transition.at(r)[0] + emission[t][0] + backward[t][0],
                                      model.transition.at(r)[1] + emission[t][1] + backward[t][1]);
    }
  }
  std::vector<double> out(n);
  for (std::size_t t = 0; t < n; ++t) {
    const double s0 = forward[t][0] + backward[t][0];
    const double s1 = forward[t][1] + backward[t][1];
    out[t] = std::exp(s1 - log_add(s0, s1));
  }
  return out;
}

}  // namespace dorq::stats
