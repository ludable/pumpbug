// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <M5Unified.h>

#include <cstdint>

#include "ui/Screen.h"
#include "ui/button.h"

// Shows extraction, network, power, memory, and crash diagnostics. A tap cycles
// pages, a long press clears the current page, and B exits.
class LogsScreen : public Screen {
 public:
  void onEnter() override;
  ScreenResult onEvent(button::Gesture event) override;
  void onLayoutChanged() override;
  ScreenResult tick() override;
  bool onDraw(LGFX_Sprite* canvas) override;
  uint32_t desiredTickMs() const override { return 250; }
  ButtonHints buttonHints() const override;

 private:
  enum class Page : uint8_t { Extraction, Net, Power, Heap, Panic };
  static constexpr uint8_t PAGE_COUNT = 5;

  Page _page = Page::Extraction;
  uint32_t _lastExtractionWrites = 0;
  uint32_t _lastNetWrites = 0;
  uint32_t _lastPowerWrites = 0;

  void clearCurrentPage();

  void drawExtraction(LGFX_Sprite* c, int x, int y, int w, int h);
  void drawNet(LGFX_Sprite* c, int x, int y, int w, int h);
  void drawPower(LGFX_Sprite* c, int x, int y, int w, int h);
  void drawHeap(LGFX_Sprite* c, int x, int y, int w, int h);
  void drawPanic(LGFX_Sprite* c, int x, int y, int w, int h);
};
