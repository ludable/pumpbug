// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <M5Unified.h>

#include "ChromeContent.h"

// Chrome content painter: a free-text line wrapped and centered on the strip.
//
// A caller selects ChromeMode::Debug and pushes a string via
// Chrome::setDebugText()
class DebugText : public ChromeContent {
 public:
  void setText(const char* text);

  ChromeFrame frame() const override { return ChromeFrame::Logical; }
  bool poll() override;
  void draw(LGFX_Sprite& canvas, ChromeEdge edge) override;

 private:
  static constexpr size_t MAX_LEN = 63;
  char _text[MAX_LEN + 1] = {0};
  bool _dirty = true;  // text changed since last paint
};
