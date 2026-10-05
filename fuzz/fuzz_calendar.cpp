// libFuzzer target: arbitrary bytes as a reference calendar file. A bad file must
// be a CalendarError and nothing else; a good one must apply cleanly.
#include <cstddef>
#include <cstdint>
#include <sstream>
#include <string>

#include "dorq/calendar.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  if (size == 0) {
    return 0;
  }
  // The first byte decides whether an exchange is asked for.
  const std::string exchange = (data[0] & 1U) != 0 ? "NYSE" : "";
  std::istringstream in(std::string(reinterpret_cast<const char*>(data + 1), size - 1));
  try {
    const dorq::ReferenceCalendar reference = dorq::read_reference_calendar(in, "fuzz", exchange);
    dorq::Calendar calendar;
    calendar.apply_reference(reference);
    static_cast<void>(calendar.sessions_between(reference.first, reference.last));
  } catch (const dorq::CalendarError&) {
  }
  return 0;
}
