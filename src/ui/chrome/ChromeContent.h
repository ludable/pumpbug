// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <M5Unified.h>

#include <cstdint>

// Shared, dependency-free types for the chrome strip (Chrome) and its content
// painters. Kept in their own header so the painter headers and Chrome can all
// include it without an include cycle.

// Generic state for the reusable BLE link glyph. Callers map their own client
// state into this so the painter stays independent of any BLE protocol.
enum class BleStatus : uint8_t {
  Off,           // BLE client is inactive
  Searching,     // scanning / connecting / reconnecting; chain pulses
  Connected,     // linked; chain solid
  Disconnected,  // link lost, not actively retrying; chain broken
};

// Which display edge the strip occupies. The host chooses it from device
// orientation — always the A-button edge; see UiHost::edgeForOrientation() for
// the per-orientation mapping.
enum class ChromeEdge : uint8_t { Bottom, Top, Left, Right };

// Strip mode: status icons, button-help tabs, or a free-text debug line.
enum class ChromeMode : uint8_t { Icons, Hints, Debug };

// A built-in action glyph for a button hint, drawn from the montserrat symbol
// font (see ui/font_glyphs.h). None means "use the label text only". The
// long-press phase is marked by a thicker accent stripe, not a glyph.
enum class HintGlyph : uint8_t {
  None,
  Power,
  Ok,      // check mark — confirm / yes
  Cancel,  // cross — cancel / no
  Back,    // left arrow — back / exit
  Forth,   // right arrow
  Edit,
  Trash,
};

// One button-hint slot: an optional action glyph and/or a short text label.
// Set one or both; an empty slot (neither set) renders nothing.
struct Hint {
  HintGlyph glyph = HintGlyph::None;
  const char* label = nullptr;
  bool empty() const {
    return glyph == HintGlyph::None && (label == nullptr || label[0] == '\0');
  }
  // Labels are string literals, so comparing the pointer is enough to detect a
  // changed hint (see HintTabs::setHints).
  bool operator==(const Hint& o) const {
    return glyph == o.glyph && label == o.label;
  }
};

// The hints for one physical button across its press types. Empty slots are
// skipped; a button with more than one non-empty slot cycles through them over
// time, the current press type marked by the tab's stripe style (thin = tap,
// thick = hold, split = double).
struct ButtonHint {
  Hint tap;        // short press
  Hint hold;       // long press
  Hint doubleTap;  // double press
  bool any() const {
    return !tap.empty() || !hold.empty() || !doubleTap.empty();
  }
  bool operator==(const ButtonHint& o) const {
    return tap == o.tap && hold == o.hold && doubleTap == o.doubleTap;
  }
};

// Button help for the hint bar: one ButtonHint per physical button.
struct ButtonHints {
  ButtonHint a, b, power;
  bool any() const { return a.any() || b.any() || power.any(); }
};

// The coordinate frame a painter draws in. Chrome keeps two buffers: a screen
// frame (the strip as it sits on the panel — full width × THICKNESS on a
// horizontal edge, THICKNESS × full height on a vertical one) and a logical
// frame (always laid out flat, the edge running along the width: length ×
// THICKNESS). Because the bar always sits on the same physical edge, the
// logical frame is a single fixed size regardless of orientation.
//
//   Screen  — the painter draws directly in panel coordinates and handles
//             orientation itself (e.g. StatusIcons swaps its tile axes).
//   Logical — the painter draws once in the flat logical frame and Chrome
//             rotates it onto the edge. On a horizontal edge the two frames
//             coincide, so Chrome hands the painter the screen canvas directly
//             and skips the rotation. The painter is still told the edge for
//             content decisions (e.g. HintTabs mirrors its layout on Top).
enum class ChromeFrame : uint8_t { Screen, Logical };

// A content painter for the chrome strip. The strip (Chrome) owns the edge, the
// off-screen canvases and their lifecycle (allocate, SRAM→PSRAM fallback, the
// logical→screen rotation), dirty coalescing and the single atomic push. A
// painter is simply handed a canvas and draws its own components — it owns no
// canvas and no lifecycle, only its own content state.
class ChromeContent {
 public:
  virtual ~ChromeContent() = default;

  // Which coordinate frame draw() expects its canvas in (see ChromeFrame).
  virtual ChromeFrame frame() const { return ChromeFrame::Screen; }

  // Capture current values / advance animations. Returns true if a repaint is
  // needed.
  virtual bool poll() = 0;

  // Paint into the (already bg-filled) canvas for the given edge. The canvas is
  // sized to THICKNESS across; its other extent is the display length along the
  // edge. For a Logical painter it is always laid out flat (length ×
  // THICKNESS).
  virtual void draw(LGFX_Sprite& canvas, ChromeEdge edge) = 0;
};
