// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#include "WifiStatusScreen.h"

#include <cstdio>
#include <string>
#include <vector>

#include "net/Auth.h"
#include "net/NetworkStatus.h"
#include "net/PairRedirectPolicy.h"
#include "net/WifiManager.h"
#include "power/PowerManager.h"
#include "ui/fonts.h"
#include "ui/icons.h"
#include "ui/layout.h"
#include "ui/message_cards.h"
#include "ui/theme.h"
#include "wifi_ui.h"

namespace {
int drawWifiHeader(LGFX_Sprite* c, NetworkStatus status) {
  return wifi_ui::drawWifiHeader(c, "WI-FI", status);
}
}  // namespace

void WifiStatusScreen::onEnter() {
  _lastView = _computeView();
  _lastPairingSec = UINT32_MAX;
  _lastPairedCount = auth.pairedCount();
  _sweep.reset();
  _overlay = Overlay::None;
  _pairShowDetails = false;
  requestDraw();
}

void WifiStatusScreen::onExit() {
  // Leaving the screen abandons the PIN/QR display, so the pairing window
  // shouldn't keep ticking in the background.
  if (auth.isPairing()) auth.cancelPairing();
}

WifiStatusScreen::View WifiStatusScreen::_computeView() const {
  if (auth.isPairing()) return View::Visit;
  switch (wifiManager.state()) {
    case WifiManager::State::Off:
      return View::Idle;
    case WifiManager::State::StaConnecting:
      return View::StaConnecting;
    case WifiManager::State::StaConnected:
      return View::StaConnected;
    case WifiManager::State::ApUp:
      return View::ApUp;
  }
  return View::Idle;
}

void WifiStatusScreen::_openConfigMenu() {
  const bool wifiIsOff = _computeView() == View::Idle;
  std::vector<Menu::Item> items = {
      Menu::Item::action(
          wifiIsOff ? "Wi-Fi on" : "Wi-Fi off",
          static_cast<int32_t>(wifiIsOff ? ConfigAction::EnableWifi
                                         : ConfigAction::DisableWifi)),
      Menu::Item::action("Network",
                         static_cast<int32_t>(ConfigAction::ChangeNetwork)),
      Menu::Item::action("Unpair",
                         static_cast<int32_t>(ConfigAction::UnpairClients)),
  };
  _chooser.init("SETTINGS", items);
  _overlay = Overlay::ConfigMenu;
  requestDraw();
}

ScreenResult WifiStatusScreen::_configConfirmed() {
  const ConfigAction action =
      static_cast<ConfigAction>(_chooser.selectedValue());
  _overlay = Overlay::None;
  switch (action) {
    case ConfigAction::ChangeNetwork:
      return push(_wizard);
    case ConfigAction::UnpairClients:
      _overlay = Overlay::ConfirmUnpair;
      break;
    case ConfigAction::EnableWifi:
      if (wifiManager.hasStaCreds()) {
        wifiManager.retrySta(/*persist=*/true);
      } else {
        return push(_wizard);
      }
      break;
    case ConfigAction::DisableWifi:
      wifiManager.requestStop(/*persist=*/true);
      break;
  }
  requestDraw();
  return stay();
}

ScreenResult WifiStatusScreen::onEvent(button::Gesture event) {
  if (_overlay == Overlay::ConfigMenu) {
    switch (_chooser.update(event)) {
      case Menu::SELECTION_CHANGED:
        requestDraw();
        return stay();
      case Menu::SELECTION_CONFIRMED:
        return _configConfirmed();
      case Menu::MENU_CLOSED:
        _overlay = Overlay::None;
        requestDraw();
        return stay();
      default:
        return ignored();
    }
    return ignored();
  }

  if (_overlay == Overlay::ConfirmUnpair) {
    switch (event) {
      case button::Gesture::A_LONG:
        auth.forgetAll();
        _overlay = Overlay::None;
        requestDraw();
        return stay();
      case button::Gesture::B_SHORT:
        _overlay = Overlay::None;
        requestDraw();
        return stay();
      default:
        return ignored();
    }
    return ignored();
  }

  const View v = _computeView();
  switch (v) {
    case View::Idle:
      switch (event) {
        case button::Gesture::A_LONG:
          _openConfigMenu();
          return stay();
        case button::Gesture::B_SHORT:
          return exit();
        default:
          break;
      }
      break;

    case View::StaConnecting:
      if (event == button::Gesture::B_SHORT) return exit();
      break;

    case View::StaConnected:
    case View::ApUp:
      switch (event) {
        case button::Gesture::A_SHORT:
          // VISIT is backed by a pairing window: an already-paired client
          // following the QR goes straight to the dashboard without consuming
          // another slot; a new client pairs on the way through.
          _pairShowDetails = false;
          auth.startPairing();
          requestDraw();
          return stay();
        case button::Gesture::A_LONG:
          _openConfigMenu();
          return stay();
        case button::Gesture::B_SHORT:
          return exit();
        default:
          break;
      }
      break;

    case View::Visit:
      switch (event) {
        case button::Gesture::A_SHORT:
          _pairShowDetails = !_pairShowDetails;
          requestDraw();
          return stay();
        case button::Gesture::B_SHORT:
          // Back to the status view; the abandoned window is cancelled so
          // its credentials stop being redeemable.
          auth.cancelPairing();
          requestDraw();
          return stay();
        default:
          break;
      }
      break;
  }
  return ignored();
}

ScreenResult WifiStatusScreen::tick() {
  // Consume the previous wizard outcome even while Wi-Fi remains configured.
  // Otherwise, cancelling a network change could later be mistaken for
  // cancelling first-time setup after the configuration is cleared (e.g.
  // through the web UI).
  const WifiSetupWizard::Outcome setupOutcome = _wizard.takeOutcome();
  if (!wifiManager.isConfigured()) {
    if (setupOutcome == WifiSetupWizard::Outcome::Cancelled) {
      return exit();
    }
    return push(_wizard);
  }

  const View v = _computeView();
  if (v != _lastView) {
    _lastView = v;
    _lastPairingSec = UINT32_MAX;
    requestDraw();
  }

  if (_overlay != Overlay::None) return stay();

  switch (v) {
    case View::Visit: {
      // Keep screen lit while the PIN/QR is visible — dimming mid-scan
      // would confuse the user mid-entry.
      power::powerManager.keepAwake();
      const uint32_t sec = (auth.pairingMsRemaining() + 999) / 1000;
      if (sec != _lastPairingSec) {
        _lastPairingSec = sec;
        requestDraw();
      }
      break;
    }
    case View::StaConnecting: {
      if (_sweep.advance(millis())) requestDraw();
      break;
    }
    case View::StaConnected:
    case View::ApUp: {
      const size_t paired = auth.pairedCount();
      if (paired != _lastPairedCount) {
        _lastPairedCount = paired;
        requestDraw();
      }
      break;
    }
    default:
      break;
  }
  return stay();
}

bool WifiStatusScreen::onDraw(LGFX_Sprite* c) {
  c->fillScreen(theme::bg());
  if (!wifiManager.isConfigured()) {
    // About to open the wizard on the next tick; don't flash a status
    // view on the way through.
    return true;
  }
  switch (_overlay) {
    case Overlay::ConfigMenu:
      _chooser.draw(c);
      return true;
    case Overlay::ConfirmUnpair:
      ui::drawCriticalMessageScreen(c, "Unpair all clients?",
                                    "Every paired browser must re-pair with a "
                                    "new PIN. Network config is kept.");
      return true;
    case Overlay::None:
      break;
  }
  switch (_lastView) {
    case View::Idle:
      _drawIdle(c);
      break;
    case View::StaConnecting:
      _drawStaConnecting(c);
      break;
    case View::StaConnected:
      _drawStaConnected(c);
      break;
    case View::ApUp:
      _drawApUp(c);
      break;
    case View::Visit:
      _drawVisit(c);
      break;
  }
  return true;
}

ButtonHints WifiStatusScreen::buttonHints() const {
  switch (_overlay) {
    case Overlay::ConfigMenu:
      return {{Hint{HintGlyph::None, "NEXT"}, Hint{HintGlyph::Ok, "SELECT"}},
              {Hint{HintGlyph::Cancel}}};
    case Overlay::ConfirmUnpair:
      return {{{}, Hint{HintGlyph::Trash, "UNPAIR"}},
              {Hint{HintGlyph::Cancel}}};
    case Overlay::None:
      break;
  }
  switch (_lastView) {
    case View::Idle:
      return {{{}, Hint{HintGlyph::None, "SETTINGS"}}, {Hint{HintGlyph::Back}}};
    case View::StaConnecting:
      return {{}, {Hint{HintGlyph::Back}}};
    case View::StaConnected:
    case View::ApUp:
      return {
          {Hint{HintGlyph::None, "VISIT"}, Hint{HintGlyph::None, "SETTINGS"}},
          {Hint{HintGlyph::Back}}};
    case View::Visit:
      return {{Hint{HintGlyph::None, _pairShowDetails ? "QR" : "TEXT"}},
              {Hint{HintGlyph::Back}}};
  }
  return {};
}

// ----- drawing helpers ------------------------------------------------------

void WifiStatusScreen::_drawIdle(LGFX_Sprite* c) {
  const int headerH = drawWifiHeader(c, NetworkStatus::Off);
  const layout::rect body =
      layout::inset({0, headerH, c->width(), c->height() - headerH}, 6);
  const auto split = layout::splitV(body, 0.6f, 2);
  c->setTextColor(theme::fg(), theme::bg());
  layout::drawCenteredInBox(c, "OFF", split.first, font::textFamily());
  c->setTextColor(theme::dim(), theme::bg());
  layout::drawWrappedCentered(c, "open SETTINGS to turn Wi-Fi on", split.second,
                              font::textFamily());
}

void WifiStatusScreen::_drawApUp(LGFX_Sprite* c) {
  // After a terminal STA failure, the header glyph shows the slash and the
  // LAST ERROR block below names the failure, so the user sees why the
  // device fell back without opening the web UI.
  const char* reason = wifiManager.lastDisconnectReason();
  const bool fallback = wifiManager.apIsFallback();
  const int headerH =
      drawWifiHeader(c, fallback ? NetworkStatus::Failed : NetworkStatus::Ap);

  const unsigned paired = static_cast<unsigned>(auth.pairedCount());
  char pairedLine[16];
  std::snprintf(pairedLine, sizeof(pairedLine), "%u paired", paired);

  ui::InfoBlock blocks[5];
  int count = 0;
  if (fallback && reason && *reason) {
    blocks[count++] = {"ERROR", reason, theme::critical(), 22};
  }
  blocks[count++] = {"NET", wifiManager.deviceId(), theme::fg(), 30};
  blocks[count++] = {"PWD", wifiManager.apPassword(), theme::fg(), 30};
  const std::string ip = wifiManager.ip().toString().c_str();
  blocks[count++] = {"http://", ip.c_str(), theme::fg(), 26};
  blocks[count++] = {"", pairedLine, theme::dim(), 20};

  ui::drawInfoScreen(c, headerH, nullptr, nullptr, blocks, count, theme::bg(),
                     wifi_ui::kLabelSizeRef);
}

void WifiStatusScreen::_drawStaConnecting(LGFX_Sprite* c) {
  const int headerH = wifi_ui::drawWifiHeader(
      c, "CONNECTING", NetworkStatus::Connecting, _sweep.bands());
  ui::drawCardBody(c, headerH, wifiManager.ssid());
}

void WifiStatusScreen::_drawStaConnected(LGFX_Sprite* c) {
  const int headerH = drawWifiHeader(c, NetworkStatus::Connected);

  char host[48];
  std::snprintf(host, sizeof(host), "%s.local", wifiManager.hostname());
  const std::string ip = wifiManager.ip().toString().c_str();
  const unsigned paired = static_cast<unsigned>(auth.pairedCount());
  char pairedLine[16];
  std::snprintf(pairedLine, sizeof(pairedLine), "%u paired", paired);

  // "http://" as the label splits the URL across the block shape: the
  // scheme still marks the value as a URL without stealing width from the
  // part the user actually reads. The IP is the technical detail, labeled
  // plainly; the paired count reads as a phrase rather than a lone digit.
  const ui::InfoBlock blocks[] = {
      {"NET", wifiManager.ssid(), theme::fg(), 30},
      {"http://", host, theme::fg(), 26},
      {"IP", ip.c_str(), theme::fg(), 26},
      {"", pairedLine, theme::dim(), 20},
  };
  ui::drawInfoScreen(c, headerH, nullptr, nullptr, blocks, 4, theme::bg(),
                     wifi_ui::kLabelSizeRef);
}

void WifiStatusScreen::_drawVisit(LGFX_Sprite* c) {
  // The countdown is the pairing window's remaining life. It only limits
  // when a NEW client can pair — an already-paired client's scan works even
  // after expiry (the redirect short-circuits without checking the token).
  const uint32_t sec = (auth.pairingMsRemaining() + 999) / 1000;
  char countdown[8];
  std::snprintf(countdown, sizeof(countdown), "%us",
                static_cast<unsigned>(sec));
  // critical(), not warn(): the yellow is unreadable on the light theme's
  // white header.
  const int headerH =
      ui::drawViewHeader(c, "VISIT", theme::accent(), countdown,
                         sec <= 10 ? theme::critical() : theme::dim());

  const bool apMode = wifiManager.mode() == WifiManager::Mode::Ap;

  if (!_pairShowDetails) {
    // In AP mode the QR routes onward to the setup page (same as the
    // wizard); connected, straight to the dashboard. The IP stays on the
    // details screen as the type-in fallback.
    char payload[128];
    wifi_ui::formatPairUrl(payload, sizeof(payload),
                           apMode ? net::kPairConfigDestinationEncoded
                                  : net::kPairHomeDestination);
    ui::drawQrWithInstruction(c, headerH, "Scan to open in your browser",
                              payload);
    return;
  }

  char host[48];
  std::snprintf(host, sizeof(host), "%s.local", wifiManager.hostname());
  const std::string ip = wifiManager.ip().toString().c_str();
  wifi_ui::drawPairDetails(c, headerH, host, ip.c_str(),
                           auth.currentPin().c_str());
}
