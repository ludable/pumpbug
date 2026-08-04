// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#include "HttpServer.h"

#include <ESPmDNS.h>
#include <M5Unified.h>

#include <memory>
#include <new>
#include <utility>

#include "WifiManager.h"
#include "embedded_assets.h"
#include "product.h"
#include "util/scoped_lock.h"

HttpServer httpServer;

bool httpEtagOr304(WebServer& s, const char* etag) {
  if (s.hasHeader("If-None-Match") && s.header("If-None-Match") == etag) {
    s.sendHeader("ETag", etag);
    s.send(304, "application/json", "");
    return true;
  }
  return false;
}

namespace {
constexpr const char* kAppPrefix = "/app/";
constexpr const char* kCookieHeader = "Cookie";
constexpr uint32_t kMdnsRetryMs = 30000;
constexpr uint32_t kRebuildRetryMs = 5000;
// Give a new client time for request bytes delayed by normal local-network
// scheduling to arrive before preferring a queued connection.
constexpr uint32_t kPendingClientGraceMs = 250;

class PendingAwareWebServer final : public WebServer {
 public:
  using WebServer::WebServer;

  void handleClient() override {
    // Prevent an idle speculative browser connection from blocking a queued
    // request for WebServer's full read timeout. Safari can open connections in
    // that order during navigation, so allow a short grace for request bytes in
    // flight before preferring the queued client.
    //
    // This relies on WebServer leaving _statusChange unchanged while awaiting
    // request bytes and clearing all request state after a disconnected client.
    // Recheck both behaviors before changing the pinned platform version.
    if (_currentStatus == HC_WAIT_READ && !_currentClient.available() &&
        millis() - _statusChange >= kPendingClientGraceMs &&
        _server.hasClient()) {
      _currentClient.stop();
    }
    WebServer::handleClient();
  }
};
}  // namespace

HttpServer::HttpServer() : _mutex(xSemaphoreCreateMutex()) {}

HttpServer::~HttpServer() {
  stop();
  if (_mutex) {
    vSemaphoreDelete(_mutex);
    _mutex = nullptr;
  }
}

bool HttpServer::begin() {
  // Already requested? Idempotent. begin/stop are only called from the
  // main task today (so no real CAS races), but the guard is cheap.
  if (_shouldRun.load(std::memory_order_acquire)) return true;

  // A previous task may still be in cleanup between _taskLoop returning
  // and vTaskDelete firing — wait for it to fully exit before spawning
  // a fresh one. Defensive; today this only happens transiently.
  while (!_taskExited.load(std::memory_order_acquire)) {
    vTaskDelay(pdMS_TO_TICKS(5));
  }
  _taskExited.store(false, std::memory_order_release);
  _shouldRun.store(true, std::memory_order_release);

  // Snapshot the hostname here, on the main task, so the server task never
  // touches WifiManager's main-task-owned cache. A rename during the
  // session shows up at the next begin() — matching the "takes effect on
  // the next server start" contract.
  strlcpy(_mdnsHost, wifiManager.hostname(), sizeof(_mdnsHost));

  // usStackDepth is in BYTES on the ESP32 FreeRTOS port; don't divide.
  const BaseType_t ok = xTaskCreatePinnedToCore(
      &HttpServer::_taskTrampoline, "http", kTaskStackBytes, this,
      kTaskPriority, &_taskHandle, kTaskCore);
  if (ok != pdPASS) {
    M5_LOGE("HttpServer: failed to spawn task");
    _shouldRun.store(false, std::memory_order_release);
    _taskExited.store(true, std::memory_order_release);
    return false;
  }
  return true;
}

void HttpServer::stop() {
  if (!_shouldRun.load(std::memory_order_acquire)) return;
  _shouldRun.store(false, std::memory_order_release);
  // Wait for the task to actually run and finish its cleanup. The
  // scheduler will get to it; _taskExited flips inside the trampoline
  // right before vTaskDelete, so we can't return before the prior task
  // has cleared its WebServer / mDNS resources.
  while (!_taskExited.load(std::memory_order_acquire)) {
    vTaskDelay(pdMS_TO_TICKS(5));
  }
  _taskHandle = nullptr;
}

void HttpServer::registerRoutes(const char* prefix,
                                std::vector<HttpRoute> routes) {
  if (!prefix || !*prefix || !_mutex) return;
  xSemaphoreTake(_mutex, portMAX_DELAY);
  // Additive: multiple modules may register routes under the same prefix.
  // Callers are responsible for not creating path collisions; the underlying
  // WebServer doesn't deduplicate.
  auto& dst = _routeGroups[prefix];
  for (auto& r : routes) dst.push_back(std::move(r));
  xSemaphoreGive(_mutex);
  _routesDirty.store(true, std::memory_order_release);
}

void HttpServer::unregisterRoutes(const char* prefix) {
  if (!prefix || !*prefix || !_mutex) return;
  xSemaphoreTake(_mutex, portMAX_DELAY);
  _routeGroups.erase(prefix);
  xSemaphoreGive(_mutex);
  _routesDirty.store(true, std::memory_order_release);
}

void HttpServer::registerApp(const char* name, std::vector<HttpRoute> routes) {
  if (!name || !*name) return;
  std::string prefix = kAppPrefix;
  prefix += name;
  registerRoutes(prefix.c_str(), std::move(routes));
}

void HttpServer::unregisterApp(const char* name) {
  if (!name || !*name) return;
  std::string prefix = kAppPrefix;
  prefix += name;
  unregisterRoutes(prefix.c_str());
}

void HttpServer::setAuthMiddleware(AuthMiddleware mw) {
  if (!_mutex) return;
  xSemaphoreTake(_mutex, portMAX_DELAY);
  _authMiddleware = std::move(mw);
  xSemaphoreGive(_mutex);
  _routesDirty.store(true, std::memory_order_release);
}

void HttpServer::_taskTrampoline(void* arg) {
  HttpServer* self = static_cast<HttpServer*>(arg);
  self->_taskLoop();
  // Publish "task has fully exited" before self-deletion so stop() can
  // wait for completion safely even if it was called before the task
  // was scheduled.
  self->_taskExited.store(true, std::memory_order_release);
  vTaskDelete(nullptr);
}

void HttpServer::_taskLoop() {
  // We may have been asked to stop before the scheduler picked us up. Bail
  // before allocating anything.
  if (!_shouldRun.load(std::memory_order_acquire)) return;

  std::unique_ptr<WebServer> server;

  auto rebuild = [&]() -> bool {
    try {
      std::unique_ptr<WebServer> replacement(new (std::nothrow)
                                                 PendingAwareWebServer(kPort));
      if (!replacement) {
        M5_LOGE("HttpServer: no heap for server");
        return false;
      }
      // "Host" is read by the IP→.local redirect path; "Cookie" by auth;
      // "If-None-Match" by the diagnostics combined-log conditional GET.
      const char* collected[] = {kCookieHeader, "Host", "If-None-Match"};
      replacement->collectHeaders(collected, 3);
      _bindRoutes(*replacement);
      WebServer* sp = replacement.get();
      sp->onNotFound([this, sp]() {
        if (!_serveEmbeddedAsset(*sp, sp->uri())) {
          sp->send(404, "text/plain", "Not found");
        }
      });
      // Keep the current server alive until its replacement is fully
      // configured. A failed allocation during a runtime route rebuild
      // therefore leaves the existing routes available.
      server = std::move(replacement);
      server->begin();
      return true;
    } catch (const std::bad_alloc&) {
      // This target has no exception emergency pool. The catch still handles
      // fragmentation failures when enough heap remains to construct
      // bad_alloc. Only explicit nothrow allocations, such as the WebServer
      // object above, can report absolute exhaustion without terminating.
      M5_LOGE("HttpServer: no heap while configuring server");
      return false;
    }
  };

  auto waitForRebuildRetry = [&]() {
    constexpr uint32_t kStopCheckMs = 100;
    for (uint32_t waitedMs = 0; waitedMs < kRebuildRetryMs;
         waitedMs += kStopCheckMs) {
      if (!_shouldRun.load(std::memory_order_acquire)) return false;
      vTaskDelay(pdMS_TO_TICKS(kStopCheckMs));
    }
    return _shouldRun.load(std::memory_order_acquire);
  };

  // Retry recoverable configuration failures inside this task. Exiting would
  // make the main task respawn us every loop while the heap is under pressure.
  while (_shouldRun.load(std::memory_order_acquire)) {
    _routesDirty.exchange(false, std::memory_order_acq_rel);
    if (rebuild()) break;
    _routesDirty.store(true, std::memory_order_release);
    if (!waitForRebuildRetry()) return;
  }
  if (!_shouldRun.load(std::memory_order_acquire)) return;

  uint32_t lastMdnsAttemptMs = 0;
  auto startMdns = [&]() {
    // _mdnsHost is begin()'s immutable snapshot of the user-overridable
    // hostname (deviceId() fallback baked in). Captures into _activeHost so
    // the IP→.local redirect points at what's currently broadcast, not at
    // a freshly updated hostname that hasn't taken effect yet.
    const char* host = _mdnsHost;
    if (lastMdnsAttemptMs != 0) MDNS.end();
    lastMdnsAttemptMs = millis();
    if (MDNS.begin(host)) {
      MDNS.addService("http", "tcp", kPort);
      _activeHost = host;
      M5_LOGI("HttpServer: mDNS %s.local up", host);
      return true;
    }
    _activeHost.clear();
    M5_LOGW("HttpServer: mDNS begin failed for %s.local; will retry", host);
    return false;
  };

  startMdns();

  M5_LOGI("HttpServer: listening on :%u", kPort);

  bool rebuildBackoff = false;
  uint32_t lastRebuildFailureMs = 0;
  while (_shouldRun.load(std::memory_order_acquire)) {
    const uint32_t nowMs = millis();
    if (_routesDirty.load(std::memory_order_acquire) &&
        (!rebuildBackoff || nowMs - lastRebuildFailureMs >= kRebuildRetryMs) &&
        _routesDirty.exchange(false, std::memory_order_acq_rel)) {
      if (rebuild()) {
        rebuildBackoff = false;
      } else {
        // Preserve the requested route set and retry without allocating on
        // every HTTP loop iteration while memory remains constrained.
        _routesDirty.store(true, std::memory_order_release);
        rebuildBackoff = true;
        lastRebuildFailureMs = nowMs;
      }
    }
    if (_activeHost.empty() && nowMs - lastMdnsAttemptMs >= kMdnsRetryMs) {
      startMdns();
    }
    server->handleClient();
    vTaskDelay(kLoopDelayTicks);
  }

  MDNS.end();
  if (server) server->close();
  server.reset();
  M5_LOGI("HttpServer: stopped");
}

void HttpServer::_bindRoutes(WebServer& server) {
  if (!_mutex) return;

  std::map<std::string, std::vector<HttpRoute>> snapshot;
  AuthMiddleware mw;
  {
    ScopedLock lock(_mutex);
    snapshot = _routeGroups;
    mw = _authMiddleware;
  }

  WebServer* sp = &server;
  for (const auto& [prefix, routes] : snapshot) {
    for (const auto& route : routes) {
      std::string full = prefix;
      if (!route.path.empty() && route.path.front() != '/') full += '/';
      full += route.path;

      auto handler = route.handler;
      if (!route.requiresAuth) {
        sp->on(full.c_str(), route.method, [sp, handler]() { handler(*sp); });
      } else if (mw) {
        AuthMiddleware gate = mw;
        sp->on(full.c_str(), route.method, [sp, handler, gate]() {
          if (!gate(*sp)) {
            sp->send(401, "application/json",
                     "{\"error\":\"unauthenticated\"}");
            return;
          }
          handler(*sp);
        });
      } else {
        // requiresAuth=true but no middleware installed — fail closed
        // rather than silently exposing the route.
        sp->on(full.c_str(), route.method, [sp]() {
          sp->send(503, "application/json", "{\"error\":\"auth unavailable\"}");
        });
      }
    }
  }
}

bool HttpServer::_isAuthenticated(WebServer& server) {
  AuthMiddleware mw;
  xSemaphoreTake(_mutex, portMAX_DELAY);
  mw = _authMiddleware;
  xSemaphoreGive(_mutex);
  return mw && mw(server);
}

void HttpServer::_sendMdnsRedirect(WebServer& server) {
  // Use the host the mDNS service is actually broadcasting, not whatever
  // hostname() currently returns — those can diverge after the user sets
  // a new hostname (it only takes effect on the next server (re)start).
  //
  // No auto-redirect: clients without mDNS need a reliable way to bail,
  // and an immediate `location.replace` runs before the fallback link
  // becomes clickable. Click-through is one extra tap for happy-path
  // users but deterministic for everyone.
  const std::string& host = _activeHost;
  std::string body =
      "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">"
      "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
      "<title>";
  body += product::PRODUCT_NAME;
  body +=
      "</title>"
      "<style>"
      "body{font:14px ui-monospace,\"SF Mono\",Menlo,monospace;"
      "background:#000;color:#fff;padding:32px 16px;text-align:center;"
      "max-width:480px;margin:0 auto}"
      "h1{font-size:1.5rem;margin:0 0 16px}"
      "a{color:#2080c0;text-decoration:none}"
      ".primary{display:inline-block;padding:12px 20px;margin:16px 0;"
      "background:#2080c0;color:#fff;border-radius:4px}"
      ".dim{color:#888;font-size:0.875rem;margin-top:24px}"
      "</style></head><body>"
      "<h1>Continue to pairing</h1>"
      "<p>Use the link below to continue at Pump Bug's local URL. This keeps "
      "this browser paired after Wi-Fi setup.</p>"
      "<p><a class=\"primary\" href=\"http://";
  body += host;
  body += ".local/config/\">Continue to pairing</a></p>";
  body +=
      "<p class=\"dim\">If the local URL does not open, "
      "<a href=\"/config/\">continue at this IP address</a> instead. "
      "You may need to pair again after setup."
      "</p></body></html>";
  // Don't cache — the destination hostname changes if the user renames
  // the device, and stale captures here would land on a dead name.
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "text/html; charset=utf-8", body.c_str());
}

bool HttpServer::_serveEmbeddedAsset(WebServer& server, const String& uri) {
  String path = uri;
  if (path == "/") {
    path = "/home/index.html";
  } else if (path.endsWith("/")) {
    path += "index.html";
  }

  // IP→.local handoff on the hub (the entry page) lets the pairing cookie land
  // on the persistent mDNS origin before the AP → STA switch. The interstitial
  // also links directly to /config/ on the IP origin for clients without mDNS.
  // Already-paired clients remain on their current origin because the .local
  // origin would not have their cookie.
  if (path == "/home/index.html" && !_activeHost.empty()) {
    const String hostHeader =
        server.hasHeader("Host") ? server.header("Host") : String();
    if (hostHeader.indexOf(".local") < 0 && !_isAuthenticated(server)) {
      _sendMdnsRedirect(server);
      return true;
    }
  }

  for (size_t i = 0; i < kEmbeddedAssetCount; ++i) {
    const EmbeddedAsset& a = kEmbeddedAssets[i];
    if (path == a.path) {
      // Assets are baked into the firmware image, so they change exactly
      // when the firmware does. A per-build ETag with no-cache makes every
      // page load a cheap conditional request (304 when unchanged) instead
      // of letting browsers serve day-old pages across a flash.
      static String etag;
      if (etag.isEmpty()) etag = "\"" + ESP.getSketchMD5() + "\"";
      if (httpEtagOr304(server, etag.c_str())) return true;
      server.sendHeader("ETag", etag.c_str());
      server.sendHeader("Cache-Control", "no-cache");
      server.sendHeader("Content-Encoding", "gzip");
      server.setContentLength(a.size);
      server.send(200, a.content_type, "");
      server.sendContent_P(reinterpret_cast<PGM_P>(a.data), a.size);
      return true;
    }
  }
  return false;
}
