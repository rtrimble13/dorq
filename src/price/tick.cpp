#include "price/tick.hpp"

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

}  // namespace dorq
