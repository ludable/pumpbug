// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <limits>

#include "ble/BleStartupRetryPolicy.h"

int main() {
  ble::BleStartupRetryPolicy policy;

  assert(!policy.shouldAttempt(false, false, 100));
  assert(policy.shouldAttempt(true, false, 100));
  assert(!policy.shouldAttempt(true, false, 5099));
  assert(policy.shouldAttempt(true, false, 5100));

  assert(!policy.shouldAttempt(true, true, 5200));
  assert(policy.shouldAttempt(true, false, 5201));

  ble::BleStartupRetryPolicy wrappingPolicy;
  const uint32_t nearWrap = std::numeric_limits<uint32_t>::max() - 1000;
  assert(wrappingPolicy.shouldAttempt(true, false, nearWrap));
  assert(!wrappingPolicy.shouldAttempt(true, false, 3998));
  assert(wrappingPolicy.shouldAttempt(true, false, 3999));

  std::puts("OK: all assertions passed");
  return 0;
}
