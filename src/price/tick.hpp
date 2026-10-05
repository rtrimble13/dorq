#pragma once

#include <cstdint>
#include <vector>

#include "dorq/date.hpp"

namespace dorq {

// The minimum price increment US equities traded in on `date` at `price`: 1/8
// until June 1997, 1/16 until decimalization (completed 2001-04-09), then a cent,
// or 0.0001 for prices under a dollar (SEC Rule 612). `--meta` will be able to
// override it per series (M5).
[[nodiscard]] double tick_size(Date date, double price) noexcept;

// The grid most prices are written on: 10^-d, where d is the 90th percentile of
// the decimals the closes were written with (a sub-dime stock quoted in cents is
// on a 0.01 grid whatever the exchange allows). 1 when `decimals` is empty.
[[nodiscard]] double written_grid(std::vector<std::uint8_t> decimals);

}  // namespace dorq
