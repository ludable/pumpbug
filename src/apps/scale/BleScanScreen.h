// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <M5Unified.h>

#include <cstdint>

#include "ble/BleScaleService.h"
#include "ui/Screen.h"
#include "ui/button.h"

// Diagnostics -> BLE scan. Drives BleScaleService's DiagScan mode and renders
// the discovered-device table: every advertiser the radio sees, with RSSI and
// an estimated advertising interval, scales we recognize highlighted. Its
// reason to exist is to figure out how new scales advertise themselves and
// measuring the scale advertising interval to fine-tune the operational scan
// duty cycle.
//
// The scan is held by a self-expiring lease that tick() renews; onExit drops
// it. B exits.
class BleScanScreen : public Screen {
 public:
  void onEnter() override;
  void onExit() override;
  ScreenResult onEvent(button::Gesture event) override;
  void onLayoutChanged() override;
  ScreenResult tick() override;
  bool onDraw(LGFX_Sprite* canvas) override;
  uint32_t desiredTickMs() const override { return 250; }

 private:
  // Lease comfortably longer than the tick interval so it never lapses mid-use.
  static constexpr uint32_t LEASE_MS = 3000;

  BleScaleService::ScanResults _last;
};
