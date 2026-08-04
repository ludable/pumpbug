// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <stdint.h>

namespace font_weight {

enum class Weight : uint8_t { Thin, Regular, Bold };

inline int round_div(int n, int d) {
  if (d <= 0) return 0;
  return (n + d / 2) / d;
}

inline int height_divisor_for(Weight weight) {
  switch (weight) {
    case Weight::Bold:
      return 8;
    case Weight::Thin:
      return 14;
    default:
      return 10;
  }
}

inline int natural_stroke_for_height(int target_px, Weight weight) {
  if (target_px < 1) target_px = 1;
  int stroke = round_div(target_px, height_divisor_for(weight));
  return stroke < 1 ? 1 : stroke;
}

}  // namespace font_weight
