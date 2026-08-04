// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#include "NetworkServicesHost.h"

#include "net/Auth.h"
#include "net/HttpServer.h"
#include "net/SseServer.h"
#include "net/WifiManager.h"

void NetworkServicesHost::registerBuiltInRoutes() {
  // First successful pair marks the device configured so the radio comes back
  // up automatically at the next boot. Symmetric callback wipes auth tokens
  // whenever the wifi config is reset. Both directions are wired here so the
  // two modules don't take a direct dependency on each other.
  auth.setOnFirstPair([]() { wifiManager.markConfigured(); });
  wifiManager.setOnReset([]() { auth.forgetAll(); });

  auth.registerWith(httpServer);
  wifiManager.registerWith(httpServer);
}

void NetworkServicesHost::startIfConfigured() {
  wifiManager.beginIfPersisted();
}

bool NetworkServicesHost::wifiReachable() const {
  const WifiManager::State s = wifiManager.state();
  return s == WifiManager::State::StaConnected || s == WifiManager::State::ApUp;
}

void NetworkServicesHost::syncServers() {
  // Track HttpServer + SseServer alongside Wi-Fi reachability. When the radio
  // drops (or comes up), bring both in/out of life to match. Route registries
  // survive restarts; SSE lives on port 81/task so streaming sessions don't
  // block normal HTTP requests.
  if (wifiReachable()) {
    if (!httpServer.isRunning()) httpServer.begin();
    if (!sseServer.isRunning()) sseServer.begin();
  } else {
    if (httpServer.isRunning()) httpServer.stop();
    if (sseServer.isRunning()) sseServer.stop();
  }
}

void NetworkServicesHost::update() {
  wifiManager.update();
  auth.update();
  syncServers();
}

NetworkStatus NetworkServicesHost::networkStatus() const {
  switch (wifiManager.state()) {
    case WifiManager::State::StaConnecting:
      return NetworkStatus::Connecting;
    case WifiManager::State::StaConnected:
      return NetworkStatus::Connected;
    case WifiManager::State::ApUp:
      // Fallback AP keeps the failure visible in the chrome, matching the
      // status screen's header glyph.
      return wifiManager.apIsFallback() ? NetworkStatus::Failed
                                        : NetworkStatus::Ap;
    case WifiManager::State::Off:
    default:
      return NetworkStatus::Off;
  }
}
