#pragma once

#include <array>
#include <span>
#include <vector>

namespace dorq::stats {

// A two-state hidden Markov model, in log space.
//
// State 0 and state 1 (in coverage: a healthy feed, and an outage). `emission[t]`
// holds log P(observation t | state) for both states; `transition[r][s]` is
// log P(state s at t+1 | state r at t); `initial` is log P(state at t = 0) before
// the first observation; `final` weighs the last state by what follows the
// sequence (log P(what follows | last state)), zeros when nothing does.
struct Hmm2 {
  std::array<double, 2> initial{};
  std::array<std::array<double, 2>, 2> transition{};
  std::array<double, 2> final{};
};

// The posterior probability of state 1 at each step (forward-backward). Exact;
// O(n). Returns an empty vector for an empty sequence.
[[nodiscard]] std::vector<double> posterior_state1(const Hmm2& model,
                                                   std::span<const std::array<double, 2>> emission);

}  // namespace dorq::stats
