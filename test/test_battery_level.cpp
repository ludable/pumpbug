// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#include <cassert>
#include <cstdio>

#include "power/battery_level.h"

int main() {
  using power::estimateStickS3BatteryPercent;

  assert(estimateStickS3BatteryPercent(3400) == 0);
  assert(estimateStickS3BatteryPercent(3450) == 0);
  assert(estimateStickS3BatteryPercent(3530) == 10);
  assert(estimateStickS3BatteryPercent(3694) == 50);
  assert(estimateStickS3BatteryPercent(4088) == 100);
  assert(estimateStickS3BatteryPercent(4200) == 100);

  assert(estimateStickS3BatteryPercent(3544) == 15);
  assert(estimateStickS3BatteryPercent(3574) == 25);
  assert(estimateStickS3BatteryPercent(3726) == 55);

  int previous = estimateStickS3BatteryPercent(2800);
  for (int voltageMv = 2801; voltageMv <= 4300; ++voltageMv) {
    const int current = estimateStickS3BatteryPercent(voltageMv);
    assert(current >= previous);
    assert(current >= 0 && current <= 100);
    previous = current;
  }

  std::puts("OK: all assertions passed");
  return 0;
}
