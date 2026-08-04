// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <cstdint>

// Network reachability state exposed to presentation layers. Kept separate
// from WifiManager::State so UI code doesn't need the Wi-Fi manager internals.
enum class NetworkStatus : uint8_t {
  Off,         // radio down / idle
  Connecting,  // STA connecting
  Connected,   // STA connected
  Ap,          // soft-AP up
  Failed,      // STA attempt failed
};
