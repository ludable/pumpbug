// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <cstddef>
#include <cstdint>

namespace pump_scale::shot_store_detail {

// Shot filenames contain an eight-digit decimal ID.
inline constexpr size_t kShotIdDigits = 8;

constexpr uint32_t maxIdForDigits(size_t digits) {
  uint32_t value = 0;
  for (size_t i = 0; i < digits; ++i) value = value * 10 + 9;
  return value;
}

// Largest ID that fits in a shot filename.
inline constexpr uint32_t kMaxShotId = maxIdForDigits(kShotIdDigits);

// Reserves an ID greater than both the last ID stored in NVS and the greatest
// existing shot-file ID. The new ID is saved to NVS before it is returned. Zero
// means allocation or persistence failed and is never a valid shot ID.
uint32_t reserveNextShotId(uint32_t greatestExistingShotId);

}  // namespace pump_scale::shot_store_detail
