// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <M5Unified.h>

#include <cstdint>

#include "ui/Screen.h"
#include "ui/button.h"
#include "ui/icons.h"

// Guided Wi-Fi setup: a sequence of screens with instructions and QR codes that
// advances automatically as the user completes the steps.
//
// Steps:
// (1) Join the device's own access point by scanning a QR (JoinQr). Advances
// automatically when the firmware detects a station joining the AP.
// (2) Open a pairing URL by scanning a second QR (OpenQr), landing the browser
// on the config page. Advances automatically when the device is paired.
// (3) Select a network in that browser (InBrowser) and enter its
// password, submit to device. The device advances and uses the credentials to
// join the network (Connecting). Success confirms the connection and directs
// the user to Pump Bug on the home network.
//
// The JOIN and PAIR steps start with a guide card, followed by a QR code and a
// text-details fallback. FINISH returns control to the browser handoff.
class WifiSetupWizard : public Screen {
 public:
  enum class Outcome : uint8_t { None, Completed, Cancelled };

  // Returns how the latest run ended and clears it for the next caller.
  Outcome takeOutcome() {
    const Outcome outcome = _outcome;
    _outcome = Outcome::None;
    return outcome;
  }

  void onEnter() override;
  void onExit() override;
  ScreenResult onEvent(button::Gesture event) override;
  // Setup owns a temporary AP and pairing window. Leave through its B action
  // so cancellation restores the radio state found on entry.
  bool allowsRootShortcut() const override { return false; }
  ScreenResult tick() override;
  bool onDraw(LGFX_Sprite* canvas) override;
  ButtonHints buttonHints() const override;
  uint32_t desiredTickMs() const override { return 100; }

 private:
  enum class Step : uint8_t {
    JoinQr,
    OpenQr,
    InBrowser,
    Connecting,
    Success,
    Failed,
  };
  // What cancelling should do with the radio, decided from the state the
  // wizard found on entry.
  enum class CancelRestore : uint8_t { LeaveOff, RetrySta, KeepAp };

  // Header metadata and guide copy for the current step — the single source
  // both the guide card and the step screens' headers read, so they can't
  // drift apart. `guide` is nullptr on steps without a card.
  struct StepInfo {
    const char* title;
    const char* counter;
    uint32_t titleColor;
    const char* guide;
  };
  StepInfo _stepInfo() const;

  void _setStep(Step s);
  ScreenResult _finish(Outcome outcome);
  // Restarts the pairing window (fresh token, so the QR re-renders) when the
  // previous one expired unredeemed.
  void _ensurePairingFresh();
  // Restores the radio to its pre-wizard state and reports cancellation to
  // the caller.
  ScreenResult _cancel();
  bool _stepHasQr() const;

  void _drawGuide(LGFX_Sprite* c);
  void _drawJoinQr(LGFX_Sprite* c);
  void _drawOpenQr(LGFX_Sprite* c);
  void _drawInBrowser(LGFX_Sprite* c);
  void _drawConnecting(LGFX_Sprite* c);
  void _drawSuccess(LGFX_Sprite* c);
  void _drawFailed(LGFX_Sprite* c);

  Step _step = Step::JoinQr;
  Outcome _outcome = Outcome::None;
  CancelRestore _restore = CancelRestore::LeaveOff;
  // True while a requested AP start is still pending. Until it processes,
  // the radio state still describes the outgoing STA connection, so the
  // wizard must not read it (tick would mistake it for setup progress).
  bool _awaitingAp = false;
  // On QR steps, A toggles between the QR and a text screen with the same
  // information in textual form (to connect without camera).
  bool _showDetails = false;
  // True while a QR step is showing its guide card instead of the code;
  // armed on every arrival at a QR step, dismissed by A.
  bool _showGuide = false;
  // Auth::redeemCount() at the start of the current pairing window; a rise
  // means our QR (or the PIN) was redeemed.
  uint32_t _pairBaseline = 0;
  ui::ConnectingSweep _sweep;
};
