// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

// strokefont_lgfx.h - optional LovyanGFX/M5GFX renderer for strokefont.
//
// The core (strokefont.h) emits resolved round-stroked line and circular-arc
// primitives. This adapter maps them to LGFX drawing calls, using the shared
// pixel geometry from strokefont_pixels.h; measurement stays in the core
// (strokefont::measure).

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

#include "strokefont.h"
#include "strokefont_pixels.h"

namespace strokefont {

template <class Gfx>
struct LgfxSink {
  Gfx* gfx;
  uint32_t color;

  struct Cap {
    int16_t x;
    int16_t y;
    int16_t diameter;
  };

  static constexpr int kMaxCaps = 96;
  Cap caps[kMaxCaps];
  int cap_count = 0;

  static int16_t clamp_i16(int v) {
    if (v < -32768) return -32768;
    if (v > 32767) return 32767;
    return static_cast<int16_t>(v);
  }

  static Cap cap_footprint(int cx, int cy, int diameter) {
    if (diameter < 1) diameter = 1;
    const int low = stroke_low_extent(diameter);
    return {clamp_i16(cx - low), clamp_i16(cy - low), clamp_i16(diameter)};
  }

  void paint_cap(const Cap& cap) {
    // A 1 px cap only repaints the pixel the stroke already drew, so skip it.
    const int diameter = cap.diameter;
    if (diameter <= 1) return;
    if (diameter & 1) {
      const int radius = diameter / 2;
      gfx->fillSmoothCircle(cap.x + radius, cap.y + radius, radius, color);
      return;
    }
    // Smooth circles are odd-diameter only; keep the even cap inside the
    // declared footprint instead of rounding up to a larger circle.
    gfx->fillSmoothRoundRect(cap.x, cap.y, diameter, diameter,
                             (diameter - 1) / 2, color);
  }

  void add_cap(int cx, int cy, int diameter) {
    const Cap cap = cap_footprint(cx, cy, diameter);
    for (int i = 0; i < cap_count; ++i) {
      if (caps[i].x == cap.x && caps[i].y == cap.y &&
          caps[i].diameter == cap.diameter) {
        return;
      }
    }
    if (cap_count < kMaxCaps) {
      caps[cap_count++] = cap;
      return;
    }
    paint_cap(cap);
  }

  void flush_caps() {
    for (int i = 0; i < cap_count; ++i) paint_cap(caps[i]);
    cap_count = 0;
  }

  void stroke_round_line(int x1, int y1, int x2, int y2, int diameter) {
    if (diameter < 1) diameter = 1;
    if (diameter <= 1) {
      gfx->drawLine(x1, y1, x2, y2, color);
      add_cap(x1, y1, diameter);
      add_cap(x2, y2, diameter);
      return;
    }

    const int low = stroke_low_extent(diameter);
    if (y1 == y2 || x1 == x2) {
      if (y1 == y2) {
        const int left = x1 < x2 ? x1 : x2;
        const int right = x1 < x2 ? x2 : x1;
        gfx->writeFillRect(left, y1 - low, right - left + 1, diameter, color);
      } else {
        const int top = y1 < y2 ? y1 : y2;
        const int bottom = y1 < y2 ? y2 : y1;
        gfx->writeFillRect(x1 - low, top, diameter, bottom - top + 1, color);
      }
    } else {
      gfx->drawWideLine(x1, y1, x2, y2, static_cast<float>(diameter - 1) * 0.5f,
                        color);
    }

    add_cap(x1, y1, diameter);
    add_cap(x2, y2, diameter);
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
          gfx->fillEllipseArc(acx, acy, outer, inner, outer, inner, s_deg,
                              e_deg, color);
        },
        [this](int x, int y, int length) {
          gfx->writeFillRect(x, y, length, 1, color);
        });

    add_cap(arc_point_x(cx, radius, start_deg),
            arc_point_y(cy, radius, start_deg), diameter);
    add_cap(arc_point_x(cx, radius, end_deg), arc_point_y(cy, radius, end_deg),
            diameter);
  }
};

// Draw `str` with its left ink edge at `x` (plus the glyph's side bearing) and
// its baseline at `baseline_y`, in `color` and the given Style. Returns the pen
// advance (the same value as measure().advance).
template <class Gfx>
inline int drawString(Gfx& gfx, const Style& s, const char* str, int x,
                      int baseline_y, uint32_t color) {
  LgfxSink<Gfx> sink{&gfx, color};
  gfx.startWrite();
  const int w = draw_text(s, str, static_cast<float>(x),
                          static_cast<float>(baseline_y), sink);
  sink.flush_caps();
  gfx.endWrite();
  return w;
}

}  // namespace strokefont
