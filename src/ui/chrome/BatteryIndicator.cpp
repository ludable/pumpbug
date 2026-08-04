// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#include "BatteryIndicator.h"

#include <M5Unified.h>

#include <algorithm>

#include "ui/theme.h"
#include "util/power.h"

// Set to 0, 1, 2 depending on level of detail
#define DEBUG_BATTERY_READING 0

namespace {

constexpr int kWarningPercent = 25;
constexpr int kCriticalPercent = 10;

}  // namespace

BatteryIndicator::BatteryIndicator() : _plug(&M5.Display) {}

void BatteryIndicator::buildPlug(int CENTER, int MIDDLE, int PLUG_R, int PLUG_H,
                                 int RECT_W, uint32_t bg) const {
  _plug.createSprite(ICON_W, ICON_H);
  _plug.fillScreen(bg);
  _plug.drawCircle(CENTER, MIDDLE, PLUG_R, theme::fg());
  _plug.drawRoundRect(CENTER, MIDDLE - PLUG_R, RECT_W, PLUG_H, 2, theme::fg());
  _plug.fillRect(CENTER, MIDDLE - PLUG_R + 1, RECT_W - 1, PLUG_H - 2, bg);
  _plug.drawFastHLine(0, MIDDLE, CENTER - PLUG_R, theme::fg());
  _plug.drawFastHLine(CENTER + RECT_W, MIDDLE - PLUG_R / 2,
                      ICON_W - (CENTER + RECT_W), theme::fg());
  _plug.drawFastHLine(CENTER + RECT_W, MIDDLE + PLUG_R / 2,
                      ICON_W - (CENTER + RECT_W), theme::fg());
  _plug.fillCircle(CENTER, MIDDLE, PLUG_R - 2, TFT_TRANSPARENT);
  _plug.fillRect(CENTER, MIDDLE - PLUG_R + 2, RECT_W - 2, PLUG_H - 4,
                 TFT_TRANSPARENT);
}

void BatteryIndicator::invalidate() {
  if (_plug.getBuffer()) {
    _plug.deleteSprite();
  }
  _plugBg = 0xFFFFFFFF;
}

bool BatteryIndicator::poll() {
  if (!_debounce()) return false;
  auto status = power::getBatteryStatus();
  if (status.percent == -1) return false;
  int pct = (_pct != -1) ? (0.5 * _pct + 0.5 * status.percent)
                         : status.percent;  // Smooth
  const bool plugged = status.hasExternalPower;
#if DEBUG_BATTERY_READING > 0
  M5_LOGD("BATT (pct=%i, ext_power=%d) => smooth_pct=%i", status.percent,
          status.hasExternalPower, pct);
#endif
  if (pct == _pct && plugged == _plugged) return false;
  _pct = pct;
  _plugged = plugged;
  return true;
}

void BatteryIndicator::draw(LGFX_Sprite& c, int sx, int sy, int sw, int sh,
                            uint32_t bg) const {
  const int x = sx + (sw - ICON_W) / 2, y = sy + (sh - ICON_H) / 2;
  const int BATT_PIP_W = 3, BATT_PIP_H = 4;
  const int BATT_W = ICON_W - BATT_PIP_W;
  const int PLUG_R = ICON_H / 2 - 1;
  const int PLUG_W = static_cast<int>(PLUG_R * 2.5);
  const int pct = std::clamp(_pct, 0, 100);

  const int MAX_FILL_W = _plugged ? PLUG_W - 4 : BATT_W - 4;
  const int fillW = map(pct, 0, 100, 0, MAX_FILL_W);
  const uint32_t fillColor = (pct <= kCriticalPercent)  ? theme::critical_fill()
                             : (pct <= kWarningPercent) ? theme::warn_fill()
                                                        : theme::ok_fill();

  if (_plugged) {
    const int CENTER = BATT_W / 2, MIDDLE = ICON_H / 2;
    const int PLUG_H = PLUG_R * 2 + 1, RECT_W = static_cast<int>(PLUG_R * 1.5);
    if (!_plug.getBuffer() || _plugBg != bg) {
      buildPlug(CENTER, MIDDLE, PLUG_R, PLUG_H, RECT_W, bg);
      _plugBg = bg;
    }
    c.fillRect(x + CENTER - PLUG_R + 2, y + MIDDLE - PLUG_R + 2, PLUG_W - 4,
               PLUG_H - 4, bg);
    if (fillW > 0) {
      c.fillRect(x + CENTER - PLUG_R + 2, y + MIDDLE - PLUG_R + 2, fillW,
                 PLUG_H - 4, fillColor);
    }
    _plug.pushSprite(&c, x, y, TFT_TRANSPARENT);
  } else {
    c.drawRoundRect(x, y + 1, BATT_W, ICON_H - 2, 2, theme::fg());
    c.fillRect(x + BATT_W, y + (ICON_H - BATT_PIP_H) / 2, BATT_PIP_W,
               BATT_PIP_H, theme::fg());
    c.fillRect(x + 2, y + 3, BATT_W - 4, ICON_H - 6, bg);
    if (fillW > 0) {
      c.fillRect(x + 2, y + 3, fillW, ICON_H - 6, fillColor);
    }
  }
}
