// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

constexpr uint16_t encode_rgb565(uint8_t r, uint8_t g, uint8_t b) {
  // R: 5 bits (drop 3 LSBs) -> bits 15-11
  // G: 6 bits (drop 2 LSBs) -> bits 10-5
  // B: 5 bits (drop 3 LSBs) -> bits 4-0
  return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
}
