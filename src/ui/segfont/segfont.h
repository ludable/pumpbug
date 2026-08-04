// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

// segfont.h - scalable seven-segment font, header-only and display-agnostic.
//
// The core is split into three ideas:
//   * Style: integer geometry for one synthesized font size.
//   * Glyph metrics: per-glyph ink bounds and advance.
//   * TextOptions: string layout policy such as tabular/proportional spacing
//     and between-glyph tracking.
//
// The core only emits filled rectangles through:
//
//   struct Sink { void fill(int x, int y, int w, int h); };
//
// Adapters wrap this in a display API.

#pragma once

#include <stdint.h>

#include "../font_weight.h"

namespace segfont {

//   AAAAA
//  F     B
//  F     B
//   GGGGG
//  E     C
//  E     C
//   DDDDD
enum : uint8_t {
  SEG_A = 1u << 0,
  SEG_B = 1u << 1,
  SEG_C = 1u << 2,
  SEG_D = 1u << 3,
  SEG_E = 1u << 4,
  SEG_F = 1u << 5,
  SEG_G = 1u << 6,
};

enum : uint8_t {
  DOT_TOP = 1u << 0,
  DOT_MID = 1u << 1,
  DOT_BOT = 1u << 2,
};

enum class GlyphKind : uint8_t { Blank, Segments, Dots };
using Weight = font_weight::Weight;
enum class SpacingMode : uint8_t { Tabular, Proportional };

struct Glyph {
  GlyphKind kind = GlyphKind::Blank;
  uint8_t segs = 0;
  uint8_t dots = 0;
};

// Segment
//     ███████████████   ▲
//   ███████████████████ │ stroke
//     ███████████████   ▼
//     |◄────core───►|
//
// Glyph
//         ███████████████        ▲
//       ███████████████████      │
//    ██   ███████████████   ██   |
//  ██████                 ██████ |
//  ██████                 ██████ |
//  ██████                 ██████ |
//  ██████                 ██████ |
//  ██████                 ██████ |
//  ██████                 ██████ |
//    ██   ███████████████   ██   |
//       ███████████████████      │ gh = 2*core_y + 3*stroke
//    ██   ███████████████   ██   |
//  ██████                 ██████ |
//  ██████                 ██████ |
//  ██████                 ██████ |
//  ██████                 ██████ |
//  ██████                 ██████ |
//  ██████                 ██████ |
//    ██   ███████████████   ██   |
//       ███████████████████      │
//         ███████████████        ▲
//   ◄─--------─────────────────►
//        gw = core_x + 2*stroke

struct Style {
  int stroke = 3;  // Segment thickness in pixels.
  int core_x = 6;  // Horizontal segment run length between miters.
  int core_y = 6;  // Vertical segment run length between miters.

  // Derived values. Build styles with make_style()/style_for_height() so these
  // remain coherent.
  int bevel = 1;
  int gw = 12;  // Digit ink box width  = core_x + 2 * stroke.
  int gh = 21;  // Digit ink box height = 2 * core_y + 3 * stroke.
  int clo = 1;  // Lower miter center = (stroke - 1) / 2.
  int chi = 1;  // Upper miter center = stroke / 2.
};

struct TextOptions {
  SpacingMode spacing = SpacingMode::Tabular;
  // Spacing applied between characters by text_width()/draw_text().
  // A text feature, not a glyph metric.
  int tracking = -1;
};

struct GlyphMetrics {
  int cell_width = 0;   // The untrimmed design cell for this glyph.
  int ink_left = 0;     // Inclusive ink bounds inside the design cell.
  int ink_right = -1;   // -1 means no ink.
  int advance = 0;      // Cursor advance before string-level tracking.
  int draw_offset = 0;  // Add to x before drawing so ink lands on the cursor.
};

inline int round_div(int n, int d) { return font_weight::round_div(n, d); }

inline int scaled_floor(int value, float scale) {
  if (scale <= 0.0f) return 0;
  return static_cast<int>(value * scale);
}

inline int scaled_round(int value, float scale) {
  if (scale <= 0.0f) return 0;
  return static_cast<int>(value * scale + 0.5f);
}

inline int default_tracking(int stroke) {
  return stroke > 2 ? (stroke + 1) / 2 : 1;
}

inline int default_tracking(const Style& s) {
  return default_tracking(s.stroke);
}

inline int dot_pad_for(int bevel) { return bevel > 1 ? bevel : 1; }

inline int max_stroke_for_size(int target_w, int target_h) {
  if (target_w < 1) target_w = 1;
  if (target_h < 1) target_h = 1;
  const int max_w = (target_w - 1) / 2;  // core_x has a minimum of 1.
  const int max_h = (target_h - 2) / 3;  // core_y has a minimum of 1.
  int max_stroke = max_w < max_h ? max_w : max_h;
  return max_stroke < 1 ? 1 : max_stroke;
}

inline int fit_stroke_for_size(int target_w, int target_h, int stroke) {
  if (stroke < 1) stroke = 1;
  const int max_stroke = max_stroke_for_size(target_w, target_h);
  return stroke > max_stroke ? max_stroke : stroke;
}

inline Style make_style(int stroke, int core_x, int core_y) {
  if (stroke < 1) stroke = 1;
  if (core_x < 1) core_x = 1;
  if (core_y < 1) core_y = 1;
  Style s;
  s.stroke = stroke;
  s.core_x = core_x;
  s.core_y = core_y;
  s.bevel = (stroke - 1) / 2;
  s.clo = (stroke - 1) / 2;
  s.chi = stroke / 2;
  s.gw = core_x + 2 * stroke;
  s.gh = 2 * core_y + 3 * stroke;
  return s;
}

inline Style make_style(int stroke, int core) {
  return make_style(stroke, core, core);
}

// Weight is defined as a target height / stroke ratio. The numbers are chosen
// so a 32 px digit naturally yields about 2 px thin, 3 px regular, and 4 px
// bold strokes while keeping the interior gaps comfortably larger than stroke.
inline int height_divisor_for(Weight weight) {
  return font_weight::height_divisor_for(weight);
}

inline int natural_stroke_for_height(int target_px, Weight weight) {
  return font_weight::natural_stroke_for_height(target_px, weight);
}

// Low-level sizing: keep this when you know the exact stroke you want.
inline Style style_for_height(int target_px, int stroke) {
  int core_y = (target_px - 3 * stroke) / 2;
  if (core_y < 1) core_y = 1;
  return make_style(stroke, core_y);
}

// Low-level anisotropic sizing: preserve an integer stroke while deriving
// independent horizontal and vertical runs for the requested ink box.
inline Style style_for_size(int target_w, int target_h, int stroke) {
  if (target_w < 1) target_w = 1;
  if (target_h < 1) target_h = 1;
  int core_x = target_w - 2 * stroke;
  int core_y = (target_h - 3 * stroke) / 2;
  if (core_x < 1) core_x = 1;
  if (core_y < 1) core_y = 1;
  return make_style(stroke, core_x, core_y);
}

// Build a coherent integer style for a precomputed metric cell. Callers choose
// how to compute the target cell (float scaling, LGFX fixed-point scaling,
// native height, etc.); this helper owns the shared geometry rule that the
// stroke is clamped to fit before core_x/core_y are derived.
inline Style resynthesize_style(int target_w, int target_h, int stroke) {
  if (target_w < 1) target_w = 1;
  if (target_h < 1) target_h = 1;
  stroke = fit_stroke_for_size(target_w, target_h, stroke);
  return style_for_size(target_w, target_h, stroke);
}

// Semantic sizing: choose a natural stroke from the requested visual weight.
inline Style style_for_height(int target_px, Weight weight) {
  return style_for_height(
      target_px, font_weight::natural_stroke_for_height(target_px, weight));
}

inline Style style_for_height(int target_px) {
  return style_for_height(target_px, Weight::Regular);
}

inline Style style_for_size(int target_w, int target_h, Weight weight) {
  int stroke = font_weight::natural_stroke_for_height(target_h, weight);
  return resynthesize_style(target_w, target_h, stroke);
}

// Cell-height helper for layouts that want roughly one stroke of vertical
// breathing room outside the ink box.
inline Style style_for_cell_height(int target_px, Weight weight) {
  int stroke = font_weight::natural_stroke_for_height(target_px, weight);
  int ink_height = target_px - stroke;
  if (ink_height < 1) ink_height = 1;
  return style_for_height(ink_height, stroke);
}

// Resynthesize a nearby integer style for a display scale. This is
// intentionally not bitmap scaling: the returned style has coherent segment
// widths and miters. Non-uniform scales stretch core_x/core_y, not individual
// rectangles.
inline Style scaled_style(const Style& base, float scale_x, float scale_y) {
  if (scale_x <= 0.0f || scale_y <= 0.0f) return base;
  int target_w = scaled_floor(base.gw, scale_x);
  int target_h = scaled_floor(base.gh, scale_y);

  const float stroke_scale = scale_x < scale_y ? scale_x : scale_y;
  int stroke = scaled_round(base.stroke, stroke_scale);

  return resynthesize_style(target_w, target_h, stroke);
}

inline Style scaled_style(const Style& base, float scale) {
  return scaled_style(base, scale, scale);
}

template <class Sink>
inline void emit_hbar(const Style& s, int x, int y, Sink& sink) {
  const int W = s.stroke;
  const int b = s.bevel;
  const int total = s.core_x + 2 * b;
  sink.fill(x + b, y, s.core_x, W);
  for (int k = 0; k < b; ++k) {
    const int y0 = y + (s.clo - k);
    const int h = (s.chi - s.clo) + 2 * k + 1;
    sink.fill(x + k, y0, 1, h);
    sink.fill(x + total - 1 - k, y0, 1, h);
  }
}

template <class Sink>
inline void emit_vbar(const Style& s, int x, int y, Sink& sink) {
  const int W = s.stroke;
  const int b = s.bevel;
  const int total = s.core_y + 2 * b;
  sink.fill(x, y + b, W, s.core_y);
  for (int k = 0; k < b; ++k) {
    const int x0 = x + (s.clo - k);
    const int w = (s.chi - s.clo) + 2 * k + 1;
    sink.fill(x0, y + k, w, 1);
    sink.fill(x0, y + total - 1 - k, w, 1);
  }
}

template <class Sink>
inline void emit_dot(const Style& s, int x, int y, Sink& sink) {
  const int W = s.stroke;
  const int b = s.bevel;
  for (int r = 0; r < W; ++r) {
    int edge_dist = r < (W - 1 - r) ? r : (W - 1 - r);
    int inset = b - edge_dist;
    if (inset < 0) inset = 0;
    sink.fill(x + inset, y + r, W - 2 * inset, 1);
  }
}

struct SegPos {
  bool horiz;
  int x;
  int y;
};

inline SegPos seg_pos(const Style& s, int seg_index) {
  const int W = s.stroke;
  const int b = s.bevel;
  const int hx = W - b;
  const int rx = s.core_x + W;
  const int uy = W - b;
  const int my = W + s.core_y;
  const int ly = 2 * W + s.core_y - b;
  const int by = 2 * s.core_y + 2 * W;
  switch (seg_index) {
    case 0:
      return {true, hx, 0};
    case 1:
      return {false, rx, uy};
    case 2:
      return {false, rx, ly};
    case 3:
      return {true, hx, by};
    case 4:
      return {false, 0, ly};
    case 5:
      return {false, 0, uy};
    default:
      return {true, hx, my};
  }
}

template <class Sink>
inline void draw_glyph(const Style& s, const Glyph& g, int x, int y,
                       Sink& sink) {
  if (g.kind == GlyphKind::Segments) {
    for (int i = 0; i < 7; ++i) {
      if (!(g.segs & (1u << i))) continue;
      SegPos p = seg_pos(s, i);
      if (p.horiz) {
        emit_hbar(s, x + p.x, y + p.y, sink);
      } else {
        emit_vbar(s, x + p.x, y + p.y, sink);
      }
    }
  } else if (g.kind == GlyphKind::Dots) {
    const int W = s.stroke;
    const int cx = dot_pad_for(s.bevel);
    const int cu = W + s.core_y / 2;
    const int cl = s.gh - W - s.core_y / 2;
    if (g.dots & DOT_TOP) emit_dot(s, x + cx, y + cu - W / 2, sink);
    if (g.dots & DOT_MID) emit_dot(s, x + cx, y + (s.gh - W) / 2, sink);
    if (g.dots & DOT_BOT) {
      int yy = (g.dots & DOT_TOP) ? (cl - W / 2) : (s.gh - W);
      emit_dot(s, x + cx, y + yy, sink);
    }
  }
}

inline Glyph glyph_for(char c) {
  switch (c) {
    case '0':
      return {GlyphKind::Segments,
              SEG_A | SEG_B | SEG_C | SEG_D | SEG_E | SEG_F, 0};
    case '1':
      return {GlyphKind::Segments, SEG_B | SEG_C, 0};
    case '2':
      return {GlyphKind::Segments, SEG_A | SEG_B | SEG_G | SEG_E | SEG_D, 0};
    case '3':
      return {GlyphKind::Segments, SEG_A | SEG_B | SEG_G | SEG_C | SEG_D, 0};
    case '4':
      return {GlyphKind::Segments, SEG_F | SEG_G | SEG_B | SEG_C, 0};
    case '5':
      return {GlyphKind::Segments, SEG_A | SEG_F | SEG_G | SEG_C | SEG_D, 0};
    case '6':
    case 'G':
      return {GlyphKind::Segments,
              SEG_A | SEG_F | SEG_G | SEG_E | SEG_C | SEG_D, 0};
    case '7':
      return {GlyphKind::Segments, SEG_A | SEG_B | SEG_C, 0};
    case '8':
      return {GlyphKind::Segments,
              SEG_A | SEG_B | SEG_C | SEG_D | SEG_E | SEG_F | SEG_G, 0};
    case '9':
    case 'g':
      return {GlyphKind::Segments,
              SEG_A | SEG_B | SEG_C | SEG_D | SEG_F | SEG_G, 0};
    case 'A':
    case 'a':
      return {GlyphKind::Segments,
              SEG_A | SEG_B | SEG_C | SEG_E | SEG_F | SEG_G, 0};
    case 'B':
    case 'b':
      return {GlyphKind::Segments, SEG_F | SEG_G | SEG_E | SEG_C | SEG_D, 0};
    case 'C':
      return {GlyphKind::Segments, SEG_A | SEG_D | SEG_E | SEG_F, 0};
    case 'c':
      return {GlyphKind::Segments, SEG_D | SEG_E | SEG_G, 0};
    case 'D':
    case 'd':
      return {GlyphKind::Segments, SEG_B | SEG_C | SEG_D | SEG_E | SEG_G, 0};
    case 'E':
    case 'e':
      return {GlyphKind::Segments, SEG_A | SEG_D | SEG_E | SEG_F | SEG_G, 0};
    case 'F':
    case 'f':
      return {GlyphKind::Segments, SEG_A | SEG_E | SEG_F | SEG_G, 0};
    case 'H':
      return {GlyphKind::Segments, SEG_B | SEG_C | SEG_E | SEG_F | SEG_G, 0};
    case 'h':
      return {GlyphKind::Segments, SEG_C | SEG_E | SEG_F | SEG_G, 0};
    case 'J':
    case 'j':
      return {GlyphKind::Segments, SEG_B | SEG_C | SEG_D | SEG_E, 0};
    case 'L':
      return {GlyphKind::Segments, SEG_D | SEG_E | SEG_F, 0};
    case 'l':
      return {GlyphKind::Segments, SEG_E | SEG_F, 0};
    case 'n':
    case 'N':
      return {GlyphKind::Segments, SEG_C | SEG_E | SEG_G, 0};
    case 'O':
      return {GlyphKind::Segments,
              SEG_A | SEG_B | SEG_C | SEG_D | SEG_E | SEG_F, 0};
    case 'o':
      return {GlyphKind::Segments, SEG_C | SEG_D | SEG_E | SEG_G, 0};
    case 'P':
    case 'p':
      return {GlyphKind::Segments, SEG_A | SEG_B | SEG_E | SEG_F | SEG_G, 0};
    case 's':
    case 'S':
      return {GlyphKind::Segments, SEG_A | SEG_C | SEG_D | SEG_F | SEG_G, 0};
    case 'U':
      return {GlyphKind::Segments, SEG_B | SEG_C | SEG_D | SEG_E | SEG_F, 0};
    case 'u':
      return {GlyphKind::Segments, SEG_C | SEG_D | SEG_E, 0};
    case '-':
      return {GlyphKind::Segments, SEG_G, 0};
    case '_':
      return {GlyphKind::Segments, SEG_D, 0};
    case '=':
      return {GlyphKind::Segments, SEG_G | SEG_D, 0};
    case ':':
      return {GlyphKind::Dots, 0, DOT_TOP | DOT_BOT};
    case '.':
      return {GlyphKind::Dots, 0, DOT_BOT};
    case ' ':
    default:
      return {GlyphKind::Blank, 0, 0};
  }
}

inline int glyph_cell_width(const Style& s, const Glyph& g) {
  switch (g.kind) {
    case GlyphKind::Segments:
      return s.gw;
    case GlyphKind::Dots:
      return s.stroke + 2 * dot_pad_for(s.bevel);
    default:
      return s.gw;
  }
}

inline int proportional_space_advance(const Style& s) {
  const int min_space = s.stroke + default_tracking(s);
  const int half_cell = round_div(s.gw, 2);
  return half_cell > min_space ? half_cell : min_space;
}

inline int font_height(const Style& s) { return s.gh; }

inline int ink_left(const Style& s, const Glyph& g) {
  if (g.kind != GlyphKind::Segments) return 0;
  const uint8_t m = g.segs;
  if (m & (SEG_F | SEG_E)) return 0;
  if (m & (SEG_A | SEG_G | SEG_D)) return s.stroke - s.bevel;
  if (m & (SEG_B | SEG_C)) return s.core_x + s.stroke;
  return s.gw / 2;
}

inline int ink_right(const Style& s, const Glyph& g) {
  if (g.kind != GlyphKind::Segments) return s.gw - 1;
  const uint8_t m = g.segs;
  if (m & (SEG_B | SEG_C)) return s.gw - 1;
  if (m & (SEG_A | SEG_G | SEG_D)) {
    return s.stroke + s.bevel + s.core_x - 1;
  }
  if (m & (SEG_F | SEG_E)) return s.stroke - 1;
  return s.gw / 2;
}

inline GlyphMetrics glyph_metrics(const Style& s, const Glyph& g,
                                  const TextOptions& options = TextOptions()) {
  GlyphMetrics m;
  m.cell_width = glyph_cell_width(s, g);
  if (g.kind == GlyphKind::Segments) {
    m.ink_left = ink_left(s, g);
    m.ink_right = ink_right(s, g);
  } else if (g.kind == GlyphKind::Dots) {
    m.ink_left = dot_pad_for(s.bevel);
    m.ink_right = m.ink_left + s.stroke - 1;
  } else {
    m.ink_left = 0;
    m.ink_right = -1;
  }
  if (options.spacing == SpacingMode::Proportional) {
    if (g.kind == GlyphKind::Segments) {
      m.advance = m.ink_right - m.ink_left + 1;
      m.draw_offset = -m.ink_left;
    } else if (g.kind == GlyphKind::Blank) {
      m.advance = proportional_space_advance(s);
      m.draw_offset = 0;
    } else {
      m.advance = m.cell_width;
      m.draw_offset = 0;
    }
  } else {
    m.advance = m.cell_width;
    m.draw_offset = 0;
  }
  return m;
}

inline int glyph_advance(const Style& s, const Glyph& g,
                         const TextOptions& options) {
  return glyph_metrics(s, g, options).advance;
}

inline int glyph_advance(const Style& s, const Glyph& g) {
  return glyph_metrics(s, g).advance;
}

inline int resolved_tracking(const Style& s, const TextOptions& options) {
  return options.tracking >= 0 ? options.tracking : default_tracking(s);
}

inline int text_width(const Style& s, const char* str,
                      TextOptions options = TextOptions()) {
  if (!str) return 0;
  const int tracking = resolved_tracking(s, options);
  int w = 0;
  bool first = true;
  for (const char* p = str; *p; ++p) {
    if (!first) w += tracking;
    w += glyph_metrics(s, glyph_for(*p), options).advance;
    first = false;
  }
  return w;
}

inline int text_width(const Style& s, const char* str, int tracking) {
  TextOptions options;
  options.tracking = tracking;
  return text_width(s, str, options);
}

template <class Sink>
inline int draw_text(const Style& s, const char* str, int x, int y, Sink& sink,
                     TextOptions options = TextOptions()) {
  if (!str) return 0;
  const int tracking = resolved_tracking(s, options);
  int cx = x;
  bool first = true;
  for (const char* p = str; *p; ++p) {
    if (!first) cx += tracking;
    const Glyph g = glyph_for(*p);
    const GlyphMetrics gm = glyph_metrics(s, g, options);
    draw_glyph(s, g, cx + gm.draw_offset, y, sink);
    cx += gm.advance;
    first = false;
  }
  return cx - x;
}

template <class Sink>
inline int draw_text(const Style& s, const char* str, int x, int y, Sink& sink,
                     int tracking) {
  TextOptions options;
  options.tracking = tracking;
  return draw_text(s, str, x, y, sink, options);
}

}  // namespace segfont
