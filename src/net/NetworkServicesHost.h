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

  NetworkStatus networkStatus() const;

 private:
  bool wifiReachable() const;
  void syncServers();
};
