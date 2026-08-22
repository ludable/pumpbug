// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include "net/NetworkStatus.h"

// Coordinates and registers routes for the system services whose lifecycle
// follows network reachability: Wi-Fi state, HTTP auth pairing, and the
// HTTP/SSE server tasks.
class NetworkServicesHost {
 public:
  // Wire cross-module callbacks and register built-in online routes.
  void registerBuiltInRoutes();

  // Bring up the radio from persisted config. Call after all routes have been
  // registered; server tasks start later from update() when Wi-Fi is reachable.
  void startIfConfigured();

  // Drive Wi-Fi/auth state and start/stop HTTP/SSE to match radio reachability.
  void update();

  // Stops HTTP and SSE, cancels pairing, and prevents network services from
  // restarting while device data is erased. Wi-Fi shutdown is attempted;
  // WifiManager logs any driver shutdown failure. The caller must reboot before
  // using network services again.
  void stopForDataErase();

  NetworkStatus networkStatus() const;

 private:
  bool _stoppedForReboot = false;
  bool wifiReachable() const;
  void syncServers();
};
