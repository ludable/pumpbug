// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#include "UiHost.h"

#include <M5Unified.h>

#include "chrome/Chrome.h"

UiHost::UiHost(DeviceOrientation& orientation) : _orientation(orientation) {}

bool UiHost::allocateCanvas() {
  return _canvas.allocate(M5.Display.width(), M5.Display.height());
}

void UiHost::begin(RootNavigation& rootNavigation) {
  _rootNavigation = &rootNavigation;
  _navigation.begin(rootNavigation.initialScreen());

  Chrome::getInstance().setEdge(edgeForOrientation());
  enterScreen(_navigation.top());

  const ContentRect content = contentRect();
  if (_canvas.allocated()) _canvas.shape(content.w, content.h);

  // Present the selected screen first. Whether chrome is wanted depends on the
  // active screen, and its mode depends on that screen's hints or debug text.
  // The first update synchronizes those inputs before the initially-dirty
  // Chrome paints itself.
  if (drawContent(content)) _dirty = false;
  _prevShowChrome = showChrome();
}

void UiHost::requestDraw() { _dirty = true; }

void UiHost::setChromeAlert(bool on) { Chrome::getInstance().setAlert(on); }

ChromeEdge UiHost::edgeForOrientation() const {
  switch (_orientation.getOrientation()) {
    case DeviceOrientation::LANDSCAPE:
      return ChromeEdge::Right;
    case DeviceOrientation::LANDSCAPE_FLIPPED:
      return ChromeEdge::Left;
    case DeviceOrientation::PORTRAIT_FLIPPED:
      return ChromeEdge::Top;
    case DeviceOrientation::PORTRAIT:
    default:
      return ChromeEdge::Bottom;
  }
}

bool UiHost::showChrome() const {
  return !_activeScreen || _activeScreen->wantsStatusBar();
}

UiHost::ContentRect UiHost::contentRect() const {
  const int W = M5.Display.width();
  const int H = M5.Display.height();
  if (!showChrome()) return {0, 0, W, H};

  const int t = Chrome::getInstance().getThickness();
  switch (Chrome::getInstance().edge()) {
    case Chrome::Edge::Left:
      return {t, 0, W - t, H};
    case Chrome::Edge::Right:
      return {0, 0, W - t, H};
    case Chrome::Edge::Top:
      return {0, t, W, H - t};
    case Chrome::Edge::Bottom:
    default:
      return {0, 0, W, H - t};
  }
}

void UiHost::enterScreen(Screen* screen) {
  if (!screen) return;
  _activeScreen = screen;
  _lastScreenTickMs = 0;  // tick immediately on first iteration
  screen->onEnter();
  requestDraw();
}

bool UiHost::applyResult(const ScreenResult& result) {
  switch (result.kind) {
    case ScreenResult::Kind::Ignored:
    case ScreenResult::Kind::Stay:
      return false;
    case ScreenResult::Kind::Push:
      if (result.to) {
        _activeScreen->onExit();
        _navigation.push(*result.to);
        enterScreen(_navigation.top());
        return true;
      }
      [[fallthrough]];
    case ScreenResult::Kind::Replace:
      if (result.to) {
        _activeScreen->onExit();
        _navigation.replace(*result.to);
        enterScreen(_navigation.top());
        return true;
      }
      // A transition without a target still leaves the screen that requested
      // it.
      [[fallthrough]];
    case ScreenResult::Kind::Exit:
      Screen* exiting = _activeScreen;
      exiting->onExit();
      if (!_navigation.pop()) {
        Screen& next = _rootNavigation->screenAfterExit(*exiting);
        _navigation.replace(next);
      }
      enterScreen(_navigation.top());
      return true;
  }
  return false;
}

void UiHost::updateActiveScreen(const button::Event& event) {
  if (!_activeScreen) return;

  if (event.gesture != button::Gesture::NONE) {
    const ScreenResult result = _activeScreen->onEvent(event.gesture);
    if (result.kind == ScreenResult::Kind::Ignored &&
        _activeScreen->allowsRootShortcut()) {
      if (Screen* destination = _rootNavigation->shortcutDestination(
              *_activeScreen, event.gesture)) {
        if (event.physical) button::feedback::accepted(event.gesture);
        _activeScreen->onExit();
        _navigation.begin(*destination);
        enterScreen(_navigation.top());
        return;
      }
    }

    if (event.physical && result.kind != ScreenResult::Kind::Ignored) {
      button::feedback::accepted(event.gesture);
    }
    if (applyResult(result)) return;
  }

  const uint32_t tickMs = _activeScreen->desiredTickMs();
  const uint32_t now = millis();
  if (tickMs == 0 || now - _lastScreenTickMs >= tickMs) {
    _lastScreenTickMs = now;
    applyResult(_activeScreen->tick());
  }
}

void UiHost::syncChrome(NetworkStatus networkStatus, uint64_t shotCount) {
  if (!showChrome()) return;

  Chrome& bar = Chrome::getInstance();
  const char* debugText = _activeScreen ? _activeScreen->debugText() : nullptr;
  if (debugText) {
    bar.setMode(ChromeMode::Debug);
    bar.setDebugText(debugText);
    return;
  }

  // A screen that returns button hints gets the hint-tab bar; otherwise the
  // strip shows the status icons. Both setters short-circuit when unchanged.
  const ButtonHints hints =
      _activeScreen ? _activeScreen->buttonHints() : ButtonHints{};
  if (hints.any()) {
    bar.setMode(ChromeMode::Hints);
    bar.setHints(hints);
    return;
  }

  bar.setMode(ChromeMode::Icons);
  bar.setNetworkStatus(networkStatus);
  bar.setShotCount(shotCount);
}

bool UiHost::drawContent(const ContentRect& content) {
  if (!_canvas.allocated()) return false;

  if (!_activeScreen || !_activeScreen->onDraw(_canvas)) return false;
  _canvas->pushSprite(content.x, content.y);
  _activeScreen->onPresented();
  return true;
}

void UiHost::syncLayoutAndDraw(bool orientationChanged) {
  const bool visibleChrome = showChrome();
  const bool chromeToggled = visibleChrome != _prevShowChrome;
  const ContentRect content = contentRect();

  if (orientationChanged || chromeToggled) {
    if (_canvas.allocated()) _canvas.shape(content.w, content.h);
    if (_activeScreen && orientationChanged) _activeScreen->onLayoutChanged();
    // Chrome and the host canvas tile the whole display. Repaint both this
    // frame so orientation flips do not expose a blank content area.
    if (visibleChrome) Chrome::getInstance().draw();
    drawContent(content);
    _dirty = false;
  } else if (visibleChrome) {
    Chrome::getInstance().update();
  }
  _prevShowChrome = visibleChrome;

  if (_dirty) {
    _dirty = false;
    drawContent(content);
  }
}

void UiHost::update(const Input& input, NetworkStatus networkStatus,
                    uint64_t shotCount) {
  if (input.orientationChanged) {
    Chrome::getInstance().setEdge(edgeForOrientation());
  }

  updateActiveScreen(input.button);

  syncChrome(networkStatus, shotCount);
  syncLayoutAndDraw(input.orientationChanged);
}
