// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

// strokefont_pixels.h - exact pixel geometry of round strokes.
//
// The core (strokefont.h) emits centerline primitives; this header defines
// which pixels a round stroke of a given diameter covers. It is shared by the
// device renderer (strokefont_lgfx.h) and the host tools so they draw the same
// pixels. Display-agnostic: output leaves through caller-supplied span
// callbacks or plain integer results.

#pragma once

#include <math.h>

#include "strokefont.h"

namespace strokefont {

inline int norm_angle(int deg) {
  deg %= 360;
  return deg < 0 ? deg + 360 : deg;
}

// Point on a circle in display space (0 at 3 o'clock, clockwise, y down).
// Axis angles take the exact branch so stroke endpoints land on the
// centerline grid regardless of float rounding.
inline int arc_point_x(int cx, int radius, int deg) {
  deg = norm_angle(deg);
  if (deg == 0) return cx + radius;
  if (deg == 90 || deg == 270) return cx;
  if (deg == 180) return cx - radius;
  return cx + round_px(cosf(deg * 0.017453292519943295f) * radius);
}

inline int arc_point_y(int cy, int radius, int deg) {
  deg = norm_angle(deg);
  if (deg == 0 || deg == 180) return cy;
  if (deg == 90) return cy + radius;
  if (deg == 270) return cy - radius;
  return cy + round_px(sinf(deg * 0.017453292519943295f) * radius);
}

// A stroke of `diameter` extends this many pixels to each side of its
// centerline coordinate. Odd diameters split evenly; even diameters put the
// extra pixel on the high side, so the covered span c-low..c+high is symmetric
// about c+0.5 - an even stroke's true centerline sits on the half-pixel grid.
inline int stroke_low_extent(int diameter) {
  if (diameter < 1) diameter = 1;
  return (diameter - 1) / 2;
}
inline int stroke_high_extent(int diameter) {
  if (diameter < 1) diameter = 1;
  return diameter - 1 - stroke_low_extent(diameter);
}

// Fill one quarter-circle arc of an even-diameter round stroke, emitting one
// horizontal pixel run per scanline through hline(x, y, length).
//
// Straight runs of an even stroke cover pixel centers symmetric about the
// half-pixel centerline (see stroke_low_extent). The arc that continues such
// runs without a step is therefore a circular annulus centered on the
// half-pixel grid at (cx+0.5, cy+0.5) with radii radius -/+ diameter/2 - a
// shape no integer-center, integer-radius ellipse arc can express, hence this
// scanline fill. Doubling all distances keeps the test in integers: the pixel
// at offset (kx, ky) from the arc center, with kx, ky >= 1 inside the
// quadrant, is ink iff
//
//   (2*radius - diameter)^2 <= X^2 + Y^2 <= (2*radius + diameter)^2
//
// where X = 2*kx - 1 and Y = 2*ky - 1 are the doubled pixel-center distances.
// X and Y are odd, so X^2 + Y^2 is 2 mod 8 while both bounds are even squares:
// no pixel ever lands exactly on a boundary, and the quadrant edges fall
// between pixel rows and columns, so clipping is a clean half-plane split.
// Angles are display-space degrees (0 at 3 o'clock, clockwise); the span must
// stay within one quadrant.
template <class HLine>
inline void fill_even_quarter_arc(int cx, int cy, int radius, int start_deg,
                                  int end_deg, int diameter, HLine&& hline) {
  int span = norm_angle(end_deg - start_deg);
  if (span == 0) span = 360;
  const int mid = norm_angle(start_deg + span / 2);
  const bool x_positive = mid < 90 || mid > 270;
  const bool y_positive = mid > 0 && mid < 180;

  const int outer = 2 * radius + diameter;
  const int inner = 2 * radius - diameter;
  const int outer2 = outer * outer;
  const int inner2 = inner > 0 ? inner * inner : 0;
  for (int ky = 1;; ++ky) {
    const int y = 2 * ky - 1;
    const int bound_o = outer2 - y * y;
    if (bound_o < 1) break;
    // Largest odd x with x^2 <= bound_o (the fix-up loops absorb sqrtf
    // rounding; they run at most once).
    int x_max = static_cast<int>(sqrtf(static_cast<float>(bound_o)));
    while (x_max * x_max > bound_o) --x_max;
    while ((x_max + 1) * (x_max + 1) <= bound_o) ++x_max;
    if (!(x_max & 1)) --x_max;
    if (x_max < 1) continue;
    // Smallest odd x with x^2 >= bound_i.
    const int bound_i = inner2 - y * y;
    int x_min = 1;
    if (bound_i >= 1) {
      int t = static_cast<int>(sqrtf(static_cast<float>(bound_i)));
      while (t * t < bound_i) ++t;
      while (t > 0 && (t - 1) * (t - 1) >= bound_i) --t;
      if (!(t & 1)) ++t;
      x_min = t < 1 ? 1 : t;
    }
    if (x_min > x_max) continue;
    const int kx_min = (x_min + 1) / 2;
    const int kx_max = (x_max + 1) / 2;
    hline(x_positive ? cx + kx_min : cx + 1 - kx_max,
          y_positive ? cy + ky : cy + 1 - ky, kx_max - kx_min + 1);
  }
}

// Fill one quarter arc of a round stroke, dispatching on stroke parity. An
// odd width is a circular annulus on the arc's integer center, which an
// ellipse-arc primitive expresses directly; an even width goes through the
// exact scanline fill above. Renderers supply their two primitives:
//
//   fill_annulus(cx, cy, outer_radius, inner_radius, start_deg, end_deg)
//   hline(x, y, length)
//
// Keeping the dispatch and radii here means every renderer draws the same
// pixels for the same primitive; a sink only adapts the drawing calls.
template <class FillAnnulus, class HLine>
inline void fill_round_arc(int cx, int cy, int radius, int start_deg,
                           int end_deg, int diameter,
                           FillAnnulus&& fill_annulus, HLine&& hline) {
  if (diameter & 1) {
    const int half = stroke_low_extent(diameter);
    const int inner = radius - half < 0 ? 0 : radius - half;
    fill_annulus(cx, cy, radius + half, inner, start_deg, end_deg);
  } else {
    fill_even_quarter_arc(cx, cy, radius, start_deg, end_deg, diameter, hline);
  }
}

}  // namespace strokefont
