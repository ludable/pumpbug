// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <M5Unified.h>

#include "ChromeContent.h"
#include "util/debounce.h"

// Stand-alone BLE link-state icon: two interlocking chain links that overlap
// when connected, pull apart when disconnected/off, and animate toward/from
// overlap while searching.
//
// The class is reusable outside the chrome strip: callers instantiate it,
// call poll() each frame, and draw() where they want it. Animation state is
// owned here so multiple instances animate independently.
class BleIcon {
 public:
  // `scale` is an integer render multiplier for the base design. It is fixed
  // for the lifetime of the instance.
  explicit BleIcon(int scale = 1);

  // Store the current status and advance the searching animation. The
  // animation advances one pixel-at-scale per tick so motion stays smooth
  // regardless of size. Returns true when the icon needs to be redrawn (status
  // change or animation tick).
  bool poll(BleStatus status);

  // Draw the icon centered in the rectangle (x, y, w, h). `bg` is the color
  // used to clear the slash band in the disconnected state.
  void draw(LGFX_Sprite& c, int x, int y, int w, int h, uint32_t bg) const;

  // Direct draw at an explicit top-left pixel position.
  void drawAt(LGFX_Sprite& c, int x, int y, uint32_t bg) const;

 private:
  // Base design (scale = 1). All drawing is derived from these constants.
  static constexpr int LINK_W = 12;
  static constexpr int LINK_H = 6;        // must be even
  static constexpr int LINK_BOLD_W = 14;  // larger link for the active state
  static constexpr int LINK_BOLD_H = 8;   // must be even
  static constexpr int STROKE = 1;        // regular link line thickness
  static constexpr int STROKE_BOLD = 2;   // bold link line thickness
  static constexpr int OVERLAP = 4;       // px the rings overlap when linked
  static constexpr int GAP = 1;           // margin/spacing at full spread
  static constexpr int ANIM_RANGE = OVERLAP + GAP;
  // A link's right cap renders to its nominal x+w, so the inclusive footprint
  // is w+1 pixels. The icon packs two footprints plus the inter-link gap.
  static constexpr int ICON_W = 2 * (LINK_W + 1) + GAP;
  static constexpr int ICON_H = 16;

  static void drawLink(LGFX_Sprite& c, int x, int y, uint32_t color,
                       bool reversed, bool bold, int scale);
  static void drawSlash(LGFX_Sprite& c, int x, int y, uint32_t color,
                        uint32_t bg, int scale);

  Debounce<100> _pulseDebounce;  // searching animation ~10 FPS
  BleStatus _status = BleStatus::Off;
  int _animFrame = 0;
  int _scale = 1;
};
