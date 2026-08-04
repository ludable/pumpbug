// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#include "Auth.h"

#include <M5Unified.h>
#include <Preferences.h>
#include <esp_random.h>
#include <nvs.h>

#include <cstring>
#include <vector>

#include "HttpServer.h"
#include "PairRedirectPolicy.h"
#include "util/hex.h"
#include "util/scoped_lock.h"

Auth auth;

namespace {
constexpr const char* kNvsNamespace = "auth";
constexpr const char* kKeyTokens = "tokens";  // blob: N * 16 bytes
constexpr const char* kCookieName = "auth_token";

using Lock = ScopedLock;
}  // namespace

Auth::Auth() : _mutex(xSemaphoreCreateMutex()) {}

Auth::~Auth() {
  if (_mutex) {
    vSemaphoreDelete(_mutex);
    _mutex = nullptr;
  }
}

std::string Auth::startPairing(uint32_t ttlMs) {
  Lock lock(_mutex);
  const uint32_t r = esp_random() % 10000;
  snprintf(_pin, sizeof(_pin), "%04u", r);
  uint8_t tok[kPairTokenBytes];
  esp_fill_random(tok, sizeof(tok));
  const std::string tokenHex = hexOf(tok, sizeof(tok));
  memcpy(_pairToken, tokenHex.c_str(), sizeof(_pairToken));
  _pinStartedMs = millis();
  _pinTtlMs = ttlMs;
  _pairingActive = true;
  _pinFailedAttempts = 0;
  M5_LOGI("Auth: pairing started, ttl=%u ms", ttlMs);
  return std::string(_pin);
}

void Auth::cancelPairing() {
  Lock lock(_mutex);
  if (!_pairingActive) return;
  _pairingActive = false;
  _pin[0] = 0;
  _pairToken[0] = 0;
  M5_LOGI("Auth: pairing cancelled");
}

bool Auth::isPairing() const {
  Lock lock(_mutex);
  return _pairingActive;
}

uint32_t Auth::pairingMsRemaining() const {
  Lock lock(_mutex);
  if (!_pairingActive) return 0;
  const uint32_t elapsed = millis() - _pinStartedMs;
  return elapsed >= _pinTtlMs ? 0 : _pinTtlMs - elapsed;
}

std::string Auth::currentPin() const {
  Lock lock(_mutex);
  return _pairingActive ? std::string(_pin) : std::string();
}

std::string Auth::currentPairToken() const {
  Lock lock(_mutex);
  return _pairingActive ? std::string(_pairToken) : std::string();
}

void Auth::update() {
  Lock lock(_mutex);
  if (!_pairingActive) return;
  if (millis() - _pinStartedMs <= _pinTtlMs) return;
  _pairingActive = false;
  _pin[0] = 0;
  _pairToken[0] = 0;
  M5_LOGI("Auth: pairing PIN expired");
}

bool Auth::isAuthenticated(WebServer& server) const {
  const std::string token = _extractCookie(server, kCookieName);
  if (token.empty()) return false;
  std::vector<uint8_t> bytes;
  if (!hexToBytes(token, bytes)) return false;
  if (bytes.size() != kTokenBytes) return false;
  return _hasTokenBytes(bytes.data(), bytes.size());
}

void Auth::forgetAll() {
  Lock lock(_mutex);
  _tokens.clear();
  _tokensLoaded = true;  // RAM is authoritative; mark as loaded
  Preferences prefs;
  if (!prefs.begin(kNvsNamespace, false)) {
    M5_LOGE("Auth: forgetAll NVS open failed; RAM cleared but NVS not");
    return;
  }
  prefs.clear();
  prefs.end();
  M5_LOGI("Auth: all tokens cleared");
}

size_t Auth::pairedCount() const { return _tokenCount(); }

void Auth::setOnFirstPair(std::function<void()> cb) {
  _onFirstPair = std::move(cb);
}

void Auth::registerWith(HttpServer& server) {
  server.setAuthMiddleware([this](WebServer& s) { return isAuthenticated(s); });
  server.registerRoutes(
      "/auth", {
                   HttpRoute{"/pair", HTTP_POST,
                             [this](WebServer& s) { _handlePair(s); },
                             /*requiresAuth=*/false},
                   HttpRoute{"/pair", HTTP_GET,
                             [this](WebServer& s) { _handlePairToken(s); },
                             /*requiresAuth=*/false},
                   HttpRoute{"/whoami", HTTP_GET,
                             [this](WebServer& s) { _handleWhoami(s); },
                             /*requiresAuth=*/false},
                   HttpRoute{"/token", HTTP_GET,
                             [this](WebServer& s) { _handleToken(s); },
                             /*requiresAuth=*/true},
               });
}

bool Auth::_consumePairing(const char* submitted, bool isToken) {
  // Wrong guesses of either credential increment the shared counter and
  // invalidate the window after kMaxPinAttempts — closes the brute-force
  // window without per-IP state. NVS work happens after this releases.
  Lock lock(_mutex);
  if (!_pairingActive) return false;
  const char* expected = isToken ? _pairToken : _pin;
  if (millis() - _pinStartedMs > _pinTtlMs) {
    // fall through to deactivate: expired
  } else if (*expected && strcmp(submitted, expected) == 0) {
    _pairingActive = false;
    _pin[0] = 0;
    _pairToken[0] = 0;
    return true;
  } else if (++_pinFailedAttempts < kMaxPinAttempts) {
    return false;
  } else {
    M5_LOGW("Auth: pairing locked out after %u failed attempts",
            static_cast<unsigned>(kMaxPinAttempts));
  }
  _pairingActive = false;
  _pin[0] = 0;
  _pairToken[0] = 0;
  return false;
}

std::string Auth::_mintClientToken(WebServer& server) {
  uint8_t bytes[kTokenBytes];
  esp_fill_random(bytes, sizeof(bytes));
  const size_t pairedCount = _appendToken(bytes, sizeof(bytes));
  if (pairedCount == 0) return "";
  _redeems.fetch_add(1, std::memory_order_relaxed);
  const std::string token = hexOf(bytes, sizeof(bytes));

  std::string cookie = kCookieName;
  cookie += "=";
  cookie += token;
  cookie += "; Path=/; HttpOnly; SameSite=Strict; Max-Age=31536000";
  server.sendHeader("Set-Cookie", cookie.c_str());

  if (pairedCount == 1 && _onFirstPair) _onFirstPair();
  M5_LOGI("Auth: pairing complete; %u paired client(s)",
          static_cast<unsigned>(pairedCount));
  return token;
}

void Auth::_handlePair(WebServer& server) {
  if (!server.hasArg("pin")) {
    server.send(400, "application/json", "{\"error\":\"missing pin\"}");
    return;
  }
  // An already-paired browser re-pairing would only burn a token slot;
  // succeed without consuming the window, which stays open for a device
  // that actually needs it. Still counts as an arrival so the wizard
  // advances for a client that paired in an earlier session.
  if (isAuthenticated(server)) {
    _redeems.fetch_add(1, std::memory_order_relaxed);
    server.send(200, "application/json", "{\"ok\":true}");
    return;
  }
  if (!_consumePairing(server.arg("pin").c_str(), /*isToken=*/false)) {
    server.send(401, "application/json",
                "{\"error\":\"invalid or expired pin\"}");
    return;
  }
  const std::string token = _mintClientToken(server);
  if (token.empty()) {
    server.send(500, "application/json",
                "{\"error\":\"token store unavailable\"}");
    return;
  }
  const std::string body = "{\"token\":\"" + token + "\"}";
  server.send(200, "application/json", body.c_str());
}

void Auth::_handlePairToken(WebServer& server) {
  // QR-code redemption: the device shows /auth/pair?token=... as a QR, so
  // scanning it proves the same physical presence as reading the PIN off the
  // screen. Always redirects rather than erroring — a stale or reused link
  // lands the browser on the config page, whose PIN form is the fallback for
  // an unpaired client.
  bool ok = false;
  if (isAuthenticated(server)) {
    // Already paired on this origin — nothing to consume or mint; the
    // window stays open for a device that needs it. Still counts as an
    // arrival so the wizard advances for a client that paired in an earlier
    // session.
    _redeems.fetch_add(1, std::memory_order_relaxed);
    ok = true;
  } else if (server.hasArg("token")) {
    ok = _consumePairing(server.arg("token").c_str(), /*isToken=*/true) &&
         !_mintClientToken(server).empty();
  }
  const String requested = server.hasArg("to") ? server.arg("to") : String();
  const char* destination = net::pairRedirectDestination(
      ok, std::string_view(requested.c_str(), requested.length()));
  server.sendHeader("Location", destination);
  server.send(302, "text/plain", ok ? "paired" : "not paired");
}

void Auth::_handleWhoami(WebServer& server) {
  const bool ok = isAuthenticated(server);
  server.send(200, "application/json",
              ok ? "{\"authenticated\":true}" : "{\"authenticated\":false}");
}

void Auth::_handleToken(WebServer& server) {
  // Hands an authenticated client its bearer token so non-browser clients can
  // reuse the session. The route requires the same token it returns.
  const std::string token = _extractCookie(server, kCookieName);
  const std::string body = "{\"token\":\"" + token + "\"}";
  server.send(200, "application/json", body.c_str());
}

std::string Auth::_extractCookie(WebServer& server, const char* name) {
  if (!server.hasHeader("Cookie")) return "";
  const String raw = server.header("Cookie");
  const std::string nameEq = std::string(name) + "=";
  const std::string s(raw.c_str());
  size_t pos = 0;
  while (pos < s.size()) {
    while (pos < s.size() && (s[pos] == ' ' || s[pos] == ';')) ++pos;
    if (pos >= s.size()) break;
    const size_t end = s.find(';', pos);
    const size_t len = (end == std::string::npos ? s.size() : end) - pos;
    const std::string pair = s.substr(pos, len);
    pos += len;
    if (pair.compare(0, nameEq.size(), nameEq) == 0) {
      return pair.substr(nameEq.size());
    }
  }
  return "";
}

bool Auth::_ensureTokensLoaded() const {
  // Caller holds _mutex. NVS isn't initialised at C++ static-init time,
  // so we lazily load on first call after M5.begin(). A successful load is
  // cached; storage failures are retried.
  if (_tokensLoaded) return true;

  const auto shouldLogLoadError = [this](esp_err_t error) {
    if (_lastTokenLoadError == error) return false;
    _lastTokenLoadError = error;
    return true;
  };

  // Preferences reports both a missing key and a failed length read as zero.
  // The NVS API keeps those cases distinct so a transient read failure cannot
  // cause a later pairing request to overwrite tokens that may still be valid.
  nvs_handle_t handle = 0;
  esp_err_t err = nvs_open(kNvsNamespace, NVS_READONLY, &handle);
  if (err == ESP_ERR_NVS_NOT_FOUND) {
    _tokens.clear();
    _tokensLoaded = true;
    return true;
  }
  if (err != ESP_OK) {
    if (shouldLogLoadError(err)) {
      M5_LOGE("Auth: token store open failed: %s", esp_err_to_name(err));
    }
    return false;
  }

  size_t total = 0;
  err = nvs_get_blob(handle, kKeyTokens, nullptr, &total);
  if (err == ESP_ERR_NVS_NOT_FOUND) {
    nvs_close(handle);
    _tokens.clear();
    _tokensLoaded = true;
    return true;
  }
  if (err == ESP_OK && total == 0) {
    nvs_close(handle);
    _tokens.clear();
    _tokensLoaded = true;
    return true;
  }

  // The cap bounds allocation from an untrusted persisted length. NVS reads a
  // blob as a whole, so a deliberate cap change needs an explicit migration.
  constexpr size_t kMaxStoredBytes = kMaxTokens * kTokenBytes;
  if (err == ESP_ERR_NVS_TYPE_MISMATCH ||
      (err == ESP_OK &&
       (total % kTokenBytes != 0 || total > kMaxStoredBytes))) {
    M5_LOGE("Auth: ignoring malformed token store");
    nvs_close(handle);
    _tokens.clear();
    _tokensLoaded = true;
    return true;
  }
  if (err != ESP_OK) {
    if (shouldLogLoadError(err)) {
      M5_LOGE("Auth: token store length read failed: %s", esp_err_to_name(err));
    }
    nvs_close(handle);
    return false;
  }

  std::vector<std::array<uint8_t, kTokenBytes>> loaded(total / kTokenBytes);
  size_t bytesRead = total;
  err = nvs_get_blob(handle, kKeyTokens, loaded.data(), &bytesRead);
  nvs_close(handle);
  if (err != ESP_OK) {
    if (shouldLogLoadError(err)) {
      M5_LOGE("Auth: token store read failed: %s", esp_err_to_name(err));
    }
    return false;
  }
  if (bytesRead != total) {
    if (shouldLogLoadError(ESP_ERR_INVALID_SIZE)) {
      M5_LOGE("Auth: token store read incomplete (%u of %u bytes)",
              static_cast<unsigned>(bytesRead), static_cast<unsigned>(total));
    }
    return false;
  }

  _tokens = std::move(loaded);
  _tokensLoaded = true;
  return true;
}

size_t Auth::_tokenCount() const {
  Lock lock(_mutex);
  if (!_ensureTokensLoaded()) return 0;
  return _tokens.size();
}

bool Auth::_hasTokenBytes(const uint8_t* candidate, size_t len) const {
  if (len != kTokenBytes) return false;
  Lock lock(_mutex);
  if (!_ensureTokensLoaded()) return false;
  for (const auto& t : _tokens) {
    if (memcmp(t.data(), candidate, kTokenBytes) == 0) return true;
  }
  return false;
}

size_t Auth::_appendToken(const uint8_t* data, size_t len) {
  if (len != kTokenBytes) return 0;
  Lock lock(_mutex);
  if (!_ensureTokensLoaded()) return 0;

  // Build the new token set aside and persist it before adopting it, so RAM
  // and NVS never diverge. At capacity the oldest token (insertion order) is
  // evicted: on this one-user device, refusing the client currently in use
  // would be worse than retiring the stalest session.
  auto candidate = _tokens;
  if (candidate.size() >= kMaxTokens) {
    candidate.erase(candidate.begin());
    M5_LOGW("Auth: token store at capacity; evicting oldest client");
  }
  std::array<uint8_t, kTokenBytes> token;
  memcpy(token.data(), data, kTokenBytes);
  candidate.push_back(token);

  // Writes need only an unambiguous success or failure result; reads use the
  // NVS API because they must distinguish an absent key from a read failure.
  Preferences prefs;
  if (!prefs.begin(kNvsNamespace, false)) {
    M5_LOGE("Auth: token store write open failed");
    return 0;
  }
  const size_t total = candidate.size() * kTokenBytes;
  const size_t written = prefs.putBytes(kKeyTokens, candidate[0].data(), total);
  prefs.end();
  // Require the complete blob as the postcondition even though the current
  // Preferences implementation reports either zero or the full byte count.
  if (written != total) {
    M5_LOGE("Auth: token store write failed");
    return 0;
  }
  _tokens = std::move(candidate);
  return _tokens.size();
}
