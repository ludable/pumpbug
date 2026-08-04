// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#include "BleScanScreen.h"

#include <algorithm>
#include <cstdio>

#include "ui/blocks.h"
#include "ui/fonts.h"
#include "ui/layout.h"
#include "ui/theme.h"

void BleScanScreen::onEnter() {
  bleScale.startDiagScan(LEASE_MS);
  _last = BleScaleService::ScanResults{};
  requestDraw();
}

void BleScanScreen::onExit() { bleScale.stopDiagScan(); }

void BleScanScreen::onLayoutChanged() { requestDraw(); }

ScreenResult BleScanScreen::tick() {
  // Renew the lease so the scan keeps running, and refresh the view.
  bleScale.startDiagScan(LEASE_MS);
  _last = bleScale.scanSnapshot();
  requestDraw();
  return stay();
}

ScreenResult BleScanScreen::onEvent(button::Gesture event) {
  switch (event) {
    case button::Gesture::B_SHORT:
      return exit();
    default:
      return ignored();
  }
}

namespace {
// An estimate is only trustworthy once a few plausible interval samples have
// accumulated (see ScanEntry::samples); until then show a placeholder rather
// than a misleading first-gap number.
constexpr uint16_t MIN_SAMPLES_FOR_INTERVAL = 4;

void formatInterval(const BleScaleService::ScanEntry& e, char* out, size_t n) {
  if (e.samples >= MIN_SAMPLES_FOR_INTERVAL && e.minIntervalMs > 0) {
    std::snprintf(out, n, "~%lums",
                  static_cast<unsigned long>(e.minIntervalMs));
  } else {
    std::snprintf(out, n, "~?");
  }
}

// Label for a row: the advertised name, or an abbreviated address (last three
// bytes) for nameless devices.
void formatLabel(const BleScaleService::ScanEntry& e, char* out, size_t n) {
  if (e.name[0] != '\0') {
    std::snprintf(out, n, "%s", e.name);
  } else {
    std::snprintf(out, n, "·%02X%02X%02X", e.addr[3], e.addr[4], e.addr[5]);
  }
}
}  // namespace

bool BleScanScreen::onDraw(LGFX_Sprite* c) {
  constexpr int pad = 3;
  const int W = c->width();
  const int H = c->height();
  c->fillScreen(theme::bg());

  char status[16] = {};
  const char* right = nullptr;
  if (!_last.busy) {
    std::snprintf(status, sizeof(status), "%u DEV", _last.count);
    right = status;
  }
  const int headerH = ui::drawViewHeader(c, "BLE", theme::accent(), right);

  const int x = pad;
  const int y = headerH;
  const int w = W - 2 * pad;
  const int h = H - headerH - pad;

  c->setFont(font::tiny());
  c->setTextSize(1);
  if (_last.busy) {
    c->setTextColor(theme::dim(), theme::bg());
    layout::drawTopLeft(c, "scale in use", x, y);
    return true;
  }

  if (_last.count == 0) {
    c->setTextColor(theme::dim(), theme::bg());
    layout::drawTopLeft(c, _last.active ? "scanning…" : "starting…", x, y);
    return true;
  }

  // Sort a local index list: recognized first, then strongest RSSI.
  uint8_t order[BleScaleService::SCAN_TABLE_CAP];
  for (uint8_t i = 0; i < _last.count; ++i) order[i] = i;
  std::sort(order, order + _last.count, [&](uint8_t a, uint8_t b) {
    const auto& ea = _last.entries[a];
    const auto& eb = _last.entries[b];
    if (ea.recognized != eb.recognized) return ea.recognized;
    return ea.rssi > eb.rssi;
  });

  const int lineH = font::metrics(font::tiny()).height + 1;

  int cy = y;
  for (uint8_t i = 0; i < _last.count && cy + lineH <= y + h;
       ++i, cy += lineH) {
    const BleScaleService::ScanEntry& e = _last.entries[order[i]];
    char label[24];
    char interval[12];
    formatLabel(e, label, sizeof(label));
    formatInterval(e, interval, sizeof(interval));
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%-12s %ddB %s", label,
                  static_cast<int>(e.rssi), interval);
    // Recognized scales pop in accent; everything else is secondary.
    c->setTextColor(e.recognized ? theme::accent() : theme::dim(), theme::bg());
    layout::drawTopLeft(c, buf, x, cy);
  }
  return true;
}
