// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <M5Unified.h>

#include <algorithm>
#include <utility>

#include "ui/fonts.h"

namespace layout {

// Layout geometry primitives. All coordinates are content-box pixels.
struct rect {
  int x, y, w, h;
};

// Shrink a rect by `pad` on all sides. When the box is too small to absorb the
// padding, the width/height clamp to 0 (the origin stays at the padded corner)
// rather than going negative, so drawing into the result is a safe no-op.
inline rect inset(rect r, int pad) {
  return {r.x + pad, r.y + pad, std::max(0, r.w - 2 * pad),
          std::max(0, r.h - 2 * pad)};
}

// A span that is `1/divisor` of `dim`, clamped to [lo, hi]. Names the recurring
// "proportional but bounded" sizing used for label bands, title strips, and the
// like. If hi < lo (a box narrower than the bounds assume), hi wins.
inline int clampedSpan(int dim, int divisor, int lo, int hi) {
  return std::min(std::max(lo, dim / divisor), hi);
}

namespace _impl {

// Clamp a ratio to [0, 1].
inline float clampRatio(float f) {
  if (f < 0.0f) f = 0.0f;
  if (f > 1.0f) f = 1.0f;
  return f;
}

inline void drawWithDatum(LGFX_Sprite* c, const char* text, int x, int y,
                          m5gfx::datum_t datum) {
  const auto prev = c->getTextDatum();
  c->setTextDatum(datum);
  c->drawString(text, x, y);
  c->setTextDatum(prev);
}

}  // namespace _impl

// Confine drawing to a rect for the lifetime of the object, restoring the
// prior clip when it goes out of scope. The box-fit text helpers use this to
// contain output when their minimum scale or line limits cannot satisfy the
// requested bounds.
struct ClipScope {
  LGFX_Sprite* c;
  int32_t x, y, w, h;
  ClipScope(LGFX_Sprite* canvas, int32_t cx, int32_t cy, int32_t cw, int32_t ch)
      : c(canvas) {
    c->getClipRect(&x, &y, &w, &h);
    c->setClipRect(cx, cy, cw, ch);
  }
  ~ClipScope() { c->setClipRect(x, y, w, h); }
  ClipScope(const ClipScope&) = delete;
  ClipScope& operator=(const ClipScope&) = delete;
};

// Split `r` horizontally: `ratio` of the width goes to the left child,
// the remainder to the right. `gap` pixels are removed from between the
// children. Negative or >1 ratios are clamped. The height is unchanged.
inline std::pair<rect, rect> splitH(rect r, float ratio, int gap = 0) {
  const int avail = std::max(0, r.w - gap);
  const int leftW = static_cast<int>(avail * _impl::clampRatio(ratio));
  const rect left = {r.x, r.y, leftW, r.h};
  const rect right = {r.x + leftW + gap, r.y, r.w - leftW - gap, r.h};
  return {left, right};
}

// Split `r` vertically: `ratio` of the height goes to the top child. The
// width is unchanged.
inline std::pair<rect, rect> splitV(rect r, float ratio, int gap = 0) {
  const int avail = std::max(0, r.h - gap);
  const int topH = static_cast<int>(avail * _impl::clampRatio(ratio));
  const rect top = {r.x, r.y, r.w, topH};
  const rect bottom = {r.x, r.y + topH + gap, r.w, r.h - topH - gap};
  return {top, bottom};
}

// Fixed-size horizontal split: left child is `fixedW` wide, right child gets
// the rest. If fixedW exceeds the available width, the right child collapses.
inline std::pair<rect, rect> splitHFixed(rect r, int fixedW, int gap = 0) {
  const int avail = std::max(0, r.w - gap);
  const int leftW = std::min(fixedW, avail);
  const rect left = {r.x, r.y, leftW, r.h};
  const rect right = {r.x + leftW + gap, r.y, r.w - leftW - gap, r.h};
  return {left, right};
}

// Fixed-size vertical split: top child is `fixedH` tall.
inline std::pair<rect, rect> splitVFixed(rect r, int fixedH, int gap = 0) {
  const int avail = std::max(0, r.h - gap);
  const int topH = std::min(fixedH, avail);
  const rect top = {r.x, r.y, r.w, topH};
  const rect bottom = {r.x, r.y + topH + gap, r.w, r.h - topH - gap};
  return {top, bottom};
}

// Fixed-size vertical split: bottom child is `fixedH` tall, top child gets
// the remainder. If fixedH exceeds the available height, the top child
// collapses.
inline std::pair<rect, rect> splitVFixedBottom(rect r, int fixedH,
                                               int gap = 0) {
  const int avail = std::max(0, r.h - gap);
  const int bottomH = std::min(fixedH, avail);
  const int topH = r.h - bottomH - gap;
  const rect top = {r.x, r.y, r.w, topH};
  const rect bottom = {r.x, r.y + topH + gap, r.w, bottomH};
  return {top, bottom};
}

// Text-anchor helpers that wrap setTextDatum + drawString. The canvas-wide
// text datum is sticky on M5GFX surfaces, so a stray setTextDatum in one
// draw routine leaks into unrelated drawString calls elsewhere. These
// helpers save and restore the prior datum so each call is self-contained.
//
// (x, y) is the anchor point on the canvas, matching drawString's semantic.
inline void drawTopLeft(LGFX_Sprite* c, const char* text, int x, int y) {
  _impl::drawWithDatum(c, text, x, y, m5gfx::datum_t::top_left);
}
inline void drawTopCenter(LGFX_Sprite* c, const char* text, int x, int y) {
  _impl::drawWithDatum(c, text, x, y, m5gfx::datum_t::top_center);
}
inline void drawTopRight(LGFX_Sprite* c, const char* text, int x, int y) {
  _impl::drawWithDatum(c, text, x, y, m5gfx::datum_t::top_right);
}
inline void drawMiddleLeft(LGFX_Sprite* c, const char* text, int x, int y) {
  _impl::drawWithDatum(c, text, x, y, m5gfx::datum_t::middle_left);
}
inline void drawCentered(LGFX_Sprite* c, const char* text, int x, int y) {
  _impl::drawWithDatum(c, text, x, y, m5gfx::datum_t::middle_center);
}
inline void drawMiddleRight(LGFX_Sprite* c, const char* text, int x, int y) {
  _impl::drawWithDatum(c, text, x, y, m5gfx::datum_t::middle_right);
}
inline void drawBaselineLeft(LGFX_Sprite* c, const char* text, int x, int y) {
  _impl::drawWithDatum(c, text, x, y, m5gfx::datum_t::baseline_left);
}
inline void drawBaselineCenter(LGFX_Sprite* c, const char* text, int x, int y) {
  _impl::drawWithDatum(c, text, x, y, m5gfx::datum_t::baseline_center);
}
inline void drawBaselineRight(LGFX_Sprite* c, const char* text, int x, int y) {
  _impl::drawWithDatum(c, text, x, y, m5gfx::datum_t::baseline_right);
}
inline void drawBottomLeft(LGFX_Sprite* c, const char* text, int x, int y) {
  _impl::drawWithDatum(c, text, x, y, m5gfx::datum_t::bottom_left);
}
inline void drawBottomCenter(LGFX_Sprite* c, const char* text, int x, int y) {
  _impl::drawWithDatum(c, text, x, y, m5gfx::datum_t::bottom_center);
}
inline void drawBottomRight(LGFX_Sprite* c, const char* text, int x, int y) {
  _impl::drawWithDatum(c, text, x, y, m5gfx::datum_t::bottom_right);
}

// Rect overloads: anchor on the named edge/corner of the rect.
inline void drawTopLeft(LGFX_Sprite* c, const char* text, rect box) {
  drawTopLeft(c, text, box.x, box.y);
}
inline void drawTopCenter(LGFX_Sprite* c, const char* text, rect box) {
  drawTopCenter(c, text, box.x + box.w / 2, box.y);
}
inline void drawTopRight(LGFX_Sprite* c, const char* text, rect box) {
  drawTopRight(c, text, box.x + box.w, box.y);
}
inline void drawMiddleLeft(LGFX_Sprite* c, const char* text, rect box) {
  drawMiddleLeft(c, text, box.x, box.y + box.h / 2);
}
inline void drawCentered(LGFX_Sprite* c, const char* text, rect box) {
  drawCentered(c, text, box.x + box.w / 2, box.y + box.h / 2);
}
inline void drawMiddleRight(LGFX_Sprite* c, const char* text, rect box) {
  drawMiddleRight(c, text, box.x + box.w, box.y + box.h / 2);
}
inline void drawBaselineLeft(LGFX_Sprite* c, const char* text, rect box) {
  drawBaselineLeft(c, text, box.x, box.y + box.h);
}
inline void drawBaselineCenter(LGFX_Sprite* c, const char* text, rect box) {
  drawBaselineCenter(c, text, box.x + box.w / 2, box.y + box.h);
}
inline void drawBaselineRight(LGFX_Sprite* c, const char* text, rect box) {
  drawBaselineRight(c, text, box.x + box.w, box.y + box.h);
}
inline void drawBottomLeft(LGFX_Sprite* c, const char* text, rect box) {
  drawBottomLeft(c, text, box.x, box.y + box.h);
}
inline void drawBottomCenter(LGFX_Sprite* c, const char* text, rect box) {
  drawBottomCenter(c, text, box.x + box.w / 2, box.y + box.h);
}
inline void drawBottomRight(LGFX_Sprite* c, const char* text, rect box) {
  drawBottomRight(c, text, box.x + box.w, box.y + box.h);
}

struct draw_centered_opts_t {
  // If non-null, pick the font size by fitting this reference string
  // instead of `text`. Useful when multiple labels should render at the
  // same size — pass the widest representative string (e.g. "00:00" for a
  // family of MM:SS readouts and an "OFF" placeholder).
  const char* sizeRef = nullptr;
  // Anchor the text to the box's left edge instead of centering it
  // horizontally (vertical centering unchanged). For table-like groups
  // where centered values would each start at their own x.
  bool leftAlign = false;
};

// Pick the largest font (family entry or scaled single font) whose rendering
// fits within (w, h), then visually-center `text` in the box at (x, y, w, h).
// The chosen font + textSize are left applied on the canvas; callers that want
// to restore prior state should save and reset. Color is not touched — set it
// before calling.
void drawCenteredInBox(LGFX_Sprite* c, const char* text, int x, int y, int w,
                       int h, const font::sizing_spec_t fontSpec,
                       draw_centered_opts_t opts = {});

// Rect overload for drawCenteredInBox.
inline void drawCenteredInBox(LGFX_Sprite* c, const char* text, rect box,
                              const font::sizing_spec_t fontSpec,
                              draw_centered_opts_t opts = {}) {
  drawCenteredInBox(c, text, box.x, box.y, box.w, box.h, fontSpec, opts);
}

// Height of the block drawWrappedCentered would render for `text` within
// (maxW, maxH): the wrapped line count times the line height, at the same
// font choice. Lets callers size a band to its content instead of
// budgeting a fixed strip. Leaves the chosen font applied.
int measureWrapped(LGFX_Sprite* c, const char* text, int maxW, int maxH,
                   const font::sizing_spec_t fontSpec);

// Choose a font size that fits `text` to width `w` word-wrapped, then draw the
// resulting lines as one block, each line horizontally centered and the block
// vertically centered within (x, y, w, h). Unlike drawCenteredInBox (which
// auto-sizes a *single* line), every line here renders at the same size with
// uniform line spacing. Wrapping is greedy on spaces; a word wider than `w` is
// placed on its own line and may overflow. No heap allocation; runs only on
// redraw, so the per-call substring measuring is fine.
void drawWrappedCentered(LGFX_Sprite* c, const char* text, int x, int y, int w,
                         int h, const font::sizing_spec_t fontSpec);

// Rect overload for drawWrappedCentered.
inline void drawWrappedCentered(LGFX_Sprite* c, const char* text, rect box,
                                const font::sizing_spec_t fontSpec) {
  drawWrappedCentered(c, text, box.x, box.y, box.w, box.h, fontSpec);
}

}  // namespace layout

// Convenience overload so font::fit can take a layout::rect directly.
// Lives here rather than in fonts.h because fonts.h is included by layout.h
// and cannot forward-declare layout::rect without a circular dependency.
namespace font {
inline sized_font_t fit(LGFX_Sprite* canvas, const char* text, layout::rect r,
                        const sizing_spec_t& spec) {
  return fit(canvas, text, r.w, r.h, spec);
}
}  // namespace font
