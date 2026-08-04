// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#include "Chrome.h"

#include <M5Unified.h>

#include "ui/theme.h"

Chrome& Chrome::getInstance() {
  static Chrome instance;
  return instance;
}

void Chrome::setEdge(Edge e) {
  if (_edge == e) return;
  _edge = e;
  _dirty = true;
}

void Chrome::setMode(Mode m) {
  if (_mode == m) return;
  _mode = m;
  _dirty = true;
}

void Chrome::_draw(bool force) {
  const int W = M5.Display.width();
  const int H = M5.Display.height();
  const int T = THICKNESS;

  // The bar always sits on the A-button edge.
  int barX, barY, barW, barH;
  int mgnX, mgnY, mgnW, mgnH;
  switch (_edge) {
    case Edge::Left:
      barX = 0, barY = 0, barW = T, barH = H;
      mgnX = barX + barW, mgnY = 0, mgnW = MARGIN, mgnH = H;
      break;
    case Edge::Right:
      barX = W - T, barY = 0, barW = T, barH = H;
      mgnX = barX - MARGIN, mgnY = 0, mgnW = MARGIN, mgnH = H;
      break;
    case Edge::Top:
      barX = 0, barY = 0, barW = W, barH = T;
      mgnX = 0, mgnY = barY + barH, mgnW = W, mgnH = MARGIN;
      break;
    case Edge::Bottom:
    default:
      barX = 0, barY = H - T, barW = W, barH = T;
      mgnX = 0, mgnY = barY - MARGIN, mgnW = W, mgnH = MARGIN;
      break;
  }

  // Reserve the compose buffer once, on the first draw, sized for the bar's
  // largest extent. The bar always sits on the same physical edge, so its
  // length is orientation-invariant — this one size fits every edge, in either
  // the screen shape (blitted) or the flat shape (rotated). Each flip just
  // re-points the buffer rather than reallocating (see ui/sprite.h); done
  // lazily here so Chrome needs no separate init.
  if (!_canvas.allocated()) {
    const int len = barW > barH ? barW : barH;  // edge length
    if (!_canvas.allocate(len, T))
      return;  // allocation failed (SRAM + PSRAM exhausted)
  }

  // A Logical painter on a vertical edge is drawn flat (length × THICKNESS) and
  // rotated straight onto the panel, so the canvas takes the flat shape. Every
  // other case — a Screen painter, or a Logical painter on a horizontal edge,
  // where the flat and screen shapes coincide — draws in the bar's screen shape
  // and is blitted without rotation.
  const bool horizontal = _edge == Edge::Top || _edge == Edge::Bottom;
  const bool rotate = _active().frame() == Frame::Logical && !horizontal;
  const int bufW = rotate ? barH : barW;  // flat: length (barH) × THICKNESS
  const int bufH = rotate ? T : barH;

  // Re-point the buffer to the current shape (a pure transpose / reshape on a
  // flip — no realloc). sizeChanged is read before the re-point.
  const bool sizeChanged =
      _canvas->width() != bufW || _canvas->height() != bufH;
  LGFX_Sprite& canvas = _canvas.shape(bufW, bufH);

  // Poll the active painter (it captures its values / advances animations),
  // then repaint the whole bar if anything changed, on a forced redraw, or
  // after an edge/shape change.
  const bool changed = _active().poll();
  if (!(force || _dirty || sizeChanged || changed)) return;
  _dirty = false;

  // Compose the whole bar off-screen, then place it on the panel in one
  // operation (atomic / flicker-free) — a plain blit for the screen shape, or a
  // direct rotation for the flat shape. The separator margin is filled on the
  // panel beside it; the two regions don't overlap.
  canvas.fillScreen(theme::bg());
  _active().draw(canvas, _edge);
  canvas.getParent()->fillRect(mgnX, mgnY, mgnW, mgnH, theme::bg());
  if (rotate) {
    const float angle = _edge == Edge::Left ? 90.0f : 270.0f;
    canvas.setPivot(bufW / 2.0f, bufH / 2.0f);
    canvas.pushRotateZoom(canvas.getParent(), barX + barW / 2.0f,
                          barY + barH / 2.0f, angle, 1.0f, 1.0f);
  } else {
    canvas.pushSprite(barX, barY);
  }

  // Crash marker: a small red dot in the bar's outer screen corner whenever a
  // core dump is on record. Stamped on the panel after the bar is placed, so
  // it's independent of which painter/mode just composed. A no-change frame
  // early-returns above, leaving the previously-drawn dot intact; a redraw
  // re-blits the bar (erasing it) and this restamps it.
  if (_alert) {
    constexpr int r = 3, inset = r + 1;
    int cx, cy;
    switch (_edge) {
      case Edge::Top:
        cx = W - inset, cy = inset;
        break;
      case Edge::Left:
        cx = inset, cy = H - inset;
        break;
      case Edge::Right:
      case Edge::Bottom:
      default:
        cx = W - inset, cy = H - inset;
        break;
    }
    M5.Display.fillCircle(cx, cy, r, theme::critical_fill());
  }
}
