// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <M5Unified.h>

#include <cstddef>
#include <cstdint>

#include "WifiSetupWizard.h"
#include "ui/Menu.h"
#include "ui/Screen.h"
#include "ui/button.h"
#include "ui/icons.h"

// Steady-state Wi-Fi screen with connection info.
// A VISIT affordance displays a QR with an associated pairing window, so it
// opens the dashboard for paired clients or pairs new ones. A URL and PIN
// provide the no-camera fallback.
//
// A SETTINGS affordance opens the configuration actions: change network
// (WifiSetupWizard), unpair clients, and turn Wi-Fi on or off. Unconfigured
// devices are sent to WifiSetupWizard on entry.
class WifiStatusScreen : public Screen {
 public:
  void onEnter() override;
  void onExit() override;
  ScreenResult onEvent(button::Gesture event) override;
  ScreenResult tick() override;
  bool onDraw(LGFX_Sprite* canvas) override;
  ButtonHints buttonHints() const override;
  uint32_t desiredTickMs() const override { return 100; }

 private:
  enum class View : uint8_t {
    Idle,
    StaConnecting,
    StaConnected,
    ApUp,   // covers post-fallback AP too; banner reflects which
    Visit,  // the visit/pair QR, shown while a pairing window is open
  };
  // Modal layers over the current view.
  enum class Overlay : uint8_t { None, ConfigMenu, ConfirmUnpair };

  enum class ConfigAction : uint8_t {
    EnableWifi,
    DisableWifi,
    ChangeNetwork,
    UnpairClients,
  };

  View _computeView() const;
  void _openConfigMenu();
  ScreenResult _configConfirmed();

  void _drawIdle(LGFX_Sprite* c);
  void _drawApUp(LGFX_Sprite* c);
  void _drawStaConnecting(LGFX_Sprite* c);
  void _drawStaConnected(LGFX_Sprite* c);
  void _drawVisit(LGFX_Sprite* c);

  WifiSetupWizard _wizard;
  Menu _chooser;
  Overlay _overlay = Overlay::None;
  // In the visit view, A toggles between the QR and a text screen with the
  // PIN and address laid out readably.
  bool _pairShowDetails = false;

  // Cached for change detection / animation cadence.
  View _lastView = View::Idle;
  uint32_t _lastPairingSec = UINT32_MAX;
  size_t _lastPairedCount = 0;
  ui::ConnectingSweep _sweep;
};
