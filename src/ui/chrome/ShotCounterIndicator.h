// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <M5Unified.h>

#include <cstdint>

#include "ui/layout.h"

// Two-unit chrome painter for the persistent shot count. It owns the display
// cap and the palette-sensitive cup sprite because both are presentation
// details rather than properties of the persisted counter.
class ShotCounterIndicator {
 public:
  ShotCounterIndicator();

  bool poll(uint64_t count);
  void invalidate();
  void draw(LGFX_Sprite& c, int sx, int sy, int sw, int sh);

 private:
  static constexpr int CUP_W = 20;
  static constexpr int CUP_H = 15;
  static constexpr uint16_t MAX_DISPLAY_COUNT = 999;

  static void drawCount(LGFX_Sprite& c, const char* label,
                        const layout::rect& box);
  void buildCupMask(uint32_t bg, uint32_t fg);
  void drawCup(LGFX_Sprite& c, int x, int y);

  LGFX_Sprite _cup;
  uint32_t _cupBg = 0xFFFFFFFF;
  uint32_t _cupFg = 0xFFFFFFFF;
  uint64_t _count = 0;
};
