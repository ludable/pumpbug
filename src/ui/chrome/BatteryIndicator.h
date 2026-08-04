// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <M5Unified.h>

#include "util/debounce.h"

// Battery glyph painter. Reads and smooths the power/battery state, then
// draws a centered battery icon into a caller-supplied rectangle. The charging
// plug variant caches a palette-aware sprite; callers must call invalidate()
// when the active theme changes.
class BatteryIndicator {
 public:
  BatteryIndicator();

  // Poll the battery status. Returns true if the smoothed level or plugged
  // state changed and the indicator should be redrawn.
  bool poll();

  // Draw the battery glyph centered in (x, y, w, h). `bg` is used as the
  // background color for the charging-plug sprite so the glyph blends with
  // the surrounding surface.
  void draw(LGFX_Sprite& c, int x, int y, int w, int h, uint32_t bg) const;

  // Drop cached palette-dependent glyphs so the next draw rebuilds with the
  // current theme colors.
  void invalidate();

 private:
  static constexpr int ICON_W = 24;
  static constexpr int ICON_H = 16;

  Debounce<500> _debounce;
  mutable LGFX_Sprite _plug;
  mutable uint32_t _plugBg = 0xFFFFFFFF;  // sentinel: rebuild on first draw
  int _pct = -1;
  bool _plugged = false;

  void buildPlug(int CENTER, int MIDDLE, int PLUG_R, int PLUG_H, int RECT_W,
                 uint32_t bg) const;
};
