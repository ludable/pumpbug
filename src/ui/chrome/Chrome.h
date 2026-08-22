// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <M5Unified.h>

#include "ChromeContent.h"
#include "DebugText.h"
#include "HintTabs.h"
#include "StatusIcons.h"
#include "ui/sprite.h"

// Chrome strip owner / switcher. Owns the display edge, the single off-screen
// canvas and its lifecycle (recreate on edge/size change, SRAM→PSRAM fallback),
// dirty coalescing and the one atomic pushSprite. The strip itself draws no
// content: it routes poll()/draw() to the active ChromeContent painter —
// StatusIcons (battery, shot counter, Wi-Fi) or HintTabs (button-help) — and
// forwards their inputs to them.
//
// The host keeps the strip on the A-button edge (see
// UiHost::edgeForOrientation()). Painters short-circuit on unchanged values so
// the host can provide fresh state every frame without forcing a redraw.
class Chrome {
 public:
  using Edge = ChromeEdge;
  using Mode = ChromeMode;
  using Frame = ChromeFrame;

  static constexpr int THICKNESS = 32;  // px consumed off the chosen edge
  static constexpr int MARGIN = 2;      // separation from main content

  static Chrome& getInstance();

  void setEdge(Edge e);
  void setMode(Mode m);
  void setHints(const ButtonHints& h) { _hints.setHints(h); }
  void setNetworkStatus(NetworkStatus status) {
    _icons.setNetworkStatus(status);
  }
  void setShotCount(uint64_t count) { _icons.setShotCount(count); }
  void setDebugText(const char* t) { _debug.setText(t); }

  // Drop any palette-cached glyphs so the next draw uses the active theme.
  // Call after switching light/dark mode.
  void invalidateThemeCache() { _icons.invalidate(); }

  // Raise/lower a small red dot in the bar's outer corner — the persistent
  // "unacknowledged crash on record" marker (set at boot from
  // diagnostics::hasPanicDump()). Drawn over whatever painter/mode is active,
  // so it shows everywhere the bar does.
  void setAlert(bool on) {
    if (_alert == on) return;
    _alert = on;
    _dirty = true;
  }

  Edge edge() const { return _edge; }
  int getThickness() const { return THICKNESS + MARGIN; }

  void draw() { _draw(true); }  // Forces a full redraw.
  void update() { _draw(false); }

 private:
  Chrome() = default;
  void _draw(bool force);
  ChromeContent& _active() {
    switch (_mode) {
      case Mode::Hints:
        return _hints;
      case Mode::Debug:
        return _debug;
      case Mode::Icons:
      default:
        return _icons;
    }
  }

  // The bar's off-screen compose buffer. The active painter draws the whole bar
  // into it, then it's placed on the panel in one operation: a plain blit when
  // it holds the bar's screen shape, or a direct rotation when a Logical
  // painter on a vertical edge draws it flat (length × THICKNESS) to be turned
  // onto the edge. Either way it's one buffer — the bar always sits on the same
  // physical edge, so the pixel count is invariant, and it's allocated once and
  // only re-pointed per flip, never reallocated (see ui/sprite.h).
  ui::PersistentSprite _canvas;
  Edge _edge = Edge::Bottom;
  Mode _mode = Mode::Icons;
  StatusIcons _icons;
  HintTabs _hints;
  DebugText _debug;  // diagnostic mode; see DebugText
  // Forces a full redraw on the next _draw(). True at construction so UiHost
  // can synchronize the initial mode before the first bar paints via update().
  bool _dirty = true;
  bool _alert = false;  // crash marker (red corner dot); see setAlert()
};
