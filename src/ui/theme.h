// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <cstdint>

// Shared UI palette. The palette is runtime-switchable.
// All values are 24-bit RGB888 literals (M5GFX accepts them directly).
namespace theme {

struct Palette {
  // Surfaces
  uint32_t bg;       // primary background
  uint32_t bg_alt;   // alternating row / block shade
  uint32_t surface;  // pills, inactive chips, button bg
  uint32_t muted;    // disabled / inactive border

  // Text
  uint32_t fg;   // primary text
  uint32_t dim;  // labels, secondary text, range readouts

  // Selection / focus
  uint32_t highlight;
  uint32_t highlight_fg;

  // Brand / accents
  uint32_t accent;
  uint32_t accent_light;

  // State signaling: text and stroke colors, tuned per palette to contrast
  // with bg (the light palette can't reuse the dark one's brights for text).
  uint32_t ok;
  uint32_t warn;
  uint32_t critical;
  uint32_t alarm;
  // State fills: saturated area colors (battery/disk fill levels, banner
  // and armed-state backgrounds, marker dots). Shapes read without text
  // contrast, so these stay bright in both palettes.
  uint32_t ok_fill;
  uint32_t warn_fill;
  uint32_t critical_fill;
  uint32_t critical_fill_fg;

  // Charts keep their own dark ground in both palettes, so chart strokes are
  // palette-invariant brights.
  uint32_t chart_bg;
  uint32_t chart_fg;
  uint32_t chart_flow;  // flow series and its chart-yellow accents
};

// Accessors read the active palette.
extern const Palette& palette();

inline uint32_t bg() { return palette().bg; }
inline uint32_t bg_alt() { return palette().bg_alt; }
inline uint32_t surface() { return palette().surface; }
inline uint32_t muted() { return palette().muted; }
inline uint32_t fg() { return palette().fg; }
inline uint32_t dim() { return palette().dim; }
inline uint32_t highlight() { return palette().highlight; }
inline uint32_t highlight_fg() { return palette().highlight_fg; }
inline uint32_t accent() { return palette().accent; }
inline uint32_t accent_light() { return palette().accent_light; }
inline uint32_t ok() { return palette().ok; }
inline uint32_t warn() { return palette().warn; }
inline uint32_t critical() { return palette().critical; }
inline uint32_t alarm() { return palette().alarm; }
inline uint32_t ok_fill() { return palette().ok_fill; }
inline uint32_t warn_fill() { return palette().warn_fill; }
inline uint32_t critical_fill() { return palette().critical_fill; }
inline uint32_t critical_fill_fg() { return palette().critical_fill_fg; }
inline uint32_t chart_bg() { return palette().chart_bg; }
inline uint32_t chart_fg() { return palette().chart_fg; }
inline uint32_t chart_flow() { return palette().chart_flow; }

// Switch between the global light and dark palettes.
void setLightMode(bool light);
bool isLightMode();

}  // namespace theme
