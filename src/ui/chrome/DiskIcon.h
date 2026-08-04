// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <M5Unified.h>

#include <algorithm>

#include "ui/theme.h"

namespace ui {

// Reusable storage glyph. Capacity polling is intentionally not part of the
// painter; callers choose the meaning and supply the state they already own.
inline void drawDiskIcon(LGFX_Sprite& c, int x, int y, int usedPct,
                         bool healthy, uint32_t background) {
  constexpr int iconW = 12;
  constexpr int iconH = 16;
  constexpr int notch = 5;
  const int fillX = x + 1;
  const int fillY = y + 1;
  const int fillW = iconW - 2;
  const int fillH = iconH - 2;
  usedPct = std::max(0, std::min(100, usedPct));

  // Paint the interior before the outline, then remove the upper-right
  // triangle so the capacity fill cannot square off the disk's chamfer.
  if (!healthy) {
    c.fillRect(fillX, fillY, fillW, fillH, theme::muted());
  } else {
    const uint32_t color = usedPct >= 90   ? theme::critical_fill()
                           : usedPct >= 70 ? theme::warn_fill()
                                           : theme::ok_fill();
    const int usedHeight = (usedPct * fillH) / 100;
    if (usedHeight < fillH) {
      c.fillRect(fillX, fillY, fillW, fillH - usedHeight, theme::muted());
    }
    if (usedHeight > 0) {
      c.fillRect(fillX, fillY + fillH - usedHeight, fillW, usedHeight, color);
    }
  }

  for (int i = 0; i < notch; ++i) {
    c.drawFastHLine(x + iconW - notch + i, y + i, notch - i, background);
  }
  c.drawFastVLine(x, y, iconH, theme::fg());
  c.drawFastVLine(x + iconW - 1, y + notch, iconH - notch, theme::fg());
  c.drawFastHLine(x, y + iconH - 1, iconW, theme::fg());
  c.drawFastHLine(x, y, iconW - notch, theme::fg());
  for (int i = 0; i < notch; ++i) {
    c.drawPixel(x + iconW - notch + i, y + i, theme::fg());
  }

  if (!healthy) {
    constexpr int badgeRadius = 5;
    const int badgeX = x + iconW - 2;
    const int badgeY = y + iconH - 3;
    c.fillCircle(badgeX, badgeY, badgeRadius, theme::critical_fill());
    c.drawLine(badgeX - 2, badgeY - 2, badgeX + 2, badgeY + 2,
               theme::critical_fill_fg());
    c.drawLine(badgeX + 2, badgeY - 2, badgeX - 2, badgeY + 2,
               theme::critical_fill_fg());
  }
}

}  // namespace ui
