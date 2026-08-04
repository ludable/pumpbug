// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <cstdint>

namespace ble {

// Controls one finite connect-mode scan and the delay before the next scan.
struct ScanPolicy {
  uint16_t intervalMs;
  uint16_t windowMs;
  uint16_t durationMs;
  uint16_t pauseMs;

  constexpr bool operator==(const ScanPolicy& other) const {
    return intervalMs == other.intervalMs && windowMs == other.windowMs &&
           durationMs == other.durationMs && pauseMs == other.pauseMs;
  }
};

constexpr bool isValidScanPolicy(const ScanPolicy& policy) {
  return policy.intervalMs != 0 && policy.windowMs != 0 &&
         policy.durationMs != 0 && policy.windowMs <= policy.intervalMs;
}

// This is a 15% scan duty cycle: listen for 75 ms every 500 ms. Higher duty
// cycle costs more battery; lower duty cycle can take longer to reconnect. The
// Acaia Lunar was measured with the BLE scan diagnostic at roughly a 25 ms
// advertising interval. A 75 ms window should usually catch 2-3 adverts; in the
// common case, worst-phase discovery is about the 425 ms idle gap plus one
// advertising interval.
inline constexpr ScanPolicy kDefaultScanPolicy{500, 75, 10000, 0};
static_assert(isValidScanPolicy(kDefaultScanPolicy));

}  // namespace ble
