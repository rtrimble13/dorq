#include "price/tick.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace dorq {

double tick_size(Date date, double price) noexcept {
  static const Date kSixteenths = Date::from_ymd(1997, 6, 24);
  static const Date kDecimal = Date::from_ymd(2001, 4, 9);
  if (date < kSixteenths) {
    return 0.125;
  }
  if (date < kDecimal) {
    return 0.0625;
  }
  return price < 1.0 ? 0.0001 : 0.01;
}

double written_grid(std::vector<std::uint8_t> decimals) {
  if (decimals.empty()) {
    return 1.0;
  }
  const auto at = decimals.begin() + static_cast<std::ptrdiff_t>(decimals.size() * 9 / 10);
  std::nth_element(decimals.begin(), at, decimals.end());
  return std::pow(10.0, -static_cast<double>(*at));
}

}  // namespace dorq
