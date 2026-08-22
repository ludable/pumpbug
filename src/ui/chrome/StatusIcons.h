// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include "ChromeContent.h"
#include "net/NetworkStatus.h"

// Chrome content painter: battery, shot counter, and Wi-Fi laid out as 1:2:1
// tiles along the strip. The concrete painters live in the .cpp.
class StatusIcons : public ChromeContent {
 public:
  void setNetworkStatus(NetworkStatus status) { _networkStatus = status; }
  void setShotCount(uint64_t count) { _shotCount = count; }

  // The battery and cup sprites cache the active palette. Call this when the
  // global theme switches so both glyphs rebuild on their next draw.
  void invalidate();

  bool poll() override;
  void draw(LGFX_Sprite& bar, ChromeEdge edge) override;

 private:
  NetworkStatus _networkStatus = NetworkStatus::Off;
  uint64_t _shotCount = 0;
};
