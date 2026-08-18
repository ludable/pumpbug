// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>

#include "ble/scale_time.h"

// Espresso-extraction data model
//
// Record of an espresso extraction.
namespace pump_scale {

// States of the extraction detection
enum class Phase : uint8_t {
  IDLE,
  RUNNING,    // pump on
  POST_PUMP,  // pump off, waiting for the scale to settle
  DONE,
};

enum class EndCause : uint8_t {
  NONE,
  STABLE,   // the pour settled; the shot ended cleanly
  TIMEOUT,  // POST_PUMP exceeded a safety backstop
};

// Yield status for the finished record.
enum class YieldStatus : uint8_t {
  NONE,  // no usable pour formed (grinder, flush, spurious window)
  OK,    // yield is the difference between the final and starting weights
  DISTURBED = 2,  // final endpoint was replaced after cup or scale movement
};

enum class EventKind : uint8_t {
  BEGIN,
  TARE,
  PUMP_ON,
  PUMP_OFF_CONFIRMED,
  SCALE_CONNECTED,
  SCALE_DISCONNECTED,
  STABLE_DETECTED,
  END,
  ALARM_TRIGGERED,  // target alert fired STOP_NOW
};

// A persisted EXTR version identifies its binary representation and recorded
// semantics. Decoded records retain that source version; display transports
// may project supported records into the current schema.
enum class ExtractionVersion : uint8_t {
  UNKNOWN = 0,
  V6 = 6,
  V7 = 7,
};

inline constexpr ExtractionVersion kCurrentExtractionVersion =
    ExtractionVersion::V7;

struct PumpOffConfirmedPayload {
  // Zero means that confirmation carried no decay estimate.
  uint16_t signalDecayLeadMsPlusOne = 0;

  bool hasSignalDecayOnset() const { return signalDecayLeadMsPlusOne != 0; }

  uint16_t signalDecayLeadMs() const {
    return hasSignalDecayOnset() ? signalDecayLeadMsPlusOne - 1 : 0;
  }

  uint32_t signalDecayOnsetMs(uint32_t confirmationMs) const {
    return confirmationMs - signalDecayLeadMs();
  }
};

union EventPayload {
  // Most events don't carry a payload. On disk, events without a payload don't
  // serialize it thus requiring no extra space. In memory, an Event is a
  // minimum of 8 bytes: timestamp (4 bytes) and event kind (1 byte),
  // word-aligned. The current payload is 2 bytes, so it doesn't inflate the
  // in-memory layout; take this into account when adding new payload types.
  PumpOffConfirmedPayload pumpOffConfirmed = {};
};

struct Event {
  uint32_t tMs;  // millis() at the event
  EventKind kind;
  EventPayload payload{};
};

static_assert(sizeof(Event) == 8, "Extraction events must remain 8 bytes");

// The smallest target accepted by both the editor and persistent settings.
constexpr int16_t kMinTargetCg = 1000;

// Target-weight alert settings recorded with each extraction.
struct TargetCoeffs {
  int16_t targetCg = 0;         // target shot yield; 0 ⇒ no target set
  bool armed = false;           // gates the alert only
  uint16_t tauMs = 1000;        // flow-proportional drip lead
  int16_t cCg = 0;              // constant drip (signed)
  uint16_t reactionLeadMs = 0;  // manual reaction lag
};

// Measurement bundle captured at alarm time and at the pump-off edge for
// calibration. The same four fields are recorded in both contexts, so they
// share one struct rather than being destructured into loose scalars.
struct AlarmContext {
  int16_t yieldCg = INT16_MIN;           // trusted yield (NO_WEIGHT sentinel)
  int16_t flowCgPerS = 0;                // estimated flow
  bool flowValid = false;                // was the flow estimate usable?
  int16_t projectedFinalCg = INT16_MIN;  // projected settled yield
};

// The first time the target alert fired STOP_NOW this shot. tMs is 0 when the
// alarm never fired.
struct AlarmTrigger {
  uint32_t tMs = 0;  // when STOP_NOW first fired
  AlarmContext ctx;
};

// A sample holds the scale weight exactly as the scale reported it, in
// centigrams, at BLE arrival time. Yield-relative values are never stored:
// every consumer (live gauge, final yield, replay, chart) derives yield at
// read time by subtracting a start weight (see startRawCg below). Deriving at
// read time is what keeps mid-shot tares and cup lifts from corrupting the
// record — they only change what a renderer chooses to subtract.
//
// Throughout this module, "raw" in a name or comment means this as-reported
// weight, as opposed to a yield (a difference of two raw weights).
struct Sample {
  uint32_t tMs;  // host millis() at BLE weight arrival
  int16_t cg;    // raw scale weight in centigrams (0.01 g)
  // Scale timer embedded in the weight sample by some scales.
  // scale_time::UNKNOWN_MS means the scale did not provide one. Values are
  // stored in milliseconds; Acaia timer packets currently resolve to 100 ms
  // ticks. Consumers validate ordering before using it for elapsed time.
  uint32_t scaleTimerMs = scale_time::UNKNOWN_MS;
};

inline bool sampleHasScaleTimer(const Sample& sample) {
  return scale_time::isKnown(sample.scaleTimerMs);
}

// When a scale doesn't produce timings, we may not store the empty fields.
inline bool sampleRangeHasScaleTimer(const Sample* samples, size_t count) {
  for (size_t i = 0; i < count; ++i) {
    if (sampleHasScaleTimer(samples[i])) return true;
  }
  return false;
}

// BLE scale snapshot at tick time. Fed into ExtractionRecorder::update().
// timestampMs is the host BLE-arrival time; 0 means "no reading available".
// If the scale embeds its own timer in the weight sample, scaleTimerMs
// carries it; otherwise it is scale_time::UNKNOWN_MS. The recorder dedupes by
// comparing timestampMs `!= last`.
struct ScaleSnapshot {
  bool present;  // BLE READY
  float grams;
  uint32_t timestampMs;
  uint32_t scaleTimerMs = scale_time::UNKNOWN_MS;
  uint32_t sequence = 0;
};

// A knock-robust live yield reading for the target alert. The recorder
// advances it only on flow-consistent ("trusted") samples, ones the pour
// tracker did not flag as an over-step jump, so an abrupt operator knock that
// spikes the weight is not reflected here and never reaches the alert's flow
// estimator. This is not intended to defeat a deliberate/slow manual bias of
// the scale near the target; at that point the operator is interfering with
// the control signal and a cut alert is acceptable. `seq` bumps once per
// trusted advance and is the alert's dedup key; during a knock the recorder
// re-reports the last trusted reading with the same `seq`, so the held values
// simply persist and the flow estimate ages out instead of ingesting the
// jump. `have` is false until the first pour start weight exists.
struct TrustedYieldSample {
  bool have = false;
  int16_t yieldCg = 0;
  uint32_t tMs = 0;
  uint32_t seq = 0;
};

struct Extraction {
  static constexpr size_t MAX_SAMPLES = 768;  // ≈ 75 s at 10 Hz
  // There are ~5 baseline events per shot (BEGIN, PUMP_ON,
  // PUMP_OFF_CONFIRMED, STABLE_DETECTED, END) but we budget for BLE flapping
  // under marginal conditions. Each BLE flap consumes 2 events
  // (SCALE_DISCONNECTED + SCALE_CONNECTED). 64 covers ~29 flap cycles before
  // essential events start being dropped by appendEvent's full-buffer check.
  static constexpr size_t MAX_EVENTS = 64;
  static constexpr int16_t NO_WEIGHT = INT16_MIN;

  ExtractionVersion version = ExtractionVersion::UNKNOWN;
  Phase phase = Phase::IDLE;
  EndCause endCause = EndCause::NONE;

  // All "*Ms" fields are millis() values, not offsets.
  // Renderer subtracts beginMs to derive offsets.
  uint32_t beginMs = 0;  // BEGIN (== first PUMP_ON; shots begin on pump)
  // Latest confirmed pump-off; the recorder enters POST_PUMP at this time.
  uint32_t lastPumpOffConfirmedMs = 0;
  uint32_t stableMs = 0;       // STABLE_DETECTED, 0 if not declared
  uint32_t endMs = 0;          // END, 0 if not yet DONE
  uint32_t totalPumpOnMs = 0;  // accumulated pump-on duration (coalesce-aware)

  // Wall-clock seconds (Unix epoch) at beginMs. Zero means no valid RTC or NTP
  // time was available when the extraction started.
  uint32_t startUtcSec = 0;

  // Describes how yieldCg was set at the end of the shot recording. OK uses the
  // settled endpoint difference. DISTURBED means scale or cup movement
  // invalidated that endpoint, so yieldCg uses an earlier valid endpoint or the
  // accumulated pour gain.
  YieldStatus yieldStatus = YieldStatus::NONE;

  // Displayed shot yield in centigrams. Normally settledRawCg - startRawCg;
  // yieldStatus records when finalization substitutes a recovery value. The
  // endpoint difference ignores transient scale movement that returns to its
  // previous reference.
  int16_t yieldCg = NO_WEIGHT;

  // The raw scale weight at the start of the first pour: the last settled
  // sample before flow began, taken from the shot-end evidence pass so the
  // leading edge of the pour is not lost. This is the display zero; renderers
  // subtract it from samples to show yield.
  int16_t startRawCg = NO_WEIGHT;

  // Final observed raw scale weight. It is the normal yield endpoint;
  // DISTURBED recovery may use an earlier endpoint or the accumulated pour
  // gain instead.
  int16_t settledRawCg = NO_WEIGHT;

  // Running total of samples observed across the shot, NOT limited by
  // sample buffer overflow. Classifiers should use this rather than walking
  // samples[] (which is bounded at MAX_SAMPLES).
  uint16_t observedSampleCount = 0;

  // Graduation evidence: summed pour-run gain and duration, from the same
  // shot-end evidence pass that finds the pour, but deliberately separate from
  // the displayed yield. Each run's gain is its peak trusted weight minus its
  // start; the peak only rises on trusted samples, so a cup lift cannot walk
  // decisionGainCg down. pourMs is the sustained-flow duration that rejects
  // grinders and brief jostles.
  uint32_t pourMs = 0;
  int32_t decisionGainCg = 0;
  // True if any non-finalize event was dropped because the events buffer
  // was full. STABLE_DETECTED and END have reserved slots so they always
  // land — when this flag is set, the events[] array is an incomplete
  // record of the transitions during the shot. Sample buffer overflow is
  // separately derivable (observedSampleCount vs the buffered count in
  // samples[]).
  bool eventsOverflowed = false;

  Event events[MAX_EVENTS];
  uint16_t eventCount = 0;

  Sample samples[MAX_SAMPLES];
  uint16_t sampleCount = 0;

  // True when the target alert config was explicitly recorded for this shot.
  // This is an in-RAM marker only: current wire records always contain the
  // target block, while a live record stays false until setTargetSnapshot()
  // captures its settings. It distinguishes "recorded as zero/disarmed" from
  // "no snapshot has been captured."
  bool hasTargetSnapshot = false;

  // Target alert config and alarm trigger context. Filled at shot start and
  // when the alert first fires STOP_NOW, respectively. These live outside the
  // events[] stream so they are preserved even if the event buffer is full,
  // and they carry flow/projection values that are not in the stored samples.
  TargetCoeffs target;
  AlarmTrigger alarm;
};

// Saturate an integer centigram value into the representable range, never
// producing the NO_WEIGHT sentinel. The floor is INT16_MIN + 1 so a genuine
// reading at the bottom of the range can't be mistaken for "no reading".
inline int16_t saturateCg(int32_t v) {
  static_assert(Extraction::NO_WEIGHT == INT16_MIN,
                "saturateCg floor assumes NO_WEIGHT == INT16_MIN");
  constexpr int32_t kMin = INT16_MIN + 1;
  constexpr int32_t kMax = INT16_MAX;
  if (v < kMin) return static_cast<int16_t>(kMin);
  if (v > kMax) return static_cast<int16_t>(kMax);
  return static_cast<int16_t>(v);
}

// Convert grams to saturated centigrams (0.01 g). Returns false (out
// untouched) for non-finite input; otherwise out is clamped via saturateCg so
// it never collides with NO_WEIGHT. The clamp happens in the float domain to
// avoid an out-of-range float->int cast for absurd inputs.
inline bool gramsToCg(float grams, int16_t& out) {
  if (!std::isfinite(grams)) return false;
  float cg = std::round(grams * 100.0f);
  constexpr float kMin = static_cast<float>(INT16_MIN + 1);
  constexpr float kMax = static_cast<float>(INT16_MAX);
  if (cg < kMin) cg = kMin;
  if (cg > kMax) cg = kMax;
  out = static_cast<int16_t>(cg);
  return true;
}

// True once any sample has been observed during the shot. Distinct from
// "BLE scale was connected" (which has a separate Event-driven trail): a scale
// that connects but never sends a sample never flips this. Derived from
// observedSampleCount rather than a stored flag so the two cannot drift.
inline bool hasSampleData(const Extraction& e) {
  return e.observedSampleCount > 0;
}

// Time elapsed since BEGIN, or 0 if IDLE
// nowMs is ignored for finished extractions
inline uint32_t extractionElapsedMs(const Extraction& e, uint32_t nowMs) {
  if (e.phase == Phase::IDLE) return 0;
  if (e.phase == Phase::DONE) return e.endMs - e.beginMs;
  return nowMs - e.beginMs;
}

// Resolve which target-alert coefficients to use when replaying `shot`.
// Whole-struct replacement on the recorded snapshot: if the shot explicitly
// recorded its target config at the start of the pull, use that snapshot so
// the replay alarm fires at exactly the same point it did originally. If the
// shot has no recorded snapshot, fall back to the caller-supplied live/user
// coefficients. Mid-shot edits to live coefficients intentionally do not
// affect a shot that has already started.
inline TargetCoeffs resolveReplayCoeffs(const Extraction& shot,
                                        const TargetCoeffs& live) {
  return shot.hasTargetSnapshot ? shot.target : live;
}

}  // namespace pump_scale
