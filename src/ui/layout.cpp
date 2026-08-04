// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#include "ui/layout.h"

#include <algorithm>
#include <cstring>

namespace layout {
namespace {

constexpr int kMaxWrappedLines = 12;

struct WrappedText {
  const char* begin[kMaxWrappedLines];
  int len[kMaxWrappedLines];
  int count = 0;
  int maxLineW = 0;
  bool truncated = false;
};

WrappedText wrapText(LGFX_Sprite* c, const char* text, int w) {
  WrappedText wrapped;
  char buf[160];
  auto widthOf = [&](const char* s, const char* e) -> int {
    int l = static_cast<int>(e - s);
    if (l > static_cast<int>(sizeof(buf)) - 1) l = sizeof(buf) - 1;
    std::memcpy(buf, s, l);
    buf[l] = '\0';
    return c->textWidth(buf);
  };
  auto store = [&](const char* s, const char* e) {
    while (e > s && e[-1] == ' ') --e;  // trim trailing spaces
    while (s < e && *s == ' ') ++s;     // trim leading spaces
    if (e <= s) return;
    if (wrapped.count >= kMaxWrappedLines) {
      wrapped.truncated = true;
      return;
    }
    wrapped.begin[wrapped.count] = s;
    wrapped.len[wrapped.count] = static_cast<int>(e - s);
    wrapped.maxLineW = std::max(wrapped.maxLineW, widthOf(s, e));
    ++wrapped.count;
  };

  // Greedy wrap: extend the current line word by word until the next word
  // would exceed `w`, then break before it. The first word of a line is always
  // placed so a too-long word cannot stall the loop.
  const char* lineStart = text;
  const char* scan = text;
  while (*scan && !wrapped.truncated) {
    const char* ws = scan;
    while (*ws == ' ') ++ws;  // next word start
    if (!*ws) break;
    const char* we = ws;
    while (*we && *we != ' ') ++we;  // next word end
    if (widthOf(lineStart, we) <= w || lineStart == ws) {
      scan = we;
    } else {
      store(lineStart, ws);
      lineStart = ws;
      scan = ws;
    }
  }
  store(lineStart, scan);
  return wrapped;
}

bool wrappedFits(LGFX_Sprite* c, const WrappedText& wrapped, int w, int h) {
  if (wrapped.count == 0 || wrapped.truncated) return false;
  return wrapped.maxLineW <= w && wrapped.count * c->fontHeight() <= h;
}

WrappedText fitScaledWrapped(LGFX_Sprite* c, font::font_t f, const char* text,
                             int w, int h, float startSize) {
  float size = startSize;
  WrappedText wrapped;
  for (int i = 0; i < 3; ++i) {
    c->setFont(f);
    c->setTextSize(size);
    wrapped = wrapText(c, text, w);
    if (wrappedFits(c, wrapped, w, h)) return wrapped;

    const int blockH = wrapped.count * c->fontHeight();
    const float sx =
        wrapped.maxLineW > 0 ? static_cast<float>(w) / wrapped.maxLineW : 1.0f;
    const float sy = blockH > 0 ? static_cast<float>(h) / blockH : 1.0f;
    const float factor = std::min(sx, sy);
    if (factor >= 1.0f) break;
    size *= factor;
    if (size < 0.25f) size = 0.25f;
  }
  c->setFont(f);
  c->setTextSize(size);
  return wrapText(c, text, w);
}

WrappedText fitWrapped(LGFX_Sprite* c, const char* text, int w, int h,
                       const font::sizing_spec_t& fontSpec) {
  if (fontSpec.kind == font::sizing_spec_t::kind_t::Family) {
    const auto family = fontSpec.family;
    for (size_t i = 0; i < family.count; ++i) {
      font::apply(c, family.fonts[i]);
      WrappedText wrapped = wrapText(c, text, w);
      if (wrappedFits(c, wrapped, w, h)) return wrapped;
    }

    const auto smallest = family.fonts[family.count - 1];
    return fitScaledWrapped(c, smallest.font, text, w, h, smallest.size);
  }

  return fitScaledWrapped(c, fontSpec.font, text, w, h, 1.0f);
}

}  // namespace

void drawCenteredInBox(LGFX_Sprite* c, const char* text, int x, int y, int w,
                       int h, const font::sizing_spec_t fontSpec,
                       draw_centered_opts_t opts) {
  font::fit(c, opts.sizeRef ? opts.sizeRef : text, w, h, fontSpec);
  const int cx = opts.leftAlign ? x : x + w / 2;
  const int cy = y + h / 2;
  ClipScope clip(c, x, y, w, h);
  if (opts.leftAlign) {
    drawMiddleLeft(c, text, cx, cy);
  } else {
    drawCentered(c, text, cx, cy);
  }
}

int measureWrapped(LGFX_Sprite* c, const char* text, int maxW, int maxH,
                   const font::sizing_spec_t fontSpec) {
  const WrappedText wrapped = fitWrapped(c, text, maxW, maxH, fontSpec);
  return wrapped.count * c->fontHeight();
}

void drawWrappedCentered(LGFX_Sprite* c, const char* text, int x, int y, int w,
                         int h, const font::sizing_spec_t fontSpec) {
  const WrappedText wrapped = fitWrapped(c, text, w, h, fontSpec);
  const int lineH = c->fontHeight();
  char buf[160];

  // Center the block of em-cells in the box; each cell is lineH tall and tiled
  // top-to-bottom. middle_center centers the full font cell (height ==
  // fontHeight()) on the given point, independent of the font, so each line
  // lands exactly in its slot. The clip only guards genuine overflow (more
  // lines or a word wider than the box than `f` can fit).
  const int blockTop = y + (h - wrapped.count * lineH) / 2;
  const int cx = x + w / 2;
  ClipScope clip(c, x, y, w, h);
  for (int i = 0; i < wrapped.count; ++i) {
    int l = wrapped.len[i];
    if (l > static_cast<int>(sizeof(buf)) - 1) l = sizeof(buf) - 1;
    std::memcpy(buf, wrapped.begin[i], l);
    buf[l] = '\0';
    drawCentered(c, buf, cx, blockTop + i * lineH + lineH / 2);
  }
}

}  // namespace layout
