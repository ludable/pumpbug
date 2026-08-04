// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#include "WifiSetupWizard.h"

#include <cstdio>
#include <string>

#include "net/Auth.h"
#include "net/PairRedirectPolicy.h"
#include "net/WifiManager.h"
#include "power/PowerManager.h"
#include "ui/theme.h"
#include "wifi_ui.h"

void WifiSetupWizard::onEnter() {
  _outcome = Outcome::None;
  switch (wifiManager.state()) {
    case WifiManager::State::StaConnected:
    case WifiManager::State::StaConnecting:
      _restore = CancelRestore::RetrySta;
      break;
    case WifiManager::State::ApUp:
      _restore = CancelRestore::KeepAp;
      break;
    default:
      _restore = CancelRestore::LeaveOff;
      break;
  }
  // The deferred start processes on the next main-loop tick — before a human
  // can scan anything — and the guide card covers the gap, so even the
  // radio-off case takes the request path rather than special-casing a
  // direct startAp() here.
  _awaitingAp = wifiManager.state() != WifiManager::State::ApUp;
  if (_awaitingAp) wifiManager.requestStartAp();
  _step = Step::JoinQr;
  _showDetails = false;
  _showGuide = true;
  _sweep.reset();
  requestDraw();
}

void WifiSetupWizard::onExit() {
  if (auth.isPairing()) auth.cancelPairing();
}

void WifiSetupWizard::_setStep(Step s) {
  _step = s;
  _showDetails = false;
  _showGuide = _stepHasQr();
  if (s == Step::OpenQr) {
    _pairBaseline = auth.redeemCount();
    auth.startPairing();
  }
  requestDraw();
}

void WifiSetupWizard::_ensurePairingFresh() {
  if (!auth.isPairing() && auth.redeemCount() == _pairBaseline) {
    auth.startPairing();
    requestDraw();
  }
}

ScreenResult WifiSetupWizard::_finish(Outcome outcome) {
  _outcome = outcome;
  return exit();
}

ScreenResult WifiSetupWizard::_cancel() {
  if (auth.isPairing()) auth.cancelPairing();
  switch (_restore) {
    case CancelRestore::RetrySta:
      wifiManager.requestRetrySta(/*persist=*/false);
      return _finish(Outcome::Cancelled);
    case CancelRestore::LeaveOff:
      // From-scratch entry (radio was off, nothing configured): back out to
      // the menu. The status screen would auto-bounce an unconfigured device
      // right back into the wizard.
      wifiManager.requestStop(/*persist=*/false);
      return _finish(Outcome::Cancelled);
    case CancelRestore::KeepAp:
      return _finish(Outcome::Cancelled);
  }
  return _finish(Outcome::Cancelled);
}

bool WifiSetupWizard::_stepHasQr() const {
  return _step == Step::JoinQr || _step == Step::OpenQr;
}

ScreenResult WifiSetupWizard::onEvent(button::Gesture event) {
  if (event == button::Gesture::A_SHORT && _showGuide) {
    _showGuide = false;
    requestDraw();
    return stay();
  }
  if (event == button::Gesture::A_SHORT && _stepHasQr()) {
    _showDetails = !_showDetails;
    requestDraw();
    return stay();
  }

  switch (_step) {
    case Step::JoinQr:
    case Step::OpenQr:
    case Step::InBrowser:
      if (event == button::Gesture::B_SHORT) return _cancel();
      break;
    case Step::Connecting:
      // The STA attempt continues on its own; leaving here just returns to
      // the status screen, which shows the outcome.
      if (event == button::Gesture::B_SHORT) {
        return _finish(Outcome::Cancelled);
      }
      break;
    case Step::Success:
      // A advertises the completion action. B also exits because completed
      // setup can no longer be cancelled, but it stays out of the hint bar.
      if (event == button::Gesture::A_SHORT ||
          event == button::Gesture::B_SHORT) {
        return _finish(Outcome::Completed);
      }
      break;
    case Step::Failed:
      if (event == button::Gesture::A_SHORT) {
        // The failed STA attempt already fell back to AP, so retry re-enters
        // at the join step with the same AP credentials.
        _setStep(Step::JoinQr);
        return stay();
      }
      if (event == button::Gesture::B_SHORT) {
        return _finish(Outcome::Cancelled);
      }
      break;
  }
  return ignored();
}

ScreenResult WifiSetupWizard::tick() {
  // A QR on a dimmed panel can't be scanned; hold the screen awake for the
  // wizard's whole lifetime.
  power::powerManager.keepAwake();

  if (_awaitingAp) {
    if (wifiManager.state() != WifiManager::State::ApUp) return stay();
    _awaitingAp = false;
  }

  // A client paired in an earlier session can skip the initial steps entirely:
  // its cookie still works, so it can submit credentials from the web at any
  // point. Follow the radio from whichever step the wizard is on.
  if (_step != Step::Connecting && _step != Step::Success) {
    switch (wifiManager.state()) {
      case WifiManager::State::StaConnecting:
        _setStep(Step::Connecting);
        return stay();
      case WifiManager::State::StaConnected:
        _setStep(Step::Success);
        return stay();
      default:
        break;
    }
  }

  switch (_step) {
    case Step::JoinQr:
      if (wifiManager.apStationCount() > 0) _setStep(Step::OpenQr);
      break;
    case Step::OpenQr:
      _ensurePairingFresh();
      if (auth.redeemCount() > _pairBaseline) _setStep(Step::InBrowser);
      break;
    case Step::InBrowser:
      // Waits for the radio: the overtake rule above moves to Connecting
      // when the credentials arrive.
      break;
    case Step::Connecting: {
      if (_sweep.advance(millis())) requestDraw();
      switch (wifiManager.state()) {
        case WifiManager::State::StaConnected:
          _setStep(Step::Success);
          break;
        case WifiManager::State::ApUp:
          // A deliberate AP started by another surface shouldn't read as a
          // failed attempt, so check the manager's fallback flag rather than
          // inferring failure from AP-up alone.
          if (wifiManager.apIsFallback()) _setStep(Step::Failed);
          break;
        default:
          break;
      }
      break;
    }
    case Step::Success:
      break;
    case Step::Failed:
      break;
  }
  return stay();
}

// ----- drawing ------------------------------------------------------------

WifiSetupWizard::StepInfo WifiSetupWizard::_stepInfo() const {
  switch (_step) {
    case Step::JoinQr:
      return {"JOIN", "1/3", theme::accent(),
              "This device creates a setup Wi-Fi network. Scan with a phone, "
              "or choose TEXT to join manually."};
    case Step::OpenQr:
      return {"PAIR", "2/3", theme::accent(),
              "Open the setup page in your browser. Scan the next code or "
              "choose TEXT to enter the address."};
    case Step::InBrowser:
      return {"IN BROWSER", "2/3", theme::accent(), nullptr};
    case Step::Connecting:
      // The header's right slot shows the animated Wi-Fi glyph instead of a
      // counter (see _drawConnecting).
      return {"CONNECTING", nullptr, theme::accent(), nullptr};
    case Step::Success:
      return {"FINISH", "3/3", theme::ok(),
              "Connected! Reconnect to your Wi-Fi, then open Pump Bug in the "
              "browser."};
    case Step::Failed:
      // critical(), not warn(): the yellow is unreadable on the light
      // theme's white background.
      return {"FAILED", nullptr, theme::critical(), nullptr};
  }
  return {"", nullptr, theme::accent(), nullptr};
}

// The pre-QR guide card. Consecutive QR screens look alike at this size, so
// the card between them is what makes a step change register, and allows the
// help text to be large enough to be read.
void WifiSetupWizard::_drawGuide(LGFX_Sprite* c) {
  const StepInfo info = _stepInfo();
  if (!info.guide) return;
  ui::drawGuideCard(c, info.title, info.titleColor, info.counter, info.guide);
}

void WifiSetupWizard::_drawJoinQr(LGFX_Sprite* c) {
  const StepInfo info = _stepInfo();
  const int headerH =
      ui::drawViewHeader(c, info.title, info.titleColor, info.counter);
  if (!_showDetails) {
    // deviceId() is the AP's SSID and is valid even while a requested AP
    // start is still pending (ssid() would still name the outgoing STA
    // network at that point). No escaping needed: SSID and password both
    // come from fixed charsets (see _ensureApSsid / kApPassCharset).
    char payload[128];
    std::snprintf(payload, sizeof(payload), "WIFI:T:WPA;S:%s;P:%s;;",
                  wifiManager.deviceId(), wifiManager.apPassword());
    ui::drawQrWithInstruction(c, headerH, "Scan to join setup Wi-Fi", payload);
    return;
  }
  const ui::InfoBlock blocks[] = {
      {"NET", wifiManager.deviceId(), theme::fg(), 30},
      {"PWD", wifiManager.apPassword(), theme::fg(), 30},
  };
  ui::drawInfoScreen(c, headerH, "Or join this network manually:", nullptr,
                     blocks, 2, theme::bg(), wifi_ui::kLabelSizeRef);
}

void WifiSetupWizard::_drawOpenQr(LGFX_Sprite* c) {
  const StepInfo info = _stepInfo();
  const int headerH =
      ui::drawViewHeader(c, info.title, info.titleColor, info.counter);
  if (!_showDetails) {
    // Pairing at the mDNS origin lets the browser keep using its cookie after
    // the device moves to the home network. The AP answers .local queries;
    // the IP remains available as the type-in fallback.
    char payload[128];
    wifi_ui::formatPairUrl(payload, sizeof(payload),
                           net::kPairConfigDestinationEncoded);
    ui::drawQrWithInstruction(c, headerH, "Scan to open the setup page",
                              payload);
    return;
  }
  const std::string ip = wifiManager.ip().toString().c_str();
  wifi_ui::drawPairDetails(c, headerH, /*host=*/nullptr, ip.c_str(),
                           auth.currentPin().c_str());
}

void WifiSetupWizard::_drawInBrowser(LGFX_Sprite* c) {
  const StepInfo info = _stepInfo();
  ui::drawGuideCard(c, info.title, info.titleColor, info.counter,
                    "Pick your Wi-Fi network in the browser");
}

void WifiSetupWizard::_drawConnecting(LGFX_Sprite* c) {
  const StepInfo info = _stepInfo();
  const int headerH = wifi_ui::drawWifiHeader(
      c, info.title, NetworkStatus::Connecting, _sweep.bands());
  ui::drawCardBody(c, headerH, wifiManager.ssid());
}

void WifiSetupWizard::_drawSuccess(LGFX_Sprite* c) {
  const StepInfo info = _stepInfo();
  ui::drawGuideCard(c, info.title, info.titleColor, info.counter, info.guide);
}

void WifiSetupWizard::_drawFailed(LGFX_Sprite* c) {
  const StepInfo info = _stepInfo();
  const char* reason = wifiManager.lastDisconnectReason();
  ui::drawGuideCard(c, info.title, info.titleColor, nullptr,
                    (reason && *reason) ? reason : "no response from network");
}

bool WifiSetupWizard::onDraw(LGFX_Sprite* c) {
  c->fillScreen(theme::bg());
  if (_showGuide) {
    _drawGuide(c);
    return true;
  }
  switch (_step) {
    case Step::JoinQr:
      _drawJoinQr(c);
      break;
    case Step::OpenQr:
      _drawOpenQr(c);
      break;
    case Step::InBrowser:
      _drawInBrowser(c);
      break;
    case Step::Connecting:
      _drawConnecting(c);
      break;
    case Step::Success:
      _drawSuccess(c);
      break;
    case Step::Failed:
      _drawFailed(c);
      break;
  }
  return true;
}

ButtonHints WifiSetupWizard::buttonHints() const {
  if (_showGuide) {
    return {{Hint{HintGlyph::None, "NEXT"}}, {Hint{HintGlyph::Cancel}}};
  }
  const Hint toggle = Hint{HintGlyph::None, _showDetails ? "QR" : "TEXT"};
  switch (_step) {
    case Step::JoinQr:
    case Step::OpenQr:
      return {{toggle}, {Hint{HintGlyph::Cancel}}};
    case Step::InBrowser:
      return {{}, {Hint{HintGlyph::Cancel}}};
    case Step::Connecting:
      return {{}, {Hint{HintGlyph::Back}}};
    case Step::Success:
      return {{Hint{HintGlyph::Ok, "DONE"}}, {}};
    case Step::Failed:
      return {{Hint{HintGlyph::None, "RETRY"}}, {Hint{HintGlyph::Back}}};
  }
  return {};
}
