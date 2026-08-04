// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

// segfont_lgfx.h - optional LovyanGFX/M5GFX string renderer.

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

template <class Gfx>
struct LgfxSink {
  Gfx* gfx;
  uint32_t color;

  void fill(int x, int y, int w, int h) {
    gfx->writeFillRect(x, y, w, h, color);
  }
};

template <class Gfx>
inline int drawString(Gfx& gfx, const Style& s, const char* str, int x, int y,
                      uint32_t color, TextOptions options = TextOptions()) {
  LgfxSink<Gfx> sink{&gfx, color};
  gfx.startWrite();
  const int w = draw_text(s, str, x, y, sink, options);
  gfx.endWrite();
  return w;
}

template <class Gfx>
inline int drawString(Gfx& gfx, const Style& s, const char* str, int x, int y,
                      uint32_t color, int tracking) {
  TextOptions options;
  options.tracking = tracking;
  return drawString(gfx, s, str, x, y, color, options);
}

}  // namespace segfont
