// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <WebServer.h>
#include <esp_err.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

class HttpServer;

// Local-network auth via a time-boxed pairing window. A window carries two
// equivalent credentials, both proving the holder can read the device screen:
// a 4-digit PIN the user types into the web form, and a random hex pair-token
// redeemed by opening GET /auth/pair?token=... (the device shows that URL as
// a QR code). Redeeming either issues a client token that rides as the
// auth_token cookie; server-side, client tokens are 16 random bytes persisted
// in NVS namespace "auth", capped at kMaxTokens with the oldest evicted when
// a new client pairs at capacity. The PIN and pair-token are only ever in
// RAM — once consumed (or expired), they're gone.
class Auth {
 public:
  static constexpr size_t kTokenBytes = 16;
  static constexpr size_t kMaxTokens = 8;
  static constexpr size_t kPairTokenBytes = 6;
  static constexpr uint32_t kDefaultPairingTtlMs = 60000;
  static constexpr uint8_t kMaxPinAttempts = 5;

  Auth();
  ~Auth();

  // Starts a pairing window, minting a fresh PIN and pair-token. Returns the
  // PIN as a 4-char string (the device displays it for the user to type).
  // Replaces any in-progress pairing.
  std::string startPairing(uint32_t ttlMs = kDefaultPairingTtlMs);
  void cancelPairing();
  bool isPairing() const;
  uint32_t pairingMsRemaining() const;
  // Snapshot of the active PIN; returns "" if not currently pairing.
  std::string currentPin() const;
  // Snapshot of the active pair-token as lowercase hex; returns "" if not
  // currently pairing. The device embeds it in the pairing QR's URL.
  std::string currentPairToken() const;

  // Drives PIN expiry. Call from the main loop.
  void update();

  // True if `server` carries a valid auth_token cookie. Reads NVS.
  bool isAuthenticated(WebServer& server) const;

  // Wipes all paired-client tokens. Resetting the WifiManager configured
  // flag is the caller's responsibility.
  void forgetAll();
  size_t pairedCount() const;

  // Monotonic count of clients that arrived through the pairing flow since
  // boot: fresh pairings AND already-paired clients whose PIN/QR submission
  // short-circuited. The setup wizard advances on this — an already-paired
  // client following a pairing URL must register even though nothing is minted.
  // Use this, not pairedCount(), for "did someone just come through": at
  // capacity the store evicts one token per mint, so its size stays flat.
  uint32_t redeemCount() const {
    return _redeems.load(std::memory_order_relaxed);
  }

  // Invoked when paired count goes 0 -> 1 (first ever successful pair).
  // main.cpp wires this to WifiManager::markConfigured() so the radio
  // comes back up at boot. Must be set before any pairing happens.
  void setOnFirstPair(std::function<void()> cb);

  // Installs the auth middleware and registers /auth/pair (POST for the PIN
  // form, GET for QR-token redemption), /auth/whoami, and /auth/token. Call
  // once at boot, before the server starts.
  void registerWith(HttpServer& server);

 private:
  static constexpr size_t kPinLen = 4;

  // Guards _pairingActive, _pin, _pairToken, _pinStartedMs, _pinTtlMs,
  // _pinFailedAttempts, _tokens, _tokensLoaded, _lastTokenLoadError. NVS
  // operations may run while the mutex is held — they have their own internal
  // locking and the per-request paths are off the hot path.
  mutable SemaphoreHandle_t _mutex;
  bool _pairingActive = false;
  char _pin[kPinLen + 1] = {0};
  char _pairToken[2 * kPairTokenBytes + 1] = {0};
  uint32_t _pinStartedMs = 0;
  uint32_t _pinTtlMs = 0;
  uint8_t _pinFailedAttempts = 0;

  // RAM cache of paired tokens. Loaded lazily on first access (NVS isn't
  // initialised at C++ static-init time, so we can't load in the ctor).
  // Every authenticated request scans this vector, so opening NVS per
  // request is avoided.
  mutable std::vector<std::array<uint8_t, kTokenBytes>> _tokens;
  mutable bool _tokensLoaded = false;
  mutable esp_err_t _lastTokenLoadError = ESP_OK;
  std::atomic<uint32_t> _redeems{0};
  // Returns false when storage could not be read. A malformed blob is treated
  // as empty so a later physical pairing can replace it.
  bool _ensureTokensLoaded() const;  // mutex must be held

  std::function<void()> _onFirstPair;

  void _handlePair(WebServer& server);
  void _handlePairToken(WebServer& server);
  void _handleWhoami(WebServer& server);
  void _handleToken(WebServer& server);

  // Consumes the pairing window if `submitted` matches the credential
  // selected by `isToken`. Wrong guesses count toward the shared attempt
  // limit; expiry and lockout both end the window.
  bool _consumePairing(const char* submitted, bool isToken);
  // Mints a client token, persists it, and sets the auth cookie header on
  // `server` (must be called before the response body is sent). Returns the
  // token as hex, or "" if token storage is unavailable.
  std::string _mintClientToken(WebServer& server);

  static std::string _extractCookie(WebServer& server, const char* name);
  bool _hasTokenBytes(const uint8_t* candidate, size_t len) const;
  // Returns the updated paired-client count, or zero if storage failed.
  size_t _appendToken(const uint8_t* data, size_t len);
  size_t _tokenCount() const;
};

extern Auth auth;
