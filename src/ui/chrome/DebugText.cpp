// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#include "DebugText.h"

#include <M5Unified.h>

#include <cstring>

#include "ui/fonts.h"
#include "ui/layout.h"
#include "ui/theme.h"

void DebugText::setText(const char* text) {
  if (!text) text = "";
  // String literals and formatted scratch buffers both flow through here every
  // frame, so compare contents (not the pointer) to short-circuit unchanged
  // frames the way the other painters' setters do.
  if (std::strncmp(_text, text, MAX_LEN + 1) == 0) return;
  std::strncpy(_text, text, MAX_LEN);
  _text[MAX_LEN] = '\0';
  _dirty = true;
}

bool DebugText::poll() { return _dirty; }

void DebugText::draw(LGFX_Sprite& canvas, ChromeEdge /*edge*/) {
  // Logical frame: the canvas is laid out flat (length × T), with Chrome
  // handling any rotation onto the edge — so the text layout is fully
  // orientation-agnostic and the edge is unused.
  const int length = canvas.width();
  const int T = canvas.height();

  canvas.setTextColor(theme::warn(), theme::bg());
  // Wrap on spaces across the strip's length instead of shrinking one line to
  // fit its full width (which goes unreadably small). The debug string is
  // space-separated tokens, so it breaks into ~2 lines. tiny() is a compact
  // 16px bitmap font: two lines fit the 32px strip and stay legible.
  layout::drawWrappedCentered(&canvas, _text, 0, 0, length, T, font::tiny());

  _dirty = false;
}
