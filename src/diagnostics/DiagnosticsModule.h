// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <vector>

#include "apps/scale/BleScanScreen.h"
#include "apps/scale/ScaleMsgScreen.h"
#include "apps/system/EraseDataScreen.h"
#include "apps/system/LogsScreen.h"
#include "apps/system/StorageRecoveryScreen.h"
#include "diagnostics_routes.h"
#include "ui/Menu.h"
#include "ui/Screen.h"
#include "util/storage.h"

class HttpServer;

// Owns the diagnostic screens, their menu, and their HTTP routes.
class DiagnosticsModule {
 public:
  void begin(HttpServer& http) {
    registerDiagnosticsRoutes(http);
    std::vector<Menu::Item> items{
        Menu::Item::open("Logs", _logsScreen),
        Menu::Item::open("BLE scan", _bleScanScreen),
        Menu::Item::open("Scale msgs", _scaleMsgScreen),
    };
    if (storage::mountState() != storage::MountState::Ready) {
      items.push_back(Menu::Item::open("Shot storage", _storageRecoveryScreen));
    }
    items.push_back(Menu::Item::open("Erase all data", _eraseDataScreen));
    _menu.init("DIAGNOSTICS", items);
  }

  Screen& menu() { return _menu; }
  StorageRecoveryScreen& storageRecoveryScreen() {
    return _storageRecoveryScreen;
  }

 private:
  LogsScreen _logsScreen;
  BleScanScreen _bleScanScreen;
  ScaleMsgScreen _scaleMsgScreen;
  EraseDataScreen _eraseDataScreen;
  StorageRecoveryScreen _storageRecoveryScreen;
  Menu _menu;
};
