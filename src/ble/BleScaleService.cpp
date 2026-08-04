// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#include "BleScaleService.h"

#include <M5Unified.h>
#include <NimBLEDevice.h>

#include <cassert>
#include <cstdio>
#include <cstring>

#include "AcaiaScaleDriver.h"
#include "diagnostics/RuntimeEventLog.h"
#include "util/monotonic.h"

#define BLE_LOG_VERBOSE 0
#if BLE_LOG_VERBOSE
#define LOGV M5_LOGV
#else
#define LOGV(...)
#endif

namespace {

// DiagScan uses equal interval and window values for a
// near-continuous listen so we catch consecutive adverts and can estimate
// each device's advertising interval from inter-arrival gaps.
constexpr uint16_t DIAG_SCAN_INTERVAL_MS = 38;
constexpr uint16_t DIAG_SCAN_WINDOW_MS = 38;
// Run the discovery scan in finite slices rather than one endless scan: with
// maxResults==0, NimBLE only flushes its internal result cache on callback or
// scan *completion*, and scannable adverts held pending a scan-response would
// otherwise pile up forever in a never-completing scan. Each slice completes,
// flushes, and runMode(DiagScan) restarts it. The brief inter-slice gap only
// inflates the occasional inter-arrival delta, which the min-based interval
// estimate shrugs off. Our own _scanTable persists across slices.
constexpr uint32_t DIAG_SCAN_SLICE_MS = 5000;
// Floor for a *plausible* inter-advertisement gap. BLE legacy advertising can't
// run below 20 ms, so any smaller gap between received PDUs is a within-event
// artifact — the SCAN_RSP that active scan solicits arrives ~1-2 ms after the
// ADV, and the same event can be delivered once per advertising channel.
// Counting those would drag the min-based interval estimate toward zero. 15 ms
// (a little under the 20 ms spec floor, for jitter) keeps real intervals.
constexpr uint32_t MIN_PLAUSIBLE_INTERVAL_MS = 15;
constexpr uint32_t HEARTBEAT_PERIOD_MS = 2000;
// A delivered subscription does not guarantee that the scale starts sending
// weight data. Retry the application handshake when the useful stream stays
// silent; a working scale normally begins streaming well within this interval.
constexpr uint32_t RESUBSCRIBE_PERIOD_MS = 1000;
constexpr uint8_t MAX_RESUBSCRIBE_ATTEMPTS = 5;
constexpr uint32_t MANAGER_TICK_MS = 100;
constexpr uint32_t CONNECT_TIMEOUT_MS = 8000;
constexpr uint8_t CMD_QUEUE_DEPTH = 8;
constexpr int8_t NIMBLE_TX_POWER_DBM = 9;

bool initializeNimble() {
  if (NimBLEDevice::isInitialized()) return true;
  M5_LOGI("BleScaleService: NimBLE init");
  if (!NimBLEDevice::init("")) {
    M5_LOGE("BleScaleService: NimBLE init failed");
    return false;
  }
  if (!NimBLEDevice::setPower(NIMBLE_TX_POWER_DBM)) {
    M5_LOGE("BleScaleService: NimBLE transmit-power setup failed");
    NimBLEDevice::deinit(false);
    return false;
  }
  return true;
}

const char* stateName(BleScaleService::State s) {
  switch (s) {
    case BleScaleService::State::OFF:
      return "OFF";
    case BleScaleService::State::SCANNING:
      return "SCANNING";
    case BleScaleService::State::CONNECTING:
      return "CONNECTING";
    case BleScaleService::State::READY:
      return "READY";
    case BleScaleService::State::RECONNECTING:
      return "RECONNECTING";
  }
  return "?";
}

// Format the first up-to-`n` bytes of `data` as "EF DD 0C ..." into `out`.
// Bounded — does not log full packets. Returns out for chaining.
const char* hexHead(char* out, size_t outCap, const uint8_t* data, size_t len,
                    size_t n) {
  if (outCap == 0) return out;
  out[0] = '\0';
  size_t cur = 0;
  const size_t k = len < n ? len : n;
  for (size_t i = 0; i < k && cur + 4 < outCap; ++i) {
    cur += std::snprintf(out + cur, outCap - cur, "%02X ", data[i]);
  }
  if (k < len && cur + 4 < outCap) {
    std::snprintf(out + cur, outCap - cur, "...");
  }
  return out;
}

}  // namespace

// The single process-wide service instance. Constructor is trivial; real
// init happens in begin() called from setup().
BleScaleService bleScale;

// ---- ScanCallbacks ----------------------------------------------------------

class BleScaleService::ScanCallbacks : public NimBLEScanCallbacks {
 public:
  explicit ScanCallbacks(BleScaleService* owner) : _owner(owner) {}
  void onResult(const NimBLEAdvertisedDevice* d) override {
    // DiagScan mode: record every advertiser (named or not) and bail before
    // any match/connect logic.
    if (_owner->_diagMode) {
      _owner->recordDiagAdvert(d);
      return;
    }
    if (_owner->_foundDevice) return;
    if (!d->haveName()) {
      LOGV("BLE adv: %s (no name, RSSI %d)", d->getAddress().toString().c_str(),
           d->getRSSI());
      return;
    }
    const std::string& name = d->getName();
    if (!BleScaleService::matchesScale(name)) {
      LOGV("BLE adv: '%s' @ %s RSSI %d (no match)", name.c_str(),
           d->getAddress().toString().c_str(), d->getRSSI());
      return;
    }
    NimBLEAddress addr = d->getAddress();
    M5_LOGI("BLE adv match: '%s' @ %s RSSI %d", name.c_str(),
            addr.toString().c_str(), d->getRSSI());
    // NimBLE stores native (LE) bytes; getVal() returns them in that order.
    // The NimBLEAddress(uint8_t[6], type) constructor reverse-copies its input
    // (it expects display-order BE), so we pre-reverse here to round-trip.
    const uint8_t* native = addr.getVal();
    for (int i = 0; i < 6; ++i) _owner->_targetAddrBytes[i] = native[5 - i];
    _owner->_targetAddrType = addr.getType();
    // Publish the edge and return WITHOUT calling any NimBLE API. The manager
    // stops the scan when it observes _foundDevice (see runStateMachine). This
    // upholds the "callbacks never call NimBLE" invariant and, critically,
    // avoids calling scan->stop() from inside the host event handler: with
    // maxResults==0 (sticky from a prior DiagScan), stop() frees the result
    // store while the handler still dereferences this advert pointer after we
    // return — a use-after-free. The early-return guard above stops us from
    // reprocessing later adverts in the ~1 tick before the manager stops it.
    _owner->_foundDevice = true;
  }

 private:
  BleScaleService* _owner;
};

// ---- ClientCallbacks --------------------------------------------------------

class BleScaleService::ClientCallbacks : public NimBLEClientCallbacks {
 public:
  explicit ClientCallbacks(BleScaleService* owner) : _owner(owner) {}
  void onConnect(NimBLEClient* c) override {
    M5_LOGI("BLE connected to %s", c->getPeerAddress().toString().c_str());
  }
  void onDisconnect(NimBLEClient* c, int reason) override {
    M5_LOGI("BLE disconnected from %s (reason %d)",
            c->getPeerAddress().toString().c_str(), reason);
    _owner->_disconnectFlag = true;
  }

 private:
  BleScaleService* _owner;
};

// ---- Public API -------------------------------------------------------------

bool BleScaleService::begin() {
  if (_managerTask) {
    M5_LOGD("BleScaleService::begin already running");
    return true;
  }

  if (!_mutex) {
    _mutex = xSemaphoreCreateMutex();
    if (!_mutex) {
      M5_LOGE("BleScaleService: mutex alloc failed");
      return false;
    }
  }

  if (!_scanMutex) {
    _scanMutex = xSemaphoreCreateMutex();
    if (!_scanMutex) {
      M5_LOGE("BleScaleService: scan mutex alloc failed");
      return false;
    }
  }

  if (!_msgMutex) {
    _msgMutex = xSemaphoreCreateMutex();
    if (!_msgMutex) {
      M5_LOGE("BleScaleService: msg mutex alloc failed");
      return false;
    }
  }

  if (!_cmdQueue) {
    _cmdQueue = xQueueCreate(CMD_QUEUE_DEPTH, sizeof(Cmd));
    if (!_cmdQueue) {
      M5_LOGE("BleScaleService: command queue alloc failed");
      return false;
    }
  }

  _rxReceiver.setEventCallback(s_onEvent, this);

  if (!initializeController()) return false;
  _controllerState.store(ControllerState::Running, std::memory_order_release);

  if (!_scanCallbacks) _scanCallbacks = new ScanCallbacks(this);
  if (!_clientCallbacks) _clientCallbacks = new ClientCallbacks(this);

  // Pinned to core 0 (same core NimBLE host runs on) to keep BLE work off the
  // Arduino loop running on core 1. 4 KB stack covers GATT discovery depth.
  BaseType_t ok = xTaskCreatePinnedToCore(s_managerTask, "scale_svc", 4096,
                                          this, 5, &_managerTask, 0);
  if (ok != pdPASS) {
    M5_LOGE("BleScaleService: manager task alloc failed");
    return false;
  }
  M5_LOGI("BleScaleService: started (idle, awaiting enable)");
  return true;
}

void BleScaleService::setStartupRequired(bool required) {
  _startupRequired = required;
  updateStartup(millis());
}

void BleScaleService::updateStartup(uint32_t nowMs) {
  if (!_startupRetry.shouldAttempt(_startupRequired, isStarted(), nowMs))
    return;
  if (!begin()) M5_LOGE("BleScaleService: startup failed; retry scheduled");
}

bool BleScaleService::initializeController() {
  const bool wasInitialized = NimBLEDevice::isInitialized();
  if (!initializeNimble()) return false;
  if (!wasInitialized) {
    const uint32_t generation =
        _controllerGeneration.fetch_add(1, std::memory_order_acq_rel) + 1;
    if (_controllerStartedCallback) _controllerStartedCallback(generation);
  }
  return true;
}

void BleScaleService::enable() {
  if (!_connectWanted.exchange(true, std::memory_order_acq_rel)) {
    M5_LOGI("BleScaleService: enable (Connect requested)");
  }
}

void BleScaleService::disable() {
  if (_connectWanted.exchange(false, std::memory_order_acq_rel)) {
    M5_LOGI("BleScaleService: disable (Connect released)");
  }
}

BleScaleService::DemandSnapshot BleScaleService::demandSnapshot() const {
  const uint32_t deadline = _diagScanDeadlineMs.load(std::memory_order_acquire);
  return {
      _connectWanted.load(std::memory_order_acquire),
      deadline != 0 && static_cast<int32_t>(deadline - millis()) > 0,
  };
}

void BleScaleService::setPowerSuspended(bool suspended) {
  _powerSuspended.store(suspended, std::memory_order_release);
}

void BleScaleService::setScanPolicy(ble::ScanPolicy policy) {
  assert(ble::isValidScanPolicy(policy));
  if (!_cmdQueue) return;
  const Cmd cmd{CmdType::SetScanPolicy, 0, policy};
  if (xQueueSend(_cmdQueue, &cmd, 0) != pdPASS) {
    M5_LOGW("BleScaleService::setScanPolicy: command queue full");
  }
}

bool BleScaleService::requestControllerStop() {
  if (!_managerTask || !_cmdQueue) return false;
  if (controllerState() != ControllerState::Running) return false;
  bool pending = false;
  if (!_controllerRequestPending.compare_exchange_strong(
          pending, true, std::memory_order_acq_rel))
    return false;
  const Cmd cmd{CmdType::StopController, 0};
  if (xQueueSend(_cmdQueue, &cmd, 0) != pdPASS) {
    _controllerRequestPending.store(false, std::memory_order_release);
    M5_LOGW("BleScaleService::requestControllerStop: command queue full");
    return false;
  }
  return true;
}

bool BleScaleService::requestControllerStart() {
  if (!_managerTask || !_cmdQueue) return false;
  if (controllerState() != ControllerState::Stopped) return false;
  bool pending = false;
  if (!_controllerRequestPending.compare_exchange_strong(
          pending, true, std::memory_order_acq_rel))
    return false;
  const Cmd cmd{CmdType::StartController, 0};
  if (xQueueSend(_cmdQueue, &cmd, 0) != pdPASS) {
    _controllerRequestPending.store(false, std::memory_order_release);
    M5_LOGW("BleScaleService::requestControllerStart: command queue full");
    return false;
  }
  return true;
}

bool BleScaleService::requestTare() {
  if (!_cmdQueue || !_mutex) return false;
  // Capture the current epoch atomically with the state read. The manager
  // will compare against its (own-thread) epoch when it dequeues; a tare
  // posted in epoch N that arrives after the next entry-into-READY (epoch
  // N+1) is silently dropped.
  xSemaphoreTake(_mutex, portMAX_DELAY);
  const uint32_t epoch = _connectionEpoch;
  xSemaphoreGive(_mutex);
  const Cmd cmd{CmdType::Tare, epoch};
  if (xQueueSend(_cmdQueue, &cmd, 0) != pdPASS) {
    M5_LOGW("BleScaleService::requestTare: command queue full");
    return false;
  }
  return true;
}

BleScaleService::Snapshot BleScaleService::snapshot() const {
  Snapshot s;
  if (!_mutex) return s;
  xSemaphoreTake(_mutex, portMAX_DELAY);
  s.state = _state;
  s.connectionEpoch = _connectionEpoch;
  s.weight = _weight;
  s.battery = _battery;
  xSemaphoreGive(_mutex);
  return s;
}

void BleScaleService::startDiagScan(uint32_t leaseMs) {
  uint32_t deadline = millis() + leaseMs;
  if (deadline == 0) deadline = 1;
  _diagScanDeadlineMs.store(deadline, std::memory_order_release);
}

void BleScaleService::stopDiagScan() {
  _diagScanDeadlineMs.store(0, std::memory_order_release);
}

BleScaleService::ScanResults BleScaleService::scanSnapshot() const {
  ScanResults r;
  if (!_scanMutex) return r;
  xSemaphoreTake(_scanMutex, portMAX_DELAY);
  r.count = _scanCount;
  for (uint8_t i = 0; i < _scanCount; ++i) r.entries[i] = _scanTable[i];
  xSemaphoreGive(_scanMutex);
  // The manager writes the scan status while the HTTP task may read this
  // snapshot, so both fields are atomic.
  r.active = _diagMode;
  r.busy = _connectWanted;
  return r;
}

void BleScaleService::armMessageLog() {
  if (!_cmdQueue) return;
  const Cmd cmd{CmdType::LogArm, 0};
  if (xQueueSend(_cmdQueue, &cmd, 0) != pdPASS) {
    M5_LOGW("BleScaleService::armMessageLog: command queue full");
  }
}

void BleScaleService::disarmMessageLog() {
  if (!_cmdQueue) return;
  const Cmd cmd{CmdType::LogDisarm, 0};
  if (xQueueSend(_cmdQueue, &cmd, 0) != pdPASS) {
    M5_LOGW("BleScaleService::disarmMessageLog: command queue full");
  }
}

BleScaleService::MsgLogSnapshot BleScaleService::messageLogSnapshot() const {
  MsgLogSnapshot s;
  if (!_msgMutex) return s;
  xSemaphoreTake(_msgMutex, portMAX_DELAY);
  // Read armed under the lock so it's coherent with writes (both are mutated
  // together under _msgMutex on arm/disarm); the ETag derives from writes, so
  // the pair must agree. Packet capture reads the atomic without this lock and
  // can tolerate one in-flight packet at an arm or disarm boundary.
  s.armed = _logArmed;
  s.writes = _msgWrites;
  for (int i = 0; i < MSG_TAG_COUNT; ++i) s.byTag[i] = _msgByTag[i];
  s.count = _msgCount;
  // Copy newest-first: walk back from the most recently written slot.
  for (uint8_t i = 0; i < _msgCount; ++i) {
    const uint8_t slot =
        static_cast<uint8_t>((_msgNext + MSG_LOG_CAP - 1 - i) % MSG_LOG_CAP);
    s.records[i] = _msgLog[slot];
  }
  xSemaphoreGive(_msgMutex);
  return s;
}

// ---- Manager task -----------------------------------------------------------

void BleScaleService::s_managerTask(void* arg) {
  static_cast<BleScaleService*>(arg)->managerLoop();
}

void BleScaleService::managerLoop() {
  for (;;) {
    drainCommands();
    // A stopped controller bypasses mode selection, so lease cleanup belongs
    // before that early return. Otherwise an expired deadline can appear live
    // again after the signed millis() half-wrap.
    expireDiagScanLease(millis());
    if (processControllerTransition()) {
      vTaskDelay(pdMS_TO_TICKS(MANAGER_TICK_MS));
      continue;
    }
    const Mode desired = selectMode();
    if (desired != _mode) {
      exitMode(_mode);
      _mode = desired;
      enterMode(_mode);
    }
    runMode(_mode);
    vTaskDelay(pdMS_TO_TICKS(MANAGER_TICK_MS));
  }
}

void BleScaleService::expireDiagScanLease(uint32_t nowMs) {
  uint32_t deadline = _diagScanDeadlineMs.load(std::memory_order_acquire);
  if (deadline != 0 && static_cast<int32_t>(deadline - nowMs) <= 0) {
    _diagScanDeadlineMs.compare_exchange_strong(deadline, 0,
                                                std::memory_order_acq_rel);
  }
}

void BleScaleService::drainCommands() {
  Cmd cmd;
  while (xQueueReceive(_cmdQueue, &cmd, 0) == pdTRUE) {
    switch (cmd.type) {
      case CmdType::Tare: {
        // Epoch check makes the race impossible: a tare for an earlier
        // session simply doesn't match the current one.
        if (_mode == Mode::Connect && _state == State::READY &&
            cmd.arg == _connectionEpoch) {
          uint8_t buf[AcaiaV2::MAX_PACKET];
          const size_t n = AcaiaV2::buildTare(buf);
          M5_LOGI("BleScaleService: tare (epoch %u)",
                  static_cast<unsigned>(cmd.arg));
          sendPacket(buf, n);
        } else {
          M5_LOGD(
              "BleScaleService: tare dropped (state=%s, epoch %u vs current "
              "%u)",
              stateName(_state), static_cast<unsigned>(cmd.arg),
              static_cast<unsigned>(_connectionEpoch));
        }
        break;
      }

      case CmdType::LogArm:
        // Reset the ring + counts, open the gate, and bump writes — all under
        // _msgMutex so a concurrent messageLogSnapshot() reads a coherent
        // {armed, writes} pair. The ETag is writes-only, so an armed flip that
        // wasn't paired with its writes bump could otherwise be served as a 304
        // and strand the web UI on the stale state.
        xSemaphoreTake(_msgMutex, portMAX_DELAY);
        _msgNext = 0;
        _msgCount = 0;
        for (int i = 0; i < MSG_TAG_COUNT; ++i) _msgByTag[i] = 0;
        _logArmed = true;
        ++_msgWrites;
        xSemaphoreGive(_msgMutex);
        break;

      case CmdType::LogDisarm:
        xSemaphoreTake(_msgMutex, portMAX_DELAY);
        _logArmed = false;
        ++_msgWrites;
        xSemaphoreGive(_msgMutex);
        break;

      case CmdType::SetScanPolicy:
        _scanPolicy = cmd.scanPolicy;
        _connectScanNextStartMs = 0;
        if (_mode == Mode::Connect) stopScanIfActive();
        M5_LOGI(
            "BleScaleService: scan policy interval=%ums window=%ums run=%ums "
            "pause=%ums",
            static_cast<unsigned>(_scanPolicy.intervalMs),
            static_cast<unsigned>(_scanPolicy.windowMs),
            static_cast<unsigned>(_scanPolicy.durationMs),
            static_cast<unsigned>(_scanPolicy.pauseMs));
        break;

      case CmdType::StopController:
        if (controllerState() == ControllerState::Running) {
          _controllerState.store(ControllerState::Stopping,
                                 std::memory_order_release);
          M5_LOGI("BleScaleService: Bluetooth stop requested");
        }
        _controllerRequestPending.store(false, std::memory_order_release);
        break;

      case CmdType::StartController:
        if (controllerState() == ControllerState::Stopped) {
          _controllerState.store(ControllerState::Starting,
                                 std::memory_order_release);
          M5_LOGI("BleScaleService: Bluetooth start requested");
        }
        _controllerRequestPending.store(false, std::memory_order_release);
        break;
    }
  }
}

bool BleScaleService::processControllerTransition() {
  const ControllerState state = controllerState();
  if (state == ControllerState::Running) return false;

  if (state == ControllerState::Stopping) {
    if (_mode != Mode::Idle) {
      exitMode(_mode);
      _mode = Mode::Idle;
      enterMode(_mode);
    }
    _scanActive = false;
    _diagMode = false;

    // Retain stack-owned objects to avoid allocation churn across repeated
    // controller restarts. exitMode() has already removed the live client.
    const bool stopped = !NimBLEDevice::isInitialized() ||
                         NimBLEDevice::deinit(/*clearAll=*/false);
    _controllerState.store(
        stopped ? ControllerState::Stopped : ControllerState::Failed,
        std::memory_order_release);
    M5_LOGI("BleScaleService: Bluetooth controller %s",
            stopped ? "stopped" : "stop failed");
    return true;
  }

  if (state == ControllerState::Starting) {
    const bool started = initializeController();
    _controllerState.store(
        started ? ControllerState::Running : ControllerState::Failed,
        std::memory_order_release);
    M5_LOGI("BleScaleService: Bluetooth controller %s",
            started ? "started" : "start failed");
    return true;
  }

  // A stopped controller keeps draining commands so it can accept a start.
  // Failed remains parked because recovery requires a reboot.
  return true;
}

void BleScaleService::runStateMachine() {
  // Connect-mode only (called from runMode(Connect)). Teardown on release is
  // handled by exitMode(Connect); this just advances the inner state machine.
  switch (_state) {
    case State::OFF:
      // Connect just entered — start the scan/connect cycle.
      _foundDevice = false;
      _hadReadyThisEnable = false;
      if (startScan()) {
        transition(State::SCANNING);
      } else {
        M5_LOGW("BleScaleService: initial scan->start failed; will retry");
      }
      break;

    case State::SCANNING:
    case State::RECONNECTING:
      if (_foundDevice) {
        // The scan-match callback only published the edge; stop the scan here,
        // on the manager task, where it's safe (never from inside onResult).
        stopScanIfActive();
        transition(State::CONNECTING);
      } else if (!NimBLEDevice::getScan()->isScanning()) {
        const bool completedNaturally =
            _scanActive.exchange(false, std::memory_order_relaxed);
        if (completedNaturally) {
          M5_LOGW(
              "BleScaleService: scan window elapsed, no supported scale seen");
        }
        if (_connectScanNextStartMs == 0) {
          if (_scanPolicy.pauseMs == 0) {
            if (!startScan()) {
              M5_LOGW(
                  "BleScaleService: scan->start failed; will retry next tick");
            }
            break;
          }
          _connectScanNextStartMs = millis() + _scanPolicy.pauseMs;
          if (_connectScanNextStartMs == 0) _connectScanNextStartMs = 1;
          M5_LOGI("BleScaleService: scan complete; pause=%ums",
                  static_cast<unsigned>(_scanPolicy.pauseMs));
        }
        if (static_cast<int32_t>(millis() - _connectScanNextStartMs) >= 0) {
          _connectScanNextStartMs = 0;
          if (!startScan()) {
            M5_LOGW(
                "BleScaleService: scan->start failed; will retry next tick");
          }
        }
      }
      break;

    case State::CONNECTING: {
      _disconnectFlag = false;
      const bool connected = tryConnect();
      const bool handshook = connected && doHandshake();
      if (handshook) {
        _hadReadyThisEnable = true;
        transition(State::READY);
        _lastHeartbeatMs = millis();
        _armMs = millis();
        _rxWeightCountAtArm = _rxWeightCount;
        _resubscribeAttempts = 0;
      } else {
        runtimeEventLog.pushNetFailure(
            diagnostics::NetSource::Ble,
            static_cast<uint16_t>(connected
                                      ? diagnostics::BleFailureCode::Handshake
                                      : diagnostics::BleFailureCode::Connect),
            connected ? "handshake failed" : "connect failed");
        teardownClient();
        // After a previous READY this enable session, we're reconnecting;
        // otherwise we're still in the first-connection attempt.
        transition(_hadReadyThisEnable ? State::RECONNECTING : State::SCANNING);
      }
      break;
    }

    case State::READY:
      if (_disconnectFlag.exchange(false, std::memory_order_acq_rel)) {
        runtimeEventLog.pushNetFailure(
            diagnostics::NetSource::Ble,
            static_cast<uint16_t>(diagnostics::BleFailureCode::Disconnect),
            "disconnected");
        teardownClient();
        transition(State::RECONNECTING);
        break;
      }
      if (millis() - _lastHeartbeatMs >= HEARTBEAT_PERIOD_MS) {
        _lastHeartbeatMs = millis();
        uint8_t buf[AcaiaV2::MAX_PACKET];
        const size_t n = AcaiaV2::buildHeartbeat(buf);
        sendPacket(buf, n);
      }
      // Retry the application handshake while the link is connected but has
      // not produced weight data. Reconnect if repeated retries cannot start
      // the useful stream.
      if (_rxWeightCount == _rxWeightCountAtArm &&
          millis() - _armMs >= RESUBSCRIBE_PERIOD_MS) {
        if (_resubscribeAttempts < MAX_RESUBSCRIBE_ATTEMPTS) {
          M5_LOGW(
              "BleScaleService: no weight data; retrying handshake "
              "(attempt %u)",
              static_cast<unsigned>(_resubscribeAttempts + 1));
          // Use writes without response so a failing link cannot block the
          // manager for the GATT timeout; this watchdog provides the retry.
          uint8_t buf[AcaiaV2::MAX_PACKET];
          size_t n = AcaiaV2::buildIdentify(buf);
          sendPacket(buf, n, /*ack=*/false);
          n = AcaiaV2::buildEventSubscribe(buf);
          sendPacket(buf, n, /*ack=*/false);
          ++_resubscribeAttempts;
          _armMs = millis();
        } else {
          M5_LOGW("BleScaleService: weight stream stayed silent; reconnecting");
          runtimeEventLog.pushNetFailure(
              diagnostics::NetSource::Ble,
              static_cast<uint16_t>(diagnostics::BleFailureCode::Handshake),
              "stream never armed");
          teardownClient();
          transition(State::RECONNECTING);
        }
      }
      break;
  }
}

void BleScaleService::transition(State next) {
  if (_state == next) return;
  M5_LOGI("BleScaleService state: %s -> %s", stateName(_state),
          stateName(next));

  const bool leavingReady = (_state == State::READY) && (next != State::READY);
  const bool enteringReady = (next == State::READY) && (_state != State::READY);

  xSemaphoreTake(_mutex, portMAX_DELAY);
  if (leavingReady) {
    // Weight is live data — invalid once the link drops.
    _weight = WeightReading{};
  }
  if (enteringReady) {
    // Each entry into READY is a new session for epoch-tagged commands.
    ++_connectionEpoch;
    // If the cached battery came from a different scale, drop it.
    if (_battery.timestampMs != 0 &&
        std::memcmp(_batteryAddrBytes, _targetAddrBytes,
                    sizeof(_batteryAddrBytes)) != 0) {
      _battery = BatteryReading{};
      std::memset(_batteryAddrBytes, 0, sizeof(_batteryAddrBytes));
    }
  }
  if (next == State::OFF) {
    // Disable clears the live readings outright; battery in particular
    // shouldn't carry across an explicit disable (the user may pair the
    // scale with another device in between).
    _weight = WeightReading{};
    _battery = BatteryReading{};
    std::memset(_batteryAddrBytes, 0, sizeof(_batteryAddrBytes));
  }
  _state = next;
  xSemaphoreGive(_mutex);
}

// ---- Mode multiplexer -------------------------------------------------------

BleScaleService::Mode BleScaleService::selectMode() {
  if (_powerSuspended.load(std::memory_order_acquire)) return Mode::Idle;
  // Connect always wins; a diag-scan lease is honored only when the radio is
  // otherwise free.
  if (_connectWanted.load(std::memory_order_acquire)) return Mode::Connect;
  if (_diagScanDeadlineMs.load(std::memory_order_acquire) != 0)
    return Mode::DiagScan;
  return Mode::Idle;
}

void BleScaleService::enterMode(Mode m) {
  switch (m) {
    case Mode::Connect:
      M5_LOGI("BleScaleService: mode -> Connect");
      // Fresh session. _state is OFF here (initial, or reset by the prior
      // exitMode(Connect)); runStateMachine()'s OFF case starts the scan.
      _foundDevice = false;
      _hadReadyThisEnable = false;
      _connectScanNextStartMs = 0;
      break;
    case Mode::DiagScan:
      M5_LOGI("BleScaleService: mode -> DiagScan");
      clearScanTable();
      _diagMode = true;
      if (!startDiagScanRadio()) {
        M5_LOGW("BleScaleService: diag scan start failed; will retry");
      }
      break;
    case Mode::Idle:
      M5_LOGI("BleScaleService: mode -> Idle");
      break;
  }
}

void BleScaleService::exitMode(Mode m) {
  switch (m) {
    case Mode::Connect:
      // Disconnect, stop any scan, and park the inner machine in OFF so a
      // later re-entry starts clean.
      teardownClient();
      stopScanIfActive();
      _foundDevice = false;
      _connectScanNextStartMs = 0;
      transition(State::OFF);
      break;
    case Mode::DiagScan:
      stopScanIfActive();
      _diagMode = false;
      break;
    case Mode::Idle:
      break;
  }
}

void BleScaleService::runMode(Mode m) {
  switch (m) {
    case Mode::Connect:
      runStateMachine();
      break;
    case Mode::DiagScan:
      // The discovery scan runs in finite slices (DIAG_SCAN_SLICE_MS); each
      // completion flushes NimBLE's result cache. Restart whenever it isn't
      // running — this is the normal slice loop, and also covers a transient
      // stack hiccup.
      if (!NimBLEDevice::getScan()->isScanning()) {
        _scanActive = false;
        if (!startDiagScanRadio()) {
          M5_LOGW("BleScaleService: diag scan restart failed");
        }
      }
      break;
    case Mode::Idle:
      break;
  }
}

// ---- DiagScan internals -----------------------------------------------------

bool BleScaleService::matchesScale(const std::string& name) {
  return AcaiaScaleDriver::matchesAdvertisedName(name);
}

bool BleScaleService::startDiagScanRadio() {
  NimBLEScan* scan = NimBLEDevice::getScan();
  scan->setScanCallbacks(_scanCallbacks, /*wantDuplicates=*/true);
  scan->setActiveScan(true);
  // Callbacks-only: do not let NimBLE retain a result for every advertiser in
  // its internal vector. We read everything through onResult, so 0 is correct.
  // Note this alone does not bound the cache for a never-completing scan (see
  // DIAG_SCAN_SLICE_MS); pair it with finite slices + clearResults().
  scan->setMaxResults(0);
  scan->clearResults();  // drop anything retained by the previous slice
  scan->setInterval(DIAG_SCAN_INTERVAL_MS);
  scan->setWindow(DIAG_SCAN_WINDOW_MS);
  M5_LOGI("BleScaleService: diag scan slice start (%ums, duplicates)",
          static_cast<unsigned>(DIAG_SCAN_SLICE_MS));
  // Finite slice; on completion isScanning() goes false and runMode(DiagScan)
  // restarts it (flushing the cache each time). Non-blocking.
  const bool started = scan->start(DIAG_SCAN_SLICE_MS, /*isContinue=*/false,
                                   /*restart=*/true);
  _scanActive = started;
  return started;
}

void BleScaleService::clearScanTable() {
  if (!_scanMutex) return;
  xSemaphoreTake(_scanMutex, portMAX_DELAY);
  _scanCount = 0;
  xSemaphoreGive(_scanMutex);
}

// Runs on the BLE host task (scan callback). Upserts the advertiser into the
// table and refines its advertising-interval estimate. Holds only _scanMutex,
// never calls back into NimBLE.
void BleScaleService::recordDiagAdvert(const NimBLEAdvertisedDevice* d) {
  if (!_scanMutex) return;
  const uint32_t now = millis();
  NimBLEAddress addr = d->getAddress();
  // Store display-order (BE) bytes so consumers can print addr[0]:addr[1]:…
  // directly; getVal() yields little-endian.
  const uint8_t* native = addr.getVal();
  uint8_t disp[6];
  for (int i = 0; i < 6; ++i) disp[i] = native[5 - i];
  const std::string name = d->haveName() ? d->getName() : std::string();
  const int8_t rssi = static_cast<int8_t>(d->getRSSI());

  xSemaphoreTake(_scanMutex, portMAX_DELAY);
  ScanEntry* e = nullptr;
  for (uint8_t i = 0; i < _scanCount; ++i) {
    if (std::memcmp(_scanTable[i].addr, disp, 6) == 0) {
      e = &_scanTable[i];
      break;
    }
  }
  if (!e) {
    if (_scanCount < SCAN_TABLE_CAP) {
      e = &_scanTable[_scanCount++];
    } else {
      // Table full: evict the weakest-RSSI entry.
      uint8_t weakest = 0;
      for (uint8_t i = 1; i < _scanCount; ++i) {
        if (_scanTable[i].rssi < _scanTable[weakest].rssi) weakest = i;
      }
      e = &_scanTable[weakest];
    }
    *e = ScanEntry{};
    std::memcpy(e->addr, disp, 6);
    e->lastSeenMs = now;
  } else {
    const uint32_t delta = now - e->lastSeenMs;
    // Only plausible gaps (>= the spec floor) refine the estimate and advance
    // the reference timestamp. Sub-floor gaps are within-event artifacts
    // (SCAN_RSP, per-channel duplicates) — ignoring them keeps the min from
    // collapsing toward 0, and keeping lastSeenMs anchored to the last real
    // advert makes the *next* gap a clean interval rather than a short one.
    if (delta >= MIN_PLAUSIBLE_INTERVAL_MS) {
      if (e->minIntervalMs == 0 || delta < e->minIntervalMs) {
        e->minIntervalMs = delta;
      }
      e->lastSeenMs = now;
      // Count only plausible interval measurements toward the display
      // threshold. Counting every callback would let sub-floor artifacts
      // (SCAN_RSP, per-channel duplicates) satisfy "seen enough" after just
      // one real interval, showing an unsettled estimate as if trustworthy.
      if (e->samples < 0xFFFF) ++e->samples;
    }
  }
  // RSSI/name/recognized track every reception (the scale's name often arrives
  // in the SCAN_RSP, which is one of the sub-floor callbacks).
  e->rssi = rssi;
  if (!name.empty()) {
    std::strncpy(e->name, name.c_str(), sizeof(e->name) - 1);
    e->name[sizeof(e->name) - 1] = '\0';
    e->recognized = matchesScale(name);
  }
  xSemaphoreGive(_scanMutex);
}

// ---- Message log internals --------------------------------------------------

namespace {
BleScaleService::MsgTag mapInbound(AcaiaV2::InboundKind k) {
  using IK = AcaiaV2::InboundKind;
  using T = BleScaleService::MsgTag;
  switch (k) {
    case IK::Weight:
      return T::RxWeight;
    case IK::Battery:
      return T::RxBattery;
    case IK::Timer:
      return T::RxTimer;
    case IK::Key:
      return T::RxKey;
    case IK::Unknown:
      return T::RxUnknown;
    case IK::Mixed:
      return T::RxMixed;
    case IK::Rejected:
      return T::RxRejected;
    case IK::Settings:
      return T::RxSettings;
  }
  return T::RxRejected;
}
BleScaleService::MsgTag mapOutbound(AcaiaV2::OutboundKind k) {
  using OK = AcaiaV2::OutboundKind;
  using T = BleScaleService::MsgTag;
  switch (k) {
    case OK::Heartbeat:
      return T::TxHeartbeat;
    case OK::Tare:
      return T::TxTare;
    case OK::Timer:
      return T::TxTimer;
    case OK::Identify:
      return T::TxIdentify;
    case OK::Subscribe:
      return T::TxSubscribe;
    case OK::Other:
      return T::TxOther;
  }
  return T::TxOther;
}
}  // namespace

void BleScaleService::recordMsg(uint8_t dir, MsgTag tag, const uint8_t* data,
                                size_t len) {
  if (!_msgMutex) return;
  const uint32_t now = millis();
  xSemaphoreTake(_msgMutex, portMAX_DELAY);
  MsgRecord& r = _msgLog[_msgNext];
  r.ms = now;
  r.dir = dir;
  r.tag = tag;
  r.wireLen = len > 255 ? 255 : static_cast<uint8_t>(len);
  const size_t cap = sizeof(r.raw);
  const size_t rawLen = len < cap ? len : cap;
  r.rawLen = static_cast<uint8_t>(rawLen);
  std::memcpy(r.raw, data, rawLen);
  _msgNext = static_cast<uint8_t>((_msgNext + 1) % MSG_LOG_CAP);
  if (_msgCount < MSG_LOG_CAP) ++_msgCount;
  const int ti = static_cast<int>(tag);
  if (ti >= 0 && ti < MSG_TAG_COUNT) ++_msgByTag[ti];
  ++_msgWrites;
  xSemaphoreGive(_msgMutex);
}

// ---- Scan / connect / handshake --------------------------------------------

bool BleScaleService::startScan() {
  NimBLEScan* scan = NimBLEDevice::getScan();
  scan->setScanCallbacks(_scanCallbacks, /*wantDuplicates=*/false);
  scan->setActiveScan(true);
  scan->setInterval(_scanPolicy.intervalMs);
  scan->setWindow(_scanPolicy.windowMs);
  // Fully (re)configure the sticky maxResults on entry rather than inheriting
  // whatever the last mode left (DiagScan sets 0). Connect is callback-driven
  // and never reads getResults(), so 0 (callback-only, no result store) is the
  // intended value here too. Safe now that stop() is only ever called from the
  // manager task, never from inside onResult.
  scan->setMaxResults(0);
  M5_LOGI("BleScaleService: scan start (window=%ums)",
          static_cast<unsigned>(_scanPolicy.durationMs));
  // Non-blocking start; callback runs from BLE host task.
  const bool started = scan->start(_scanPolicy.durationMs,
                                   /*isContinue=*/false, /*restart=*/true);
  _scanActive = started;
  return started;
}

void BleScaleService::stopScanIfActive() {
  NimBLEScan* scan = NimBLEDevice::getScan();
  if (scan && scan->isScanning()) scan->stop();
  _scanActive = false;
}

namespace {

void dumpPeerGatt(NimBLEClient* client) {
  if (!client) return;
  const auto& svcs = client->getServices(true);
  for (NimBLERemoteService* svc : svcs) {
    if (!svc) continue;
    M5_LOGW("  peer service: %s", svc->getUUID().toString().c_str());
    const auto& chars = svc->getCharacteristics(true);
    for (NimBLERemoteCharacteristic* ch : chars) {
      if (!ch) continue;
      M5_LOGW("    char: %s (read=%d write=%d wnr=%d notify=%d)",
              ch->getUUID().toString().c_str(), ch->canRead() ? 1 : 0,
              ch->canWrite() ? 1 : 0, ch->canWriteNoResponse() ? 1 : 0,
              ch->canNotify() ? 1 : 0);
    }
  }
}

}  // namespace

bool BleScaleService::tryConnect() {
  if (!_client) {
    _client = NimBLEDevice::createClient();
    if (!_client) {
      M5_LOGE("BleScaleService: createClient failed");
      return false;
    }
    _client->setClientCallbacks(_clientCallbacks, /*deleteCallbacks=*/false);
    // Modest timeouts so a hung connect doesn't stall reconnect attempts.
    _client->setConnectionParams(12, 12, 0, 200);
    _client->setConnectTimeout(CONNECT_TIMEOUT_MS);
    // The service state machine owns retry timing and failure reporting.
    _client->setConnectRetries(0);
  }

  NimBLEAddress addr(_targetAddrBytes, _targetAddrType);
  M5_LOGI("BleScaleService: connecting to %s (type %u)",
          addr.toString().c_str(), _targetAddrType);
  const uint32_t t0 = millis();
  if (!_client->connect(addr, /*deleteAttibutes=*/true)) {
    M5_LOGW("BleScaleService: connect failed after %u ms", millis() - t0);
    return false;
  }
  M5_LOGI("BleScaleService: connect succeeded in %u ms", millis() - t0);
  return true;
}

bool BleScaleService::doHandshake() {
  AcaiaScaleDriver::ResolvedGatt gatt;
  if (!AcaiaScaleDriver::resolveGatt(_client, &gatt)) {
    M5_LOGE("BleScaleService: no supported Acaia GATT profile found");
    dumpPeerGatt(_client);
    return false;
  }
  _writeChar = gatt.writeChar;
  NimBLERemoteCharacteristic* notifyChar = gatt.notifyChar;
  M5_LOGI("BleScaleService: using Acaia %s GATT profile", gatt.profileLabel);

  // Subscribe before identify so the scale's first notifications aren't lost.
  M5_LOGI("BleScaleService: subscribing to notifications");
  xSemaphoreTake(_mutex, portMAX_DELAY);
  _weight = WeightReading{};
  xSemaphoreGive(_mutex);
  uint32_t rxGeneration = ++_rxGenerationIssued;
  if (rxGeneration == 0) rxGeneration = ++_rxGenerationIssued;
  _rxAcceptedGeneration = rxGeneration;
  bool subscribed = notifyChar->subscribe(
      /*notifications=*/true,
      [this, rxGeneration](NimBLERemoteCharacteristic* /*c*/, uint8_t* data,
                           size_t len, bool /*isNotify*/) {
        this->onNotify(rxGeneration, data, len);
      });
  if (!subscribed) {
    _rxAcceptedGeneration = 0;
    M5_LOGE("BleScaleService: subscribe failed");
    return false;
  }

  M5_LOGI("BleScaleService: sending identify + event-subscribe");
  uint8_t buf[AcaiaV2::MAX_PACKET];
  size_t n = AcaiaV2::buildIdentify(buf);
  if (!sendPacket(buf, n, /*ack=*/true)) {
    _rxAcceptedGeneration = 0;
    M5_LOGE("BleScaleService: identify write failed");
    return false;
  }
  n = AcaiaV2::buildEventSubscribe(buf);
  if (!sendPacket(buf, n, /*ack=*/true)) {
    _rxAcceptedGeneration = 0;
    M5_LOGE("BleScaleService: event-subscribe write failed");
    return false;
  }
  M5_LOGI("BleScaleService: handshake complete");
  return true;
}

bool BleScaleService::sendPacket(const uint8_t* data, size_t len, bool ack) {
  if (!_writeChar) return false;
  char hex[40];
  LOGV("BleScaleService: TX %u bytes: %s", static_cast<unsigned>(len),
       hexHead(hex, sizeof(hex), data, len, 8));
  // Steady-state commands (heartbeat) use write-without-response: faster, and
  // what the Acaia firmware expects for the command stream. Handshake commands
  // pass ack=true to get write-with-response so a dropped write is observable —
  // but only if the characteristic actually advertises the Write property;
  // otherwise fall back to no-response (the re-subscribe watchdog covers it).
  const bool response = ack && _writeChar->canWrite();
  bool ok = _writeChar->writeValue(const_cast<uint8_t*>(data), len, response);
  if (!ok)
    M5_LOGW("BleScaleService: writeValue returned false (len=%u)",
            static_cast<unsigned>(len));
  // Diagnostic tap: log the attempt (even if the write failed) when armed.
  if (_logArmed) {
    recordMsg(MSG_DIR_TX, mapOutbound(AcaiaV2::classifyOutbound(data, len)),
              data, len);
  }
  return ok;
}

void BleScaleService::teardownClient() {
  _rxAcceptedGeneration = 0;
  if (_client) {
    if (_client->isConnected()) {
      M5_LOGD("BleScaleService: teardown — disconnecting");
      _client->disconnect();
    }
    NimBLEDevice::deleteClient(_client);
    _client = nullptr;
  }
  _writeChar = nullptr;
  // The scan-match flag is bound to the connection attempt we just ended.
  // Leaving it set would make the next SCANNING/RECONNECTING tick skip
  // scanning and dial the stale _targetAddrBytes — fine on a static-MAC
  // peer but a permanent stall if the peer rotated its address.
  _foundDevice = false;
}

// ---- Notify + event dispatch ------------------------------------------------

void BleScaleService::onNotify(uint32_t generation, uint8_t* data, size_t len) {
  const uint32_t accepted = _rxAcceptedGeneration;
  if (generation == 0 || generation != accepted) {
    LOGV("BleScaleService: stale RX notify ignored (gen=%u accepted=%u)",
         static_cast<unsigned>(generation), static_cast<unsigned>(accepted));
    return;
  }
  if (_rxHostGeneration != generation) {
    _rxReceiver.reset();
    _rxHostGeneration = generation;
  }
  char hex[40];
  LOGV("BleScaleService: RX %u bytes: %s", static_cast<unsigned>(len),
       hexHead(hex, sizeof(hex), data, len, 8));
  _rxReceiver.pushNotify(data, len, s_onFrame, this);
}

void BleScaleService::s_onFrame(const uint8_t* frame, size_t frameLen,
                                AcaiaV2::InboundKind kind, size_t emittedEvents,
                                void* user) {
  static_cast<BleScaleService*>(user)->onFrame(frame, frameLen, kind,
                                               emittedEvents);
}

void BleScaleService::onFrame(const uint8_t* frame, size_t frameLen,
                              AcaiaV2::InboundKind kind, size_t emittedEvents) {
  if (emittedEvents == 0 && kind == AcaiaV2::InboundKind::Rejected) {
    LOGV("BleScaleService: RX frame rejected by decoder (len=%u)",
         static_cast<unsigned>(frameLen));
  }
  if (_logArmed) {
    recordMsg(MSG_DIR_RX, mapInbound(kind), frame, frameLen);
  }
}

void BleScaleService::s_onEvent(const AcaiaV2::Event& ev, void* user) {
  static_cast<BleScaleService*>(user)->onEvent(ev);
}

void BleScaleService::onEvent(const AcaiaV2::Event& ev) {
  if (!_mutex) return;
  const uint32_t arrivalMs = millis();
  xSemaphoreTake(_mutex, portMAX_DELAY);
  switch (ev.type) {
    case AcaiaV2::EventType::WEIGHT: {
      uint32_t t = ensureMonotonicTimestamp(arrivalMs, _weight.timestampMs);
      const uint32_t sequence =
          _rxWeightCount.fetch_add(1, std::memory_order_relaxed) + 1;
      _weight.grams = ev.weight.grams;
      _weight.stable = ev.weight.stable;
      _weight.scaleTimerMs = ev.weight.hasScaleTimer ? ev.weight.scaleTimerMs
                                                     : scale_time::UNKNOWN_MS;
      _weight.sequence = sequence;
      _weight.timestampMs = t;
      LOGV("BleScaleService: WEIGHT %.2f g stable=%d scaleTimer=%u t=%u",
           ev.weight.grams, ev.weight.stable,
           static_cast<unsigned>(_weight.scaleTimerMs),
           static_cast<unsigned>(t));
      break;
    }
    case AcaiaV2::EventType::BATTERY: {
      uint32_t t = ensureMonotonicTimestamp(millis(), _battery.timestampMs);
      _battery.percent = ev.battery.percent;
      _battery.timestampMs = t;
      std::memcpy(_batteryAddrBytes, _targetAddrBytes,
                  sizeof(_batteryAddrBytes));
      M5_LOGI("BleScaleService: BATTERY %u%%", ev.battery.percent);
      break;
    }
    case AcaiaV2::EventType::TIMER:
      LOGV("BleScaleService: TIMER %u ms", ev.timer.ms);
      break;
    case AcaiaV2::EventType::KEY:
      LOGV("BleScaleService: KEY 0x%02X", ev.key.code);
      break;
    default:
      LOGV("BleScaleService: event type %u (unhandled)",
           static_cast<unsigned>(ev.type));
      break;
  }
  xSemaphoreGive(_mutex);
}
