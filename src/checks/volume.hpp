#pragma once

#include <vector>

#include "checks/check.hpp"

namespace dorq {

// DQ4xx: volume. See doc/checks/DQ401.md, DQ402.md and DQ403.md.

// DQ401: the volume level steps by a clean ratio (a change of units, volume
// adjusted for a split) with no price change to explain it.
class VolumeScaleShift final : public Check {
 public:
  [[nodiscard]] const CheckInfo& info() const noexcept override;
  void run(const SeriesContext& context, std::vector<Violation>& out) const override;
};

// DQ402: info, extreme volume on a day the price did not move.
class VolumeSpikeNoMove final : public Check {
 public:
  [[nodiscard]] const CheckInfo& info() const noexcept override;
  void run(const SeriesContext& context, std::vector<Violation>& out) const override;
};

// DQ403: a price change on a bar with no volume, on a series where that is rare.
class MoveOnZeroVolume final : public Check {
 public:
  [[nodiscard]] const CheckInfo& info() const noexcept override;
  void run(const SeriesContext& context, std::vector<Violation>& out) const override;
};

}  // namespace dorq
