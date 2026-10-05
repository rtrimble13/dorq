#pragma once

#include <cstdint>

namespace dorq {

// The process exit status. This is a contract with every script and job that runs
// dorq (fafnir's nightly DQ run among them), so the values never change. A new
// outcome gets a new number. A process status is one byte, hence the base type.
enum class ExitCode : std::uint8_t {
  kOk = 0,          // ran, and found nothing at or above --fail-on
  kViolations = 1,  // ran, and found violations at or above --fail-on
  kUsage = 2,       // bad command line or configuration; nothing was checked
  kInput = 3,       // the input could not be read or parsed
};

[[nodiscard]] constexpr int to_int(ExitCode code) noexcept { return static_cast<int>(code); }

}  // namespace dorq
