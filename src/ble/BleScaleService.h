// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <atomic>
#include <cstdint>
#include <string>

#include "AcaiaV2Codec.h"
#include "ble/BleStartupRetryPolicy.h"
#include "ble/ScanPolicy.h"
#include "ble/bluetooth_controller_state.h"
#include "ble/scale_time.h"

// Forward-declare NimBLE types so this header stays NimBLE-free for includers.
class NimBLEClient;
class NimBLERemoteCharacteristic;
class NimBLEAdvertisedDevice;

// BleScaleService
//
// Supports Acaia scales using either the current UART GATT profile (Lunar
// 2021/Pyxis/current firmware) or the legacy single-characteristic profile
// (earlier Lunar/Pearl firmware).
//
// One service per process. NimBLE is a process-singleton and all its callbacks
// fire on the single BLE host task, so all NimBLE work must be serialized onto
// one task.
//
// =============================================================================
//   Modes
// =============================================================================
//
// The radio can only do one thing at a time, so the manager runs in one of
// three mutually-exclusive modes, recomputed every tick from two independent
// "wants":
//
//   Idle     — nobody wants the radio.
//   Connect  — the application wants to connect to the scale and stream
//              weight/battery (the state machine below). Set via
//              enable()/disable().
//   DiagScan — the diagnostic wants a discovery scan of every advertiser
//              (no connect). Set via startDiagScan()/stopDiagScan().
//
// Any conflicts are resolved by selectMode(), with the rule that Connect
// always wins. A diag scan requested while the scale is in use doesn't run
// until the application releases it.
//
//   selectMode():
//     power suspended       -> Idle
//     connect wanted        -> Connect
//     diag lease still live -> DiagScan
//     otherwise             -> Idle
//
// =============================================================================
//   Service API
// =============================================================================
//
//   bleScale.setStartupRequired(true);
//   bleScale.updateStartup(millis());
//
//   // Application (Connect mode):
//   bleScale.enable();                // called from e.g. onEnter()
//   bleScale.disable();               // called from e.g. onExit()
//   bleScale.requestTare();           // called from e.g. onEvent() etc.
//   auto snap = bleScale.snapshot();  // called from e.g. tick() / handlers
//
//   // Diagnostic (DiagScan mode):
//   bleScale.startDiagScan(leaseMs);  // arm/renew a self-expiring scan lease
//   bleScale.stopDiagScan();          // drop the lease now
//   auto scan = bleScale.scanSnapshot();
//
// All entry points are safe to call from any task. Connection demand,
// diagnostic-scan demand, and power suspension are published atomically so
// they remain visible while the manager task is waiting for a stopped
// controller. Operations that must run on the manager task use its FreeRTOS
// command queue. snapshot()/scanSnapshot() copy state under a small mutex.
//
// =============================================================================
//   Connect-mode state diagram
// =============================================================================
//
// The Connect mode runs the scale link state machine. It is entered with
// _state == OFF and torn down to OFF when the mode is exited (disable, or a
// preempting want change). Within Connect:
//
//                          enter Connect
//                             |
//                             v
//                          +-----+
//                +-------->| OFF |
//                |         +--+--+
//                |            |
//                |            v
//                |      +----------+
//                |      | SCANNING |<------+
//                |      +----+-----+       | scan window
//                |           |             | elapsed, no match
//                |   advert  |             |
//                |   match   +-------------+
//                |           |
//                |           v
//                |      +--------------------+
//                |      |     CONNECTING     |
//                |      +---------+----------+
//                |                |
//                |  connect +     |  connect / handshake fail
//                |  handshake ok  +----------------+
//                |                v                |
//                |          +----------+           |
//                |          |  READY   |           v
//                |          +----+-----+   +--------------+
//                |               |         | RECONNECTING |
//                |     disconnect|         +------+-------+
//                |               v                ^
//                |       +--------------+         | advert match (same
//                +-------| RECONNECTING |---------+ scan machinery as
//                        +--------------+           SCANNING)
//
//   SCANNING and RECONNECTING share the same underlying scan+match machinery.
//   The split exists purely so UI can distinguish "first connection this
//   session" from "we had it and lost it." Exiting Connect mode (from any
//   inner state) disconnects, stops the scan, and resets to OFF.
//
// =============================================================================
//   Command queue
// =============================================================================
//
//   Operations that must run on the manager task are posted as commands and
//   drained every tick before selectMode():
//
//     Tare(epoch)     — send a tare iff still Connect/READY on `epoch`
//     LogArm          — start the message-log tap (resets the ring + counts)
//     LogDisarm       — stop the message-log tap
//     StopController  — tear down the active mode and stop Bluetooth
//     StartController — initialize Bluetooth after a requested stop
//
//   Tare carries a `connectionEpoch` captured at request time. The manager
//   increments the epoch on every entry into READY, so a request from a lost
//   connection cannot fire after reconnecting.
//
//   The diag-scan lease is self-expiring unless renewed each tick. This
//   releases DiagScan even if onExit does not run.
//
// =============================================================================
//   Snapshots
// =============================================================================
//
//   Connect-mode readers call snapshot(): a coherent {state, epoch, weight,
//   battery} copy under a brief mutex hold. This is the only path for reading
//   scale state. When the mode isn't Connect, state is OFF and the live
//   readings are cleared.
//
//   `connectionEpoch` starts at 0 and increments on every entry into READY.
//   A reader holding a snapshot can detect "the session that produced this
//   weight has ended" by comparing epochs against a later snapshot.
//
//   The scale message log is a passive diagnostic tap on Connect mode, not a
//   mode of its own: when armed (LogArm), onNotify (RX) and sendPacket (TX)
//   each append one classified record to a ring under _msgMutex; when disarmed
//   the capture lines are skipped, so a normal extraction is never logged.
//   messageLogSnapshot() returns {armed, writes, byTag counts, recent records}.
//   Like the BLE-scan diagnostic, the on-device UI drives it and the web view
//   is read-only.
//
//   DiagScan readers call scanSnapshot(): a copy of the discovered-device
//   table plus `active` (a scan is running) and `busy` (the application holds
//   the radio, so the scan is being suppressed). Guarded by a separate mutex
//   from the scale snapshot.
//
// =============================================================================
//   Synchronization invariants
// =============================================================================
//
//   * The manager task is the only writer of any service state, the only
//     caller of NimBLE APIs, and the only consumer of the command queue. All
//     mode transitions (and thus all NimBLE setup/teardown) happen there.
//   * BLE callbacks publish single-byte edges (_foundDevice, _disconnectFlag)
//     and the target peer address, append to the scan table (DiagScan), or
//     advance the host-owned RX receiver. They never call NimBLE APIs and never
//     touch the mode state machine directly.
//   * _rxReceiver is BLE-host-task owned. The manager publishes a scalar
//     accepted notification generation before subscribing; the notify callback
//     ignores stale generations and resets the receiver itself on the first
//     notification for a new generation. That keeps reset() and pushNotify()
//     on the same task.
//   * Both Connect-mode cross-task edges are bound to one connection attempt
//     and must be reset at the right boundary or they leak into the next:
//       _foundDevice is reset in teardownClient(), because the scan match
//       that produced the address belongs to the attempt being torn down.
//       Leaving it set would make the next SCANNING/RECONNECTING tick skip
//       scanning and dial a stale address.
//       _disconnectFlag is cleared immediately before tryConnect(). A
//       disconnect from that connection attempt then remains set until the
//       first READY tick consumes it, so the service cannot remain in READY
//       with a dead client.
//   * _mutex protects only the scale snapshot fields (state, epoch, weight,
//     battery, cached battery peer address). _scanMutex protects only the
//     diag scan table. Neither is ever held during NimBLE calls or packet
//     writes (recordDiagAdvert runs inside a scan callback but only writes the
//     table; it never calls back into NimBLE under the lock).
//   * Callback objects, the command queue, the manager task, and both mutexes
//     are created once in begin() and never freed. The scan table is a fixed
//     static array (no heap).
class BleScaleService {
 public:
  using ControllerStartedCallback = void (*)(uint32_t generation);
  // Connect-mode link state. Numeric values are part of the SSE wire format
  // (one byte in STATE packets; see ExtractionStream.h). Do not reorder or
  // remove without updating the web client's SCALE_STATE table.
  enum class State : uint8_t {
    OFF = 0,
    SCANNING = 1,
    CONNECTING = 2,
    READY = 3,
    RECONNECTING = 4,
  };

  // Which of the radio's mutually-exclusive activities is currently running.
  // Manager-owned; not part of any wire format.
  enum class Mode : uint8_t {
    Idle = 0,
    Connect = 1,
    DiagScan = 2,
  };

  // Bluetooth controller lifecycle. Stop and start requests are completed by
  // the manager task so NimBLE setup and teardown stay serialized with scans,
  // connections, and callbacks.
  using ControllerState = ble::ControllerState;

  struct WeightReading {
    float grams = 0.0f;
    bool stable = false;
    uint32_t scaleTimerMs = scale_time::UNKNOWN_MS;
    // Increments for every decoded weight event and remains monotonic across
    // reconnects, so consumers can detect readings replaced before a snapshot.
    uint32_t sequence = 0;
    // 0 == no reading yet this session. Otherwise host BLE-arrival millis().
    uint32_t timestampMs = 0;
  };

  struct BatteryReading {
    uint8_t percent = 0;
    uint32_t timestampMs = 0;  // 0 == no reading yet this session
  };

  // Coherent scale snapshot. `connectionEpoch` increments once per entry into
  // READY (so 0 means "never connected since begin()").
  struct Snapshot {
    State state = State::OFF;
    uint32_t connectionEpoch = 0;
    WeightReading weight;
    BatteryReading battery;
  };

  // Radio demand remains observable while power policy suspends the service
  // or stops the controller. Connect mode takes priority when both fields are
  // true, matching selectMode().
  struct DemandSnapshot {
    bool connectWanted = false;
    bool diagnosticScanWanted = false;
  };

  // One discovered advertiser in DiagScan mode. `minIntervalMs` is the
  // smallest inter-arrival gap observed for this device — a robust estimate of
  // its advertising interval (missed adverts only inflate gaps). It is only
  // meaningful once `samples` is large enough (a few). `samples` counts
  // *plausible* interval measurements (gaps at/above the BLE floor), not raw
  // callbacks — so sub-floor artifacts can't satisfy the threshold.
  // `lastSeenMs` is the working timestamp the estimate is computed from;
  // readers may ignore it.
  struct ScanEntry {
    uint8_t addr[6] = {};
    char name[20] = {};
    int8_t rssi = 0;
    uint32_t lastSeenMs = 0;
    uint32_t minIntervalMs = 0;
    uint16_t samples = 0;
    bool recognized = false;  // matchesScale(name)
  };

  // Max advertisers tracked at once. Fixed static storage (no heap); when full
  // the weakest-RSSI entry is evicted to make room.
  static constexpr uint8_t SCAN_TABLE_CAP = 24;

  // Coherent diag-scan snapshot. `active` is true while a scan is actually
  // running; `busy` is true when the application holds the radio (Connect) so
  // the scan is suppressed even though a lease is set.
  struct ScanResults {
    bool active = false;
    bool busy = false;
    uint8_t count = 0;
    ScanEntry entries[SCAN_TABLE_CAP];
  };

  // --- Scale message log (diagnostic; passive tap on Connect mode) ----------
  // Per-message tag spanning both directions. `MsgTagCount` is the array size
  // for `byTag` counts; keep it last. Maps from the codec's classifyInbound /
  // classifyOutbound. The label table lives in the web client, not on-device.
  enum class MsgTag : uint8_t {
    RxWeight,
    RxBattery,
    RxTimer,
    RxKey,
    RxUnknown,
    RxMixed,
    RxRejected,
    RxSettings,
    TxHeartbeat,
    TxTare,
    TxTimer,
    TxIdentify,
    TxSubscribe,
    TxOther,
    MsgTagCount,
  };
  static constexpr int MSG_TAG_COUNT = static_cast<int>(MsgTag::MsgTagCount);

  // Direction byte stored per record.
  static constexpr uint8_t MSG_DIR_RX = 0;
  static constexpr uint8_t MSG_DIR_TX = 1;

  // One captured packet. `wireLen` is the true on-air length; only the first
  // `rawLen = min(wireLen, sizeof raw)` bytes are kept, so a reader renders a
  // truncation marker when wireLen > rawLen.
  struct MsgRecord {
    uint32_t ms = 0;
    uint8_t dir = MSG_DIR_RX;
    uint8_t wireLen = 0;
    uint8_t rawLen = 0;
    MsgTag tag = MsgTag::RxRejected;
    uint8_t raw[20] = {};
  };

  // Max captured packets retained (newest-first in the snapshot). Fixed static
  // storage (no heap).
  static constexpr uint8_t MSG_LOG_CAP = 64;

  // Coherent message-log snapshot. `armed` is whether capture is on; `writes`
  // is a monotonic counter (bumps per record and on every arm/disarm) used as
  // the web ETag so an armed-state flip always invalidates a 304.
  struct MsgLogSnapshot {
    bool armed = false;
    uint32_t writes = 0;
    uint32_t byTag[MSG_TAG_COUNT] = {};
    uint8_t count = 0;
    MsgRecord records[MSG_LOG_CAP];
  };

  // Creates the manager resources and initializes NimBLE. Successful
  // prerequisite allocations survive a failed attempt so the caller may
  // retry. Returns true after a successful start or if the service is already
  // running, and false when initialization fails.
  bool begin();
  // Starts a required service despite transient initialization failures.
  // Setting the requirement makes the first attempt immediately;
  // updateStartup() retries failures at a bounded rate.
  void setStartupRequired(bool required);
  void updateStartup(uint32_t nowMs);
  // Reports each successful controller initialization. Install the callback
  // before begin(); it may then run on that caller or on the BLE manager task,
  // so it must not perform driver work directly.
  void setControllerStartedCallback(ControllerStartedCallback callback) {
    _controllerStartedCallback = callback;
  }
  // Reports whether begin() completed and the manager task can accept
  // controller lifecycle requests.
  bool isStarted() const { return _managerTask != nullptr; }

  // Application (Connect mode) intent. Both are non-blocking, idempotent, and
  // safe to call from any task. Intent is published immediately so it remains
  // visible while the controller is stopped; the manager applies it when it
  // can run the requested mode.
  void enable();
  void disable();

  // Reports radio demand independently of its current power suspension.
  DemandSnapshot demandSnapshot() const;

  // Temporarily prevents radio demand from selecting a service
  // mode. Demand is retained so clearing the suspension restores the mode the
  // application still wants. This does not start or stop the controller.
  void setPowerSuspended(bool suspended);

  // Applies connect-mode scan timing on the manager task. Changing the policy
  // stops any current connect scan so the next scan uses one coherent policy.
  void setScanPolicy(ble::ScanPolicy policy);

  // Reports whether either service mode currently has a BLE scan running.
  bool scanActive() const {
    return _scanActive.load(std::memory_order_relaxed);
  }

  // Requests a complete Bluetooth controller stop or restart. Both operations
  // are asynchronous because the manager task owns every NimBLE lifecycle
  // call. A request returns false if the service is unavailable, the command
  // queue is full, or the controller is not in the required source state.
  // Failed is terminal because the controller state is unknown; reboot before
  // attempting another Bluetooth operation.
  bool requestControllerStop();
  bool requestControllerStart();
  ControllerState controllerState() const {
    return _controllerState.load(std::memory_order_acquire);
  }

  // Post a tare. Returns false only if the service has not been begin()-ed
  // or the command queue is full (extremely rare). Tare is applied iff the
  // service is still in Connect/READY on the same `connectionEpoch` when the
  // manager drains the command, so tares issued just before a drop are
  // dropped rather than fired on the next connection.
  bool requestTare();

  // Atomic read of {state, epoch, weight, battery}. Brief mutex hold.
  Snapshot snapshot() const;

  // Diagnostic (DiagScan mode) intent. Arm or renew a self-expiring scan lease
  // (`leaseMs` from now); callers renew periodically to keep it alive. The
  // scan only actually runs while the application is not using the radio.
  // Non-blocking, safe from any task.
  void startDiagScan(uint32_t leaseMs);
  void stopDiagScan();

  // Atomic read of the discovered-device table + scan status. Brief mutex hold.
  ScanResults scanSnapshot() const;

  // The shared recognition predicate: true for a device name the application
  // (Connect mode) would attempt to talk to. Used both by the Connect-mode
  // scan match and the DiagScan `recognized` flag — the one seam the
  // diagnostic needs into the application's notion of "our device."
  static bool matchesScale(const std::string& name);

  // Scale message log (diagnostic). Arm/disarm the passive capture tap; both
  // are non-blocking, safe from any task, and post a single command. Arming
  // resets the ring + counts. Capture only records while armed *and* connected,
  // so a normal extraction (which never arms) is never logged.
  // messageLogSnapshot() is scanSnapshot's sibling: an atomic copy of {armed,
  // writes, counts, recent records}.
  void armMessageLog();
  void disarmMessageLog();
  MsgLogSnapshot messageLogSnapshot() const;

 private:
  enum class CmdType : uint8_t {
    Tare,
    LogArm,
    LogDisarm,
    SetScanPolicy,
    StopController,
    StartController,
  };
  struct Cmd {
    CmdType type;
    // Connection epoch for Tare.
    uint32_t arg = 0;
    ble::ScanPolicy scanPolicy = ble::kDefaultScanPolicy;
  };

  // --- Snapshot-protected (held by _mutex) ----------------------------------
  mutable SemaphoreHandle_t _mutex = nullptr;
  State _state = State::OFF;
  uint32_t _connectionEpoch = 0;
  WeightReading _weight;
  BatteryReading _battery;
  uint8_t _batteryAddrBytes[6] = {};  // peer the cached _battery is from

  // --- Scan-table-protected (held by _scanMutex) ---------------------------
  mutable SemaphoreHandle_t _scanMutex = nullptr;
  ScanEntry _scanTable[SCAN_TABLE_CAP];
  uint8_t _scanCount = 0;

  // --- Message-log-protected (held by _msgMutex) ---------------------------
  // The diagnostic message ring. Written by both tasks (RX on the host task via
  // onNotify, TX on the manager task via sendPacket) and by the manager on
  // arm/disarm, so it has its own mutex (never nested with _mutex/_scanMutex).
  // `_msgNext` is the next write slot in the circular buffer; `_msgCount` caps
  // at MSG_LOG_CAP. `_msgWrites` is monotonic (per record + each arm/disarm).
  mutable SemaphoreHandle_t _msgMutex = nullptr;
  MsgRecord _msgLog[MSG_LOG_CAP];
  uint8_t _msgNext = 0;
  uint8_t _msgCount = 0;
  uint32_t _msgByTag[MSG_TAG_COUNT] = {};
  uint32_t _msgWrites = 0;

  // --- Manager-owned (no cross-task access) --------------------------------
  Mode _mode = Mode::Idle;
  bool _hadReadyThisEnable = false;  // chooses SCANNING vs RECONNECTING label
  uint32_t _connectScanNextStartMs = 0;
  ble::ScanPolicy _scanPolicy = ble::kDefaultScanPolicy;
  uint32_t _lastHeartbeatMs = 0;
  uint32_t _rxGenerationIssued = 0;
  // A scale connection is usable only after it produces weight data.
  // Auxiliary events can arrive while measurements remain silent.
  uint32_t _armMs = 0;
  uint32_t _rxWeightCountAtArm = 0;
  uint8_t _resubscribeAttempts = 0;

  // Capture condition for the message log. The manager writes it on arm and
  // disarm; the BLE host and manager tasks read it while recording packets.
  std::atomic<bool> _logArmed{false};

  // --- Cross-task edges -----------------------------------------------------
  // Radio demand and power suspension are independent. Callers publish
  // demand directly so PowerManager can observe it even while the manager task
  // is parked with the Bluetooth controller stopped.
  std::atomic<bool> _connectWanted{false};
  std::atomic<uint32_t> _diagScanDeadlineMs{0};
  std::atomic<bool> _powerSuspended{false};
  std::atomic<bool> _foundDevice{false};
  std::atomic<bool> _disconnectFlag{false};
  std::atomic<bool> _scanActive{false};
  std::atomic<ControllerState> _controllerState{ControllerState::Stopped};
  std::atomic<bool> _controllerRequestPending{false};
  // Manager -> BLE host scalar. Notifications whose captured generation does
  // not match are stale callbacks from a prior subscription and are ignored.
  std::atomic<uint32_t> _rxAcceptedGeneration{0};
  std::atomic<uint32_t> _controllerGeneration{0};
  ControllerStartedCallback _controllerStartedCallback = nullptr;
  // Selects what the scan callback does: match-for-scale (false) or
  // record-every-advert into the table (true). Single writer (manager, at
  // mode entry/exit), single reader (scan callback).
  std::atomic<bool> _diagMode{false};
  // Target address bytes are written by the scan callback strictly before
  // `_foundDevice` becomes true. The atomic publication makes those bytes
  // visible before the manager reads them.
  uint8_t _targetAddrBytes[6] = {};
  uint8_t _targetAddrType = 0;

  // --- BLE-host-owned ------------------------------------------------------
  AcaiaV2::Receiver _rxReceiver;
  uint32_t _rxHostGeneration = 0;
  // The BLE host task increments this count for each decoded weight event. The
  // manager reads it to decide whether a subscription produced useful data.
  std::atomic<uint32_t> _rxWeightCount{0};

  // --- Lifetime-permanent resources (created in begin(), never freed) ------
  TaskHandle_t _managerTask = nullptr;
  QueueHandle_t _cmdQueue = nullptr;
  ble::BleStartupRetryPolicy _startupRetry;
  bool _startupRequired = false;

  NimBLEClient* _client = nullptr;
  NimBLERemoteCharacteristic* _writeChar = nullptr;

  class ScanCallbacks;
  class ClientCallbacks;
  ScanCallbacks* _scanCallbacks = nullptr;
  ClientCallbacks* _clientCallbacks = nullptr;

  // --- Manager internals ---------------------------------------------------
  static void s_managerTask(void* arg);
  bool initializeController();
  void managerLoop();
  void drainCommands();
  void expireDiagScanLease(uint32_t nowMs);
  bool processControllerTransition();

  // Mode multiplexer: pick the desired mode, transition if it changed, then
  // run the current mode's per-tick work.
  Mode selectMode();  // mutates: clears self-expired diag-scan leases
  void enterMode(Mode m);
  void exitMode(Mode m);
  void runMode(Mode m);

  // Connect-mode state machine (the diagram above). Only called from
  // runMode(Connect); transitions only between Connect's inner states.
  void runStateMachine();
  void transition(State next);

  bool startScan();  // Connect-mode scan (match for our scale)
  void stopScanIfActive();
  bool tryConnect();
  bool doHandshake();
  bool sendPacket(const uint8_t* data, size_t len, bool ack = false);
  void teardownClient();
  void publishBatteryOnReadyEntry();

  // DiagScan-mode internals.
  bool startDiagScanRadio();  // one finite, duplicate-keeping discovery slice
  void recordDiagAdvert(
      const NimBLEAdvertisedDevice* d);  // upsert into _scanTable
  void clearScanTable();

  // Message-log internals. recordMsg appends one captured packet, called from
  // onNotify/sendPacket only when _logArmed. (Arm/disarm reset the ring + bump
  // _msgWrites inline in drainCommands under _msgMutex.)
  void recordMsg(uint8_t dir, MsgTag tag, const uint8_t* data, size_t len);

  // --- NimBLE callback plumbing (run on host task) -------------------------
  static void s_onEvent(const AcaiaV2::Event& ev, void* user);
  static void s_onFrame(const uint8_t* frame, size_t frameLen,
                        AcaiaV2::InboundKind kind, size_t emittedEvents,
                        void* user);
  void onFrame(const uint8_t* frame, size_t frameLen, AcaiaV2::InboundKind kind,
               size_t emittedEvents);
  void onEvent(const AcaiaV2::Event& ev);
  void onNotify(uint32_t generation, uint8_t* data, size_t len);
};

extern BleScaleService bleScale;
