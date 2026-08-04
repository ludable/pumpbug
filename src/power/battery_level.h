// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <array>
#include <cstddef>

#include "power/LowBatteryShutdownPolicy.h"

namespace power {

struct BatteryLevelPoint {
  int voltageMv;
  int percent;
};

// Estimates remaining usable battery for the StickS3 by interpolating its
// measured discharge under the normal connected, dimmed workload. Zero
// represents entry into orderly-shutdown confirmation rather than the cell's
// remaining chemical capacity.
// Terminal voltage also varies with load, temperature, cell age, and battery
// replacement, so callers should smooth the estimate before presenting it.
inline int estimateStickS3BatteryPercent(int voltageMv) {
  static constexpr std::array<BatteryLevelPoint, 11> kDischargeCurve{{
      {kLowBatteryConfirmationVoltageMv, 0},
      {3530, 10},
      {3558, 20},
      {3590, 30},
      {3638, 40},
      {3694, 50},
      {3758, 60},
      {3822, 70},
      {3892, 80},
      {3964, 90},
      {4088, 100},
  }};

  if (voltageMv <= kDischargeCurve.front().voltageMv) return 0;
  if (voltageMv >= kDischargeCurve.back().voltageMv) return 100;

  for (std::size_t i = 1; i < kDischargeCurve.size(); ++i) {
    const BatteryLevelPoint upper = kDischargeCurve[i];
    if (voltageMv > upper.voltageMv) continue;
    const BatteryLevelPoint lower = kDischargeCurve[i - 1];
    return lower.percent + (voltageMv - lower.voltageMv) *
                               (upper.percent - lower.percent) /
                               (upper.voltageMv - lower.voltageMv);
  }
  return 100;
}

}  // namespace power
