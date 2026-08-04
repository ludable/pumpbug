// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <M5Unified.h>

#include "ui/blocks.h"
#include "ui/fonts.h"
#include "ui/layout.h"
#include "ui/theme.h"

namespace ui {

// Neutral confirmation for consequential but non-destructive choices. Button
// affordances remain the responsibility of the owning Screen.
inline void drawConfirmationScreen(LGFX_Sprite* c, const char* title,
                                   const char* body) {
  c->fillScreen(theme::bg());
  drawGuideCard(c, title, theme::accent(), nullptr, body);
}

// Critical notice or confirmation with a red heading band.
inline void drawCriticalMessageScreen(LGFX_Sprite* c, const char* title,
                                      const char* body) {
  const int w = c->width();
  const int h = c->height();
  constexpr int margin = 4;
  constexpr int headingH = 30;
  const int boxW = w - 2 * margin;

  c->fillScreen(theme::bg());
  c->fillRect(0, 0, w, headingH, theme::critical_fill());
  c->setTextColor(theme::critical_fill_fg(), theme::critical_fill());
  layout::drawCenteredInBox(c, title, margin, 0, boxW, headingH,
                            font::textFamily());

  c->setTextColor(theme::dim(), theme::bg());
  layout::drawWrappedCentered(c, body, margin, headingH, boxW, h - headingH,
                              font::body());
}

}  // namespace ui
