// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

// strokefont_raster.h - host-only software rasterizer that approximates the
// on-device strokefont pixels, for inspection and debugging. Not firmware.
//
// The core (strokefont.h) emits resolved round-stroke primitives; this sink
// rasterizes them into a 1-byte-per-pixel bitmap. Fidelity by primitive:
//
//   * Axis-aligned round strokes and arcs: pixel-exact. Straight runs and
//     even-width arcs go through the same shared geometry the device renderer
//     uses (strokefont_pixels.h); odd-width arcs go through a verbatim port of
//     LovyanGFX's LGFXBase::fill_arc_helper / fillEllipseArc
//     (M5GFX/src/lgfx/v1/LGFXBase.cpp) - re-sync if LovyanGFX changes.
//   * Diagonal strokes and end caps: approximate (a distance field and a simple
//     disc/rounded-rect), close enough for shape inspection but not bit-exact.
//
// The dedup/flush ordering of the device cap buffer is not reproduced: caps are
// painted immediately, which yields identical pixels for a single-color glyph.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "../strokefont.h"
#include "../strokefont_pixels.h"

namespace strokefont {
namespace raster {

struct Bitmap {
  int w = 0;
  int h = 0;
  std::vector<uint8_t> px;  // 0 = empty, 1 = ink

  Bitmap(int width, int height)
      : w(width), h(height), px(static_cast<size_t>(width) * height, 0) {}

  bool inside(int x, int y) const { return x >= 0 && y >= 0 && x < w && y < h; }
  uint8_t at(int x, int y) const { return inside(x, y) ? px[y * w + x] : 0; }
  void put(int x, int y) {
    if (inside(x, y)) px[y * w + x] = 1;
  }
  void hline(int x, int y, int len) {
    for (int i = 0; i < len; ++i) put(x + i, y);
  }
  void fill_rect(int x, int y, int rw, int rh) {
    for (int j = 0; j < rh; ++j) hline(x, y + j, rw);
  }
};

// --- verbatim port of LovyanGFX arc rasterization ---------------------------
namespace lgfx_port {

inline void fill_arc_helper(Bitmap& b, int32_t cx, int32_t cy,
                            int32_t oradius_x, int32_t iradius_x,
                            int32_t oradius_y, int32_t iradius_y, float start,
                            float end) {
  const float deg_to_rad = 0.017453292519943295769f;
  const int32_t _clip_l = 0, _clip_t = 0, _clip_r = b.w - 1, _clip_b = b.h - 1;
  float s_cos = cosf(start * deg_to_rad);
  float e_cos = cosf(end * deg_to_rad);
  float sslope = s_cos / sinf(start * deg_to_rad);
  float eslope = -1000000;
  if (end != 360.0f) eslope = e_cos / sinf(end * deg_to_rad);
  float swidth = 0.5f / s_cos;
  float ewidth = -0.5f / e_cos;

  bool start180 = !(start < 180);
  bool end180 = end < 180;
  bool reversed = start + 180 < end || (end < start && start < end + 180);

  int32_t xleft = -oradius_x;
  int32_t xright = oradius_x + 1;
  int32_t y = -oradius_y;
  int32_t ye = oradius_y;
  if (!reversed) {
    if ((end >= 270 || end < 90) && (start >= 270 || start < 90))
      xleft = 0;
    else if (end < 270 && end >= 90 && start < 270 && start >= 90)
      xright = 1;
    if (end >= 180 && start >= 180)
      ye = 0;
    else if (end < 180 && start < 180)
      y = 0;
  }
  if (y < _clip_t - cy) y = _clip_t - cy;
  if (ye > _clip_b - cy + 1) ye = _clip_b - cy + 1;
  if (xleft < _clip_l - cx) xleft = _clip_l - cx;
  if (xright > _clip_r - cx + 1) xright = _clip_r - cx + 1;

  bool trueCircle = (oradius_x == oradius_y) && (iradius_x == iradius_y);

  int32_t iradius_y2 = iradius_y * (iradius_y - 1);
  int32_t iradius_x2 = iradius_x * (iradius_x - 1);
  float irad_rate =
      iradius_x2 && iradius_y2 ? (float)iradius_x2 / (float)iradius_y2 : 0;
  int32_t oradius_y2 = oradius_y * (oradius_y + 1);
  int32_t oradius_x2 = oradius_x * (oradius_x + 1);
  float orad_rate =
      oradius_x2 && oradius_y2 ? (float)oradius_x2 / (float)oradius_y2 : 0;

  do {
    int32_t y2 = y * y;
    int32_t compare_o = oradius_y2 - y2;
    int32_t compare_i = iradius_y2 - y2;
    if (!trueCircle) {
      compare_i = floorf(compare_i * irad_rate);
      compare_o = ceilf(compare_o * orad_rate);
    }
    int32_t xe = ceilf(sqrtf((float)(compare_o < 0 ? 0 : compare_o)));
    int32_t x = 1 - xe;
    if (x < xleft) x = xleft;
    if (xe > xright) xe = xright;
    float ysslope = (y + swidth) * sslope;
    float yeslope = (y + ewidth) * eslope;
    int len = 0;
    do {
      bool flg1 = start180 != (x <= ysslope);
      bool flg2 = end180 != (x <= yeslope);
      int32_t x2 = x * x;
      if (x2 >= compare_i && ((flg1 && flg2) || (reversed && (flg1 || flg2))) &&
          x != xe && x2 < compare_o) {
        ++len;
      } else {
        if (len) {
          b.hline(cx + x - len, cy + y, len);
          len = 0;
        }
        if (x2 >= compare_o) break;
        if (x < 0 && x2 < compare_i) x = -x;
      }
    } while (++x <= xe);
  } while (++y <= ye);
}

inline void fillEllipseArc(Bitmap& b, int32_t x, int32_t y, int32_t r0x,
                           int32_t r1x, int32_t r0y, int32_t r1y, float start,
                           float end) {
  if (r0x < r1x) std::swap(r0x, r1x);
  if (r0y < r1y) std::swap(r0y, r1y);
  if (r1x < 0 || r1y < 0) return;
  bool ring = fabsf(start - end) >= 360;
  start = fmodf(start, 360);
  end = fmodf(end, 360);
  if (start < 0.0f) start = fmodf(start + 360.0f, 360);
  if (end < 0.0f) end = fmodf(end + 360.0f, 360);
  if (ring && (fabsf(start - end) <= 0.0001f)) {
    start = 0.f;
    end = 360.0f;
  }
  fill_arc_helper(b, x, y, r0x, r1x, r0y, r1y, start, end);
}

}  // namespace lgfx_port

// Sink implementing the interface strokefont's core draws through. Feed it to
// draw_text / draw_glyph.
struct Sink {
  Bitmap* bmp;
  bool draw_caps = true;

  // A cap is a disc (odd) or rounded square (even), approximating LGFX. A 1 px
  // cap only repaints a pixel the stroke already drew, so skip it.
  void paint_cap(int cx, int cy, int d) {
    if (!draw_caps || d <= 1) return;
    const int low = stroke_low_extent(d);
    const int x0 = cx - low, y0 = cy - low;
    const int rr = (d - 1) / 2;
    const float r = d / 2, ccx = x0 + r, ccy = y0 + r;
    for (int j = 0; j < d; ++j)
      for (int i = 0; i < d; ++i) {
        if (d & 1) {
          if ((x0 + i - ccx) * (x0 + i - ccx) +
                  (y0 + j - ccy) * (y0 + j - ccy) <=
              r * r + 0.25f)
            bmp->put(x0 + i, y0 + j);
        } else {
          // rounded-square: reject only the four corners past radius rr
          const bool lx = i < rr, hx = i >= d - rr;
          const bool ly = j < rr, hy = j >= d - rr;
          if ((lx || hx) && (ly || hy)) {
            const float ox = lx ? rr - 0.5f : d - rr - 0.5f;
            const float oy = ly ? rr - 0.5f : d - rr - 0.5f;
            if ((i - ox) * (i - ox) + (j - oy) * (j - oy) > rr * rr + 0.5f)
              continue;
          }
          bmp->put(x0 + i, y0 + j);
        }
      }
  }

  // Approximate wide diagonal as a distance-field capsule (device uses LGFX
  // drawWideLine; not bit-exact, but no shipping glyph relies on diagonals).
  void wide_line(int x1, int y1, int x2, int y2, int d) {
    const float r = (d - 1) * 0.5f + 0.5f;
    const int minx = std::min(x1, x2) - d, maxx = std::max(x1, x2) + d;
    const int miny = std::min(y1, y2) - d, maxy = std::max(y1, y2) + d;
    const float dx = x2 - x1, dy = y2 - y1;
    const float len2 = dx * dx + dy * dy;
    for (int py = miny; py <= maxy; ++py)
      for (int px = minx; px <= maxx; ++px) {
        float t = len2 > 0 ? ((px - x1) * dx + (py - y1) * dy) / len2 : 0;
        t = t < 0 ? 0 : (t > 1 ? 1 : t);
        const float qx = x1 + t * dx - px, qy = y1 + t * dy - py;
        if (qx * qx + qy * qy <= r * r) bmp->put(px, py);
      }
  }

  void stroke_round_line(int x1, int y1, int x2, int y2, int diameter) {
    if (diameter < 1) diameter = 1;
    const int low = stroke_low_extent(diameter);
    if (y1 == y2) {
      const int l = std::min(x1, x2), r = std::max(x1, x2);
      bmp->fill_rect(l, y1 - low, r - l + 1, diameter);
    } else if (x1 == x2) {
      const int t = std::min(y1, y2), btm = std::max(y1, y2);
      bmp->fill_rect(x1 - low, t, diameter, btm - t + 1);
    } else {
      wide_line(x1, y1, x2, y2, diameter);
    }
    paint_cap(x1, y1, diameter);
    paint_cap(x2, y2, diameter);
  }

  void stroke_round_arc(int cx, int cy, int radius, int start_deg, int end_deg,
                        int diameter) {
    if (diameter < 1) diameter = 1;
    if (radius <= 0) {
      stroke_round_line(arc_point_x(cx, radius, start_deg),
                        arc_point_y(cy, radius, start_deg),
                        arc_point_x(cx, radius, end_deg),
                        arc_point_y(cy, radius, end_deg), diameter);
      return;
    }
    fill_round_arc(
        cx, cy, radius, start_deg, end_deg, diameter,
        [this](int acx, int acy, int outer, int inner, int s_deg, int e_deg) {
          lgfx_port::fillEllipseArc(*bmp, acx, acy, outer, inner, outer, inner,
                                    (float)s_deg, (float)e_deg);
        },
        [this](int x, int y, int length) { bmp->hline(x, y, length); });
    paint_cap(arc_point_x(cx, radius, start_deg),
              arc_point_y(cy, radius, start_deg), diameter);
    paint_cap(arc_point_x(cx, radius, end_deg),
              arc_point_y(cy, radius, end_deg), diameter);
  }
};

// Render a string to a tightly-sized bitmap with `margin` px of padding.
inline Bitmap render(const Style& s, const char* str, bool draw_caps = true,
                     int margin = 3) {
  const Metrics m = measure(s, str);
  Bitmap bmp(m.advance + 2 * margin, m.ascent + 2 * margin);
  Sink sink{&bmp, draw_caps};
  draw_text(s, str, static_cast<float>(margin),
            static_cast<float>(bmp.h - margin), sink);
  return bmp;
}

}  // namespace raster
}  // namespace strokefont
