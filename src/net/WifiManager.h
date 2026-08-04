// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <IPAddress.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

#include "net/wifi_power_save.h"

class HttpServer;
class WebServer;

// Owns Wi-Fi configuration and switches the radio between client and access
// point modes.
//
// The radio starts automatically only after the user saves client credentials
// or pairs while using the device access point. Until then it stays off at boot.
//
// A failed client connection falls back to the access point after a timeout.
// Network scans briefly enable the client interface alongside the access point.
class WifiManager {
 public:
  enum class Mode : uint8_t { Off, Sta, Ap };
  enum class State : uint8_t {
    Off,
    StaConnecting,
    StaConnected,
    ApUp,
  };
  WifiManager();

  // Restores the saved Wi-Fi mode after the device has been configured.
  void beginIfPersisted();

  // Schedules a Wi-Fi driver start-stop cycle after Bluetooth initialization.
  // On the ESP32-S3, Bluetooth initialized before any complete Wi-Fi driver
  // lifecycle can remain in a substantially higher-current shared-radio state.
  // Starting and stopping Wi-Fi after Bluetooth corrects that state without
  // joining a network.
  //
  // Each Bluetooth initialization receives a new generation number because a
  // controller restart removes the effect. This method only records the latest
  // generation; update() performs the driver work on the main task while Wi-Fi
  // is off. A request made during a Wi-Fi session waits until that session
  // stops.
  void scheduleBluetoothConditioning(uint32_t controllerGeneration);

  // Reports whether the current Bluetooth controller completed its Wi-Fi cycle
  // and Wi-Fi is still off.
  bool isBluetoothConditioned() const;

  // Applies pending radio changes and connection timeouts.
  void update();

  // Connects as a client. When `persist` is true, also saves the credentials and
  // selects client mode for the next boot.
  bool startSta(const char* ssid, const char* pass, bool persist = true);

  // Starts the device access point. When `persist` is true, also selects access
  // point mode for the next boot.
  bool startAp(bool persist = true);

  // Queues radio changes from UI and HTTP tasks. update() stops the servers and
  // applies each accepted request on the main task.
  void requestConnectSta(const char* ssid, const char* pass);

  // Queues deletion of Wi-Fi configuration. A terminal reset refuses every
  // later request because it is followed by a device reboot.
  void requestReset(bool terminal = false);

  // Queues access point startup without replacing a configured device's saved
  // client mode.
  void requestStartAp();
  void requestStop(bool persist);
  void requestRetrySta(bool persist);

  // Reports whether update() still has a queued radio change to apply.
  bool hasPendingAction() const;

  // Reconnects with saved client credentials. When `persist` is true, also
  // selects client mode for the next boot.
  bool retrySta(bool persist = false);

  // Selects and verifies how aggressively an associated STA sleeps between
  // access-point beacons. The selection also applies to later STA restarts in
  // the same boot. The maximum mode preserves the connection but may add
  // latency.
  bool setPowerSave(wifi::PowerSave mode);
  // Reads the mode applied by the ESP-IDF Wi-Fi driver.
  bool readPowerSave(wifi::PowerSave& mode) const;

  // Reports whether client credentials are stored.
  bool hasStaCreds() const;

  // Stops Wi-Fi without deleting configuration. When `persist` is true, Wi-Fi
  // also stays off after reboot.
  void stop(bool persist = false);

  // Deletes saved credentials, mode, and configuration state. `scrubFramework`
  // also erases the ESP-IDF copy and therefore requires an immediate reboot.
  void resetAllConfig(bool scrubFramework);

  // Allows the saved Wi-Fi mode to start automatically at boot.
  void markConfigured();
  bool isConfigured() const;

  Mode mode() const { return _mode.load(std::memory_order_relaxed); }
  State state() const { return _state.load(std::memory_order_relaxed); }
  IPAddress ip() const;

  // Reports whether the access point started after a client connection failed.
  bool apIsFallback() const {
    return _apIsFallback.load(std::memory_order_relaxed);
  }

  // Returns a short reason for the current client connection's last failure.
  const char* lastDisconnectReason() const;

  // Returns the client network or device access point name, or an empty string
  // while Wi-Fi is off.
  const char* ssid() const;

  // Returns the generated, persistent access point password.
  const char* apPassword();

  // Number of stations currently associated with the device's AP. The setup
  // flow advances past its join-QR step when this goes positive.
  uint8_t apStationCount() const {
    return _apStations.load(std::memory_order_relaxed);
  }

  // Returns the MAC-derived name used for the access point and default hostname.
  const char* deviceId();

  // Returns the hostname active for the current Wi-Fi session.
  const char* hostname();

  // Copies the saved hostname, which takes effect on the next Wi-Fi start.
  void hostnameSnapshot(char* buf, size_t n);

  // Saves an RFC 1123 hostname for the next Wi-Fi session.
  bool setHostname(const char* name);

  // Sets additional cleanup to run after Wi-Fi configuration is deleted.
  void setOnReset(std::function<void()> cb);

  // Registers /sys/wifi/status, /sys/wifi/sta, /sys/wifi/scan,
  // /sys/wifi/forget on `server`. All routes are auth-gated by the server's
  // middleware.
  void registerWith(HttpServer& server);

 private:
  static constexpr uint32_t kStaTimeoutMs = 15000;
  static constexpr size_t kMaxHostnameLen = 31;  // DNS label cap

  std::atomic<State> _state{State::Off};
  std::atomic<Mode> _mode{Mode::Off};
  std::atomic<bool> _apIsFallback{false};
  // A value of -1 reloads the saved boolean on the next query.
  mutable std::atomic<int8_t> _staCredsCached{-1};
  mutable std::atomic<int8_t> _configuredCached{-1};
  // Once STA gets an IP, ordinary reconnect drops should not fall back to AP.
  // Credential-looking reconnect failures clear this so AP fallback is again
  // bounded by kStaTimeoutMs.
  std::atomic<bool> _staFallbackTimeoutSuppressed{false};
  struct BluetoothConditioningState {
    std::atomic<uint32_t> requestedGeneration{0};
    std::atomic<uint32_t> completedGeneration{0};
    uint32_t attemptedGeneration = 0;
  };
  BluetoothConditioningState _bluetoothConditioning;
  // True once the current STA attempt has associated (STA_CONNECTED seen
  // since startSta). GOT_IP requires it, so a stale IP event queued across
  // a credential swap — e.g. a DHCP renewal from the previous session —
  // can't mark the new attempt connected and suppress its fallback timeout.
  std::atomic<bool> _staAssociated{false};
  // Raw wifi_event_sta_disconnected_t.reason from the last relevant STA
  // disconnect; 0 means no disconnect is recorded for the current attempt.
  std::atomic<uint8_t> _lastDisconnectReason{0};
  bool _eventsRegistered = false;
  std::atomic<uint32_t> _staConnectingStartedMs{0};
  std::atomic<wifi::PowerSave> _staPowerSave{wifi::kDefaultStaPowerSave};

  std::string _staSsid;
  std::string _apSsid;
  std::string _apPass;
  std::atomic<uint8_t> _apStations{0};
  // Main-task copy of the hostname active for the current Wi-Fi session.
  char _hostname[kMaxHostnameLen + 1] = "";

  std::function<void()> _onReset;

  enum class PendingAction : uint8_t {
    None,
    ConnectSta,
    Reset,
    StartAp,
    Stop,
    RetrySta,
  };
  // Holds one cross-task request. A newer mode change replaces an older one,
  // but an accepted reset cannot be replaced by a mode change. A terminal reset
  // refuses every later request until reboot.
  struct PendingRequest {
    PendingAction action = PendingAction::None;
    bool persist = false;    // for Stop / RetrySta
    bool terminal = false;   // for Reset
    std::string ssid, pass;  // for ConnectSta
  };
  SemaphoreHandle_t _pendingMutex;
  PendingRequest _pending;

  // Queues a request unless an accepted reset takes precedence.
  void _post(PendingRequest r);
  void _processPending();
  void _attemptBluetoothConditioning();
  // Reports whether the active client session already uses these credentials.
  bool _staSessionMatches(const std::string& ssid,
                          const std::string& pass) const;

  void _handleStatus(WebServer& server);
  void _handleSetSta(WebServer& server);
  void _handleScan(WebServer& server);
  void _handleForget(WebServer& server);
  void _handleSetHostname(WebServer& server);

  void _ensureEventsRegistered();
  void _ensureApSsid();
  void _ensureApPassword();
  void _loadHostname();
  static bool _isValidHostname(const char* s);
  // event is arduino_event_id_t; disconnectReason is the
  // wifi_event_sta_disconnected_t.reason byte (only meaningful when
  // event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED, 0 otherwise).
  void _onWifiEvent(int event, uint8_t disconnectReason);
  bool _loadStaCreds(std::string& ssid, std::string& pass) const;
  void _saveStaCreds(const char* ssid, const char* pass);
  void _persistMode(Mode m);
  Mode _persistedMode() const;
};

extern WifiManager wifiManager;
