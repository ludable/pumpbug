// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <M5Unified.h>

#include <cstdint>

#include "button.h"
#include "chrome/ChromeContent.h"

class Screen;

// The result of a Screen event or tick. Ignored means an event had no action
// in the current state. Stay keeps the current Screen in the foreground; the
// other results change it.
struct ScreenResult {
  enum class Kind : uint8_t { Ignored, Stay, Exit, Replace, Push };
  Kind kind = Kind::Stay;
  Screen* to = nullptr;  // Replace and Push
};

class Screen {
 public:
  virtual ~Screen() = default;

  static ScreenResult ignored() {
    return {ScreenResult::Kind::Ignored, nullptr};
  }
  static ScreenResult stay() { return {}; }
  static ScreenResult exit() { return {ScreenResult::Kind::Exit, nullptr}; }
  static ScreenResult replaceWith(Screen& next) {
    return {ScreenResult::Kind::Replace, &next};
  }
  static ScreenResult push(Screen& next) {
    return {ScreenResult::Kind::Push, &next};
  }

  // Called when this Screen becomes foreground. onDraw() may run immediately
  // after this returns, before the first tick(), so initialize every field the
  // draw path reads here. The host requests that draw; implementations do not
  // need to request it themselves.
  virtual void onEnter() = 0;

  // Called when this Screen stops being foreground. Restore any global state
  // touched in onEnter. Runs whenever another Screen takes the foreground.
  virtual void onExit() {}

  // Button event. Return ignored() when the event has no action in the current
  // state; the host uses this distinction to confirm accepted physical gestures
  // with sound.
  virtual ScreenResult onEvent(button::Gesture event) { return ignored(); }

  // Whether an unhandled product-wide navigation shortcut may leave this
  // screen. Modal workflows override this while interruption would abandon
  // required cleanup or bypass an in-progress operation.
  virtual bool allowsRootShortcut() const { return true; }

  // Orientation or geometry changed. Screens that hold geometry-dependent
  // state (cached sizes, secondary sprites) should recompute it here. The host
  // marks the screen dirty around this call, so an explicit draw is not needed.
  virtual void onLayoutChanged() {}

  // Periodic update. Called at most every desiredTickMs() ms. State changes
  // that affect what's on screen should call requestDraw(). May transition
  // just like onEvent.
  virtual ScreenResult tick() { return stay(); }

  // Render. Called by the host whenever the screen is dirty (see requestDraw).
  // `canvas` is sized to the content area (display minus status bar margin
  // when wantsStatusBar()). Return true if the host should push `canvas`
  // after this returns. Screens that manage their own canvas (e.g. persistent
  // view buffers) and draw directly to the display should return false.
  virtual bool onDraw(LGFX_Sprite* canvas) { return false; }

  // Called after a canvas returned by onDraw() has been copied to the display.
  // Screens can use this when work must be tied to visible output rather than
  // to rendering into the off-screen canvas.
  virtual void onPresented() {}

  // Minimum interval between tick() calls in ms.
  // 0 means every host iteration (no throttling).
  virtual uint32_t desiredTickMs() const { return 0; }

  // Whether the global status bar should be drawn while this Screen is
  // foreground. Screens that paint the full display should return false. The
  // host redraws the status bar when returning to a Screen that wants it,
  // and resizes the host canvas accordingly. By default, the bar itself shows
  // battery, shot-count, and Wi-Fi status, or can be switched to button help
  // (hints) or debug text, see below.
  virtual bool wantsStatusBar() const { return true; }

  // Button-help shown in the chrome strip. Returning non-empty hints
  // (ButtonHints::any()) switches the strip from status icons to button-help
  // tabs that point at the physical controls; the host pushes these into the
  // Chrome strip each loop. Screens return state-dependent labels.
  virtual ButtonHints buttonHints() const { return {}; }

  // Facility to display a debug text in the status bar area. Takes precedence
  // over buttonHints. The Screen owns the buffer and keeps it allocated while
  // active.
  virtual const char* debugText() const { return nullptr; }
};

// Mark the active Screen as needing a repaint. Multiple calls in
// one host loop iteration coalesce into a single onDraw + push.
void requestDraw();
