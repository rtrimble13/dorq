#pragma once

#include "dorq/date.hpp"

namespace dorq {

// The minimum price increment US equities traded in on `date` at `price`: 1/8
// until June 1997, 1/16 until decimalization (completed 2001-04-09), then a cent,
// or 0.0001 for prices under a dollar (SEC Rule 612). `--meta` will be able to
// override it per series (M5).
[[nodiscard]] double tick_size(Date date, double price) noexcept;

}  // namespace dorq
