// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#include "StatusIcons.h"

#include <M5Unified.h>

#include "BatteryIndicator.h"
#include "ShotCounterIndicator.h"
#include "ui/icons.h"
#include "ui/theme.h"

// Each indicator captures its current value in poll() and paints a supplied
// tile in draw(). StatusIcons repaints all three tiles when any value changes.

// Wi-Fi is drawn as an origin dot and three nested bands. Connecting animates
// the bands; connected and access-point modes use distinct accent colors; a
// failed station connection adds the warning slash in drawWifiIcon().
class WifiIndicator {
  static constexpr int ICON_W = 24;
  static constexpr int ICON_H = 20;

  ui::ConnectingSweep _sweep;
  NetworkStatus _status = NetworkStatus::Off;

 public:
  bool poll(NetworkStatus status) {
    if (status != _status) {
      _status = status;
      return true;
    }
    return status == NetworkStatus::Connecting && _sweep.advance(millis());
  }

  void draw(LGFX_Sprite& c, int sx, int sy, int sw, int sh) {
    const int activeBands =
        _status == NetworkStatus::Connecting ? _sweep.bands() : 3;
    ui::drawWifiIcon(
        &c, {sx + (sw - ICON_W) / 2, sy + (sh - ICON_H) / 2, ICON_W, ICON_H},
        _status, theme::bg_alt(), activeBands);
  }
};

namespace {
BatteryIndicator batteryIndicator;
ShotCounterIndicator shotCounterIndicator;
WifiIndicator wifiIndicator;
constexpr int UNIT_COUNT = 4;
constexpr int SLOT_GAP = 2;
}  // namespace

bool StatusIcons::poll() {
  // Poll every indicator; short-circuiting would skip a later animation tick.
  const bool battery = batteryIndicator.poll();
  const bool counter = shotCounterIndicator.poll(_shotCount);
  const bool wifi = wifiIndicator.poll(_networkStatus);
  return battery || counter || wifi;
}

void StatusIcons::invalidate() {
  batteryIndicator.invalidate();
  shotCounterIndicator.invalidate();
}

void StatusIcons::draw(LGFX_Sprite& bar, ChromeEdge edge) {
  const bool horizontal = edge == ChromeEdge::Bottom || edge == ChromeEdge::Top;
  const int thickness = horizontal ? bar.height() : bar.width();
  const int length = horizontal ? bar.width() : bar.height();

  auto drawTile = [&](int firstUnit, int unitSpan, auto&& painter) {
    const int start = (length * firstUnit) / UNIT_COUNT;
    const int end = (length * (firstUnit + unitSpan)) / UNIT_COUNT;
    const int span = end - start - SLOT_GAP;
    const int sx = horizontal ? start : 0;
    const int sy = horizontal ? 0 : start;
    const int sw = horizontal ? span : thickness;
    const int sh = horizontal ? thickness : span;
    bar.fillRoundRect(sx, sy, sw, sh, 1, theme::bg_alt());
    bar.setClipRect(sx, sy, sw, sh);
    painter(sx, sy, sw, sh);
    bar.clearClipRect();
  };

  drawTile(0, 1, [&](int x, int y, int w, int h) {
    batteryIndicator.draw(bar, x, y, w, h, theme::bg_alt());
  });
  drawTile(1, 2, [&](int x, int y, int w, int h) {
    shotCounterIndicator.draw(bar, x, y, w, h);
  });
  drawTile(3, 1, [&](int x, int y, int w, int h) {
    wifiIndicator.draw(bar, x, y, w, h);
  });
}
