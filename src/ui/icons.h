// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <M5Unified.h>

#include <algorithm>

#include "net/NetworkStatus.h"
#include "ui/layout.h"
#include "ui/theme.h"

// Status glyphs shared between the chrome strip and full screens, drawn at
// whatever size the caller's box allows.
namespace ui {

// Wi-Fi signal fan: a dot with three arcs, colored by status (Failed adds a
// slash). `activeBands` limits how many arcs light up — the chrome
// indicator animates Connecting by sweeping it. `bg` is the surface the
// icon sits on; the Failed slash clears through the arcs with it.
//
// The glyph is designed on a 24x20 grid and scales to fit the box,
// centered. Pass a 24x20 box for the original chrome rendering.
inline void drawWifiIcon(LGFX_Sprite* c, layout::rect box, NetworkStatus status,
                         uint32_t bg, int activeBands = 3) {
  const float s = std::min(box.w / 24.0f, box.h / 20.0f);
  if (s <= 0) return;
  const int w = static_cast<int>(24 * s);
  const int h = static_cast<int>(20 * s);
  const int x = box.x + (box.w - w) / 2;
  const int y = box.y + (box.h - h) / 2;
  const int cx = x + w / 2;
  const int cy = y + h - std::max(1, static_cast<int>(2 * s));

  uint32_t base;
  switch (status) {
    case NetworkStatus::Connected:
    case NetworkStatus::Ap:
      base = theme::accent();
      break;
    case NetworkStatus::Connecting:
      base = theme::fg();
      break;
    case NetworkStatus::Failed:
      base = theme::warn();
      break;
    case NetworkStatus::Off:
    default:
      base = theme::muted();
      break;
  }

  c->fillCircle(cx, cy, std::max(1, static_cast<int>(s)), base);
  static constexpr int kOuter[3] = {5, 11, 17};
  const int thickness = std::max(2, static_cast<int>(2 * s));
  for (int b = 0; b < 3; ++b) {
    const uint32_t col = (b < activeBands) ? base : theme::muted();
    const int outer = std::max(2, static_cast<int>(kOuter[b] * s));
    c->fillArc(cx, cy, outer, std::max(1, outer - thickness), 230, 310, col);
  }

  if (status == NetworkStatus::Failed) {
    // Slash across the bands, clearing a band around it so it reads as cutting
    // through the signal.
    const int incline = w / 6;
    for (int i = -3; i <= 3; ++i)
      c->drawLine(cx - incline + i, y + h, cx + incline + i, y, bg);
    c->drawLine(cx - incline, y + h - 3, cx + incline, y + 3, theme::warn());
  }
}

// Connecting animation for the Wi-Fi glyph: a triangle wave over its three
// bands (1→2→3→2→…, fill up then back down). Owners call advance() from
// their tick/poll and repaint when it returns true; bands() feeds
// drawWifiIcon's activeBands.
struct ConnectingSweep {
  static constexpr uint32_t kPeriodMs = 250;  // ~4 FPS sweep
  static constexpr int kPeriod = 4;           // 2*(bands-1)

  void reset() {
    _phase = 0;
    _lastMs = 0;
  }
  bool advance(uint32_t nowMs) {
    if (nowMs - _lastMs < kPeriodMs) return false;
    _lastMs = nowMs;
    _phase = (_phase + 1) % kPeriod;
    return true;
  }
  int bands() const { return 1 + (_phase <= 2 ? _phase : kPeriod - _phase); }

 private:
  uint8_t _phase = 0;
  uint32_t _lastMs = 0;
};

inline void drawTargetIcon(LGFX_Sprite* canvas, layout::rect box,
                           uint32_t color) {
  const int diameter = std::min(box.w, box.h);
  if (diameter <= 0) return;
  if (diameter < 3) {
    canvas->drawPixel(box.x + box.w / 2, box.y + box.h / 2, color);
    return;
  }
  const int radius = (diameter - 1) / 2;
  const int cx = box.x + box.w / 2;
  const int cy = box.y + box.h / 2;
  const int outerRingInnerRadius = radius >= 4 ? radius - 1 : radius;
  canvas->fillArc(cx, cy, radius, outerRingInnerRadius, 0, 360, color);
  if (radius >= 4) {
    const int innerRadius = std::max(1, (radius + 1) / 2);
    const int innerRingInnerRadius =
        innerRadius >= 3 ? innerRadius - 1 : innerRadius;
    canvas->fillArc(cx, cy, innerRadius, innerRingInnerRadius, 0, 360, color);
  }
  if (radius >= 6) {
    canvas->fillCircle(cx, cy, 1, color);
  } else {
    canvas->drawPixel(cx, cy, color);
  }
}
}  // namespace ui
