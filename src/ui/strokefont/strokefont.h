// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

// strokefont.h - a small monoline stroke font, header-only and
// display-agnostic.
//
// It draws the unit text the seven-segment gauge font cannot render legibly:
// grams "g", seconds "s", milliliters "ml", pressure "bar", percent "%", and
// "degC". It is built to pair with segfont and, like segfont, glyphs are
// composed from a fixed kit rather than drawn freehand:
//
//   * segfont composes digits from seven straight segments.
//   * strokefont composes letters from cell stroke types - straight strokes
//     and quarter-circle arcs - placed on a grid, one per cell.
//
// A glyph is therefore an ASCII block: one row per line, top row first. Each
// cell names a stroke type the renderer expands into a primitive; cells that
// need several stroke types join them with "+" (for example "]+d"). Metrics
// come from that declared grid, while drawing consumes the expanded strokes
// inside it. A shape can be discussed and edited cell by cell without making
// text layout depend on the particular strokes used to draw the glyph.
//
//   Each glyph declares its own grid - a variable number of cell rows and
//   columns - and fills it (ink in the top and bottom rows, no headroom). A
//   lone glyph resolves the requested height to an integer cell pitch for that
//   glyph, like segfont resolves requested heights to coherent integer segment
//   geometry. Text resolves every glyph against the tallest glyph in the
//   string; shorter glyphs get empty rows above them so row height stays
//   constant across the text. Actual metrics can therefore land slightly below
//   the requested height. This is a proportional font, not monospace. Cells are
//   square in design space; after scaling, the bottom row's centerline is inset
//   by the stroke diameter, so the bottom ink row is the row immediately above
//   the baseline.
//
//     row 3   a  ~  b     "o" is four rows tall, three wide:   a~b
//     row 2   [  .  ]                                          [.]
//     row 1   [  .  ]                                          [.]
//     row 0   c  _  d     (baseline)                           c_d
//            col0 1  2      baseline = first pixel row below ink
//
//   Stroke types (each fills one cell; corner arcs have radius = one cell):
//
//     |  center vertical     a  corner arc, rounds top-left
//     -  center horizontal   b  corner arc, rounds top-right
//     [  left edge           c  corner arc, rounds bottom-left
//     ]  right edge          d  corner arc, rounds bottom-right
//     _  bottom edge         /  \  diagonals
//     ~  top edge            .  (space) empty
//
//   Corner arcs join the two cell edges they round, so edges and corners
//   compose into rounded shapes. The block above spells "o".
//
//   A cell may contain several strokes by joining their characters with "+".
//   The "+" does not add a column; it overlays the next stroke in the same
//   cell. Use "." for intentionally empty cells. Blank source lines are
//   ignored.
//
// Design space: baseline at y = 0, x-height at y = 4 grid units, y up, x right.
// Coordinates are stroke centerlines; ink extends by the renderer's exact
// round stroke. Everything is above the baseline, nothing below it - the unit
// stays bottom-aligned with the magnitude beside it.

#pragma once

#include <math.h>
#include <stdint.h>

#include "../font_weight.h"

namespace strokefont {

// A cell is one grid unit. Each glyph declares its own grid and resolves that
// grid to an integer pixel pitch near the requested height.
constexpr float kCell = 1.0f;

// A stroke type expanded into design-space geometry.
struct Prim {
  enum Kind : uint8_t { Segment, Arc } kind;
  // Segment: from (a, b) to (c, d).
  // Arc: center (a, b), radius c, from angle e to angle f in degrees
  // (counter-clockwise from +x, y up). Glyph arcs are quarter circles with a
  // single radius; d is unused.
  float a, b, c, d, e, f;
};

// A glyph is an ASCII block: rows top-first, one cell expression per cell.
struct Glyph {
  uint8_t rows;
  uint8_t cols;
  const char* art;
};

struct Style {
  int height = 20;  // Requested pixel ink height.
  int stroke = 3;   // Nominal monoline thickness in pixels.
};

using Weight = font_weight::Weight;

inline int stroke_diameter_for_nominal(int stroke) {
  if (stroke < 1) stroke = 1;
  return stroke;
}

inline int stroke_diameter(const Style& s) {
  return stroke_diameter_for_nominal(s.stroke);
}

inline int round_px(float v) {
  return static_cast<int>(v >= 0.0f ? v + 0.5f : v - 0.5f);
}

inline float stroke_center_offset(const Style& s) {
  return static_cast<float>(stroke_diameter(s) - 1) * 0.5f;
}

inline float stroke_baseline_offset(const Style& s) {
  return static_cast<float>(stroke_diameter(s) + 1) * 0.5f;
}

// Same height/stroke family as segfont, so a unit set near a run of digits
// lands on a matching stroke.
inline int height_divisor_for(Weight weight) {
  return font_weight::height_divisor_for(weight);
}

inline int natural_stroke_for_height(int height_px, Weight weight) {
  return font_weight::natural_stroke_for_height(height_px, weight);
}

inline Style style_for_height(int height_px, int stroke) {
  if (height_px < 1) height_px = 1;
  if (stroke < 1) stroke = 1;
  while (stroke > 1 && stroke_diameter_for_nominal(stroke) > height_px)
    --stroke;
  Style s;
  s.height = height_px;
  s.stroke = stroke;
  return s;
}

inline Style style_for_height(int height_px, Weight weight = Weight::Regular) {
  return style_for_height(
      height_px, font_weight::natural_stroke_for_height(height_px, weight));
}

// --- Glyph table -----------------------------------------------------------
//
// Each glyph is an ASCII block of stroke types. See the kit in the file header.

inline Glyph consume_glyph(const char*& str) {
  switch (static_cast<unsigned char>(*str++)) {
    case 'a':
      return {4, 3,
              "a~b\n"
              "..]\n"
              "a~b+]\n"
              "c_d+]"};
    case 'b':
      return {6, 3,
              "[..\n"
              "[..\n"
              "[+a~b\n"
              "[.]\n"
              "[.]\n"
              "[+c_d"};
    case 'c':
      return {3, 2,
              "a~\n"
              "[.\n"
              "c_"};
    case 'C':
      return {4, 3,
              "a~b\n"
              "[..\n"
              "[..\n"
              "c_d"};
    case 'd':
      return {5, 3,
              "..]\n"
              "..]\n"
              "a~b+]\n"
              "[.]\n"
              "c_d+]"};
    case 'e':
      return {4, 3,
              "a~b\n"
              "[+__d\n"
              "[..\n"
              "c_d"};
    case 'g':
      return {5, 3,
              "a~b+]\n"
              "[.]\n"
              "c_d+]\n"
              "..]\n"
              "c_d"};
    case 'G':
      // Taller slender g (not G-shaped). Its tail stays open at small sizes.
      return {6, 3,
              "a~b+]\n"
              "[.]\n"
              "c_d+]\n"
              "..]\n"
              "..]\n"
              "c_d"};
    case 'l':
      return {4, 1,
              "[\n"
              "[\n"
              "[\n"
              "c"};
    case 'm':
      return {3, 4,
              "[+abab\n"
              "[].]\n"
              "[].]"};
    case 'o':
      return {4, 3,
              "a~b\n"
              "[.]\n"
              "[.]\n"
              "c_d"};
    case 'r':
      return {4, 3,
              "[+a~b\n"
              "[.]\n"
              "[..\n"
              "[.."};
    case 's':
      return {4, 3,
              "a~b\n"
              "c_.\n"
              "..b\n"
              "c_d\n"};
    case 'S':
      // Taller slender S. Doesn't close (become an 8) at small sizes.
      return {6, 3,
              "a~b\n"
              "[..\n"
              "c_.\n"
              "..b\n"
              "..]\n"
              "c_d\n"};
    case '%':
      return {5, 5,
              "ab...\n"
              "cd./.\n"
              "../..\n"
              "./.ab\n"
              "...cd"};
    case '/':
      return {5, 3,
              "...\n"
              "../\n"
              "./.\n"
              "/..\n"
              "..."};
    case 0xC2:
      switch (static_cast<unsigned char>(*str++)) {
        case 0xB0:  // °
          return {4, 2,
                  "ab\n"
                  "cd\n"
                  "..\n"
                  ".."};
        case 0:
          // Avoid skipping over a string terminator
          --str;
          return {0, 0, nullptr};
        default:
          return {0, 0, nullptr};
      }
    default:
      return {0, 0, nullptr};
  }
}

inline Glyph missing_glyph() {
  return {2, 2,
          "\\/\n"
          "/\\"};
}

inline Glyph consume_glyph_drawable(const char*& str) {
  const Glyph g = consume_glyph(str);
  return g.art ? g : missing_glyph();
}

// --- Stroke-type expansion -------------------------------------------------
//
// A cell character becomes one primitive positioned at the cell whose
// lower-left corner is (cx, cy), in grid units. Returns false for empty cells.

inline bool cell_prim(char ch, float cx, float cy, Prim& out) {
  const float s = kCell;
  switch (ch) {
    case '|':
      out = {Prim::Segment, cx + s / 2, cy, cx + s / 2, cy + s, 0, 0};
      return true;
    case '-':
      out = {Prim::Segment, cx, cy + s / 2, cx + s, cy + s / 2, 0, 0};
      return true;
    case '_':
      out = {Prim::Segment, cx, cy, cx + s, cy, 0, 0};
      return true;
    case '~':
      out = {Prim::Segment, cx, cy + s, cx + s, cy + s, 0, 0};
      return true;
    case '[':
      out = {Prim::Segment, cx, cy, cx, cy + s, 0, 0};
      return true;
    case ']':
      out = {Prim::Segment, cx + s, cy, cx + s, cy + s, 0, 0};
      return true;
    case '/':
      out = {Prim::Segment, cx, cy, cx + s, cy + s, 0, 0};
      return true;
    case '\\':
      out = {Prim::Segment, cx, cy + s, cx + s, cy, 0, 0};
      return true;
    case 'a':  // bulge top-left: center at cell bottom-right corner
      out = {Prim::Arc, cx + s, cy, s, s, 90, 180};
      return true;
    case 'b':  // bulge top-right: center at cell bottom-left corner
      out = {Prim::Arc, cx, cy, s, s, 0, 90};
      return true;
    case 'c':  // bulge bottom-left: center at cell top-right corner
      out = {Prim::Arc, cx + s, cy + s, s, s, 180, 270};
      return true;
    case 'd':  // bulge bottom-right: center at cell top-left corner
      out = {Prim::Arc, cx, cy + s, s, s, 270, 360};
      return true;
    default:  // '.' ' ' and anything else: empty
      return false;
  }
}

inline int snap_center(float v, int diameter) {
  // Even-diameter strokes are centered between pixels in design space. LGFX
  // primitives take integer centers, so snap those centers down; paired with
  // stroke_low/high_extent this preserves the declared ink box.
  if ((diameter & 1) == 0) return static_cast<int>(floorf(v));
  return round_px(v);
}

inline int display_arc_angle(float design_angle) {
  int a = round_px(360.0f - design_angle) % 360;
  if (a < 0) a += 360;
  return a;
}

inline int centerline_span(const Style& s);
inline int cell_pitch_for_rows(const Style& s, int rows);
inline int cell_pitch(const Style& s, const Glyph& g);

// Resolved centerline grid for one glyph draw. Coordinates are snapped once at
// the glyph origin and every primitive uses offsets from that same grid, so
// straight strokes and corner arcs agree at joins.
struct ResolvedGrid {
  int rows = 0;
  int cols = 0;
  int diameter = 1;
  int pitch = 0;
  int x0 = 0;
  int y0 = 0;  // Display-space centerline for design y=0.

  int x(float grid_x) const { return x0 + round_px(grid_x * pitch); }
  int y(float grid_y) const { return y0 - round_px(grid_y * pitch); }
};

inline ResolvedGrid resolve_grid(const Style& s, const Glyph& g, float origin_x,
                                 float baseline_y, int grid_rows) {
  ResolvedGrid grid;
  grid.rows = grid_rows > g.rows ? grid_rows : g.rows;
  grid.cols = g.cols;
  grid.diameter = stroke_diameter(s);
  if (grid.rows < 1 || grid.cols < 1) return grid;
  grid.pitch = cell_pitch_for_rows(s, grid.rows);
  if (grid.pitch <= 0) return grid;
  const float centerline_baseline = baseline_y - stroke_baseline_offset(s);
  grid.x0 = snap_center(origin_x, grid.diameter);
  grid.y0 = snap_center(centerline_baseline, grid.diameter);
  return grid;
}

inline ResolvedGrid resolve_grid(const Style& s, const Glyph& g, float origin_x,
                                 float baseline_y) {
  return resolve_grid(s, g, origin_x, baseline_y, g.rows);
}

// Emit one primitive in resolved display space. Sinks implement:
//
//   void stroke_round_line(int x1, int y1, int x2, int y2,
//                          int diameter);
//   void stroke_round_arc(int cx, int cy, int radius,
//                         int start_deg, int end_deg, int diameter);
//
// Every glyph arc is a quarter of a circle, so it has a single radius. Arc
// angles are in display coordinates: 0 is 3 o'clock and angles increase
// clockwise, matching the arc drawing API.
template <class Sink>
inline void emit_prim(const Prim& p, const ResolvedGrid& grid, Sink& sink) {
  if (p.kind == Prim::Segment) {
    sink.stroke_round_line(grid.x(p.a), grid.y(p.b), grid.x(p.c), grid.y(p.d),
                           grid.diameter);
  } else {
    sink.stroke_round_arc(grid.x(p.a), grid.y(p.b), round_px(p.c * grid.pitch),
                          display_arc_angle(p.f), display_arc_angle(p.e),
                          grid.diameter);
  }
}

inline bool row_blank(const char* begin, const char* end) {
  while (begin < end) {
    const char ch = *begin++;
    if (ch != ' ' && ch != '\t' && ch != '\r') return false;
  }
  return true;
}

inline const char* next_row(const char* p, const char** begin,
                            const char** end) {
  *begin = p;
  while (*p && *p != '\n') ++p;
  *end = p;
  if (*end > *begin && (*end)[-1] == '\r') --(*end);
  while (*end > *begin && ((*end)[-1] == ' ' || (*end)[-1] == '\t')) --(*end);
  return *p == '\n' ? p + 1 : p;
}

template <class Fn>
inline int parse_row_cells(const char* begin, const char* end, Fn&& fn) {
  int cols = 0;
  int last_col = -1;
  bool join_next = false;
  for (const char* p = begin; p < end; ++p) {
    const char ch = *p;
    if (ch == '+') {
      join_next = true;
      continue;
    }
    if (ch == ' ' || ch == '\t') {
      if (join_next) continue;  // whitespace after "+"
      const char* q = p + 1;
      while (q < end && (*q == ' ' || *q == '\t')) ++q;
      if (q < end && *q == '+') {
        p = q - 1;  // whitespace before "+"
        continue;
      }
    }

    int col = cols;
    if (join_next && last_col >= 0) {
      col = last_col;
    } else {
      last_col = col;
      ++cols;
    }
    join_next = false;
    fn(col, ch);
  }
  return cols;
}

// Requested centerline budget. Per-glyph drawing resolves this to an integer
// cell pitch so all cells in a glyph stay square and equal-sized.
inline int centerline_span(const Style& s) {
  const int span = s.height - stroke_diameter(s);
  return span > 0 ? span : 0;
}

inline int cell_pitch_for_rows(const Style& s, int rows) {
  if (rows < 1) return 0;
  const int span = centerline_span(s);
  const int pitch = span / rows;
  return pitch > 0 ? pitch : 1;
}

inline int cell_pitch(const Style& s, const Glyph& g) {
  return cell_pitch_for_rows(s, g.rows);
}

inline float grid_scale(const Style& s, const Glyph& g) {
  return static_cast<float>(cell_pitch(s, g));
}

// Walk a glyph's cells and emit every stroke type's centerline. Rows are
// written top-first. Within a row, "+" overlays the next stroke in the previous
// logical cell instead of advancing the column.
template <class Sink>
inline void emit_cells(const Glyph& g, const Style& s, float ox,
                       float baseline_y, int grid_rows, Sink& sink) {
  const int glyph_row_count = g.rows;
  const ResolvedGrid grid = resolve_grid(s, g, ox, baseline_y, grid_rows);
  if (glyph_row_count < 1 || grid.rows < 1 || grid.pitch <= 0) return;
  const char* p = g.art;
  int line = 0;
  while (*p) {
    const char* begin = nullptr;
    const char* end = nullptr;
    p = next_row(p, &begin, &end);
    if (row_blank(begin, end)) continue;
    const int grid_row = (glyph_row_count - 1) - line;
    parse_row_cells(begin, end, [&](int col, char stroke) {
      Prim pr;
      if (cell_prim(stroke, col * kCell, grid_row * kCell, pr))
        emit_prim(pr, grid, sink);
    });
    ++line;
  }
}

template <class Sink>
inline void emit_cells(const Glyph& g, const Style& s, float ox,
                       float baseline_y, Sink& sink) {
  emit_cells(g, s, ox, baseline_y, g.rows, sink);
}

// --- Metrics and placement -------------------------------------------------

struct Metrics {
  int advance = 0;  // Pen advance for the string.
  int ascent = 0;   // Ink above the baseline.
  int descent = 0;  // Ink below the baseline (0 for this no-descender font).
};

struct GlyphMetrics {
  Metrics metrics;
  // Add to x before drawing so ink lands after the side bearing.
  float draw_offset = 0.0f;
};

inline int side_bearing(const Style& s) {
  int b = (s.height + 5) / 10;  // ~0.1 of the glyph height
  return b < s.stroke ? s.stroke : b;
}

inline int text_grid_rows(const char* str) {
  int rows = 0;
  if (!str) return rows;
  for (const char* p = str; *p;) {
    const int glyph_row_count = consume_glyph_drawable(p).rows;
    if (glyph_row_count > rows) rows = glyph_row_count;
  }
  return rows;
}

inline GlyphMetrics glyph_metrics(const Style& s, const Glyph& g,
                                  int grid_rows) {
  GlyphMetrics gm;
  if (!g.art) return gm;
  if (g.rows < 1 || g.cols < 1) return gm;
  if (grid_rows < g.rows) grid_rows = g.rows;
  const int pitch = cell_pitch_for_rows(s, grid_rows);
  if (pitch <= 0) return gm;
  const int bearing = side_bearing(s);
  const int centerline_width = g.cols * pitch;
  const int actual_height = grid_rows * pitch + stroke_diameter(s);
  gm.metrics.advance = centerline_width + stroke_diameter(s) + 2 * bearing;
  gm.metrics.ascent = actual_height;
  gm.metrics.descent = 0;
  gm.draw_offset = static_cast<float>(bearing) + stroke_center_offset(s);
  return gm;
}

inline GlyphMetrics glyph_metrics(const Style& s, const Glyph& g) {
  return glyph_metrics(s, g, g.rows);
}

inline Metrics measure_glyph(const Style& s, const Glyph& g) {
  return glyph_metrics(s, g).metrics;
}

template <class Sink>
inline int draw_glyph(const Style& s, const Glyph& g, float x, float baseline_y,
                      Sink& sink) {
  if (!g.art) return 0;
  const GlyphMetrics gm = glyph_metrics(s, g);
  if (gm.metrics.advance <= 0) return 0;
  emit_cells(g, s, x + gm.draw_offset, baseline_y, sink);
  return gm.metrics.advance;
}

template <class Sink>
inline void draw_glyph_raw(const Style& s, const Glyph& g, float origin_x,
                           float baseline_y, Sink& sink) {
  emit_cells(g, s, origin_x, baseline_y, sink);
}

inline Metrics measure(const Style& s, const char* str, int rows) {
  Metrics m;
  if (!str) return m;
  if (rows < 1) rows = text_grid_rows(str);
  for (const char* p = str; *p;) {
    const Glyph g = consume_glyph_drawable(p);
    const Metrics gm = glyph_metrics(s, g, rows).metrics;
    m.advance += gm.advance;
    if (gm.ascent > m.ascent) m.ascent = gm.ascent;
    if (gm.descent > m.descent) m.descent = gm.descent;
  }
  return m;
}

inline Metrics measure(const Style& s, const char* str) {
  return measure(s, str, text_grid_rows(str));
}

template <class Sink>
inline int draw_text(const Style& s, const char* str, float x, float baseline_y,
                     int rows, Sink& sink) {
  if (!str) return 0;
  if (rows < 1) rows = text_grid_rows(str);
  float pen = x;
  for (const char* p = str; *p;) {
    const Glyph g = consume_glyph_drawable(p);
    const GlyphMetrics gm = glyph_metrics(s, g, rows);
    if (gm.metrics.advance <= 0) continue;
    emit_cells(g, s, pen + gm.draw_offset, baseline_y, rows, sink);
    pen += gm.metrics.advance;
  }
  return static_cast<int>(pen - x);
}

template <class Sink>
inline int draw_text(const Style& s, const char* str, float x, float baseline_y,
                     Sink& sink) {
  return draw_text(s, str, x, baseline_y, text_grid_rows(str), sink);
}

}  // namespace strokefont
