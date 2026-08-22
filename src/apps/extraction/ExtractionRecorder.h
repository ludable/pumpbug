// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <cstdint>

#include "Extraction.h"
#include "LivePourTracker.h"
#include "vibration/PumpSignalObservation.h"

namespace pump_scale {

// Turns pump activity and scale readings into an extraction record.
//
// Raw scale samples remain unchanged in the record. Live yield uses the first
// pour's starting weight; final yield and shot evidence are recomputed from all
// stored samples. Event timestamps are non-decreasing, and END is always the
// final event.
//
// State model:
//
//   IDLE --pump starts--> RUNNING --pump stops--> POST_PUMP
//     ^                       ^                     |
//     |                       +--same extraction---+
//     |                                             |
//     +-------clear or next extraction-- DONE <-----+
//                                      settled or timed out
//
// RUNNING records each pump-on interval. POST_PUMP keeps the record open while
// liquid may still be reaching the cup. If the pump starts again before the
// current pour has settled, the new interval joins the same extraction. Before
// meaningful flow begins, only a short interruption joins; this handles
// pre-infusion and pump pulsing without combining separate flushes. If the
// previous pour has settled, the recorder finalizes it and starts a new record
// in the same update.
//
// POST_PUMP normally finishes when the live pour tracker reports stable weight.
// Timeouts also close records that never formed a pour, lost their scale sample
// stream, or continued reporting unsettled flow beyond the safety limit.
//
// Entering DONE recomputes pour evidence from the complete stored sample set,
// derives yield from the settled and starting weights, appends END, and
// publishes the record through lastFinished() and finishedSeq(). Those two
// observations survive clear() so a consumer cannot lose a completion while
// the next extraction begins.
//
// transitionTo() is the only method that writes the phase. Events remain
// ordered between beginMs and endMs, and END has the final timestamp.

class ExtractionRecorder {
 public:
  ExtractionRecorder();

  // Advance the state machine one tick. Idempotent w.r.t. the same
  // scale.timestampMs. `utcSec` is the current wall-clock Unix epoch
  // seconds (0 = unknown); captured into Extraction.startUtcSec at the
  // IDLE→RUNNING transition and ignored otherwise. A shot that starts
  // before the wall clock is available keeps startUtcSec = 0; the
  // recorder does not backfill mid-shot.
  void update(uint32_t nowMs, const PumpSignalObservation& pumpSignal,
              const ScaleSnapshot& scale, uint32_t utcSec = 0);

  // Reset in-flight state to IDLE. _lastFinished and _finishedSeq survive.
  // BLE-link bookkeeping (lastScalePresent / lastWeightTimestampMs) is
  // also preserved as those describe the scale, not the shot.
  void clear();

  // Record the target alert config that was active when the current shot
  // began. Call once at the start of a genuinely new pull. No-op if the
  // recorder is not in a live phase (RUNNING/POST_PUMP).
  void setTargetSnapshot(const TargetCoeffs& tc);

  // Record that the target alert first fired STOP_NOW. Captured only once
  // per shot; the event is appended to the shot's events[] timeline as
  // ALARM_TRIGGERED. The caller (ExtractionController::_runTargetAlert)
  // fills AlarmTrigger::tMs and AlarmTrigger::ctx with the moment and the
  // trusted-yield/flow/projection snapshot that justified it.
  void triggerAlarm(const AlarmTrigger& a);

  // Pure observations — never mutate.
  Phase phase() const { return _current.phase; }
  const Extraction& current() const { return _current; }
  const Extraction& lastFinished() const { return _lastFinished; }
  uint32_t finishedSeq() const { return _finishedSeq; }

  bool hasPourStartWeight() const { return _tracker.hasPourStartWeight(); }
  int16_t pourStartRawCg() const {
    return _tracker.hasPourStartWeight() ? _tracker.firstPourStartRawCg()
                                         : Extraction::NO_WEIGHT;
  }

  // True once this shot's pour has delivered a meaningful amount of coffee
  // (LivePourTracker's kMeaningfulYieldCg of trusted yield). The UI uses this
  // to switch the gauge from raw scale weight to yield and to start the live
  // chart; a flush or a weighed grinder dose never produces pour-band flow, so
  // for those it stays false. Stays true once set, until the next IDLE reset.
  bool hasMeaningfulYield() const { return _tracker.hasMeaningfulYield(); }

  // Compute the current shot yield from a live scale snapshot. Returns false
  // unless a shot is live and the tracker has a first-pour start weight (the
  // display zero). The returned value is the current raw weight minus that
  // start weight.
  bool currentYieldCg(const ScaleSnapshot& scale, int16_t& out) const;

  // Returns the last flow-consistent yield for the target alert. The sequence
  // changes only when a new sample is accepted, allowing the alert to ignore
  // rejected weight changes. `have` is false until the pour starts.
  bool lastTrustedYieldSample(TrustedYieldSample& out) const {
    out = TrustedYieldSample{};
    if (_lastTrustedSeq == 0 || !_tracker.hasPourStartWeight()) return false;
    out.have = true;
    out.yieldCg = endpointYieldCg(_tracker.lastTrustedRawCg());
    out.tMs = _lastTrustedSampleTMs;
    out.seq = _lastTrustedSeq;
    return true;
  }

 private:
  // Yield for a raw weight: raw centigrams minus the first-pour start weight.
  // Precondition: _tracker.hasPourStartWeight().
  int16_t endpointYieldCg(int16_t rawCg) const {
    return saturateCg(static_cast<int32_t>(rawCg) -
                      _tracker.firstPourStartRawCg());
  }

  Extraction _current;

  // Dedup key for incoming scale samples: the host BLE-arrival timestamp of the
  // last processed scale reading (0 = none yet this shot).
  uint32_t _lastScaleTimestampMs = 0;

  bool _lastScalePresent = false;

  bool _lastPumpOn = false;
  uint32_t _pumpOnIntervalStartMs = 0;

  // Most recent accepted raw scale sample (centigrams), regardless of whether
  // it made it into _current.samples (the buffer is bounded). Used as the
  // settled endpoint for the displayed yield, and as the start weight when no
  // pour formed.
  int16_t _lastRawCg = Extraction::NO_WEIGHT;
  // Companions for lastTrustedYieldSample(): host time and sequence number of
  // the last flow-consistent (trusted) sample. The sequence bumps only when the
  // detector reports that the sample advanced trust, so a knock leaves both
  // values frozen and the alert holds. _lastTrustedSampleTMs is reset with
  // _lastRawCg on IDLE; _lastTrustedSeq is not reset between shots, so a fresh
  // shot's first trusted sample bumps it to a new value. 0 means "no trusted
  // sample yet".
  uint32_t _lastTrustedSampleTMs = 0;
  uint32_t _lastTrustedSeq = 0;
  // Recorder time of the last sample fed to the pour tracker. Drives the
  // dropout watchdog in POST_PUMP. Reset on IDLE.
  uint32_t _lastRawSampleTMs = 0;
  // Live-only pour state for the gauge, trusted target-alert stream, and
  // post-pump settle decisions. Final persisted evidence is recomputed from
  // _current.samples at finalize.
  LivePourTracker _tracker;

  // Most-recent finalized extraction. Always readable (contents undefined
  // while _finishedSeq == 0). Survives clear() and the implicit IDLE
  // transition in update()'s auto-restart paths. _finishedSeq advances
  // monotonically on each finalize; the host compares against its last-
  // observed value to detect new history.
  Extraction _lastFinished = {};
  uint32_t _finishedSeq = 0;

  // Fold one tick's scale data into the in-flight record: scale presence
  // events, and for each genuinely new reading the detector feed and the
  // appended sample. Called only while recording (RUNNING or POST_PUMP).
  void recordLiveTick(const ScaleSnapshot& scale, uint32_t nowMs);

  // One handler per state in the transition table. Each reads this tick's pump
  // edge and performs that state's transitions.
  void onIdle(bool pumpRising, uint32_t nowMs, uint32_t utcSec);
  void onRunning(bool pumpFalling, uint32_t nowMs,
                 const PumpSignalObservation& pumpSignal);
  void onPostPump(bool pumpRising, uint32_t nowMs, const ScaleSnapshot& scale,
                  uint32_t utcSec);
  void onDone(bool pumpRising, uint32_t nowMs, uint32_t utcSec);

  // POST_PUMP's two transitions, split by the event that triggers them: the
  // pump coming back (resume the same shot, or split off a new one), and the
  // pump staying off (finalize the shot if it has ended, else keep waiting).
  void resumeOrSplitShot(uint32_t nowMs, const ScaleSnapshot& scale,
                         uint32_t utcSec);
  void finalizeIfShotEnded(uint32_t nowMs);

  // The only place that writes _current.phase. Owns all per-shot resets
  // and event emission for the entering phase.
  //
  // `cause` is only consulted for transitions into DONE.
  // `utcSec` is only consulted for IDLE→RUNNING (captures startUtcSec).
  void transitionTo(Phase next, uint32_t nowMs,
                    PumpSignalObservation pumpSignal = {},
                    EndCause cause = EndCause::NONE, uint32_t utcSec = 0);
};

}  // namespace pump_scale
