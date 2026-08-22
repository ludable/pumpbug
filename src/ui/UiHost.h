// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <cstdint>

#include "Navigation.h"
#include "Screen.h"
#include "button.h"
#include "chrome/ChromeContent.h"
#include "net/NetworkStatus.h"
#include "sprite.h"
#include "util/orientation.h"

// Foreground UI host: owns the navigation stack, screen ticking, chrome sync,
// content geometry and dirty redraw coalescing.
class UiHost {
 public:
  struct Input {
    button::Event button;
    bool orientationChanged = false;
  };

  explicit UiHost(DeviceOrientation& orientation);

  // Reserve the content canvas backing buffer. Call early in setup(), before
  // LittleFS / BLE / Wi-Fi fragment the heap.
  bool allocateCanvas();

  // Start with the screen selected by the top-level navigation policy.
  void begin(RootNavigation& rootNavigation);

  // Process foreground input, update screen ticks, sync chrome, and repaint any
  // dirty content.
  void update(const Input& input, NetworkStatus networkStatus,
              uint64_t shotCount);

  void requestDraw();
  void setChromeAlert(bool on);

#if PB_UI_DEBUG_REMOTE
  void debugCycleOrientation() { _orientation.cycleOverride(); }
  void debugReleaseOrientation() { _orientation.clearOverride(); }
#endif

 private:
  struct ContentRect {
    int x, y, w, h;
  };

  ChromeEdge edgeForOrientation() const;
  bool showChrome() const;
  ContentRect contentRect() const;

  void enterScreen(Screen* screen);
  // Returns true when the foreground screen changed, meaning the previous
  // screen has exited and must not be touched again during the current update.
  bool applyResult(const ScreenResult& result);
  void updateActiveScreen(const button::Event& event);
  void syncChrome(NetworkStatus networkStatus, uint64_t shotCount);
  void syncLayoutAndDraw(bool orientationChanged);
  bool drawContent(const ContentRect& content);

  DeviceOrientation& _orientation;
  RootNavigation* _rootNavigation = nullptr;
  Screen* _activeScreen = nullptr;
  NavigationStack<Screen> _navigation;
  uint32_t _lastScreenTickMs = 0;

  ui::PersistentSprite _canvas;
  bool _prevShowChrome = true;
  bool _dirty = true;
};
