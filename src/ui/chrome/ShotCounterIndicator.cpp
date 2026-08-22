// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#include "ShotCounterIndicator.h"

#include <algorithm>
#include <cstdio>

#include "ui/fonts.h"
#include "ui/layout.h"
#include "ui/theme.h"

ShotCounterIndicator::ShotCounterIndicator() : _cup(&M5.Display) {}

bool ShotCounterIndicator::poll(uint64_t count) {
  if (_count == count) return false;
  _count = count;
  return true;
}

void ShotCounterIndicator::invalidate() {
  if (_cup.getBuffer()) _cup.deleteSprite();
  _cupBg = 0xFFFFFFFF;
  _cupFg = 0xFFFFFFFF;
}

void ShotCounterIndicator::draw(LGFX_Sprite& c, int sx, int sy, int sw,
                                int sh) {
  char label[5];
  if (_count > MAX_DISPLAY_COUNT) {
    std::snprintf(label, sizeof(label), "999+");
  } else {
    std::snprintf(label, sizeof(label), "%u", static_cast<unsigned>(_count));
  }

  const bool horizontal = sw >= sh;
  if (horizontal) {
    constexpr int edgePad = 4;
    constexpr int gap = 2;
    const int cupX = sx + edgePad;
    drawCup(c, cupX + 1, sy + (sh - CUP_H) / 2 + 1);
    const layout::rect labelBox = {cupX + CUP_W + gap, sy + 2,
                                   sx + sw - 2 - (cupX + CUP_W + gap), sh - 4};
    const int labelH = std::min(labelBox.h, 24);
    const layout::rect fittedLabelBox = {
        labelBox.x, labelBox.y + (labelBox.h - labelH) / 2, labelBox.w, labelH};
    drawCount(c, label, fittedLabelBox);
  } else {
    constexpr int gap = 2;
    constexpr int labelMaxH = 24;
    constexpr int edgePad = 4;
    const int cupX = sx + (sw - CUP_W) / 2;
    const int cupY = sy + edgePad;
    drawCup(c, cupX + 1, cupY + 1);
    const layout::rect labelBox = {sx + 2, cupY + CUP_H + gap, sw - 4,
                                   sy + sh - 2 - (cupY + CUP_H + gap)};
    const int labelH = std::min(labelBox.h, labelMaxH);
    const layout::rect fittedLabelBox = {
        labelBox.x, labelBox.y + (labelBox.h - labelH) / 2, labelBox.w, labelH};
    drawCount(c, label, fittedLabelBox);
  }
}

void ShotCounterIndicator::drawCount(LGFX_Sprite& c, const char* label,
                                     const layout::rect& box) {
  c.setTextColor(theme::fg(), theme::bg_alt());
  layout::drawCenteredInBox(&c, label, box, font::textFamily());
  c.setTextSize(1);
}

void ShotCounterIndicator::buildCupMask(uint32_t bg, uint32_t fg) {
  _cup.createSprite(CUP_W, CUP_H);
  _cup.fillScreen(bg);

  // Clip the handle at the bowl wall so only its external semicircle remains.
  // Starting the rounded rectangles above the sprite leaves the cup open
  // while preserving a curved lower edge.
  _cup.drawCircle(15, 6, 3, fg);
  _cup.fillCircle(15, 6, 1, TFT_TRANSPARENT);
  _cup.fillRect(0, 0, 14, CUP_H, bg);
  _cup.drawRoundRect(1, -5, 14, 20, 5, fg);
  _cup.fillRoundRect(3, -3, 10, 16, 3, TFT_TRANSPARENT);
  _cupBg = bg;
  _cupFg = fg;
}

void ShotCounterIndicator::drawCup(LGFX_Sprite& c, int x, int y) {
  constexpr uint32_t coffeeLight = 0xA85C2D;
  constexpr uint32_t coffeeDark = 0x663019;
  c.fillGradientRect(x + 3, y + 1, 10, 13, coffeeLight, coffeeDark,
                     lgfx::VLINEAR);

  const uint32_t bg = theme::bg_alt();
  const uint32_t fg = theme::fg();
  if (!_cup.getBuffer() || _cupBg != bg || _cupFg != fg) {
    invalidate();
    buildCupMask(bg, fg);
  }
  _cup.pushSprite(&c, x, y, TFT_TRANSPARENT);
}
