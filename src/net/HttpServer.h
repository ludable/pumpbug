// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <WebServer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

// One route. `path` is relative to whatever prefix it's registered under
// (e.g. "/snapshot" under "/app/scale" becomes "/app/scale/snapshot").
// `requiresAuth=false` is reserved for authentication bootstrap routes and the
// read-only crash summary, which must remain available if authentication fails.
struct HttpRoute {
  std::string path;
  HTTPMethod method;
  std::function<void(WebServer&)> handler;
  bool requiresAuth = true;
};

// Sync WebServer on its own FreeRTOS task pinned to core 0 (same core LWIP
// runs on), keeping the UI loop on core 1 untouched. Static assets come
// from src/net/embedded_assets.{h,cpp} (gzipped, served with
// Content-Encoding: gzip).
//
// Route registry is keyed by prefix and survives server start/stop, so
// callers register once at boot and don't have to re-register on Wi-Fi
// toggles. Runtime registry changes trigger a rebuild on the server task
// (in-flight requests get dropped — fine given how rarely this happens).
class HttpServer {
 public:
  // Returns true to allow the request, false to reject it (server replies
  // with 401). Set via setAuthMiddleware(); when not set, all routes pass.
  using AuthMiddleware = std::function<bool(WebServer&)>;

  HttpServer();
  ~HttpServer();

  bool begin();
  void stop();
  // Reports the *intent* (begin called and not yet stopped). The task body
  // hasn't necessarily reached its loop yet; callers polling whether to
  // start/stop want this, not the slower _taskExited observation.
  bool isRunning() const { return _shouldRun.load(std::memory_order_acquire); }

  // Register routes under an explicit prefix (e.g. "/auth", "/app/scale").
  // Additive: multiple registerRoutes() calls with the same prefix
  // accumulate (no dedup — callers must avoid path collisions).
  // unregisterRoutes() clears the entire group.
  void registerRoutes(const char* prefix, std::vector<HttpRoute> routes);
  void unregisterRoutes(const char* prefix);

  // Convenience wrapper that prefixes with "/app/<name>".
  void registerApp(const char* name, std::vector<HttpRoute> routes);
  void unregisterApp(const char* name);

  // Installs the middleware applied to every route with requiresAuth=true.
  // Pass an empty std::function to clear. Safe to call from any task.
  void setAuthMiddleware(AuthMiddleware mw);

 private:
  static constexpr uint16_t kPort = 80;
  static constexpr uint32_t kTaskStackBytes = 8192;
  static constexpr UBaseType_t kTaskPriority = 1;
  static constexpr BaseType_t kTaskCore = 0;
  static constexpr TickType_t kLoopDelayTicks = pdMS_TO_TICKS(10);

  static void _taskTrampoline(void* arg);
  void _taskLoop();
  void _bindRoutes(WebServer& server);
  bool _serveEmbeddedAsset(WebServer& server, const String& uri);
  void _sendMdnsRedirect(WebServer& server);
  // True if the auth middleware accepts this request; false when no
  // middleware is installed.
  bool _isAuthenticated(WebServer& server);

  // Hostname snapshot captured on the main task in begin(), immutable for
  // the server session — the server task must not read WifiManager's
  // main-task-owned hostname cache.
  char _mdnsHost[32] = "";
  // mDNS hostname the server task is currently broadcasting; captured
  // after MDNS.begin() succeeds. Empty when mDNS is down — used as a
  // signal to skip the IP→.local redirect.
  std::string _activeHost;

  std::atomic<bool> _shouldRun{false};
  // True until the trampoline has confirmed the task has finished its
  // loop. Used by stop() to wait for completion; survives the gap between
  // xTaskCreate and the scheduler actually picking up the task, so a
  // back-to-back begin/stop can't slip through and leave a zombie task
  // racing the next begin().
  std::atomic<bool> _taskExited{true};
  std::atomic<bool> _routesDirty{false};
  TaskHandle_t _taskHandle = nullptr;

  // Guards _routeGroups and _authMiddleware against concurrent access from
  // the server task (rebind) and any task calling register/unregister.
  SemaphoreHandle_t _mutex;
  std::map<std::string, std::vector<HttpRoute>> _routeGroups;
  AuthMiddleware _authMiddleware;
};

extern HttpServer httpServer;

// Conditional-GET helper for ETag validators. If the client's If-None-Match
// equals `etag`, sets the ETag response header, sends a 304, and returns true.
// On a miss it sets nothing and returns false — the caller emits its own ETag
// header before the 200 body, so the validator on a 200 always describes the
// body that's actually streamed. (Used by the runtime event logs and shot
// history.)
bool httpEtagOr304(WebServer& s, const char* etag);
