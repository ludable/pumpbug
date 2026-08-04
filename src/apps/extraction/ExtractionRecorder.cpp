// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#include "ExtractionRecorder.h"

#include <esp_log.h>  // Instead of M5_LOGx to avoid fat include

#include <cmath>

#include "ExtractionDiagnosis.h"
#include "RobustFlow.h"
#include "util/monotonic.h"

namespace {
constexpr const char* TAG = "ExtractionRecorder";
}

namespace pump_scale {

namespace {

// Dropout safety: an open pour whose sample stream stalls this long (a BLE
// drop) is force-finalized rather than left hanging. Well above the ~150 ms
// scale cadence and the settle window (kRunCloseSamples), so it never preempts
// a normal flow-settle close.
constexpr uint32_t NO_NEW_SAMPLE_TIMEOUT_MS = 3000;

// Absolute settle backstop: a pour still unsettled this long after pump-off is
// force-finalized even with a live sample stream. The normal exit is the fast
// flow-settle close (STABLE, ~1.5 s after flow reaches 0); this only fires in
// the pathological case where flow NEVER settles — e.g. a noisy scale jittering
// in the drip-tail band (0.1–0.3 g/s) indefinitely, which the dropout timeout
// above cannot catch because samples keep arriving. Measured from pump-off.
constexpr uint32_t OPEN_POUR_SETTLE_TIMEOUT_MS = 12000;

// Used when the recording has never seen the scale (no weight/flow signal
// to decide on).
constexpr uint32_t NO_SCALE_TIMEOUT_MS = 5000;

// Scale-connected, near-zero operations (flushes/cleaning with no cup yield)
// have weight data, but the weight signal still cannot tell "pre-infusion"
// from "a separate empty pump operation." Use the same short operational
// window as the no-scale path so 10s-off cleaning cycles do not coalesce
// forever just because the scale is connected and flat at 0 g.
constexpr uint32_t EMPTY_SCALE_TIMEOUT_MS = NO_SCALE_TIMEOUT_MS;

// Reserves the last 2 event slots for the finalize events
// (STABLE_DETECTED + END). Heavy BLE flapping can otherwise consume the
// whole array and silently drop the essential end-of-shot events.
constexpr size_t MAX_NONFINALIZE_EVENTS = Extraction::MAX_EVENTS - 2;

void appendEvent(Extraction& e, uint32_t tMs, EventKind kind) {
  if (e.eventCount >= MAX_NONFINALIZE_EVENTS) {
    ESP_LOGW(TAG, "event buffer full, dropping kind=%u", (unsigned)kind);
    e.eventsOverflowed = true;
    return;
  }
  e.events[e.eventCount++] = {tMs, kind};
}

// Bypasses the 2-slot reservation. Only for finalize events; everything
// else goes through appendEvent.
void appendFinalizeEvent(Extraction& e, uint32_t tMs, EventKind kind) {
  if (e.eventCount >= Extraction::MAX_EVENTS) {
    ESP_LOGW(TAG, "event buffer absolutely full, dropping finalize kind=%u",
             (unsigned)kind);
    return;
  }
  e.events[e.eventCount++] = {tMs, kind};
}

void appendSample(Extraction& e, uint32_t tMs, int16_t cg,
                  uint32_t scaleTimerMs) {
  if (e.sampleCount >= Extraction::MAX_SAMPLES) {
    ESP_LOGW(TAG, "sample buffer full, dropping cg=%d", cg);
    return;
  }
  e.samples[e.sampleCount++] = {tMs, cg, scaleTimerMs};
}

}  // namespace

bool ExtractionRecorder::currentYieldCg(const ScaleSnapshot& scale,
                                        int16_t& out) const {
  if (_current.phase != Phase::RUNNING && _current.phase != Phase::POST_PUMP) {
    return false;
  }
  if (!scale.present || !_tracker.hasPourStartWeight()) return false;
  int16_t rawCg;
  if (!gramsToCg(scale.grams, rawCg)) return false;
  out = endpointYieldCg(rawCg);
  return true;
}

void ExtractionRecorder::transitionTo(Phase next, uint32_t nowMs,
                                      EndCause cause, uint32_t utcSec) {
  const Phase prev = _current.phase;
  if (prev == next) return;

  switch (next) {
    case Phase::IDLE: {
      // Hard reset. Preserve only BLE-link bookkeeping (describes the scale
      // context, not the shot).
      const bool lsp = _lastScalePresent;
      _current = Extraction{};
      _lastScaleTimestampMs = 0;
      _lastPumpOn = false;
      _pumpOnIntervalStartMs = 0;
      _lastRawCg = Extraction::NO_WEIGHT;
      _lastTrustedSampleTMs = 0;  // _lastTrustedSeq is lifetime-monotonic
      _lastRawSampleTMs = 0;
      _tracker.reset();
      _lastScalePresent = lsp;
      break;
    }

    case Phase::RUNNING: {
      _pumpOnIntervalStartMs = nowMs;

      if (prev == Phase::IDLE) {
        // First RUNNING of a new shot. _current was wiped by the previous
        // IDLE transition; fill in the begin-time fields.
        _current.beginMs = nowMs;
        _current.startUtcSec = utcSec;
        appendEvent(_current, nowMs, EventKind::BEGIN);
      }
      appendEvent(_current, nowMs, EventKind::PUMP_ON);
      break;
    }

    case Phase::POST_PUMP:
      _current.totalPumpOnMs += nowMs - _pumpOnIntervalStartMs;
      _current.lastPumpOffMs = nowMs;
      appendEvent(_current, nowMs, EventKind::PUMP_OFF);
      break;

    case Phase::DONE: {
      if (cause == EndCause::STABLE) {
        appendFinalizeEvent(_current, nowMs, EventKind::STABLE_DETECTED);
        _current.stableMs = nowMs;
      }

      // The persisted evidence is the batch pass over the stored samples. The
      // live tracker is provisional and exists only for live UI, alert, and
      // settle decisions.
      const PourEvidence evidence =
          computePourEvidence(_current.samples, _current.sampleCount);
      _current.pourMs = evidence.pourMs;
      _current.decisionGainCg = evidence.gainCg;
      _current.yieldStatus =
          evidence.hasPour ? YieldStatus::OK : YieldStatus::NONE;

      // Yield endpoints: settledRaw - startRaw, where startRaw is the flat
      // weight before the first pour. If no pour formed, both endpoints are
      // the most recent sample, so the yield reads as zero.
      _current.startRawCg =
          evidence.hasStartRawCg ? evidence.startRawCg : _lastRawCg;
      _current.settledRawCg = _lastRawCg;
      const bool realShot = isLikelyRealShot(_current);
      int16_t endpointCg = _current.settledRawCg;
      int16_t preDiscontinuityCg = Extraction::NO_WEIGHT;
      const bool postPumpDiscontinuity =
          realShot && evidence.hasPour &&
          endpointBeforePostPumpDiscontinuity(
              _current.samples, _current.sampleCount, _current.lastPumpOffMs,
              preDiscontinuityCg);
      if (postPumpDiscontinuity) endpointCg = preDiscontinuityCg;

      if (_current.startRawCg != Extraction::NO_WEIGHT &&
          endpointCg != Extraction::NO_WEIGHT) {
        _current.yieldCg =
            saturateCg(static_cast<int32_t>(endpointCg) - _current.startRawCg);
      } else {
        _current.yieldCg = Extraction::NO_WEIGHT;
      }

      if (postPumpDiscontinuity) {
        // The first material change after pump-off ends trustworthy endpoint
        // measurement. settledRawCg retains the scale's eventual reading.
        _current.yieldStatus = YieldStatus::DISTURBED;
      } else if (realShot && _current.yieldCg != Extraction::NO_WEIGHT &&
                 _current.yieldCg <= 0) {
        // The sample buffer may fill before a late cup lift is recorded. A
        // non-positive endpoint cannot represent a shot that already passed
        // the independent duration and gain checks.
        _current.yieldCg = saturateCg(_current.decisionGainCg);
        _current.yieldStatus = YieldStatus::DISTURBED;
      }

      // No-scale TIMEOUT: the NO_SCALE_TIMEOUT_MS wait is purely operational
      // (waiting to see if the pump comes back to coalesce); no data is
      // recorded during it. Truncate the intended endMs to lastPumpOffMs so
      // the shot record reflects the meaningful end, not when the recorder
      // committed.
      const uint32_t intendedEndMs =
          (cause == EndCause::TIMEOUT && !hasSampleData(_current))
              ? _current.lastPumpOffMs
              : nowMs;
      // Trim events past intendedEndMs so the record maintains
      // "every event satisfies beginMs <= tMs <= endMs".
      while (_current.eventCount > 0 &&
             _current.events[_current.eventCount - 1].tMs > intendedEndMs) {
        --_current.eventCount;
      }
      const uint32_t endMs =
          _current.eventCount > 0
              ? ensureMonotonicTimestamp(
                    intendedEndMs, _current.events[_current.eventCount - 1].tMs)
              : intendedEndMs;
      _current.endMs = endMs;
      _current.endCause = cause;
      appendFinalizeEvent(_current, endMs, EventKind::END);

      _current.phase = next;
      _lastFinished = _current;
      ++_finishedSeq;
      return;
    }
  }

  _current.phase = next;
}

void ExtractionRecorder::update(uint32_t nowMs, bool pumpOn,
                                const ScaleSnapshot& scale, uint32_t utcSec) {
  const bool live =
      _current.phase == Phase::RUNNING || _current.phase == Phase::POST_PUMP;
  if (live) recordLiveTick(scale, nowMs);
  _lastScalePresent = scale.present;

  const bool pumpRising = pumpOn && !_lastPumpOn;
  const bool pumpFalling = !pumpOn && _lastPumpOn;
  _lastPumpOn = pumpOn;

  switch (_current.phase) {
    case Phase::IDLE:
      onIdle(pumpRising, nowMs, utcSec);
      break;
    case Phase::RUNNING:
      onRunning(pumpFalling, nowMs);
      break;
    case Phase::POST_PUMP:
      onPostPump(pumpRising, nowMs, scale, utcSec);
      break;
    case Phase::DONE:
      onDone(pumpRising, nowMs, utcSec);
      break;
  }
}

void ExtractionRecorder::recordLiveTick(const ScaleSnapshot& scale,
                                        uint32_t nowMs) {
  // Scale connect/disconnect, recorded as events on the shot.
  if (scale.present && !_lastScalePresent) {
    appendEvent(_current, nowMs, EventKind::SCALE_CONNECTED);
  } else if (!scale.present && _lastScalePresent) {
    appendEvent(_current, nowMs, EventKind::SCALE_DISCONNECTED);
  }

  // Accept a reading only if it is genuinely new: present, with a host arrival
  // timestamp we have not processed yet, that converts to a finite weight.
  int16_t rawCg;
  const bool newRawSample = scale.present && scale.timestampMs != 0 &&
                            scale.timestampMs != _lastScaleTimestampMs &&
                            gramsToCg(scale.grams, rawCg);
  if (!newRawSample) return;
  _lastScaleTimestampMs = scale.timestampMs;

  // Ignore stale pre-shot cached samples: sample times are encoded relative to
  // beginMs, and encoding a pre-begin sample underflows.
  if (static_cast<int32_t>(scale.timestampMs - _current.beginMs) < 0) {
    return;
  }

  // Feed the live tracker first: it owns the provisional pour-start weight and
  // the trusted sample stream for the target alert.
  _tracker.update(scale.timestampMs, rawCg);
  _lastRawSampleTMs = nowMs;

  // Advance the trusted-yield sample only when the tracker judged this reading
  // flow-consistent. A knock (an over-step jump) leaves the sequence frozen, so
  // the target alert holds its last trusted reading rather than ingesting the
  // jump. See lastTrustedYieldSample().
  if (_tracker.lastSampleWasTrusted()) {
    ++_lastTrustedSeq;
    _lastTrustedSampleTMs = nowMs;
  }

  // Publish the pour-start weight into the live record as soon as it exists,
  // so on-device and web live charts can subtract it from raw samples before
  // finalize. Until then startRawCg stays NO_WEIGHT and renderers plot raw.
  if (_tracker.hasPourStartWeight() &&
      _current.startRawCg == Extraction::NO_WEIGHT) {
    _current.startRawCg = _tracker.firstPourStartRawCg();
  }

  // Store the sample. Yield is derived later from the endpoints.
  appendSample(_current, scale.timestampMs, rawCg, scale.scaleTimerMs);
  _lastRawCg = rawCg;
  if (_current.observedSampleCount < UINT16_MAX) ++_current.observedSampleCount;
}

void ExtractionRecorder::onIdle(bool pumpRising, uint32_t nowMs,
                                uint32_t utcSec) {
  if (pumpRising) {
    transitionTo(Phase::RUNNING, nowMs, EndCause::NONE, utcSec);
  }
}

void ExtractionRecorder::onRunning(bool pumpFalling, uint32_t nowMs) {
  if (pumpFalling) {
    transitionTo(Phase::POST_PUMP, nowMs);
  }
}

void ExtractionRecorder::onPostPump(bool pumpRising, uint32_t nowMs,
                                    const ScaleSnapshot& scale,
                                    uint32_t utcSec) {
  if (pumpRising)
    resumeOrSplitShot(nowMs, scale, utcSec);
  else
    finalizeIfShotEnded(nowMs);
}

void ExtractionRecorder::resumeOrSplitShot(uint32_t nowMs,
                                           const ScaleSnapshot& scale,
                                           uint32_t utcSec) {
  bool coalesce;
  if (scale.present) {
    if (!_tracker.hasMeaningfulYield()) {
      coalesce = (nowMs - _current.lastPumpOffMs) <= EMPTY_SCALE_TIMEOUT_MS;
    } else if (_tracker.hasUnsettledPour()) {
      coalesce = true;
    } else {
      coalesce = false;
    }
  } else {
    coalesce = (nowMs - _current.lastPumpOffMs) <= NO_SCALE_TIMEOUT_MS;
  }

  if (coalesce) {
    transitionTo(Phase::RUNNING, nowMs);
  } else {
    const EndCause cause = scale.present && _tracker.hasMeaningfulYield()
                               ? EndCause::STABLE
                               : EndCause::TIMEOUT;
    transitionTo(Phase::DONE, nowMs, cause);
    transitionTo(Phase::IDLE, nowMs);
    transitionTo(Phase::RUNNING, nowMs, EndCause::NONE, utcSec);
  }
}

void ExtractionRecorder::finalizeIfShotEnded(uint32_t nowMs) {
  if (_tracker.settledCountReached()) {
    // Normal end: a pour formed and closed on its own.
    transitionTo(Phase::DONE, nowMs, EndCause::STABLE);
  } else if (_tracker.hasUnsettledPour()) {
    const bool streamStalled =
        _lastRawSampleTMs != 0 &&
        nowMs - _lastRawSampleTMs > NO_NEW_SAMPLE_TIMEOUT_MS;
    const bool settleBackstop =
        nowMs - _current.lastPumpOffMs > OPEN_POUR_SETTLE_TIMEOUT_MS;
    if (streamStalled || settleBackstop) {
      transitionTo(Phase::DONE, nowMs, EndCause::TIMEOUT);
    }
  } else {
    // No pour ever formed.
    const bool noScale = !hasSampleData(_current);
    const uint32_t timeoutMs =
        noScale ? NO_SCALE_TIMEOUT_MS : EMPTY_SCALE_TIMEOUT_MS;
    if (nowMs - _current.lastPumpOffMs > timeoutMs) {
      transitionTo(Phase::DONE, nowMs, EndCause::TIMEOUT);
    }
  }
}

void ExtractionRecorder::onDone(bool pumpRising, uint32_t nowMs,
                                uint32_t utcSec) {
  if (pumpRising) {
    transitionTo(Phase::IDLE, nowMs);
    transitionTo(Phase::RUNNING, nowMs, EndCause::NONE, utcSec);
  }
}

void ExtractionRecorder::clear() { transitionTo(Phase::IDLE, 0); }

void ExtractionRecorder::setTargetSnapshot(const TargetCoeffs& tc) {
  if (_current.phase != Phase::RUNNING && _current.phase != Phase::POST_PUMP) {
    return;
  }
  _current.hasTargetSnapshot = true;
  _current.target = tc;
}

void ExtractionRecorder::triggerAlarm(const AlarmTrigger& a) {
  // Record the first STOP_NOW of the running shot: store the alarm context
  // and append an ALARM_TRIGGERED event. Subsequent calls are ignored once
  // alarm.tMs is set, so the recorder captures the alarm at the edge tick
  // and never re-records it.
  if (_current.alarm.tMs != 0) return;
  if (_current.phase != Phase::RUNNING) return;
  _current.alarm = a;
  appendEvent(_current, a.tMs, EventKind::ALARM_TRIGGERED);
}

}  // namespace pump_scale
