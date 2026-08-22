// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <WebServer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <atomic>
#include <cstdint>
#include <memory>

#include "Extraction.h"
#include "ExtractionRecorder.h"
#include "ExtractionStatusSnapshot.h"
#include "RobustFlow.h"
#include "ShotCounter.h"
#include "TargetAlert.h"
#include "apps/extraction/history/ShotStore.h"
#include "vibration/PumpSignalObservation.h"

class HttpServer;

namespace pump_scale {

// ExtractionController: headless coordination engine behind ExtractionScreen.
//
// Owns the shot-recording side of the feature with no UI dependency: the
// ExtractionRecorder, the ShotStore, the cross-task mutex, the
// graduation/persistence of finished shots, and the HTTP/SSE read paths.
//
// It also plays a stored shot back for UI testing: loading one from flash into
// a device-local slot, then advancing the recorder from replayed inputs via
// replayTick() without graduating, persisting, or disturbing live recording.
//
// Responsibility split:
//   * ExtractionScreen (the on-device view) gathers inputs (vibration trigger,
//     BLE scale), calls tick() once per frame, reacts to the returned
//     TickOutcome to drive the display and the target-weight alert, and
//     forwards button commands here.
//   * ExtractionController (this) advances the recorder under the mutex,
//     decides which finished records graduate to history (isLikelyRealShot),
//     persists them to flash, and serves the HTTP/SSE readers.
//
// =============================================================================
//   HTTP paths / SSE support
// =============================================================================
//
//   /state            snapshotStatus(): _active + the bleScale snapshot first,
//                     _lastSavedSeq and storage state lock-free, then recorder
//                     fields under the mutex.
//   /current, /last   _writeCompact(): copy the chosen Extraction under the
//                     mutex, then encode + stream after releasing it. /last
//                     normalizes supported historical records to the current
//                     display schema.
//   /shots            ShotStore (owns its own mutex).
//   POST /loaded      requestLoadShot(): queues a stored shot's id; tick()
//                     decodes it (decodeCompact) into a device-local slot for
//                     display and replay without changing shot history.
//   SSE STATE         snapshotStatus() — small locked copy.
//   SSE records       snapshotExtraction() — one full Extraction copy under the
//                     mutex.
//   liveSeq()         wake-up counter the SSE task polls to decide when to
//                     re-copy snapshots.
//
// =============================================================================
//   Synchronization invariants
// =============================================================================
//
//   * The recorder is written only from the main UI loop — through tick(),
//     contextAction(), and the session calls — and read by the HTTP/SSE tasks
//     only while holding _mutex.
//   * The main task may read recorder state lock-free (current(),
//     effectiveLastShot(), currentYieldCg()) because it is the only writer;
//     the view's draw path relies on this.
//   * _active is release-written after the view's sensors are up and before
//     teardown; readers acquire-load it before trusting live scale data.
//   * _liveSeq is a wake-up counter, not a data container. Readers must
//     snapshot real state after observing it change.
//   * An accepted-shot sequence change occurs under _mutex and is followed by
//     a _liveSeq release after the lock is released. SSE can therefore defer a
//     record copied across that change and rely on a later wake-up.
//   * _lastSavedSeq advances only after ShotStore::save() succeeds; the
//     release/acquire pair lets clients treat savedSeq as "history is safe to
//     refresh now."
//   * _lastAttemptedSeq advances before each save attempt so one accepted shot
//     cannot cause repeated filesystem or NVS writes.
//   * No disk write, HTTP streaming, or BLE command runs while _mutex is held.
class ExtractionController {
 public:
  explicit ExtractionController(ShotCounter& shotCounter);
  ~ExtractionController();

  // ---- Session lifecycle (main task) --------------------------------------
  // Start a foreground session: clear in-flight recorder state, mark active,
  // and wake any SSE reader.
  void beginSession();
  // Mark inactive (release) so HTTP/SSE readers stop trusting live data. Call
  // before tearing the BLE link down.
  void deactivate();
  // Clear in-flight recorder state and wake SSE readers. Call after sensors are
  // down. _lastFinished / accepted-shot history survive (recorder contract).
  void resetInFlightAndNotify();

  // ---- Per-tick advance (main task) ---------------------------------------
  // What one tick produced, for the view to react to without touching the
  // recorder again.
  struct TickOutcome {
    Phase prevPhase;
    Phase phase;
    bool frameNeeded;  // a web/display refresh is warranted this tick
    bool active;       // recorder is RUNNING or POST_PUMP
    // The recorder has crossed from candidate pump activity into meaningful
    // pour evidence. UI may use this to focus Live without treating every
    // vibration window as a real shot.
    bool meaningfulPourStarted = false;

    // The alert decision is made inside the engine:
    // alertState carries the target-alert level and flow/projection diagnostics
    // the UI needs for beeps, flash, and the target pill.
    TargetAlert::State alertState;

    // True when a pending replay shot was decoded this tick; included in
    // frameNeeded so LastShot can refresh without continuous polling.
    bool loadedShotChanged = false;

    // Whether the latest accepted shot has a verified history record.
    bool latestAcceptedShotSaved = true;
  };
  // Advance the recorder one tick (locked), graduate + persist any finished
  // shot, and wake SSE readers when web-visible state changed. `newScaleSample`
  // and `scaleStateChanged` are observed by the view from the BLE snapshot.
  // `pumpSignal` carries the pump state and accepted decay onset determined by
  // vibration analysis.
  TickOutcome tick(uint32_t nowMs, const PumpSignalObservation& pumpSignal,
                   const ScaleSnapshot& snap, uint32_t utcSec,
                   bool newScaleSample, bool scaleStateChanged);

  // Advance the recorder one tick from REPLAYED inputs, reusing the live
  // recorder (no separate sandbox — that cost too much SRAM). It deliberately
  // does none of the live tick's history side effects: no graduation and no
  // persistence. It DOES wake SSE readers so a connected web client mirrors the
  // on-device replay gauge/timer in step with the device. The recorder's
  // in-flight state is the only thing it moves; the caller wipes it with
  // resetInFlightAndNotify() when replay ends. Returns the same TickOutcome the
  // view needs to drive the gauge and target alert. Bracket a replay session
  // with resetInFlightAndNotify() before and after.
  TickOutcome replayTick(uint32_t nowMs, bool pumpOn,
                         const ScaleSnapshot& snap);

  // ---- Button-driven recorder commands (main task) ------------------------
  enum class ContextAction { None, RequestTare, Cleared };
  // Apply the contextual A-hold action for the current phase: IDLE asks for a
  // tare and DONE clears. Returns what the view should do.
  ContextAction contextAction();

  // ---- Main-task lock-free reads for the view -----------------------------
  const Extraction& current() const { return _recorder.current(); }
  bool currentYieldCg(const ScaleSnapshot& scale, int16_t& out) const {
    return _recorder.currentYieldCg(scale, out);
  }
  bool currentInPumpWindow() const;
  bool currentFinishedShotIsDisplayable() const;
  // True when the predicted cutoff point is active right now. Derived on
  // demand from the alert state and current phase, so live and replay can
  // never drift from the engine's own state.
  bool currentCutState() const;
  // True while the shot is actually pouring: inside the pump window and real
  // coffee has flowed this shot. Main-task read, lock-free like current(); also
  // safe from SSE/HTTP while holding _mutex.
  bool currentIsPouring() const {
    return currentInPumpWindow() && _recorder.hasMeaningfulYield();
  }

  // Set the target-alert coefficients the engine should use for live shots and
  // for replaying shots that have no recorded target snapshot. ExtractionApp
  // owns the shared TargetStore; ExtractionScreen pushes its current
  // coefficients down on load and whenever the user edits them.
  //
  // Coeffs set here take effect at the start of the next shot. The per-shot
  // snapshot is fixed at the first RUNNING tick, so mid-shot edits
  // intentionally do not perturb the running predictor or the recorded
  // snapshot.
  void setTargetCoeffs(const pump_scale::TargetCoeffs& c);

  // ---- Display-shot selection ----------------------------------------------
  // Effective device Last Shot: loaded replay shot if present, otherwise the
  // last accepted live shot. The main task reads it lock-free (it is the sole
  // writer); SSE/HTTP readers must hold _mutex. Do not cache across tick().
  struct LastShotSelection {
    const Extraction* shot = nullptr;
    const FlowSeriesStats* flowStats = nullptr;
  };
  LastShotSelection effectiveLastShotSelection() const;
  const Extraction* effectiveLastShot() const;
  bool hasEffectiveLastShot() const { return effectiveLastShot() != nullptr; }
  void requestRestoreLastShotIfEmpty();

  // ---- Cross-task reads (SSE/HTTP) ----------------------------------------
  uint32_t liveSeq() const { return _liveSeq.load(std::memory_order_acquire); }
  void snapshotStatus(ExtractionStatusSnapshot& out) const;
  // wantDisplayShot copies the display shot (loaded replay shot if present,
  // else the last accepted pull); false copies the in-flight record. Returns
  // the accepted-shot sequence captured under the same lock.
  uint32_t snapshotExtraction(Extraction& out, bool wantDisplayShot) const;

  // Register the /state, /current, /last, /shots routes. Called once at setup;
  // the registration outlives session enter/exit.
  void registerWith(HttpServer& server);

 private:
  // Verdict for the most recent finalize edge.
  enum class FinalizeOutcome { None, Rejected, Accepted };

  // If a new shot has finalized since the last observation, log it and (if it
  // graduates) make a copy. Caller holds _mutex.
  FinalizeOutcome observeFinishedExtraction();
  // Attempt to persist a newly accepted shot once. Runs after tick releases
  // _mutex because LittleFS writes can block. Failures remain visible without
  // producing a main-loop retry storm.
  void persistAcceptedShot();

  // Consume a queued replay-shot load (main task): decode it from the store
  // into _loadedShot. Returns true when a shot was successfully loaded this
  // call, so tick() can signal the view.
  void requestLoadShot(uint32_t id) {
    _pendingLoadId.store(id, std::memory_order_release);
  }
  bool processPendingLoad();
  // Drop the loaded shot when a real pull supersedes it, freeing its heap
  // buffer. Main task only, and the caller must hold _mutex: the SSE and HTTP
  // tasks copy `*_loadedShot` while holding that mutex (it is the display
  // shot they serve), so freeing the buffer outside the lock could destroy it
  // mid-copy.
  void clearLoadedShot() {
    _hasLoadedShot = false;
    _loadedShot.reset();
    _loadedFlowStats = FlowSeriesStats{};
    ++_displayShotSeq;
  }

  void _handleState(WebServer& server);
  void _handleLast(WebServer& server);
  void _handleCurrent(WebServer& server);
  void _handleSetLoaded(WebServer& server);
  // Snapshot `src` under the mutex, then encode + stream after releasing.
  void _writeCompact(WebServer& server, const Extraction& src);
  void _sendSnapshot(WebServer& server);

  // Run the target-alert predictor and, the first time STOP_NOW fires in a
  // shot, record an ALARM_TRIGGERED event. Caller holds _mutex.
  void _runTargetAlert(uint32_t nowMs, bool phaseRunning,
                       const TrustedYieldSample& sample, uint32_t curBeginMs);

  ExtractionRecorder _recorder;
  ShotCounter& _shotCounter;
  ShotStore _shotStore;
  // Guards _recorder against concurrent access between the main loop (writer)
  // and the HTTP server task (readers).
  mutable SemaphoreHandle_t _mutex;
  // Scratch buffer for /current and /last: copy under the lock, release, then
  // encode + stream. The sync WebServer runs one handler at a time, so a single
  // buffer is safe to share between routes.
  Extraction _snapshot;

  // Last finishedSeq() observed, to detect new finalized shots each tick.
  uint32_t _lastObservedFinishedSeq = 0;

  // The last finalized record that passed isLikelyRealShot():
  // ExtractionScreen's notion of "the last shot," as distinct from the
  // recorder's lastFinished() ("the last finalize"). They diverge e.g. when a
  // flush/refill finalizes after a real shot: the recorder's lastFinished()
  // gets clobbered, but this copy keeps the real shot for the Last Shot screen
  // and the disk save. Written only at the finalize edge under _mutex; read by
  // the main task lock-free and by HTTP/SSE under _mutex.
  Extraction _lastAcceptedShot;
  // Derived from _lastAcceptedShot at the same finalize edge; keep it under the
  // same synchronization contract as the shot it summarizes.
  FlowSeriesStats _lastAcceptedFlowStats;
  uint32_t _lastAcceptedSeq = 0;
  // `_lastAttemptedSeq` prevents repeated writes for one accepted shot.
  // `_lastSavedSeq` advances only after a verified save and is atomic because
  // `/state` reads it from the HTTP task.
  uint32_t _lastAttemptedSeq = 0;
  std::atomic<uint32_t> _lastSavedSeq{0};

  // Target-weight alert predictor. Owned by the engine because it consumes
  // the recorder's trusted yield and produces a timeline event
  // (ALARM_TRIGGERED) the first time STOP_NOW fires.
  TargetAlert _targetAlert;
  TargetCoeffs _targetCoeffs;
  // beginMs of the shot the alert is currently tracking. The predictor +
  // STOP_NOW state reset only when this changes (a genuinely new extraction),
  // NOT on every RUNNING edge — a POST_PUMP→RUNNING coalesce keeps the same
  // beginMs and must not clear STOP_NOW mid-shot. 0 = not tracking a shot.
  uint32_t _alertShotBeginMs = 0;

  // Replay-shot load: _pendingLoadId is set by the HTTP task and consumed on
  // the main task in tick(). 0 = nothing pending. _loadedShot is heap-
  // allocated only while a shot is loaded, because an Extraction is ~10 KB.
  // Mutated only by the main task; _hasLoadedShot changes require holding
  // _mutex, because HTTP/SSE readers snapshot it for display. Main-task reads
  // (effectiveLastShotSelection) stay lock-free.
  std::atomic<uint32_t> _pendingLoadId{0};
  std::unique_ptr<Extraction> _loadedShot;
  // Derived from _loadedShot on the main task and cleared with it; main-task
  // only (HTTP/SSE readers don't serve device-generated flow stats).
  FlowSeriesStats _loadedFlowStats;
  bool _hasLoadedShot = false;
  // Display-shot change counter: advances under _mutex on every selection
  // change (graduation, load, clear), so the SSE stream knows when to re-send
  // FINAL_RECORD. It signals change only: clearing it does not mean a display
  // shot doesn't exist, that's the role of the separate hasDisplayShot field.
  uint32_t _displayShotSeq = 0;

  // Basis for the live weight/yield/elapsed reported by /state and SSE: the
  // scale status and clock in the last recorder advance. A live tick stores
  // the real BLE frame and current millis(), while in a replay scenario
  // replayTick stores the recorded frame and virtual replay clock and sets
  // _replaying, so a connected web client mirrors exactly what the device
  // gauge/timer show in either mode. Written under _mutex by
  // tick()/replayTick(), read under _mutex by snapshotStatus().
  ScaleSnapshot _statusScale{};
  uint32_t _statusNowMs = 0;
  bool _replaying = false;
  // Included in HTTP and SSE status snapshots; all access uses the controller
  // mutex.
  PumpSignalState _statusPumpSignalState = PumpSignalState::Off;

  // True while the session is foreground. Read from the HTTP task; written from
  // the main task. Published after sensors are up, cleared before teardown.
  std::atomic<bool> _active{false};

  // Wake-up counter bumped whenever a web-visible aspect changes (phase, new
  // samples, scale state, finalization, active flip). SSE tasks poll it to
  // decide when to re-copy snapshots.
  std::atomic<uint32_t> _liveSeq{0};
};

}  // namespace pump_scale
