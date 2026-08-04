// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <M5Unified.h>

#include <algorithm>

namespace font {

typedef const lgfx::IFont* font_t;  // Convenience

// Semantic font roles for UI code. Concrete font choices live here so a restyle
// is a one-file diff. Callers pass the result to setFont().
inline font_t tiny() { return &lgfx::fonts::Font2; }
inline font_t body() { return &lgfx::fonts::lv_font_montserrat_16; }
inline font_t badge() { return &lgfx::fonts::lv_font_montserrat_16; }
inline font_t menu_item() { return &lgfx::fonts::lv_font_montserrat_18; }
inline font_t button_hint() { return &lgfx::fonts::lv_font_montserrat_18; }
inline font_t view_title() { return &lgfx::fonts::lv_font_montserrat_18; }
inline font_t section_title() { return &lgfx::fonts::lv_font_montserrat_14; }

// Cached default metric for a font pointer. Makes is convenient to use:
//
//   auto height = font::metrics(f).height;
//   auto [height, width] = font::metrics(f).hw();
//
// The cache is bounded; on overflow callers fall through to direct
// computation (no growth, no eviction).

struct metrics_t : lgfx::FontMetrics {
  struct hw {
    int16_t height, width;
  };

  hw hw() { return {this->height, this->width}; };
};

inline metrics_t metrics(font_t f) {
  struct Entry {
    const lgfx::IFont* font;
    metrics_t m;
  };
  static constexpr size_t CACHE_CAP = 8;
  static Entry _cache[CACHE_CAP] = {};

  size_t i;
  for (i = 0; i < CACHE_CAP; ++i) {
    if (_cache[i].font == f) return _cache[i].m;
    if (_cache[i].font == nullptr) break;
  }

  if (i < CACHE_CAP) {
    _cache[i].font = f;
    f->getDefaultMetric(&_cache[i].m);
    return _cache[i].m;
  }

  metrics_t m;
  f->getDefaultMetric(&m);
  return m;
}

// A font choice plus a text size. Bundling these lets a "family" mix
// crisp-scaling bitmap fonts (e.g. Font0 × N) with native-size TTF-rendered
// glyphs (DejaVu*) without the caller having to know which is which.
struct sized_font_t {
  font_t font;
  float size;
};

struct family_t {
  const sized_font_t* fonts;
  size_t count;
};

// A specification for how text should be sized to fit a box: either by
// selecting the largest native entry from a font family, or by scaling a
// single font. Implicit conversions from either family_t or font_t keep call
// sites tidy.
struct sizing_spec_t {
  enum class kind_t { Family, Scalable } kind;
  union {
    family_t family;
    font_t font;
  };
  constexpr sizing_spec_t(family_t f) : kind(kind_t::Family), family(f) {}
  constexpr sizing_spec_t(font_t f) : kind(kind_t::Scalable), font(f) {}
};

// Apply `entry` to `canvas` (setFont + setTextSize).
inline void apply(LGFX_Sprite* canvas, const sized_font_t& entry) {
  canvas->setFont(entry.font);
  canvas->setTextSize(entry.size);
}

// Largest-first family selector. Walks `family` (assumed sorted largest to
// smallest) and applies the first entry whose rendered (textWidth, fontHeight)
// of `text` fits within (maxW, maxH). If none fit, scale the smallest entry
// down as a last resort; callers using family fonts asked for "fit", not
// clipping.
//
// The canvas font and size are clobbered; save and restore the prior state
// (getFont(), setTextSize(1)) around the call if you care.
inline sized_font_t fitToBox(LGFX_Sprite* canvas, const char* text, int maxW,
                             int maxH, const family_t family) {
  assert(family.count > 0);

  sized_font_t chosen = family.fonts[family.count - 1];
  for (size_t i = 0; i < family.count; ++i) {
    apply(canvas, family.fonts[i]);
    if (canvas->textWidth(text) <= maxW && canvas->fontHeight() <= maxH) {
      chosen = family.fonts[i];
      apply(canvas, chosen);
      return chosen;
    }
  }

  apply(canvas, chosen);
  const int w1 = canvas->textWidth(text);
  const int h1 = canvas->fontHeight();
  if (w1 > 0 && h1 > 0) {
    const float sx = static_cast<float>(maxW) / w1;
    const float sy = static_cast<float>(maxH) / h1;
    float scale = chosen.size * (sx < sy ? sx : sy);
    if (scale < 0.25f) scale = 0.25f;
    chosen.size = scale;
    apply(canvas, chosen);
  }

  return chosen;
}

// Scale a single font to fit a rectangle. Useful when only one font has the
// right visual style (e.g. the 7-segment Font7) and we want to keep that style
// across all digit counts. The scale is derived from a 1.0 measurement so the
// text fits both width and height without upscaling past the font's native
// resolution.
inline sized_font_t fitSingleFontToBox(LGFX_Sprite* canvas, font_t font,
                                       const char* text, int maxW, int maxH) {
  canvas->setFont(font);
  canvas->setTextSize(1.0f);
  const int w1 = canvas->textWidth(text);
  const int h1 = canvas->fontHeight();
  if (w1 <= 0 || h1 <= 0) return {font, 1.0f};

  const float sx = static_cast<float>(maxW) / w1;
  const float sy = static_cast<float>(maxH) / h1;
  float scale = sx < sy ? sx : sy;
  if (scale > 1.0f) scale = 1.0f;
  // Floor at 25% so the glyphs don't vanish; below that we assume readability
  // is gone anyway and the caller should reconsider the box size.
  if (scale < 0.25f) scale = 0.25f;

  canvas->setTextSize(scale);
  return {font, scale};
}

// Uniform box-fitting entry point: accepts either a font family or a single
// scalable font.
inline sized_font_t fit(LGFX_Sprite* canvas, const char* text, int maxW,
                        int maxH, const sizing_spec_t& spec) {
  if (spec.kind == sizing_spec_t::kind_t::Family) {
    return fitToBox(canvas, text, maxW, maxH, spec.family);
  }
  return fitSingleFontToBox(canvas, spec.font, text, maxW, maxH);
}

inline const family_t textFamily() {
  static const sized_font_t kFamily[] = {
      {&lgfx::fonts::lv_font_montserrat_48, 1.0f},
      {&lgfx::fonts::lv_font_montserrat_46, 1.0f},
      {&lgfx::fonts::lv_font_montserrat_44, 1.0f},
      {&lgfx::fonts::lv_font_montserrat_42, 1.0f},
      {&lgfx::fonts::lv_font_montserrat_40, 1.0f},
      {&lgfx::fonts::lv_font_montserrat_38, 1.0f},
      {&lgfx::fonts::lv_font_montserrat_36, 1.0f},
      {&lgfx::fonts::lv_font_montserrat_34, 1.0f},
      {&lgfx::fonts::lv_font_montserrat_32, 1.0f},
      {&lgfx::fonts::lv_font_montserrat_30, 1.0f},
      {&lgfx::fonts::lv_font_montserrat_28, 1.0f},
      {&lgfx::fonts::lv_font_montserrat_26, 1.0f},
      {&lgfx::fonts::lv_font_montserrat_24, 1.0f},
      {&lgfx::fonts::lv_font_montserrat_22, 1.0f},
      {&lgfx::fonts::lv_font_montserrat_20, 1.0f},
      {&lgfx::fonts::lv_font_montserrat_18, 1.0f},
      {&lgfx::fonts::lv_font_montserrat_16, 1.0f},
      {&lgfx::fonts::lv_font_montserrat_14, 1.0f},
      {&lgfx::fonts::lv_font_montserrat_12, 1.0f},
      {&lgfx::fonts::lv_font_montserrat_10, 1.0f},
      {&lgfx::fonts::lv_font_montserrat_8, 1.0f},
  };
  return {kFamily, sizeof(kFamily) / sizeof(kFamily[0])};
}

inline const family_t textAltFamily() {
  static const sized_font_t kFamily[] = {
      {&lgfx::fonts::DejaVu72, 1.0f}, {&lgfx::fonts::DejaVu56, 1.0f},
      {&lgfx::fonts::DejaVu40, 1.0f}, {&lgfx::fonts::DejaVu24, 1.0f},
      {&lgfx::fonts::DejaVu18, 1.0f}, {&lgfx::fonts::DejaVu12, 1.0f},
      {&lgfx::fonts::DejaVu9, 1.0f},
  };
  return {kFamily, sizeof(kFamily) / sizeof(kFamily[0])};
}

inline const family_t boldFamily() {
  static const sized_font_t kFamily[] = {
      {&lgfx::fonts::FreeSansBold24pt7b, 1.0f},
      {&lgfx::fonts::FreeSansBold18pt7b, 1.0f},
      {&lgfx::fonts::FreeSansBold12pt7b, 1.0f},
      {&lgfx::fonts::FreeSansBold9pt7b, 1.0f},
  };
  return {kFamily, sizeof(kFamily) / sizeof(kFamily[0])};
}

// LCD-style segments font (digits, dot, dash, colon only)
constexpr const lgfx::IFont* segmentDigitFont = &lgfx::fonts::Font7;

}  // namespace font
