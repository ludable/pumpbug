// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

// segfont_ifont.h - optional lgfx::v1::IFont adapter.
//
// IFont is glyph-oriented. It can expose tabular or proportional glyph
// advances, datum handling, textWidth(), print(), and drawString(). Segfont
// tracking is represented as a normal right side bearing in x_advance because
// IFont cannot express "between glyphs only" spacing. Use segfont::draw_text()
// or segfont::drawString() when exact no-trailing-tracking width matters. Text
// scaling resynthesizes integer geometry; non-uniform scales alter
// core_x/core_y rather than stretching emitted rectangles.

#pragma once

#if __has_include(<M5GFX.h>)
#include <M5GFX.h>
#elif __has_include(<LovyanGFX.hpp>)
#include <LovyanGFX.hpp>
#elif __has_include(<M5Unified.h>)
#include <M5Unified.h>
#else
#error <M5GFX.h>, <LovyanGFX.hpp>, or <M5Unified.h> required
#endif

#include "segfont.h"

namespace segfont {
namespace detail {

inline int32_t scale_metric(int value, float scale) {
  if (scale <= 0.0f) return 0;
  const int32_t fixed = static_cast<int32_t>(65536.0f * scale);
  return (value * fixed) >> 16;
}

inline int32_t scale_stroke(int value, float scale) {
  if (scale <= 0.0f) return 0;
  const int32_t fixed = static_cast<int32_t>(65536.0f * scale);
  return (value * fixed + 32768) >> 16;
}

inline int metric_advance(const Style& style, const Glyph& glyph,
                          const TextOptions& options) {
  return glyph_metrics(style, glyph, options).advance +
         resolved_tracking(style, options);
}

// LGFX asks an IFont for its unscaled metrics, then applies setTextSize()
// itself when it computes textWidth(), fontHeight(), datum offsets, and the
// top-left position passed to drawChar(). That scaling uses 16.16 fixed-point
// arithmetic, so it can round differently from plain float-to-int math.
//
// Segfont still resynthesizes geometry instead of bitmap-scaling pixels. The
// synthesized glyph must fit inside the same scaled metric cell LGFX used for
// layout, so target width/height use LGFX-compatible metric scaling. Stroke is
// a visual weight, not a layout metric, so round it to avoid systematically
// thinning the font at fractional text sizes; the core resynthesis helper still
// clamps it back into the metric cell when needed.
inline Style scaled_metric_style(const Style& base, float scale_x,
                                 float scale_y) {
  if (scale_x <= 0.0f || scale_y <= 0.0f) return base;

  int target_w = scale_metric(base.gw, scale_x);
  int target_h = scale_metric(base.gh, scale_y);
  const float stroke_scale = scale_x < scale_y ? scale_x : scale_y;
  int stroke = scale_stroke(base.stroke, stroke_scale);

  return resynthesize_style(target_w, target_h, stroke);
}

struct OffsetSink {
  lgfx::v1::LGFXBase* gfx;
  uint32_t color;
  int32_t ox;
  int32_t oy;

  void fill(int x, int y, int w, int h) {
    if (w > 0 && h > 0) gfx->fillRect(ox + x, oy + y, w, h, color);
  }
};

}  // namespace detail

class IFontAdapter : public lgfx::v1::IFont {
 public:
  explicit IFontAdapter(const Style& style, TextOptions options = TextOptions())
      : _style(style), _options(options) {}

  const Style& style() const { return _style; }
  const TextOptions& options() const { return _options; }

  void getDefaultMetric(lgfx::v1::FontMetrics* m) const override {
    m->width = _style.gw;
    m->x_advance = _style.gw + resolved_tracking(_style, _options);
    m->x_offset = 0;
    m->height = _style.gh;
    m->y_advance = _style.gh;
    m->y_offset = 0;
    m->baseline = _style.gh;
  }

  bool updateFontMetric(lgfx::v1::FontMetrics* m,
                        uint16_t uniCode) const override {
    const Glyph g =
        glyph_for(uniCode <= 0x7F ? static_cast<char>(uniCode) : ' ');
    const GlyphMetrics gm = glyph_metrics(_style, g, _options);
    m->width = gm.advance;
    m->x_advance = detail::metric_advance(_style, g, _options);
    m->x_offset = 0;
    return true;
  }

  size_t drawChar(lgfx::v1::LGFXBase* gfx, int32_t x, int32_t y, uint16_t c,
                  const lgfx::v1::TextStyle* style,
                  lgfx::v1::FontMetrics* /*metrics*/,
                  int32_t& filled_x) const override {
    const Glyph g = glyph_for(c <= 0x7F ? static_cast<char>(c) : ' ');
    const uint32_t color = style->fore_rgb888;
    const float sx = style->size_x;
    const float sy = style->size_y;

    // LGFX's drawString()/print() paths update `metrics` for each glyph before
    // calling drawChar(), but direct drawChar() calls can pass the font's
    // default metrics. The return value must still be this glyph's advance.
    const int32_t advance =
        detail::scale_metric(detail::metric_advance(_style, g, _options), sx);
    const int32_t metric_h = detail::scale_metric(_style.gh, sy);
    const Style scaled = detail::scaled_metric_style(_style, sx, sy);
    const GlyphMetrics scaled_metrics = glyph_metrics(scaled, g, _options);
    int32_t y_adjust = metric_h - scaled.gh;
    if (y_adjust < 0) y_adjust = 0;

    const int32_t fill_w =
        advance > scaled_metrics.advance ? advance : scaled_metrics.advance;
    if (style->fore_rgb888 != style->back_rgb888 && fill_w > 0 &&
        metric_h > 0) {
      const int32_t left = filled_x > x ? filled_x : x;
      const int32_t right = x + fill_w;
      if (right > left)
        gfx->fillRect(left, y, right - left, metric_h, style->back_rgb888);
      filled_x = right;
    }

    detail::OffsetSink sink{gfx, color, x, y + y_adjust};
    draw_glyph(scaled, g, scaled_metrics.draw_offset, 0, sink);

    return advance > 0 ? static_cast<size_t>(advance) : 0;
  }

 private:
  Style _style;
  TextOptions _options;
};

}  // namespace segfont
