// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#include "WifiManager.h"

#include <M5Unified.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>
#include <esp_random.h>
#include <esp_wifi.h>

#include <algorithm>
#include <vector>

#include "HttpServer.h"
#include "JsonStream.h"
#include "SseServer.h"
#include "diagnostics/RuntimeEventLog.h"
#include "product.h"

WifiManager wifiManager;

namespace {
constexpr const char* kNvsNamespace = "wifi";
constexpr const char* kKeyConfigured = "configured";
constexpr const char* kKeyMode = "mode";
constexpr const char* kKeyStaSsid = "sta_ssid";
constexpr const char* kKeyStaPass = "sta_pass";
constexpr const char* kKeyApPass = "ap_pass";
constexpr const char* kKeyHostname = "hostname";

constexpr size_t kMaxSsidLen = 32;  // 802.11 spec cap
constexpr size_t kMaxPassLen = 63;  // WPA2 passphrase cap (64 with NUL)

// Generated AP passphrase: long enough that guessing is impractical for
// this application, short enough to type when the join QR can't be scanned.
// The charset drops the look-alikes (i/l/1, o/0) since the text fallback is
// read off a small screen.
constexpr size_t kApPassLen = 8;
constexpr const char kApPassCharset[] = "abcdefghjkmnpqrstuvwxyz23456789";

constexpr size_t kMaxScanResults = 15;

bool isCredentialFailureReason(uint8_t reason) {
  return reason == WIFI_REASON_AUTH_FAIL ||
         reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT;
}

bool isActionableFailureReason(uint8_t reason) {
  return isCredentialFailureReason(reason) || reason == WIFI_REASON_NO_AP_FOUND;
}

bool wifiDriverIsOff() {
  wifi_mode_t idfMode;
  return WiFi.getMode() == WIFI_MODE_NULL &&
         esp_wifi_get_mode(&idfMode) == ESP_ERR_WIFI_NOT_INIT;
}

}  // namespace

WifiManager::WifiManager() : _pendingMutex(xSemaphoreCreateMutex()) {
  // Build _apSsid up front so cross-task readers (HttpServer task calls
  // hostname() / deviceId() for MDNS.begin, the Wi-Fi screens read ssid()
  // on the main task) never race a lazy std::string write. ESP-IDF efuse
  // is initialised before C++ static init runs on arduino-esp32, so
  // ESP.getEfuseMac() is safe here.
  _ensureApSsid();
}

void WifiManager::_post(PendingRequest r) {
  if (_pendingMutex) xSemaphoreTake(_pendingMutex, portMAX_DELAY);
  // Mode changes are interchangeable intents: the latest wins. A pending
  // reset is a promise and only yields to another reset (which is how a
  // terminal erase upgrades a pending /forget); a terminal reset is final.
  const bool locked =
      _pending.terminal || (_pending.action == PendingAction::Reset &&
                            r.action != PendingAction::Reset);
  if (!locked) _pending = std::move(r);
  if (_pendingMutex) xSemaphoreGive(_pendingMutex);
}

bool WifiManager::hasPendingAction() const {
  if (_pendingMutex) xSemaphoreTake(_pendingMutex, portMAX_DELAY);
  const bool pending = _pending.action != PendingAction::None;
  if (_pendingMutex) xSemaphoreGive(_pendingMutex);
  return pending;
}

void WifiManager::requestConnectSta(const char* ssid, const char* pass) {
  _post({.action = PendingAction::ConnectSta,
         .ssid = ssid ? ssid : "",
         .pass = pass ? pass : ""});
}

void WifiManager::requestReset(bool terminal) {
  _post({.action = PendingAction::Reset, .terminal = terminal});
}

void WifiManager::requestStartAp() {
  _post({.action = PendingAction::StartAp});
}

void WifiManager::requestStop(bool persist) {
  _post({.action = PendingAction::Stop, .persist = persist});
}

void WifiManager::requestRetrySta(bool persist) {
  _post({.action = PendingAction::RetrySta, .persist = persist});
}

void WifiManager::_processPending() {
  PendingRequest r;
  if (_pendingMutex) xSemaphoreTake(_pendingMutex, portMAX_DELAY);
  r = std::move(_pending);
  // Consume the request but keep the terminal latch — it outlives its reset.
  _pending.action = PendingAction::None;
  _pending.terminal = r.terminal;
  _pending.ssid.clear();
  _pending.pass.clear();
  if (_pendingMutex) xSemaphoreGive(_pendingMutex);
  if (r.action == PendingAction::None) return;

  // Resubmitting the live session's credentials is a no-op, decided here —
  // before the server stop — so the requesting client's connection survives
  // untouched. Acting on it instead would strand the manager: startSta()
  // rearms the connect state machine, but WiFi.begin() with an unchanged
  // config on a connected link returns without emitting any events, so the
  // "attempt" would idle 15 s and then tear the working link down into
  // fallback AP.
  if (r.action == PendingAction::ConnectSta &&
      _staSessionMatches(r.ssid, r.pass)) {
    M5_LOGI("Wi-Fi: already connected to %s; request is a no-op",
            r.ssid.c_str());
    return;
  }

  // Stop the HTTP and SSE servers before mutating the radio so we don't
  // yank sockets out from under a live handleClient / SSE session.
  httpServer.stop();
  sseServer.stop();

  switch (r.action) {
    case PendingAction::ConnectSta:
      startSta(r.ssid.c_str(), r.pass.c_str());
      break;
    case PendingAction::Reset:
      resetAllConfig(/*scrubFramework=*/r.terminal);
      break;
    case PendingAction::StartAp:
      startAp(/*persist=*/!isConfigured());
      break;
    case PendingAction::Stop:
      stop(r.persist);
      break;
    case PendingAction::RetrySta:
      retrySta(r.persist);
      break;
    default:
      break;
  }
}

bool WifiManager::_staSessionMatches(const std::string& ssid,
                                     const std::string& pass) const {
  if (state() != State::StaConnected || mode() != Mode::Sta) return false;
  if (ssid != _staSsid) return false;
  // The password isn't kept in RAM; compare against NVS. A connected
  // session's creds always match the stored ones (every connect path either
  // persisted them or loaded them from there), so a mismatch here means the
  // user really is changing the password and needs a full reconnect.
  std::string storedSsid, storedPass;
  if (!_loadStaCreds(storedSsid, storedPass)) return false;
  return storedSsid == ssid && storedPass == pass;
}

void WifiManager::_ensureEventsRegistered() {
  if (_eventsRegistered) return;
  // Keep the Arduino core from mirroring Wi-Fi config into esp_wifi's own
  // NVS storage — creds live only in our namespace, where resetAllConfig()
  // can wipe them. Must run before the first radio init (storage is chosen
  // once, in wifiLowLevelInit); this is called at the top of every start*().
  WiFi.persistent(false);
  WiFi.onEvent([this](arduino_event_id_t event, arduino_event_info_t info) {
    uint8_t reason = 0;
    if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
      reason = info.wifi_sta_disconnected.reason;
    }
    _onWifiEvent(static_cast<int>(event), reason);
  });
  _eventsRegistered = true;
}

void WifiManager::_ensureApSsid() {
  if (!_apSsid.empty()) return;
  const uint64_t mac = ESP.getEfuseMac();
  // "<prefix>-" + 6 hex digits + NUL. Guard against a prefix that would
  // overflow the buffer and silently truncate the SSID/hostname.
  char buf[16];
  static_assert(
      __builtin_strlen(product::NET_PREFIX) + 1 + 6 + 1 <= sizeof(buf),
      "product::NET_PREFIX too long for SSID buffer");
  snprintf(buf, sizeof(buf), "%s-%06lx", product::NET_PREFIX,
           static_cast<unsigned long>(mac & 0xffffffULL));
  _apSsid = buf;
}

void WifiManager::beginIfPersisted() {
  // Populate the hostname cache here — on the main task, before any server
  // task exists — so cross-task readers never race a first load.
  _loadHostname();
  if (!isConfigured()) {
    M5_LOGI("Wi-Fi: not configured, radio off at boot");
    return;
  }
  const Mode m = _persistedMode();
  if (m == Mode::Off) {
    // The user turned Wi-Fi off (stop with persist); honor that at boot.
    M5_LOGI("Wi-Fi: off by user choice, radio off at boot");
    return;
  }
  if (m == Mode::Sta) {
    std::string ssid, pass;
    if (_loadStaCreds(ssid, pass) && !ssid.empty()) {
      M5_LOGI("Wi-Fi: STA boot, ssid=%s", ssid.c_str());
      // Values just loaded from NVS — no need to write them back.
      startSta(ssid.c_str(), pass.c_str(), /*persist=*/false);
      return;
    }
    M5_LOGW("Wi-Fi: STA persisted but no creds; falling back to AP");
  }
  startAp();
}

void WifiManager::scheduleBluetoothConditioning(
    uint32_t controllerGeneration) {
  _bluetoothConditioning.requestedGeneration.store(controllerGeneration,
                                                   std::memory_order_release);
}

bool WifiManager::isBluetoothConditioned() const {
  if (_mode.load(std::memory_order_relaxed) != Mode::Off) return false;
  const uint32_t generation = _bluetoothConditioning.requestedGeneration.load(
      std::memory_order_acquire);
  return generation != 0 && _bluetoothConditioning.completedGeneration.load(
                                std::memory_order_acquire) == generation;
}

void WifiManager::_attemptBluetoothConditioning() {
  const uint32_t generation = _bluetoothConditioning.requestedGeneration.load(
      std::memory_order_acquire);
  if (generation == 0 ||
      generation == _bluetoothConditioning.attemptedGeneration)
    return;

  const auto fail = [](const char* reason) {
    M5_LOGE("Wi-Fi: %s", reason);
    runtimeEventLog.pushNetFailure(
        diagnostics::NetSource::Wifi,
        static_cast<uint16_t>(
            diagnostics::WifiFailureCode::BluetoothConditioning),
        reason);
  };

  _bluetoothConditioning.attemptedGeneration = generation;
  wifi_mode_t idfMode;
  const wifi_mode_t arduinoMode = WiFi.getMode();
  const esp_err_t idfResult = esp_wifi_get_mode(&idfMode);
  if (arduinoMode != WIFI_MODE_NULL || idfResult != ESP_ERR_WIFI_NOT_INIT) {
    fail("Driver state invalid");
    return;
  }

  WiFi.persistent(false);
  if (!WiFi.mode(WIFI_STA)) {
    // A failed start can leave Arduino's private low-level state initialized
    // while its public mode still reads Off. Retrying through the wrapper is
    // the only recovery that keeps that private state consistent.
    M5_LOGW("Wi-Fi: Bluetooth conditioning start failed; retrying once");
    if (!WiFi.mode(WIFI_STA)) {
      // A direct esp_wifi_deinit() would leave Arduino's private
      // lowLevelInitDone flag set and make every later WiFi.mode() fail.
      // Preserve network recovery even if the partial initialization retains
      // memory for the rest of this boot.
      fail("Condition start failed");
      return;
    }
  }
  WiFi.mode(WIFI_OFF);

  const wifi_mode_t finalArduinoMode = WiFi.getMode();
  const esp_err_t finalIdfResult = esp_wifi_get_mode(&idfMode);
  if (finalArduinoMode != WIFI_MODE_NULL ||
      finalIdfResult != ESP_ERR_WIFI_NOT_INIT) {
    fail("Condition stop failed");
    return;
  }

  _bluetoothConditioning.completedGeneration.store(generation,
                                                   std::memory_order_release);
  M5_LOGI("Wi-Fi: Bluetooth radio conditioning complete");
}

bool WifiManager::startSta(const char* ssid, const char* pass, bool persist) {
  if (!ssid || !*ssid) return false;
  _ensureEventsRegistered();
  // Radio transitions are where the HTTP server (and its mDNS name)
  // restarts, so a pending rename takes effect here.
  _loadHostname();

  if (persist) {
    _saveStaCreds(ssid, pass ? pass : "");
    markConfigured();
    _persistMode(Mode::Sta);
  }

  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  _staSsid = ssid;
  _mode = Mode::Sta;
  _state = State::StaConnecting;
  _apIsFallback.store(false, std::memory_order_relaxed);
  _staAssociated.store(false, std::memory_order_relaxed);
  _staFallbackTimeoutSuppressed.store(false, std::memory_order_relaxed);
  _lastDisconnectReason.store(0, std::memory_order_relaxed);
  _staConnectingStartedMs.store(millis(), std::memory_order_relaxed);
  WiFi.setAutoReconnect(true);
  const wifi::PowerSave powerSave =
      _staPowerSave.load(std::memory_order_relaxed);
  if (setPowerSave(powerSave)) {
    M5_LOGI("Wi-Fi: STA power save=%s", wifi::powerSaveName(powerSave));
  } else {
    M5_LOGE("Wi-Fi: failed to apply STA power save=%s",
            wifi::powerSaveName(powerSave));
  }
  WiFi.begin(ssid, pass);
  M5_LOGI("Wi-Fi: STA connecting to %s", ssid);
  return true;
}

bool WifiManager::startAp(bool persist) {
  _ensureEventsRegistered();
  _ensureApSsid();
  _ensureApPassword();
  // Same rationale as in startSta(): pick up a pending rename at the
  // transition the server restart follows.
  _loadHostname();
  if (persist) _persistMode(Mode::Ap);
  // Deliberate AP by default; the STA-failure fallback path in update()
  // re-sets the flag right after this returns.
  _apIsFallback.store(false, std::memory_order_relaxed);

  _apStations.store(0, std::memory_order_relaxed);
  WiFi.disconnect(true, false);
  WiFi.mode(WIFI_AP);
  if (!WiFi.softAP(_apSsid.c_str(), _apPass.c_str())) {
    M5_LOGE("Wi-Fi: softAP() failed");
    _state = State::Off;
    _mode = Mode::Off;
    return false;
  }
  _mode = Mode::Ap;
  _state = State::ApUp;
  M5_LOGI("Wi-Fi: AP up, ssid=%s ip=%s", _apSsid.c_str(),
          WiFi.softAPIP().toString().c_str());
  return true;
}

bool WifiManager::retrySta(bool persist) {
  std::string ssid, pass;
  if (!_loadStaCreds(ssid, pass) || ssid.empty()) {
    M5_LOGW("Wi-Fi: retrySta with no stored creds");
    return false;
  }
  // With persist the creds get rewritten with their own values (NVS
  // deduplicates), which is harmless; the point is recording Sta as the
  // boot mode again.
  return startSta(ssid.c_str(), pass.c_str(), persist);
}

bool WifiManager::setPowerSave(wifi::PowerSave mode) {
  const auto apply = [](wifi_ps_type_t requested) {
    // Keep Arduino's desired-mode cache aligned, then apply the mode directly.
    // Arduino's return value also means "already selected," and its getter
    // reads only that cache, so neither can verify the ESP-IDF driver state.
    WiFi.setSleep(requested);
    if (esp_wifi_set_ps(requested) != ESP_OK) return false;

    wifi_ps_type_t actual;
    return esp_wifi_get_ps(&actual) == ESP_OK && actual == requested;
  };

  bool applied = false;
  switch (mode) {
    case wifi::PowerSave::None:
      applied = apply(WIFI_PS_NONE);
      break;
    case wifi::PowerSave::MinimumModem:
      applied = apply(WIFI_PS_MIN_MODEM);
      break;
    case wifi::PowerSave::MaximumModem:
      applied = apply(WIFI_PS_MAX_MODEM);
      break;
  }
  if (applied) _staPowerSave.store(mode, std::memory_order_relaxed);
  return applied;
}

bool WifiManager::readPowerSave(wifi::PowerSave& mode) const {
  wifi_ps_type_t actual;
  if (esp_wifi_get_ps(&actual) != ESP_OK) return false;

  switch (actual) {
    case WIFI_PS_NONE:
      mode = wifi::PowerSave::None;
      return true;
    case WIFI_PS_MIN_MODEM:
      mode = wifi::PowerSave::MinimumModem;
      return true;
    case WIFI_PS_MAX_MODEM:
      mode = wifi::PowerSave::MaximumModem;
      return true;
  }
  return false;
}

bool WifiManager::hasStaCreds() const {
  int8_t cached = _staCredsCached.load(std::memory_order_relaxed);
  if (cached < 0) {
    std::string ssid, pass;
    cached = (_loadStaCreds(ssid, pass) && !ssid.empty()) ? 1 : 0;
    _staCredsCached.store(cached, std::memory_order_relaxed);
  }
  return cached > 0;
}

void WifiManager::stop(bool persist) {
  if (persist) _persistMode(Mode::Off);
  WiFi.disconnect(true, false);
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);

  const uint32_t generation = _bluetoothConditioning.requestedGeneration.load(
      std::memory_order_acquire);
  const uint32_t completedGeneration =
      _bluetoothConditioning.completedGeneration.load(
          std::memory_order_acquire);
  if (!wifiDriverIsOff()) {
    _bluetoothConditioning.completedGeneration.store(0,
                                                     std::memory_order_release);
    _bluetoothConditioning.attemptedGeneration = generation;
    constexpr char reason[] = "Driver stop failed";
    M5_LOGE("Wi-Fi: %s", reason);
    runtimeEventLog.pushNetFailure(
        diagnostics::NetSource::Wifi,
        static_cast<uint16_t>(
            diagnostics::WifiFailureCode::BluetoothConditioning),
        reason);
  } else if (generation != 0 && completedGeneration != generation &&
             _bluetoothConditioning.attemptedGeneration == generation) {
    // A later explicit shutdown gives a failed generation one new
    // conditioning attempt without creating a continuous retry loop.
    _bluetoothConditioning.attemptedGeneration = 0;
  }

  _mode = Mode::Off;
  _state = State::Off;
  _apIsFallback.store(false, std::memory_order_relaxed);
}

void WifiManager::resetAllConfig(bool scrubFramework) {
  // The scrub erases esp_wifi's own copy of the Wi-Fi config from flash.
  // Current builds never write it (WiFi.persistent(false)), but builds that
  // predate that did, and a wiped device must not hand the previous owner's
  // network password to whoever flashes it next. esp_wifi_restore() needs
  // the stack initialized, so it must run before stop() tears the stack
  // down — and with the radio off there is no stack, so bring one up just
  // for the wipe. Terminal-path-only (see the header comment): restore
  // leaves the Arduino wrapper's cached radio state stale, which the
  // imminent reboot makes moot, while the keep-running reset (/forget) must
  // be able to start the radio again afterwards.
  if (scrubFramework) {
    if (WiFi.getMode() == WIFI_MODE_NULL) WiFi.enableSTA(true);
    esp_wifi_restore();
  }
  stop();
  Preferences prefs;
  if (prefs.begin(kNvsNamespace, false)) {
    prefs.clear();
    prefs.end();
  }
  _staCredsCached.store(-1, std::memory_order_relaxed);
  _configuredCached.store(-1, std::memory_order_relaxed);
  _staSsid.clear();
  // Refresh the caches backed by the namespace just wiped, so a subsequent
  // startAp() without reboot regenerates the AP password and hostname()
  // falls back to deviceId().
  _apPass.clear();
  _loadHostname();
  _staFallbackTimeoutSuppressed.store(false, std::memory_order_relaxed);
  M5_LOGI("Wi-Fi: config wiped");
  if (_onReset) _onReset();
}

void WifiManager::setOnReset(std::function<void()> cb) {
  _onReset = std::move(cb);
}

void WifiManager::registerWith(HttpServer& server) {
  server.registerRoutes(
      "/sys/wifi",
      {
          HttpRoute{"/status", HTTP_GET,
                    [this](WebServer& s) { _handleStatus(s); }},
          HttpRoute{"/sta", HTTP_POST,
                    [this](WebServer& s) { _handleSetSta(s); }},
          HttpRoute{"/scan", HTTP_GET,
                    [this](WebServer& s) { _handleScan(s); }},
          HttpRoute{"/forget", HTTP_POST,
                    [this](WebServer& s) { _handleForget(s); }},
          HttpRoute{"/hostname", HTTP_POST,
                    [this](WebServer& s) { _handleSetHostname(s); }},
      });
}

void WifiManager::markConfigured() {
  Preferences prefs;
  if (!prefs.begin(kNvsNamespace, false)) {
    M5_LOGE(
        "Wi-Fi: NVS open failed in markConfigured; device will boot "
        "unconfigured next time");
    return;
  }
  const size_t n = prefs.putUChar(kKeyConfigured, 1);
  prefs.end();
  if (n == 0) {
    M5_LOGE("Wi-Fi: NVS write failed in markConfigured");
  } else {
    _configuredCached.store(1, std::memory_order_relaxed);
  }
}

bool WifiManager::isConfigured() const {
  int8_t cached = _configuredCached.load(std::memory_order_relaxed);
  if (cached >= 0) return cached > 0;

  Preferences prefs;
  if (!prefs.begin(kNvsNamespace, true)) return false;
  cached = prefs.getUChar(kKeyConfigured, 0) != 0 ? 1 : 0;
  prefs.end();
  _configuredCached.store(cached, std::memory_order_relaxed);
  return cached > 0;
}

IPAddress WifiManager::ip() const {
  switch (mode()) {
    case Mode::Sta:
      return WiFi.localIP();
    case Mode::Ap:
      return WiFi.softAPIP();
    default:
      return IPAddress();
  }
}

const char* WifiManager::deviceId() {
  _ensureApSsid();
  return _apSsid.c_str();
}

const char* WifiManager::apPassword() {
  _ensureApPassword();
  return _apPass.c_str();
}

void WifiManager::_ensureApPassword() {
  if (!_apPass.empty()) return;
  Preferences prefs;
  const bool nvsOk = prefs.begin(kNvsNamespace, false);
  if (nvsOk) {
    _apPass = prefs.getString(kKeyApPass, "").c_str();
  }
  if (_apPass.empty()) {
    char buf[kApPassLen + 1];
    for (size_t i = 0; i < kApPassLen; ++i) {
      buf[i] = kApPassCharset[esp_random() % (sizeof(kApPassCharset) - 1)];
    }
    buf[kApPassLen] = 0;
    _apPass = buf;
    // On NVS failure the password still works for this session; it just
    // won't survive a reboot, and the next boot generates a fresh one.
    if (!nvsOk || prefs.putString(kKeyApPass, buf) == 0) {
      M5_LOGE("Wi-Fi: failed to persist AP password");
    }
  }
  if (nvsOk) prefs.end();
}

const char* WifiManager::hostname() {
  return _hostname[0] ? _hostname : deviceId();
}

bool WifiManager::setHostname(const char* name) {
  if (!_isValidHostname(name)) return false;
  Preferences prefs;
  if (!prefs.begin(kNvsNamespace, false)) {
    M5_LOGE("Wi-Fi: NVS open failed in setHostname");
    return false;
  }
  const size_t n = prefs.putString(kKeyHostname, name);
  prefs.end();
  if (n == 0) {
    M5_LOGE("Wi-Fi: NVS write failed in setHostname");
    return false;
  }
  // Deliberately does NOT touch _hostname: this runs on the HTTP task (the
  // cache is main-task-owned), and the screens should keep showing the
  // name mDNS actually serves until the next radio transition reloads it.
  M5_LOGI("Wi-Fi: hostname set to %s (effective on next server start)", name);
  return true;
}

void WifiManager::hostnameSnapshot(char* buf, size_t n) {
  buf[0] = 0;
  Preferences prefs;
  if (prefs.begin(kNvsNamespace, true)) {
    strlcpy(buf, prefs.getString(kKeyHostname, "").c_str(), n);
    prefs.end();
  }
  // deviceId()'s backing string is built in the constructor and never
  // mutated, so borrowing it here is safe from any task.
  if (!buf[0]) strlcpy(buf, deviceId(), n);
}

void WifiManager::_loadHostname() {
  Preferences prefs;
  _hostname[0] = 0;
  if (prefs.begin(kNvsNamespace, true)) {
    strlcpy(_hostname, prefs.getString(kKeyHostname, "").c_str(),
            sizeof(_hostname));
    prefs.end();
  }
}

bool WifiManager::_isValidHostname(const char* s) {
  if (!s) return false;
  size_t len = 0;
  for (const char* p = s; *p; ++p, ++len) {
    const char c = *p;
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '-';
    if (!ok) return false;
    if (len == 0 && c == '-') return false;  // no leading hyphen
  }
  if (len < 1 || len > kMaxHostnameLen) return false;
  if (s[len - 1] == '-') return false;  // no trailing hyphen
  return true;
}

const char* WifiManager::lastDisconnectReason() const {
  const uint8_t r = _lastDisconnectReason.load(std::memory_order_relaxed);
  switch (r) {
    case 0:
      return "";
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
      return "wrong password";
    case WIFI_REASON_NO_AP_FOUND:
      return "ssid not found";
    default:
      return "connect failed";
  }
}

const char* WifiManager::ssid() const {
  switch (mode()) {
    case Mode::Sta:
      return _staSsid.c_str();
    case Mode::Ap:
      return _apSsid.c_str();
    default:
      return "";
  }
}

void WifiManager::update() {
  // Drain any radio change requested from another task (HTTP handler or
  // button press) before doing anything else this tick. Returns quickly
  // when nothing is pending.
  _processPending();
  // A generation announced during a session remains pending because
  // conditioning is attempted only when the manager is Off.
  if (_state.load(std::memory_order_relaxed) == State::Off)
    _attemptBluetoothConditioning();

  // Handle the STA-connect timeout from the main loop. The WiFi event task
  // only records disconnect state because WiFi APIs can't be called safely
  // from that task.
  const State s = _state.load(std::memory_order_relaxed);
  if (s != State::StaConnecting) return;
  if (_staFallbackTimeoutSuppressed.load(std::memory_order_relaxed)) return;
  const uint32_t startedMs =
      _staConnectingStartedMs.load(std::memory_order_relaxed);
  if (millis() - startedMs <= kStaTimeoutMs) return;

  M5_LOGW(
      "Wi-Fi: STA connect/reconnect timed out after %u ms, falling back "
      "to AP",
      kStaTimeoutMs);
  WiFi.setAutoReconnect(false);
  // lastDisconnectReason() is "" when the connect simply timed out with no
  // disconnect event recorded.
  const uint8_t reason = _lastDisconnectReason.load(std::memory_order_relaxed);
  const char* why = lastDisconnectReason();
  runtimeEventLog.pushNetFailure(diagnostics::NetSource::Wifi, reason,
                                 (why && why[0]) ? why : "timeout");
  // Transient fallback — keep persisted mode=Sta so the next boot retries
  // STA (transient router outages would otherwise pin the device into
  // AP-only forever).
  if (startAp(/*persist=*/false)) {
    _apIsFallback.store(true, std::memory_order_relaxed);
  }
}

void WifiManager::_onWifiEvent(int event, uint8_t disconnectReason) {
  switch (static_cast<arduino_event_id_t>(event)) {
    case ARDUINO_EVENT_WIFI_STA_CONNECTED:
      _staAssociated.store(mode() == Mode::Sta, std::memory_order_relaxed);
      break;
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      // A queued event can outlive its session: a GOT_IP arriving after
      // stop()/startAp() switched the mode away must not resurrect
      // StaConnected — with the radio off, no later event would ever
      // correct it, and the servers would restart against a dead radio.
      // Requiring association pins the IP to the current attempt: a stale
      // GOT_IP (e.g. a DHCP renewal queued across a credential swap) must
      // not mark the new attempt connected and suppress its fallback.
      if (mode() != Mode::Sta ||
          !_staAssociated.load(std::memory_order_relaxed)) {
        break;
      }
      _staFallbackTimeoutSuppressed.store(true, std::memory_order_relaxed);
      _state = State::StaConnected;
      _lastDisconnectReason.store(0, std::memory_order_relaxed);
      M5_LOGI("Wi-Fi: STA got IP %s", WiFi.localIP().toString().c_str());
      break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED: {
      _staAssociated.store(false, std::memory_order_relaxed);
      if (mode() != Mode::Sta) break;
      // Arduino/ESP auto-reconnect may emit local cleanup disconnect reasons
      // after the real failure. Keep the first generic reason, but let
      // actionable reasons replace it and never let generic reasons erase
      // an actionable one before AP fallback exposes it to the UI.
      const uint8_t previousReason =
          _lastDisconnectReason.load(std::memory_order_relaxed);
      if (previousReason == 0 || isActionableFailureReason(disconnectReason)) {
        _lastDisconnectReason.store(disconnectReason,
                                    std::memory_order_relaxed);
      }

      // Credential-looking failures during initial connect get the same
      // bounded timeout as every other connect failure. This is a compromise
      // for simplicity, ideally initial connects would fail immediately so
      // that the user can revise the credentials.
      // After a successful IP, re-arm that timeout only for credential-looking
      // drops; ordinary transient drops stay in Arduino's auto-reconnect path.
      const bool credentialFailure =
          isCredentialFailureReason(disconnectReason);
      if (credentialFailure) {
        const bool hadConnected =
            _staFallbackTimeoutSuppressed.load(std::memory_order_relaxed);
        if (hadConnected) {
          _staFallbackTimeoutSuppressed.store(false, std::memory_order_relaxed);
          _staConnectingStartedMs.store(millis(), std::memory_order_relaxed);
          M5_LOGW(
              "Wi-Fi: STA credential disconnect after connection, "
              "reason=%u (%s); waiting for reconnect timeout",
              static_cast<unsigned>(disconnectReason),
              WiFi.disconnectReasonName(
                  static_cast<wifi_err_reason_t>(disconnectReason)));
        } else {
          M5_LOGW(
              "Wi-Fi: STA credential disconnect during connect/reconnect, "
              "reason=%u (%s); waiting for timeout",
              static_cast<unsigned>(disconnectReason),
              WiFi.disconnectReasonName(
                  static_cast<wifi_err_reason_t>(disconnectReason)));
        }
      }
      // Transient drop or credential failure; auto-reconnect and the connect
      // timeout handle bounded AP fallback where needed.
      _state = State::StaConnecting;
      break;
    }
    case ARDUINO_EVENT_WIFI_AP_STACONNECTED:
      _apStations.fetch_add(1, std::memory_order_relaxed);
      break;
    case ARDUINO_EVENT_WIFI_AP_STADISCONNECTED: {
      // Clamp at zero: startAp() resets the count, so a disconnect for a
      // station from the previous AP session must not underflow.
      uint8_t n = _apStations.load(std::memory_order_relaxed);
      while (n > 0 && !_apStations.compare_exchange_weak(
                          n, n - 1, std::memory_order_relaxed)) {
      }
      break;
    }
    case ARDUINO_EVENT_WIFI_AP_START:
      if (mode() == Mode::Ap) _state = State::ApUp;
      break;
    case ARDUINO_EVENT_WIFI_AP_STOP:
      if (mode() == Mode::Ap) _state = State::Off;
      break;
    default:
      break;
  }
}

bool WifiManager::_loadStaCreds(std::string& ssid, std::string& pass) const {
  Preferences prefs;
  if (!prefs.begin(kNvsNamespace, true)) return false;
  ssid = prefs.getString(kKeyStaSsid, "").c_str();
  pass = prefs.getString(kKeyStaPass, "").c_str();
  prefs.end();
  return true;
}

void WifiManager::_saveStaCreds(const char* ssid, const char* pass) {
  _staCredsCached.store(-1, std::memory_order_relaxed);
  Preferences prefs;
  if (!prefs.begin(kNvsNamespace, false)) {
    M5_LOGE(
        "Wi-Fi: NVS open failed in _saveStaCreds; creds won't survive "
        "reboot");
    return;
  }
  const size_t a = prefs.putString(kKeyStaSsid, ssid);
  const size_t b = prefs.putString(kKeyStaPass, pass);
  prefs.end();
  if (a == 0 || b == 0) {
    M5_LOGE("Wi-Fi: NVS write failed in _saveStaCreds (ssid=%u, pass=%u)",
            static_cast<unsigned>(a), static_cast<unsigned>(b));
  }
}

void WifiManager::_persistMode(Mode m) {
  Preferences prefs;
  if (!prefs.begin(kNvsNamespace, false)) {
    M5_LOGE("Wi-Fi: NVS open failed in _persistMode");
    return;
  }
  const size_t n = prefs.putUChar(kKeyMode, static_cast<uint8_t>(m));
  prefs.end();
  if (n == 0) {
    M5_LOGE("Wi-Fi: NVS write failed in _persistMode");
  }
}

namespace {
const char* modeName(WifiManager::Mode m) {
  switch (m) {
    case WifiManager::Mode::Off:
      return "off";
    case WifiManager::Mode::Sta:
      return "sta";
    case WifiManager::Mode::Ap:
      return "ap";
  }
  return "off";
}

const char* stateName(WifiManager::State s) {
  switch (s) {
    case WifiManager::State::Off:
      return "off";
    case WifiManager::State::StaConnecting:
      return "sta_connecting";
    case WifiManager::State::StaConnected:
      return "sta_connected";
    case WifiManager::State::ApUp:
      return "ap_up";
  }
  return "off";
}
}  // namespace

void WifiManager::_handleStatus(WebServer& server) {
  const String ipString = ip().toString();
  wifi::PowerSave powerSave;
  const bool hasPowerSave = mode() == Mode::Sta && readPowerSave(powerSave);
  // Snapshot, not hostname(): this runs on the HTTP task, and reporting the
  // persisted name lets the config page confirm a rename right away.
  char host[kMaxHostnameLen + 1];
  hostnameSnapshot(host, sizeof(host));
  JsonStream j(server);
  j.open()
      .key("mode")
      .str(modeName(mode()))
      .key("state")
      .str(stateName(state()))
      .key("ssid")
      .str(ssid())
      .key("ip")
      .str(ipString.c_str())
      .key("configured")
      .boolean(isConfigured())
      .key("device_id")
      .str(deviceId())
      .key("hostname")
      .str(host)
      .key("reason")
      .str(lastDisconnectReason())
      .key("power_save");
  if (hasPowerSave)
    j.str(wifi::powerSaveName(powerSave));
  else
    j.null_();
  j.close();
  j.finish();
}

void WifiManager::_handleSetSta(WebServer& server) {
  if (!server.hasArg("ssid")) {
    server.send(400, "application/json", "{\"error\":\"missing ssid\"}");
    return;
  }
  const String ssid = server.arg("ssid");
  const String pass = server.hasArg("pass") ? server.arg("pass") : String();
  if (ssid.length() == 0 || ssid.length() > kMaxSsidLen) {
    server.send(400, "application/json",
                "{\"error\":\"ssid must be 1-32 bytes\"}");
    return;
  }
  if (pass.length() > kMaxPassLen) {
    server.send(400, "application/json",
                "{\"error\":\"pass must be at most 63 bytes\"}");
    return;
  }
  // Defer the switch to the main loop — startSta() would tear down the AP
  // under this very connection. The main loop stops the HTTP server
  // cleanly first, then mutates the radio, so the response flushes
  // naturally before anything is torn down.
  requestConnectSta(ssid.c_str(), pass.c_str());
  server.send(200, "application/json", "{\"ok\":true}");
}

void WifiManager::_handleScan(WebServer& server) {
  // Poll-driven async scan: the first request starts a scan and answers
  // {"scanning":true}; the client re-requests until networks arrive.
  // Results are freed once served, so each scan is delivered exactly once.
  // Scanning needs the STA interface, so in AP mode the radio runs AP+STA
  // for the scan's duration; the brief beacon gaps are fine during setup.
  const int16_t n = WiFi.scanComplete();
  if (n == WIFI_SCAN_RUNNING) {
    server.send(202, "application/json", "{\"scanning\":true}");
    return;
  }
  if (n < 0) {
    WiFi.scanNetworks(/*async=*/true);
    server.send(202, "application/json", "{\"scanning\":true}");
    return;
  }

  // Keep the strongest instance of each SSID (multi-AP networks appear once
  // per BSSID), strongest first, capped to keep the response small.
  std::vector<int16_t> picked;
  for (int16_t i = 0; i < n; ++i) {
    const String ssid = WiFi.SSID(i);
    if (ssid.isEmpty()) continue;  // hidden networks can't be picked by name
    bool duplicate = false;
    for (int16_t& p : picked) {
      if (WiFi.SSID(p) == ssid) {
        if (WiFi.RSSI(i) > WiFi.RSSI(p)) p = i;
        duplicate = true;
        break;
      }
    }
    if (!duplicate) picked.push_back(i);
  }
  std::sort(picked.begin(), picked.end(),
            [](int16_t a, int16_t b) { return WiFi.RSSI(a) > WiFi.RSSI(b); });
  if (picked.size() > kMaxScanResults) picked.resize(kMaxScanResults);

  {
    JsonStream j(server);
    j.open().key("networks").arrayOpen();
    bool first = true;
    for (const int16_t i : picked) {
      if (!first) j.comma();
      first = false;
      j.open()
          .key("ssid")
          .str(WiFi.SSID(i).c_str())
          .key("rssi")
          .i(WiFi.RSSI(i))
          .key("secure")
          .boolean(WiFi.encryptionType(i) != WIFI_AUTH_OPEN)
          .close();
    }
    j.arrayClose().close();
    j.finish();
  }
  WiFi.scanDelete();
  // Drop the STA interface the scan brought up, restoring AP-only.
  if (mode() == Mode::Ap) WiFi.enableSTA(false);
}

void WifiManager::_handleForget(WebServer& server) {
  requestReset();
  server.send(200, "application/json", "{\"ok\":true}");
}

void WifiManager::_handleSetHostname(WebServer& server) {
  if (!server.hasArg("name")) {
    server.send(400, "application/json", "{\"error\":\"missing name\"}");
    return;
  }
  const String name = server.arg("name");
  if (!setHostname(name.c_str())) {
    server.send(400, "application/json",
                "{\"error\":\"invalid name (use letters, digits, hyphens; "
                "1-31 chars; no leading/trailing hyphen)\"}");
    return;
  }
  server.send(200, "application/json", "{\"ok\":true}");
}

WifiManager::Mode WifiManager::_persistedMode() const {
  Preferences prefs;
  if (!prefs.begin(kNvsNamespace, true)) return Mode::Off;
  const uint8_t v = prefs.getUChar(kKeyMode, static_cast<uint8_t>(Mode::Off));
  prefs.end();
  return static_cast<Mode>(v);
}
