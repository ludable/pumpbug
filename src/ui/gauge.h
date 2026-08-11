// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <M5Unified.h>

#include "ui/layout.h"
#include "ui/strokefont/strokefont.h"

// Numeric readouts: a value with a trailing unit, drawn so the value reads as
// the primary number and the unit as a smaller secondary label.
//
// The value may carry a decimal fraction (for example "12.3" or "-1.0").
// When opts_t::smallFraction is set, the fractional part (the decimal point and
// the digits after it) is rendered smaller than the whole-number part. This
// keeps the whole-number part dominant, and stops a fast-changing final digit
// from drawing the eye on a live gauge.
//
// Digits and punctuation are drawn with segfont. Units are drawn as compact
// reverse strokefont labels: the current text foreground fills a rounded rect,
// and the unit glyph is cut through it with the current text background. The
// unit is smaller than the fraction digits and vertically centered within the
// readout; the stroke font resolves that request to its own coherent integer
// cell grid.
//
// Color and background are the caller's responsibility.
namespace gauge {

struct opts_t {
  // If non-null, sizes the canonical value column against this reference string
  // instead of the live value, so the digit size, unit size, and baseline stay
  // put as the live value changes (for example "0.0" versus "88.8"). Centered
  // gauges also center this reference-width group and align the live value
  // within it. Callers should pass live values with the same decimal precision
  // as the reference; variable-precision formatting is a separate layout
  // policy.
  const char* sizeRef = nullptr;
  // Render the decimal fraction smaller than the whole-number part. When false
  // the value is drawn at a single size.
  bool smallFraction = true;
};

// Resolved size and drawing metrics for a reverse-color unit label.
struct UnitBoxLayout {
  int width = 0;
  int height = 0;
  strokefont::Style style{};
  strokefont::Metrics metrics{};
};

// Fit a padded unit label within `boxHeight` and return a box exactly that
// tall.
UnitBoxLayout layoutUnitBoxWithinHeight(const char* unit, int boxHeight);

// Draw a resolved unit label with its top-left at (x, y) using the box size
// from a resolved layout.
void drawUnitBox(LGFX_Sprite* c, const char* unit,
                 const UnitBoxLayout& unitLayout, int x, int y, uint32_t fg,
                 uint32_t bg);

// Fixed-column readout: the unit is anchored to the right edge and the value is
// right-aligned before it, so the decimal point holds a stable column as digits
// change. This is the live weight/timer gauge style. Values wider than sizeRef
// re-fit the live value: the integer part shrinks while the fraction stays
// smaller (capped at the minimum gauge text height). Once the integer would
// also have to shrink below that minimum, the whole numeric value falls back to
// one smaller uniform size. The integer value is vertically centered in the
// box; decimal fractions share the integer baseline, and the unit label is
// centered independently.
void drawFixedColumn(LGFX_Sprite* c, const char* value, const char* unit, int x,
                     int y, int w, int h, opts_t opts = {});

inline void drawFixedColumn(LGFX_Sprite* c, const char* value, const char* unit,
                            layout::rect box, opts_t opts = {}) {
  drawFixedColumn(c, value, unit, box.x, box.y, box.w, box.h, opts);
}

// Centered readout: the value and unit are sized as one horizontal group and
// centered in the box. With sizeRef, that reference-width group stays centered
// and the live value is right-aligned before the fixed unit. The value and unit
// label are vertically centered independently, so the unit treatment does not
// pull the value off center.
void drawCentered(LGFX_Sprite* c, const char* value, const char* unit, int x,
                  int y, int w, int h, opts_t opts = {});

inline void drawCentered(LGFX_Sprite* c, const char* value, const char* unit,
                         layout::rect box, opts_t opts = {}) {
  drawCentered(c, value, unit, box.x, box.y, box.w, box.h, opts);
}

}  // namespace gauge
