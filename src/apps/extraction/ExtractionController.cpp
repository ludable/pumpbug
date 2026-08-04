// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#include "ExtractionController.h"

#include <esp_log.h>

#include "ExtractionDiagnosis.h"
#include "ExtractionWire.h"
#include "ShotCounter.h"
#include "apps/extraction/history/ShotHistoryRoutes.h"
#include "ble/BleScaleService.h"
#include "diagnostics/RuntimeEventLog.h"
#include "net/HttpServer.h"
#include "net/JsonStream.h"
#include "scale_snapshot_util.h"
#include "util/scoped_lock.h"
#include "util/storage.h"

namespace pump_scale {

namespace {
using Lock = ScopedLock;

const char* phaseName(Phase p) {
  switch (p) {
    case Phase::IDLE:
      return "IDLE";
    case Phase::RUNNING:
      return "RUNNING";
    case Phase::POST_PUMP:
      return "POST_PUMP";
    case Phase::DONE:
      return "DONE";
  }
  return "IDLE";
}

const char* scaleName(BleScaleService::State s) {
  switch (s) {
    case BleScaleService::State::OFF:
      return "off";
    case BleScaleService::State::SCANNING:
      return "scanning";
    case BleScaleService::State::CONNECTING:
      return "connecting";
    case BleScaleService::State::READY:
      return "connected";
    case BleScaleService::State::RECONNECTING:
      return "reconnecting";
  }
  return "off";
}
}  // namespace

ExtractionController::ExtractionController()
    : _mutex(xSemaphoreCreateMutex()) {}

ExtractionController::~ExtractionController() {
  if (_mutex) {
    vSemaphoreDelete(_mutex);
    _mutex = nullptr;
  }
}

void ExtractionController::beginSession() {
  {
    Lock l(_mutex);
    _recorder.clear();
    // Declare "caught up to the recorder's current history" explicitly. Today
    // finishedSeq() == 0 on a fresh recorder, but priming this way decouples us
    // from that detail so a future reset/preload won't spuriously log a
    // "finished" line on the first tick.
    _lastObservedFinishedSeq = _recorder.finishedSeq();
  }
  // Publish active=true after the clear so an HTTP request landing mid-enter
  // doesn't see active=true over stale in-flight state. (The view enables the
  // sensors before calling this.)
  _active.store(true, std::memory_order_release);
  _liveSeq.fetch_add(1, std::memory_order_release);
}

void ExtractionController::deactivate() {
  _active.store(false, std::memory_order_release);
}

void ExtractionController::resetInFlightAndNotify() {
  // Reset in-flight recorder state so /state and /current stop reporting a
  // stale RUNNING/POST_PUMP with an ever-growing elapsedMs. _lastFinished and
  // _finishedSeq survive clear() (recorder contract), so /last is preserved.
  {
    Lock l(_mutex);
    _recorder.clear();
    _lastObservedFinishedSeq = _recorder.finishedSeq();
    _targetAlert.reset();
    _alertShotBeginMs = 0;
  }
  _liveSeq.fetch_add(1, std::memory_order_release);
}

ExtractionController::TickOutcome ExtractionController::tick(
    uint32_t nowMs, bool pumpOn, const ScaleSnapshot& snap, uint32_t utcSec,
    bool newScaleSample, bool scaleStateChanged) {
  // Service a queued replay-shot load before the recorder work.
  const bool loadedShotChanged = processPendingLoad();

  TickOutcome out{};
  uint16_t prevSampleCount, curSampleCount;
  uint32_t prevFinishedSeq, curFinishedSeq;
  FinalizeOutcome finalizeOutcome = FinalizeOutcome::None;
  bool hadMeaningfulYield = false;
  bool hasMeaningfulYield = false;
  {
    Lock l(_mutex);
    const Extraction& curBefore = _recorder.current();
    out.prevPhase = curBefore.phase;
    prevSampleCount = curBefore.sampleCount;
    prevFinishedSeq = _recorder.finishedSeq();
    hadMeaningfulYield = _recorder.hasMeaningfulYield();
    // utcSec is 0 until the clock is set; the recorder consumes it only on
    // IDLE→RUNNING and otherwise ignores it.
    _recorder.update(nowMs, pumpOn, snap, utcSec);
    finalizeOutcome = observeFinishedExtraction();
    // A new finalize that didn't graduate (flush, grinder dose, spurious pump
    // window) should not leave the recorder stuck in DONE: the on-device UI
    // and web clients both treat it as a return to Ready. Clear back to IDLE
    // unless the recorder already moved on (e.g. auto-restart into a new
    // RUNNING shot in the same tick).
    if (finalizeOutcome == FinalizeOutcome::Rejected &&
        _recorder.current().phase == Phase::DONE) {
      _recorder.clear();
    } else if (finalizeOutcome == FinalizeOutcome::Accepted) {
      // A stored shot loaded for replay remains the user's chosen display shot
      // while spurious pump windows are rejected. Once a real shot graduates,
      // it becomes the genuine last shot and supersedes the loaded replay slot.
      // This call is also what advances _displayShotSeq for the graduated shot
      // (pushing the FINAL_RECORD to web clients) — keep it unconditional.
      clearLoadedShot();
    }
    const Extraction& curAfter = _recorder.current();
    out.phase = curAfter.phase;
    curSampleCount = curAfter.sampleCount;
    curFinishedSeq = _recorder.finishedSeq();
    const uint32_t curBeginMs = curAfter.beginMs;
    hasMeaningfulYield = _recorder.hasMeaningfulYield();

    // Target-alert predictor: runs inside the engine on the same clock as the
    // recorder so its STOP_NOW edge is recorded at-source.
    TrustedYieldSample ts;
    _recorder.lastTrustedYieldSample(ts);
    _runTargetAlert(nowMs, out.phase == Phase::RUNNING, ts, curBeginMs);
    out.alertState = _targetAlert.state();

    // Record the frame/clock this advance ran on, so /state and SSE report the
    // same live weight/yield/elapsed the device gauge shows.
    _statusScale = snap;
    _statusNowMs = nowMs;
    _replaying = false;
  }

  // Does anything want a fresh frame this tick? Display and SSE answer the same
  // question — phase/sample/finalize edges, BLE scale flap, a new weight, or
  // just "we're mid-shot and the timer/curve should keep animating."
  out.active = out.phase == Phase::RUNNING || out.phase == Phase::POST_PUMP;
  out.meaningfulPourStarted = !hadMeaningfulYield && hasMeaningfulYield;
  out.frameNeeded = out.active || out.phase != out.prevPhase ||
                    curSampleCount != prevSampleCount ||
                    curFinishedSeq != prevFinishedSeq || newScaleSample ||
                    scaleStateChanged || out.alertState.stopNowEdge ||
                    loadedShotChanged;
  out.loadedShotChanged = loadedShotChanged;
  if (out.frameNeeded) _liveSeq.fetch_add(1, std::memory_order_release);

  // The counter follows shot graduation, not filesystem retention. Keep its
  // NVS write outside _mutex for the same reason as shot persistence below.
  if (finalizeOutcome == FinalizeOutcome::Accepted) shot_counter::increment();
  persistAcceptedShot();
  out.latestAcceptedShotSaved =
      _lastAcceptedSeq == _lastSavedSeq.load(std::memory_order_acquire);
  return out;
}

bool ExtractionController::currentInPumpWindow() const {
  const Phase p = _recorder.current().phase;
  return p == Phase::RUNNING || p == Phase::POST_PUMP;
}

bool ExtractionController::currentCutState() const {
  const Phase p = _recorder.current().phase;
  return _targetAlert.state().stopNowFired &&
         (p == Phase::RUNNING || p == Phase::POST_PUMP);
}

bool ExtractionController::currentFinishedShotIsDisplayable() const {
  const Extraction& cur = _recorder.current();
  return cur.phase == Phase::DONE && isLikelyRealShot(cur);
}

ExtractionController::ContextAction ExtractionController::contextAction() {
  bool tareNeeded = false;
  bool cleared = false;
  {
    Lock l(_mutex);
    switch (_recorder.current().phase) {
      case Phase::IDLE:
        tareNeeded = true;
        break;
      case Phase::RUNNING:
      case Phase::POST_PUMP:
        break;
      case Phase::DONE:
        _recorder.clear();
        cleared = true;
        break;
    }
  }
  if (cleared) {
    // The clear changes DONE to IDLE between tick calls, so the next tick sees
    // IDLE both before and after its update. Wake SSE readers here.
    _liveSeq.fetch_add(1, std::memory_order_release);
    return ContextAction::Cleared;
  }
  return tareNeeded ? ContextAction::RequestTare : ContextAction::None;
}

ExtractionController::FinalizeOutcome
ExtractionController::observeFinishedExtraction() {
  // The finalize edge: the single chokepoint where a record stops being
  // in-flight and becomes a history candidate. The recorder bumps finishedSeq()
  // for EVERY pump cycle (flush, refill, real shot); here we apply the verdict
  // and graduate only the real shots. Caller holds _mutex, so the accept-copy
  // below is safe against the HTTP/SSE readers that snapshot _lastAcceptedShot
  // under the same lock.
  const uint32_t seq = _recorder.finishedSeq();
  if (seq == _lastObservedFinishedSeq) return FinalizeOutcome::None;
  _lastObservedFinishedSeq = seq;
  const Extraction& f = _recorder.lastFinished();
  const bool real = isLikelyRealShot(f);
  const uint32_t durMs = f.endMs - f.beginMs;
  ESP_LOGI("ExtractionController",
           "finished: cause=%u dur=%lums pump=%lums final=%d cg real=%d",
           static_cast<unsigned>(f.endCause), static_cast<unsigned long>(durMs),
           static_cast<unsigned long>(f.totalPumpOnMs), f.yieldCg,
           real ? 1 : 0);

  // Record every finalize (flush/refill included) for the runtime event log;
  // the real-shot verdict rides along so the Logs view shows what was graded.
  runtimeEventLog.pushExtractionStat(
      {f.startUtcSec, f.beginMs, f.lastPumpOffMs, f.stableMs, f.endMs,
       f.totalPumpOnMs, f.yieldCg, f.startRawCg, f.settledRawCg, f.pourMs,
       f.decisionGainCg, f.observedSampleCount,
       static_cast<uint8_t>(f.endCause), static_cast<uint8_t>(f.yieldStatus),
       real});

  if (!real) return FinalizeOutcome::Rejected;  // flush or refill.

  // Graduate: take a copy so a later candidate (e.g. a flush) finalizing over
  // the recorder's lastFinished() can't displace this real shot.
  _lastAcceptedShot = f;
  _lastAcceptedFlowStats = computeFlowSeriesStats(
      _lastAcceptedShot.samples, _lastAcceptedShot.sampleCount);
  _lastAcceptedSeq = seq;
  return FinalizeOutcome::Accepted;
}

void ExtractionController::persistAcceptedShot() {
  // Save _lastAcceptedShot (our own copy of the last real pull), not
  // _recorder.lastFinished() — so a flush/refill finalizing after a real shot
  // never reaches disk. The lock-free read here is safe: the main task is the
  // only writer (observeFinishedExtraction, earlier this tick) and HTTP readers
  // take _mutex. Each accepted sequence is attempted exactly once; retrying an
  // unavailable or damaged filesystem every main-loop tick would only repeat
  // flash writes and directory work. A later accepted shot gets its own
  // attempt.
  if (_lastAcceptedSeq == _lastAttemptedSeq) return;
  _lastAttemptedSeq = _lastAcceptedSeq;
  if (_shotStore.save(_lastAcceptedShot)) {
    // Release: pair with snapshotStatus()'s acquire so a client that observes
    // the new savedSeq is guaranteed to find the file via /shots.
    _lastSavedSeq.store(_lastAcceptedSeq, std::memory_order_release);
  }
  // Wake the SSE stream with either the new savedSeq or the accepted/saved
  // mismatch that reports a failed save.
  _liveSeq.fetch_add(1, std::memory_order_release);
}

void ExtractionController::registerWith(HttpServer& server) {
  server.registerApp(
      "extraction",
      {
          HttpRoute{"/state", HTTP_GET,
                    [this](WebServer& s) { _handleState(s); }},
          HttpRoute{"/last", HTTP_GET,
                    [this](WebServer& s) { _handleLast(s); }},
          HttpRoute{"/current", HTTP_GET,
                    [this](WebServer& s) { _handleCurrent(s); }},
          HttpRoute{"/shots", HTTP_GET,
                    [this](WebServer& s) {
                      pump_scale::handleShotHistory(s, _shotStore);
                    }},
          HttpRoute{"/loaded", HTTP_POST,
                    [this](WebServer& s) { _handleSetLoaded(s); }},
      });
}

bool ExtractionController::processPendingLoad() {
  const uint32_t id = _pendingLoadId.exchange(0, std::memory_order_acquire);
  if (id == 0) return false;
  // Allocate the ~10 KB record; a failed allocation just drops the request.
  if (!_loadedShot) {
    _loadedShot.reset(new (std::nothrow) Extraction());
    if (!_loadedShot) {
      ESP_LOGW("ExtractionController", "replay: no heap to load shot %lu",
               static_cast<unsigned long>(id));
      return false;
    }
  }
  // Unpublish while we overwrite the slot: HTTP/SSE readers could snapshot it
  // concurrently, and the load does file I/O so it's not practical to protect
  // it under mutex. Readers fall back to the accepted shot (if any) for the few
  // ms of the decode.
  {
    Lock l(_mutex);
    _hasLoadedShot = false;
  }
  // Decode straight from the store into the slot. A failed decode (near-
  // impossible: the HTTP route pre-validates the id) frees the buffer and keeps
  // the flag off so nothing stale shows.
  if (_shotStore.load(id, *_loadedShot)) {
    _loadedFlowStats =
        computeFlowSeriesStats(_loadedShot->samples, _loadedShot->sampleCount);
    {
      Lock l(_mutex);
      _hasLoadedShot = true;
      ++_displayShotSeq;
    }
    ESP_LOGI("ExtractionController", "replay: loaded shot %lu",
             static_cast<unsigned long>(id));
    return true;
  } else {
    {
      Lock l(_mutex);
      clearLoadedShot();
    }
    ESP_LOGW("ExtractionController", "replay: load of shot %lu failed",
             static_cast<unsigned long>(id));
    return false;
  }
}

ExtractionController::LastShotSelection
ExtractionController::effectiveLastShotSelection() const {
  if (_hasLoadedShot) return {_loadedShot.get(), &_loadedFlowStats};
  if (_lastAcceptedSeq != 0)
    return {&_lastAcceptedShot, &_lastAcceptedFlowStats};
  return {};
}

const Extraction* ExtractionController::effectiveLastShot() const {
  return effectiveLastShotSelection().shot;
}

void ExtractionController::requestRestoreLastShotIfEmpty() {
  if (hasEffectiveLastShot()) return;
  if (const uint32_t newestId = _shotStore.newestId()) {
    requestLoadShot(newestId);
  }
}

ExtractionController::TickOutcome ExtractionController::replayTick(
    uint32_t nowMs, bool pumpOn, const ScaleSnapshot& snap) {
  TickOutcome out{};
  {
    Lock l(_mutex);
    const Extraction& before = _recorder.current();
    out.prevPhase = before.phase;
    // utcSec is irrelevant to a replay (the record already has its start time).
    _recorder.update(nowMs, pumpOn, snap, /*utcSec=*/0);
    const Extraction& after = _recorder.current();
    out.phase = after.phase;
    const uint32_t curBeginMs = after.beginMs;

    // Run the predictor on the same virtual clock as the recorder.
    TrustedYieldSample ts;
    _recorder.lastTrustedYieldSample(ts);
    _runTargetAlert(nowMs, out.phase == Phase::RUNNING, ts, curBeginMs);
    out.alertState = _targetAlert.state();

    // Base /state's live weight/yield/elapsed on the synthesized frame and the
    // virtual replay clock (nowMs), so a connected web client mirrors the
    // on-device replay gauge and timer instead of the real BLE scale / wall
    // clock.
    _statusScale = snap;
    _statusNowMs = nowMs;
    _replaying = true;
  }
  out.active = out.phase == Phase::RUNNING || out.phase == Phase::POST_PUMP;
  out.frameNeeded = true;
  // Wake SSE so a web client replays the chart + gauge in step with the device.
  // Still NO observeFinishedExtraction / persistAcceptedShot: a replay must not
  // graduate or persist. The in-flight recorder state it moves is reset by
  // resetInFlightAndNotify() when the caller ends the replay.
  _liveSeq.fetch_add(1, std::memory_order_release);
  return out;
}

void ExtractionController::_handleSetLoaded(WebServer& server) {
  if (!server.hasArg("id")) {
    server.send(400, "application/json", "{\"error\":\"missing id\"}");
    return;
  }
  const long id = server.arg("id").toInt();
  if (id <= 0) {
    server.send(404, "application/json", "{\"error\":\"no such shot\"}");
    return;
  }
  // Distinguish an unreadable history volume from an absent record.
  if (storage::mountState() != storage::MountState::Ready) {
    server.send(503, "application/json",
                "{\"error\":\"shot history unavailable\"}");
    return;
  }
  ShotMeta meta;
  if (!_shotStore.readMeta(static_cast<uint32_t>(id), meta)) {
    server.send(404, "application/json", "{\"error\":\"no such shot\"}");
    return;
  }
  // Queue for the main task; the decode + slot write happen there in tick().
  requestLoadShot(static_cast<uint32_t>(id));
  server.send(200, "application/json", "{\"ok\":true}");
}

void ExtractionController::_writeCompact(WebServer& server,
                                         const Extraction& src) {
  // Brief lock — struct copy, ~50µs. Releasing before encoding keeps the
  // network write off the main-task hot path.
  {
    Lock l(_mutex);
    _snapshot = src;
  }
  _sendSnapshot(server);
}

void ExtractionController::_sendSnapshot(WebServer& server) {
  const size_t total = encodeCompactSize(_snapshot);
  server.setContentLength(total);
  server.send(200, "application/octet-stream", "");
  encodeCompact(_snapshot, [&server](const uint8_t* data, size_t len) {
    server.sendContent(reinterpret_cast<const char*>(data), len);
  });
}

void ExtractionController::_handleLast(WebServer& server) {
  // /last mirrors the device's Last Shot screen: the loaded replay shot when
  // present (including the boot-restored newest shot), else the last accepted
  // pull. 404 when neither exists.
  bool have = false;
  {
    Lock l(_mutex);
    if (const Extraction* d = effectiveLastShot()) {
      _snapshot = *d;
      have = true;
    }
  }
  if (!have) {
    server.send(404, "application/json", "{\"error\":\"no shot recorded\"}");
    return;
  }
  _sendSnapshot(server);
}

void ExtractionController::_handleCurrent(WebServer& server) {
  // Phase IDLE is still a valid encoding (header-only blob); the frontend
  // dispatches on the phase byte. No 404 here — the client polls /state for
  // routing decisions; this endpoint always returns the in-flight record.
  _writeCompact(server, _recorder.current());
}

void ExtractionController::_handleState(WebServer& server) {
  ExtractionStatusSnapshot snap;
  snapshotStatus(snap);
  const char* scaleStr = snap.active ? scaleName(snap.scaleState) : "absent";

  // currentWeightCg is the raw live scale weight; currentYieldCg is the shot
  // yield, emitted once the recorder's baseline is set. The rest are always
  // emitted.
  JsonStream j(server);
  j.open()
      .key("active")
      .boolean(snap.active)
      .key("phase")
      .str(phaseName(snap.currentPhase))
      .key("acceptedSeq")
      .u(snap.acceptedSeq)
      .key("savedSeq")
      .u(snap.savedSeq)
      .key("storage")
      .str(storage::mountStateName(snap.storageState))
      .key("scale")
      .str(scaleStr)
      .key("elapsedMs")
      .u(snap.currentElapsedMs)
      .key("pouring")
      .boolean(snap.currentPouring);
  if (snap.hasCurrentWeight) {
    j.key("currentWeightCg").i(snap.currentWeightCg);
  }
  if (snap.hasCurrentYield) {
    j.key("currentYieldCg").i(snap.currentYieldCg);
  }
  j.close();
  j.finish();
}

void ExtractionController::snapshotStatus(ExtractionStatusSnapshot& out) const {
  // bleScale.snapshot() is mutex-protected on its own; _active is atomic; only
  // the recorder access needs our mutex.
  out.active = _active.load(std::memory_order_acquire);
  out.scaleState = bleScale.snapshot().state;

  out.hasCurrentWeight = false;
  out.currentWeightCg = 0;
  out.currentWeightSequence = 0;
  out.hasCurrentYield = false;
  out.currentYieldCg = 0;

  // savedSeq is atomic; read without the recorder lock.
  out.savedSeq = _lastSavedSeq.load(std::memory_order_acquire);
  out.storageState = storage::mountState();

  Lock l(_mutex);
  const Extraction& cur = _recorder.current();
  out.currentPhase = cur.phase;
  out.currentBeginMs = cur.beginMs;
  out.currentSampleCount = cur.sampleCount;
  // Live weight/yield/elapsed come from the frame and clock the recorder last
  // advanced on (_statusScale/_statusNowMs): the real BLE reading + millis()
  // during a live session, the synthesized frame + the virtual replay clock
  // during a replay. So /state and the SSE STATE packet mirror the on-device
  // gauge in both modes. Both the JSON /state endpoint and the binary STATE
  // packet convert the weight to centigrams through gramsToCg() (which rounds
  // and clamps to the int16 range), so they always report the identical number.
  if (out.active && _statusScale.present) {
    out.hasCurrentWeight = gramsToCg(_statusScale.grams, out.currentWeightCg);
    out.currentWeightSequence = _statusScale.sequence;
    out.hasCurrentYield =
        _recorder.currentYieldCg(_statusScale, out.currentYieldCg);
  }
  out.currentPouring = currentIsPouring();
  out.currentElapsedMs =
      extractionElapsedMs(cur, _replaying ? _statusNowMs : millis());
  // Accepted-shot seq drives /state and history signalling. It must advance
  // only for real shots, not flushes/refills.
  out.acceptedSeq = _lastAcceptedSeq;
  // The display shot drives the stream's FINAL_RECORD: what the device's Last
  // Shot screen shows, including a boot-restored or web-loaded shot.
  out.hasDisplayShot = hasEffectiveLastShot();
  out.displayShotSeq = _displayShotSeq;
}

uint32_t ExtractionController::snapshotExtraction(Extraction& out,
                                                  bool wantDisplayShot) const {
  Lock l(_mutex);
  // With no display shot the accepted slot's empty default is copied; callers
  // gate on hasDisplayShot.
  if (wantDisplayShot) {
    const Extraction* d = effectiveLastShot();
    out = d ? *d : _lastAcceptedShot;
  } else {
    out = _recorder.current();
  }
  return _lastAcceptedSeq;
}

void ExtractionController::setTargetCoeffs(const pump_scale::TargetCoeffs& c) {
  Lock l(_mutex);
  _targetCoeffs = c;
}

void ExtractionController::_runTargetAlert(uint32_t nowMs, bool phaseRunning,
                                           const TrustedYieldSample& sample,
                                           uint32_t curBeginMs) {
  // Reset the predictor only on a genuinely new shot, keyed by beginMs. A
  // POST_PUMP→RUNNING coalesce keeps the same beginMs and must preserve the
  // flow estimate and STOP_NOW state across the brief pump-off.
  if (phaseRunning && curBeginMs != _alertShotBeginMs) {
    _alertShotBeginMs = curBeginMs;
    _targetAlert.onRunningEntry();
    _targetAlert.setCoeffs(_targetCoeffs);
    // Stamp the per-shot target snapshot into the record at-source.
    _recorder.setTargetSnapshot(_targetAlert.coeffs());
  }

  const TargetAlert::State& st =
      _targetAlert.update(nowMs, phaseRunning, sample);
  if (st.stopNowEdge) {
    _recorder.triggerAlarm(AlarmTrigger{nowMs, _targetAlert.snapshotAtStop()});
  }
}

}  // namespace pump_scale
