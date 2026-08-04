// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#include "BleIcon.h"

#include <algorithm>

#include "ui/theme.h"

void BleIcon::drawLink(LGFX_Sprite& c, int x, int y, uint32_t color,
                       bool reversed, bool bold, int scale) {
  if (scale < 1) scale = 1;

  const int w = (bold ? LINK_BOLD_W : LINK_W) * scale;
  const int h = (bold ? LINK_BOLD_H : LINK_H) * scale;
  const int stroke = (bold ? STROKE_BOLD : STROKE) * scale;

  const int r_out = h / 2;
  // LGFX's fill_arc_helper treats the outer radius as inclusive (+1) and the
  // inner radius as exclusive (-1), so the actual ring thickness is
  // (r_out - r_in) + 1. Compensate so the arc matches the body bar thickness.
  const int r_in = std::max(0, r_out - stroke + 1);
  const int body_len = w - 2 * r_out;
  const int right_cx = x + r_out + body_len;  // == x + w - r_out

  if (!reversed) {
    // Left closed end: full semicircle (left half).
    c.fillArc(x + r_out, y + r_out, r_out, r_in, 90, 270, color);
    // Right open end: two separate arcs forming upper and lower jaws,
    // leaving a horizontal mouth pointing right. 0° is 3 o'clock and angles
    // increase clockwise. Open the outer (upper) jaw a bit wider (270-340)
    // and keep the inner (lower) jaw tighter (40-90) so the links interlock
    // cleanly without touching.
    c.fillArc(right_cx, y + r_out, r_out, r_in, 270, 340, color);
    c.fillArc(right_cx, y + r_out, r_out, r_in, 35, 90, color);
    // Top and bottom body bars, exactly `stroke` pixels thick.
    c.fillRect(x + r_out, y, body_len, stroke, color);
    c.fillRect(x + r_out, y + h - stroke + 1, body_len, stroke, color);
  } else {
    // Right closed end: full semicircle (right half).
    c.fillArc(right_cx, y + r_out, r_out, r_in, 270, 90, color);
    // Left open end: upper and lower jaws, mouth pointing left.
    // Mirror the right open end: upper jaw is outer/closed (200-270),
    // lower jaw is outer/open (90-160).
    c.fillArc(x + r_out, y + r_out, r_out, r_in, 215, 270, color);
    c.fillArc(x + r_out, y + r_out, r_out, r_in, 90, 160, color);
    // Top and bottom body bars.
    c.fillRect(x + r_out, y, body_len, stroke, color);
    c.fillRect(x + r_out, y + h - stroke + 1, body_len, stroke, color);
  }
}

void BleIcon::drawSlash(LGFX_Sprite& c, int x, int y, uint32_t color,
                        uint32_t bg, int scale) {
  if (scale < 1) scale = 1;
  const int iconW = ICON_W * scale;
  const int iconH = ICON_H * scale;
  const int cx = x + iconW / 2;
  const int incline = iconH / 6;
  const int band = 3 * scale;
  for (int i = -band; i <= band; ++i)
    c.drawLine(cx - incline + i, y + iconH, cx + incline + i, y, bg);
  c.drawLine(cx - incline, y + iconH - 3 * scale, cx + incline, y + 3 * scale,
             color);
}

BleIcon::BleIcon(int scale) : _scale(std::max(1, scale)) {}

bool BleIcon::poll(BleStatus status) {
  if (status != _status) {
    _status = status;
    return true;
  }
  if (status == BleStatus::Searching && _pulseDebounce()) {
    // Advance one pixel-at-scale per tick so the motion looks smooth at every
    // render size. The frame counter runs in unscaled units; draw() maps it
    // to the instance's fixed scale.
    ++_animFrame;
    _animFrame %= ANIM_RANGE * 2 * _scale;
    return true;
  }
  return false;
}

void BleIcon::draw(LGFX_Sprite& c, int x, int y, int w, int h,
                   uint32_t bg) const {
  const int scaledIconW = ICON_W * _scale;
  const int scaledIconH = ICON_H * _scale;
  drawAt(c, x + (w - scaledIconW) / 2, y + (h - scaledIconH) / 2, bg);
}

void BleIcon::drawAt(LGFX_Sprite& c, int x, int y, uint32_t bg) const {
  const int scale = _scale;
  int dx;
  uint32_t color = theme::fg();
  bool bold = false;
  bool slash = false;

  switch (_status) {
    case BleStatus::Connected:
      bold = true;
      color = theme::accent();
      dx = (GAP + OVERLAP) * scale;
      break;
    case BleStatus::Searching: {
      // _animFrame is a scaled phase: one tick advances one pixel at the
      // instance's render scale. Map it back to the current pixel offset.
      int phase = _animFrame;
      const int maxPhase = (OVERLAP + GAP) * scale;
      if (phase > maxPhase) phase = 2 * maxPhase - phase;
      dx = phase;
      break;
    }
    case BleStatus::Disconnected:
      color = theme::fg();
      slash = true;
      dx = GAP * scale;
      break;
    case BleStatus::Off:
    default:
      color = theme::fg();
      dx = GAP * scale;
      break;
  }

  const int w = (bold ? LINK_BOLD_W : LINK_W) * scale;
  const int h = (bold ? LINK_BOLD_H : LINK_H) * scale;
  const int gap = GAP * scale;
  const int iconW = 2 * (w + 1) + gap;
  const int iconH = ICON_H * scale;

  const int h2 = h * 3 / 2;
  const int x0 = dx;
  const int x1 = iconW - 1 - w - dx;
  const int y0 = (iconH - h2) / 2;
  const int y1 = y0 + h2 / 3;

  drawLink(c, x + x0, y + y0, color, false, bold, scale);
  drawLink(c, x + x1, y + y1, color, true, bold, scale);
  if (slash) drawSlash(c, x, y, color, bg, scale);
}
