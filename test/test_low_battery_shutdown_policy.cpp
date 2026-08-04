// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#include <cassert>
#include <cstdio>

#include "power/LowBatteryShutdownPolicy.h"

namespace {

using Decision = power::LowBatteryShutdownPolicy::Decision;

void testRequiresSustainedLowVoltage() {
  power::LowBatteryShutdownPolicy policy;

  assert(policy.update(3440, false, 1000) == Decision::None);
  assert(policy.update(3480, false, 10000) == Decision::None);
  assert(policy.update(3440, false, 15999) == Decision::None);
  assert(policy.update(3440, false, 16000) == Decision::Shutdown);
  assert(policy.update(3440, false, 20000) == Decision::None);
}

void testRecoveryCancelsPendingShutdown() {
  power::LowBatteryShutdownPolicy policy;

  assert(policy.update(3440, false, 1000) == Decision::None);
  assert(policy.update(3500, false, 10000) == Decision::None);
  assert(policy.update(3440, false, 20000) == Decision::None);
  assert(policy.update(3440, false, 34999) == Decision::None);
  assert(policy.update(3440, false, 35000) == Decision::Shutdown);
}

void testExternalPowerCancelsConfirmation() {
  power::LowBatteryShutdownPolicy policy;

  assert(policy.update(3440, false, 1000) == Decision::None);
  assert(policy.update(3440, true, 10000) == Decision::None);
  assert(policy.update(3440, false, 20000) == Decision::None);
  assert(policy.update(3440, false, 34999) == Decision::None);
  assert(policy.update(3440, false, 35000) == Decision::Shutdown);
}

void testInvalidReadingPreservesConfirmation() {
  power::LowBatteryShutdownPolicy policy;

  assert(policy.update(3440, false, 1000) == Decision::None);
  assert(policy.update(-1, false, 10000) == Decision::None);
  assert(policy.confirmationActive());
  assert(policy.update(3440, false, 15999) == Decision::None);
  assert(policy.update(3440, false, 16000) == Decision::Shutdown);
}

}  // namespace

int main() {
  testRequiresSustainedLowVoltage();
  testRecoveryCancelsPendingShutdown();
  testExternalPowerCancelsConfirmation();
  testInvalidReadingPreservesConfirmation();
  std::puts("OK: all assertions passed");
  return 0;
}
