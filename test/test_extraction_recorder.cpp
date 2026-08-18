// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

// The recorder stores RAW scale weight samples. Displayed yield is the endpoint
// difference between the settled and starting weights. Shot acceptance uses
// pour-shaped gain and duration.

#include <cstdio>

#include "ExtractionRecorder.h"
#include "ble/scale_time.h"

namespace {

int g_failures = 0;

#define CHECK(cond)                                               \
  do {                                                            \
    if (!(cond)) {                                                \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                               \
    }                                                             \
  } while (0)

bool yieldFrameUsable(const pump_scale::ExtractionRecorder& r) {
  return r.hasPourStartWeight();
}

pump_scale::ScaleSnapshot scaleAt(
    uint32_t tMs, float grams, uint32_t scaleTimerMs = scale_time::UNKNOWN_MS) {
  return {true, grams, tMs, scaleTimerMs};
}

PumpSignalObservation pumpOn() { return {PumpSignalState::On}; }

PumpSignalObservation pumpOff() { return {PumpSignalState::Off}; }

PumpSignalObservation pumpOffWithDecayOnset(uint32_t onsetMs) {
  return {PumpSignalState::Off, onsetMs, true};
}

void pumpOnWithZeroScale(pump_scale::ExtractionRecorder& r, uint32_t tMs) {
  CHECK(tMs > 1500);
  r.update(tMs - 1500, pumpOff(), scaleAt(tMs - 1500, 0.0f));
  r.update(tMs - 900, pumpOff(), scaleAt(tMs - 900, 0.0f));
  r.update(tMs - 300, pumpOff(), scaleAt(tMs - 300, 0.0f));
  r.update(tMs, pumpOn(), scaleAt(tMs, 0.0f));
  r.update(tMs + 100, pumpOn(), scaleAt(tMs + 100, 0.0f));
}

// Drives a pour into the recorder: pump on, a short flat prelude, then a
// steady ramp up by approximately peakG.
// Returns the next free scale timestamp; the pump is left on and the scale
// sits at the final weight (startG + g), returned via outG.
uint32_t pourToFrom(pump_scale::ExtractionRecorder& r, uint32_t t0,
                    float startG, float peakG, float& outG) {
  r.update(t0, pumpOn(),
           scaleAt(t0, startG));  // pump-rising tick (sample skipped)
  uint32_t t = t0 + 150;
  for (int i = 0; i < 4; ++i, t += 150)
    r.update(t, pumpOn(), scaleAt(t, startG));  // establish the starting weight
  float g = 0.0f;
  while (g + 0.6f <= peakG) {
    g += 0.6f;  // 0.6 g / 150 ms = 4 g/s, squarely in the pour band
    r.update(t, pumpOn(), scaleAt(t, startG + g));
    t += 150;
  }
  outG = startG + g;
  return t;
}

uint32_t pourTo(pump_scale::ExtractionRecorder& r, uint32_t t0, float peakG,
                float& outG) {
  return pourToFrom(r, t0, 0.0f, peakG, outG);
}

// --- Coalesce / finalize ---

void testEmptyScaleOperationsDoNotCoalesceAcrossLongGap() {
  pump_scale::ExtractionRecorder r;

  pumpOnWithZeroScale(r, 2000);
  r.update(6000, pumpOff(), scaleAt(6000, 0.0f));
  r.update(11100, pumpOff(), scaleAt(11100, 0.0f));

  CHECK(r.finishedSeq() == 1);
  const pump_scale::Extraction& first = r.lastFinished();
  CHECK(first.endCause == pump_scale::EndCause::TIMEOUT);
  CHECK(first.totalPumpOnMs == 4000);
  CHECK(first.yieldStatus == pump_scale::YieldStatus::NONE);
  CHECK(first.yieldCg == 0);

  pumpOnWithZeroScale(r, 15000);
  CHECK(r.finishedSeq() == 1);
  CHECK(r.phase() == pump_scale::Phase::RUNNING);
  CHECK(r.current().beginMs == 15000);
}

void testEmptyScaleOperationsCoalesceAcrossShortGap() {
  pump_scale::ExtractionRecorder r;

  pumpOnWithZeroScale(r, 2000);
  r.update(6000, pumpOff(), scaleAt(6000, 0.0f));
  r.update(10000, pumpOn(), scaleAt(10000, 0.0f));

  CHECK(r.finishedSeq() == 0);
  CHECK(r.phase() == pump_scale::Phase::RUNNING);
  CHECK(r.current().beginMs == 2000);
  CHECK(r.current().totalPumpOnMs == 4000);
}

// --- Sample recording ---

void testSamplesUseHostScaleTimestamp() {
  pump_scale::ExtractionRecorder r;

  r.update(2000, pumpOn(),
           scaleAt(2000, 0.0f));  // pump-rising tick: not yet live
  r.update(2200, pumpOn(),
           scaleAt(2111, 5.0f));  // first live sample (tMs 2111)

  const pump_scale::Extraction& cur = r.current();
  CHECK(cur.sampleCount == 1);
  if (cur.sampleCount == 1) {
    CHECK(cur.samples[0].tMs == 2111);
    CHECK(cur.samples[0].cg == 500);  // raw centigrams
    CHECK(cur.samples[0].scaleTimerMs == scale_time::UNKNOWN_MS);
  }
}

void testPreBeginScaleSampleIgnored() {
  pump_scale::ExtractionRecorder r;

  r.update(2000, pumpOn(), scaleAt(1990, 10.0f));  // pump-rising tick: not live
  r.update(2100, pumpOn(),
           scaleAt(1990, 10.0f));  // stale cached pre-begin sample
  CHECK(!r.hasPourStartWeight());
  CHECK(r.current().sampleCount == 0);
  CHECK(r.current().observedSampleCount == 0);

  r.update(2200, pumpOn(), scaleAt(2200, 10.5f));  // first true in-shot sample

  const pump_scale::Extraction& cur = r.current();
  CHECK(cur.sampleCount == 1);
  CHECK(cur.observedSampleCount == 1);
  if (cur.sampleCount == 1) {
    CHECK(cur.samples[0].tMs == 2200);
    CHECK(cur.samples[0].cg == 1050);  // raw, not baseline-relative
  }
}

void testPostWrapScaleSampleAccepted() {
  pump_scale::ExtractionRecorder r;
  constexpr uint32_t beginMs = 0xfffffff0u;
  constexpr uint32_t afterWrapMs = 0x00000020u;

  r.update(beginMs, pumpOn(),
           scaleAt(beginMs, 10.0f));  // pump-rising: not live
  r.update(afterWrapMs, pumpOn(), scaleAt(afterWrapMs, 10.5f));

  const pump_scale::Extraction& cur = r.current();
  CHECK(cur.sampleCount == 1);
  CHECK(cur.observedSampleCount == 1);
  if (cur.sampleCount == 1) {
    CHECK(cur.samples[0].tMs == afterWrapMs);
    CHECK(cur.samples[0].cg == 1050);  // raw centigrams
  }
}

void testSamplesCaptureScaleTimerTimestamp() {
  pump_scale::ExtractionRecorder r;

  r.update(2000, pumpOn(), scaleAt(2000, 0.0f));
  r.update(2200, pumpOn(), scaleAt(2111, 5.0f, 2300));

  const pump_scale::Extraction& cur = r.current();
  CHECK(cur.sampleCount == 1);
  if (cur.sampleCount == 1) {
    CHECK(cur.samples[0].tMs == 2111);
    CHECK(cur.samples[0].scaleTimerMs == 2300);
  }
}

void testDuplicateScaleTimestampDeduped() {
  pump_scale::ExtractionRecorder r;

  r.update(2000, pumpOn(), scaleAt(2000, 0.0f));  // pump-rising; not live
  r.update(2100, pumpOn(), scaleAt(2100, 1.0f));  // first live sample
  r.update(2150, pumpOn(), scaleAt(2100, 1.0f));  // same scale tMs -> deduped
  r.update(2200, pumpOn(), scaleAt(2200, 2.0f));  // distinct

  const pump_scale::Extraction& cur = r.current();
  CHECK(cur.sampleCount == 2);
  CHECK(cur.observedSampleCount == 2);
}

// --- Raw-sample / endpoint-yield behavior ---

void testRawSamplesStoredUntared() {
  pump_scale::ExtractionRecorder r;

  // A 100 g cup sits on the (untared) scale at pump start. Samples are stored
  // raw; yield starts from the flat weight before the pour.
  float g;
  uint32_t t =
      pourToFrom(r, 2000, 100.0f, 5.0f, g);  // flat 100 g cup, pour to ~5 g

  const pump_scale::Extraction& cur = r.current();
  CHECK(cur.sampleCount >= 3);
  CHECK(cur.samples[0].cg == 10000);  // raw flat cup
  // Live yield is available once the pour's starting weight is known.
  CHECK(r.hasPourStartWeight());
  CHECK(r.pourStartRawCg() == 10000);
  int16_t y = 0;
  CHECK(r.currentYieldCg(scaleAt(t, g), y));
  CHECK(y >= 450 && y <= 550);  // endpoint yield = poured amount
}

void testPreYieldTareDoesNotRebaseSamples() {
  pump_scale::ExtractionRecorder r;

  // Cup on the scale, operator tares BEFORE any meaningful yield. With raw
  // samples there is no rebase: the recorded samples include the step down.
  // The tracker discards the pre-tare reference and uses the new zero when the
  // pour begins.
  uint32_t t = 2000;
  r.update(t, pumpOn(),
           scaleAt(t, 100.0f));  // pump-rising tick (sample skipped)
  t += 150;
  for (int i = 0; i < 4; ++i, t += 150)
    r.update(t, pumpOn(), scaleAt(t, 100.0f));  // flat cup
  r.update(t, pumpOn(),
           scaleAt(t, 0.0f));  // physical tare (downward, pre-yield)
  t += 150;
  // Flat at the new zero through pre-infusion, then a real ramped pour.
  for (int i = 0; i < 20; ++i, t += 150)
    r.update(t, pumpOn(), scaleAt(t, 0.0f));
  float g = 0.0f;
  while (g + 0.6f <= 30.0f) {
    g += 0.6f;
    r.update(t, pumpOn(), scaleAt(t, g));
    t += 150;
  }

  const pump_scale::Extraction& cur = r.current();
  CHECK(r.hasPourStartWeight());
  CHECK(yieldFrameUsable(r));
  // Raw samples include the tare step.
  bool sawTareStep = false;
  for (uint16_t i = 1; i < cur.sampleCount; ++i) {
    if (cur.samples[i].cg - cur.samples[i - 1].cg <= -9000) sawTareStep = true;
  }
  CHECK(sawTareStep);
  // The starting weight is near the post-tare zero (flow-estimator lag may put
  // it a fraction of a gram above 0). The live yield reads the poured amount
  // (~30 g), not the 100 g cup.
  CHECK(r.pourStartRawCg() >= -50 && r.pourStartRawCg() <= 200);
  int16_t y = 0;
  CHECK(r.currentYieldCg(scaleAt(t, g), y));
  CHECK(y > 2500 && y < 3000);
}

void testPreYieldUpwardTareHandled() {
  pump_scale::ExtractionRecorder r;

  // A scale below zero is tared before any meaningful yield. The upward step
  // must not become the starting weight for the pour.
  uint32_t t = 2000;
  r.update(t, pumpOn(),
           scaleAt(t, -8.0f));  // pump-rising tick (sample skipped)
  t += 150;
  for (int i = 0; i < 4; ++i, t += 150)
    r.update(t, pumpOn(), scaleAt(t, -8.0f));  // flat below zero
  r.update(t, pumpOn(), scaleAt(t, 0.0f));     // operator tare: jumps UP to 0
  t += 150;
  for (int i = 0; i < 3; ++i, t += 150) r.update(t, pumpOn(), scaleAt(t, 0.0f));

  float g = 0.0f;
  while (g + 0.6f <= 30.0f) {
    g += 0.6f;
    r.update(t, pumpOn(), scaleAt(t, g));
    t += 150;
  }

  // The starting weight is near the post-tare zero.
  CHECK(r.pourStartRawCg() >= -50 && r.pourStartRawCg() <= 200);
  int16_t y = 0;
  CHECK(r.currentYieldCg(scaleAt(t, g), y));
  CHECK(y > 2800 && y < 3100);
}

void testPreYieldSustainedTareStaysValid() {
  pump_scale::ExtractionRecorder r;

  // A pre-yield tare (cup 100 g -> 0) that stays flat before the pour begins
  // must not invalidate the shot. No coherent pour has accrued at the drop, so
  // the later pour remains usable.
  r.update(2000, pumpOn(), scaleAt(2000, 100.0f));
  r.update(2100, pumpOn(), scaleAt(2100, 100.0f));
  r.update(2200, pumpOn(), scaleAt(2200, 0.0f));  // physical tare (pre-yield)
  // Stay flat at zero before beginning the pour.
  uint32_t t = 2300;
  for (; t < 4000; t += 150) r.update(t, pumpOn(), scaleAt(t, 0.0f));
  // Now a real ramped pour.
  float g = 0.0f;
  while (g + 0.6f <= 8.0f) {
    g += 0.6f;
    r.update(t, pumpOn(), scaleAt(t, g));
    t += 150;
  }

  CHECK(r.hasPourStartWeight());
  CHECK(yieldFrameUsable(r));
}

void testPostMeaningfulSustainedLiftDoesNotDisturbLiveFrame() {
  pump_scale::ExtractionRecorder r;

  // A sustained cup lift after meaningful yield must leave the live yield
  // usable. The trusted-yield stream freezes across the non-pour step.
  float g;
  uint32_t t = pourTo(r, 2000, 8.0f, g);    // real pour to ~8 g
  r.update(t, pumpOn(), scaleAt(t, 0.0f));  // cup lifted: -8 g step
  t += 150;
  for (int i = 0; i < 5; ++i, t += 150) r.update(t, pumpOn(), scaleAt(t, 0.0f));

  CHECK(yieldFrameUsable(r));
}

void testPostMeaningfulTransientDipDoesNotInvalidate() {
  pump_scale::ExtractionRecorder r;

  // A cup-sized downward step after meaningful yield that RECOVERS within the
  // watch (an operator nudge, or the pour resuming) must NOT invalidate.
  float g;
  uint32_t t = pourTo(r, 2000, 8.0f, g);        // real pour to ~8 g
  r.update(t, pumpOn(), scaleAt(t, g - 5.0f));  // -5 g jostle: opens watch
  t += 150;
  r.update(t, pumpOn(), scaleAt(t, g + 0.5f));  // recovered (pour resumes)
  t += 150;
  r.update(t, pumpOn(), scaleAt(t, g + 1.0f));
  t += 150;
  r.update(t, pumpOn(), scaleAt(t, g + 1.5f));

  CHECK(yieldFrameUsable(r));
}

void testSmallDipDoesNotInvalidate() {
  pump_scale::ExtractionRecorder r;

  // Load-cell jitter (a fraction of a gram) after meaningful yield must NOT
  // invalidate — only a real cup-sized downward step does.
  float g;
  uint32_t t = pourTo(r, 2000, 8.0f, g);        // real pour to ~8 g
  r.update(t, pumpOn(), scaleAt(t, g - 0.2f));  // -0.2 g jitter (not a step)

  CHECK(yieldFrameUsable(r));
}

void testScaleAbsentAtStartStoresRaw() {
  pump_scale::ExtractionRecorder r;

  // Scale absent at pump start; the first live sample once it connects is
  // stored raw. No pour has established a starting weight yet.
  const pump_scale::ScaleSnapshot absent{false, 0.0f, 0,
                                         scale_time::UNKNOWN_MS};
  r.update(2000, pumpOn(), absent);
  CHECK(!r.hasPourStartWeight());

  r.update(2100, pumpOn(), scaleAt(2100, 5.0f));  // connects
  r.update(2200, pumpOn(), scaleAt(2200, 7.0f));

  const pump_scale::Extraction& cur = r.current();
  CHECK(!r.hasPourStartWeight());
  CHECK(cur.sampleCount == 2);
  if (cur.sampleCount == 2) {
    CHECK(cur.samples[0].cg == 500);  // raw
    CHECK(cur.samples[1].cg == 700);  // raw
  }
}

void testCurrentYieldCgIsEndpointYield() {
  pump_scale::ExtractionRecorder r;

  // currentYieldCg reports the current raw weight minus the pour's starting
  // weight. It returns false before that starting weight is known.
  int16_t y = 0;
  uint32_t t = 2000;
  for (int i = 0; i < 3; ++i, t += 150)
    r.update(t, pumpOn(), scaleAt(t, 0.0f));  // flat prelude
  CHECK(!r.currentYieldCg(scaleAt(t, 0.0f), y));

  float g = 0.0f;
  while (g + 0.6f <= 8.0f) {  // real ramped pour to ~8 g
    g += 0.6f;
    r.update(t, pumpOn(), scaleAt(t, g));
    t += 150;
  }
  CHECK(r.currentYieldCg(scaleAt(t, g), y));
  CHECK(y > 700 && y <= 850);
}

void testPostPumpCupLiftUsesPreDiscontinuityEndpoint() {
  pump_scale::ExtractionRecorder r;

  float g = 0.0f;
  uint32_t t = pourTo(r, 2000, 40.0f, g);

  // Some Acaia modes alternate between positive and negative tare readings
  // after the cup is lifted. Exercise the less obvious positive phase.
  constexpr float liftedReadingG = 71.4f;
  r.update(t, pumpOff(), scaleAt(t, liftedReadingG));
  for (int i = 0; i < 100 && r.finishedSeq() == 0; ++i) {
    t += 150;
    r.update(t, pumpOff(), scaleAt(t, liftedReadingG));
  }

  CHECK(r.finishedSeq() == 1);
  if (r.finishedSeq() != 1) return;

  const pump_scale::Extraction& done = r.lastFinished();
  CHECK(done.yieldStatus == pump_scale::YieldStatus::DISTURBED);
  CHECK(done.settledRawCg == 7140);
  CHECK(done.yieldCg > 3500 && done.yieldCg < 4500);
}

const pump_scale::Extraction& finishAfterPrePumpEndpointChange(
    pump_scale::ExtractionRecorder& r, uint32_t t, int16_t endpointCg) {
  // Apply the changed scale reference before pump-off, then hold that reading
  // until the recorder finalizes.
  const float endpointG = static_cast<float>(endpointCg) / 100.0f;
  r.update(t, pumpOn(), scaleAt(t, endpointG));
  t += 150;
  r.update(t, pumpOff(), scaleAt(t, endpointG));
  for (int i = 0; i < 100 && r.finishedSeq() == 0; ++i) {
    t += 150;
    r.update(t, pumpOff(), scaleAt(t, endpointG));
  }
  CHECK(r.finishedSeq() == 1);
  return r.lastFinished();
}

void testSevereEndpointGainDisagreementUsesPourGain() {
  pump_scale::ExtractionRecorder r;

  float g = 0.0f;
  const uint32_t t = pourTo(r, 2000, 40.0f, g);
  const pump_scale::Extraction& done =
      finishAfterPrePumpEndpointChange(r, t, 30);

  CHECK(done.decisionGainCg > 3500 && done.decisionGainCg < 4500);
  CHECK(done.settledRawCg == 30);
  CHECK(done.yieldStatus == pump_scale::YieldStatus::DISTURBED);
  CHECK(done.yieldCg == done.decisionGainCg);
}

void testSevereDisagreementOverridesPostPumpEndpointRecovery() {
  pump_scale::ExtractionRecorder r;

  float g = 0.0f;
  uint32_t t = pourTo(r, 2000, 40.0f, g);
  r.update(t, pumpOn(), scaleAt(t, 0.3f));  // reference resets before pump-off
  t += 150;
  r.update(t, pumpOff(), scaleAt(t, 71.4f));  // cup moves after pump-off
  for (int i = 0; i < 100 && r.finishedSeq() == 0; ++i) {
    t += 150;
    r.update(t, pumpOff(), scaleAt(t, 71.4f));
  }

  CHECK(r.finishedSeq() == 1);
  if (r.finishedSeq() != 1) return;
  const pump_scale::Extraction& done = r.lastFinished();
  CHECK(done.settledRawCg == 7140);
  CHECK(done.yieldStatus == pump_scale::YieldStatus::DISTURBED);
  CHECK(done.yieldCg == done.decisionGainCg);
}

void testEndpointAtThreeQuartersOfPourGainIsDisturbed() {
  pump_scale::ExtractionRecorder r;

  float g = 0.0f;
  const uint32_t t = pourTo(r, 2000, 40.0f, g);
  const int16_t pourEndCg = static_cast<int16_t>(g * 100.0f + 0.5f);
  CHECK(pourEndCg % 4 == 0);
  const int16_t endpointCg = pourEndCg * 3 / 4;
  const pump_scale::Extraction& done =
      finishAfterPrePumpEndpointChange(r, t, endpointCg);

  CHECK(done.decisionGainCg == pourEndCg);
  CHECK(static_cast<int64_t>(done.settledRawCg - done.startRawCg) * 4 ==
        static_cast<int64_t>(done.decisionGainCg) * 3);
  CHECK(done.yieldStatus == pump_scale::YieldStatus::DISTURBED);
  CHECK(done.yieldCg == done.decisionGainCg);
}

void testEndpointAboveThreeQuartersOfPourGainRemainsOk() {
  pump_scale::ExtractionRecorder r;

  float g = 0.0f;
  const uint32_t t = pourTo(r, 2000, 44.0f, g);
  const pump_scale::Extraction& done =
      finishAfterPrePumpEndpointChange(r, t, 4068);

  CHECK(static_cast<int64_t>(done.settledRawCg - done.startRawCg) * 4 >
        static_cast<int64_t>(done.decisionGainCg) * 3);
  CHECK(done.yieldStatus == pump_scale::YieldStatus::OK);
  CHECK(done.yieldCg == done.settledRawCg - done.startRawCg);
}

void testSetTargetSnapshotRecordsTarget() {
  pump_scale::ExtractionRecorder r;

  // Before the shot starts there is nowhere to store the snapshot.
  pump_scale::TargetCoeffs tc;
  tc.targetCg = 3600;
  tc.armed = true;
  tc.tauMs = 800;
  tc.cCg = 50;
  tc.reactionLeadMs = 250;
  r.setTargetSnapshot(tc);
  CHECK(!r.current().hasTargetSnapshot);

  // Drive the shot into RUNNING, then record the snapshot.
  r.update(2000, pumpOn(), scaleAt(2000, 0.0f));
  r.update(2100, pumpOn(), scaleAt(2100, 0.5f));
  CHECK(r.phase() == pump_scale::Phase::RUNNING);

  r.setTargetSnapshot(tc);
  const pump_scale::Extraction& cur = r.current();
  CHECK(cur.hasTargetSnapshot);
  CHECK(cur.target.targetCg == 3600);
  CHECK(cur.target.armed == true);
  CHECK(cur.target.tauMs == 800);
  CHECK(cur.target.cCg == 50);
  CHECK(cur.target.reactionLeadMs == 250);

  // A second call overwrites; the snapshot is "whatever was current when the
  // caller last set it". In practice ExtractionController calls this once at
  // the first RUNNING tick of each shot.
  pump_scale::TargetCoeffs other = tc;
  other.targetCg = 9999;
  r.setTargetSnapshot(other);
  CHECK(r.current().target.targetCg == 9999);
}

void testPumpOffConfirmationCarriesAcceptedSignalDecayOnset() {
  pump_scale::ExtractionRecorder r;
  r.update(2000, pumpOn(), scaleAt(2000, 0.0f));
  r.update(2100, pumpOn(), scaleAt(2100, 0.0f));

  CHECK(r.current().version == pump_scale::ExtractionVersion::V7);
  r.update(3000, pumpOffWithDecayOnset(2400),
           {false, 0.0f, 3000, scale_time::UNKNOWN_MS});

  const pump_scale::Extraction& cur = r.current();
  CHECK(cur.phase == pump_scale::Phase::POST_PUMP);
  CHECK(cur.eventCount == 4);
  if (cur.eventCount == 4) {
    CHECK(cur.events[2].kind == pump_scale::EventKind::SCALE_DISCONNECTED);
    CHECK(cur.events[3].kind == pump_scale::EventKind::PUMP_OFF_CONFIRMED);
    CHECK(cur.events[3].tMs == 3000);
    const auto& pumpOff = cur.events[3].payload.pumpOffConfirmed;
    CHECK(pumpOff.hasSignalDecayOnset());
    CHECK(pumpOff.signalDecayLeadMs() == 600);
    CHECK(pumpOff.signalDecayOnsetMs(cur.events[3].tMs) == 2400);
  }
  // Pump-off is recorded at confirmation; the decay onset is event metadata.
  CHECK(cur.lastPumpOffConfirmedMs == 3000);
  CHECK(cur.totalPumpOnMs == 1000);
}

void testPumpOffConfirmationWithoutEstimateHasNoPayload() {
  pump_scale::ExtractionRecorder r;
  r.update(2000, pumpOn(), scaleAt(2000, 0.0f));
  r.update(3000, pumpOff(), scaleAt(3000, 0.0f));

  const pump_scale::Event& pumpOff =
      r.current().events[r.current().eventCount - 1];
  CHECK(pumpOff.kind == pump_scale::EventKind::PUMP_OFF_CONFIRMED);
  CHECK(!pumpOff.payload.pumpOffConfirmed.hasSignalDecayOnset());
}

void testEachPumpIntervalCarriesItsOwnSignalDecayEstimate() {
  pump_scale::ExtractionRecorder r;
  const pump_scale::ScaleSnapshot noScale = {false, 0.0f, 0,
                                             scale_time::UNKNOWN_MS};

  r.update(2000, pumpOn(), noScale);
  r.update(3000, pumpOffWithDecayOnset(2400), noScale);
  r.update(3500, pumpOn(), noScale);
  r.update(4500, pumpOffWithDecayOnset(4200), noScale);

  const pump_scale::Extraction& cur = r.current();
  CHECK(cur.phase == pump_scale::Phase::POST_PUMP);
  CHECK(cur.eventCount == 5);
  if (cur.eventCount == 5) {
    CHECK(cur.events[2].kind == pump_scale::EventKind::PUMP_OFF_CONFIRMED);
    CHECK(cur.events[2].tMs == 3000);
    CHECK(cur.events[2].payload.pumpOffConfirmed.signalDecayOnsetMs(
              cur.events[2].tMs) == 2400);
    CHECK(cur.events[4].kind == pump_scale::EventKind::PUMP_OFF_CONFIRMED);
    CHECK(cur.events[4].tMs == 4500);
    CHECK(cur.events[4].payload.pumpOffConfirmed.signalDecayOnsetMs(
              cur.events[4].tMs) == 4200);
  }
  CHECK(cur.lastPumpOffConfirmedMs == 4500);
  CHECK(cur.totalPumpOnMs == 2000);
}

}  // namespace

// Regression for the absolute settle backstop. A live pour that never
// flow-settles must not hang POST_PUMP forever.
void testUnsettledPourFinalizesOnBackstop() {
  pump_scale::ExtractionRecorder r;
  constexpr uint32_t kPumpOffMs = 4550;
  constexpr uint32_t kBackstopMs = 12000;  // = OPEN_POUR_SETTLE_TIMEOUT_MS

  r.update(1900, pumpOff(), scaleAt(1900, 0.0f));
  float g = 0.0f;
  for (uint32_t t = 2000; t <= 4400; t += 150) {
    r.update(t, pumpOn(), scaleAt(t, g));
    g += 0.30f;  // +0.3 g / 150 ms = 2 g/s, squarely in the POUR band
  }
  CHECK(r.current().phase == pump_scale::Phase::RUNNING);

  r.update(kPumpOffMs, pumpOff(), scaleAt(kPumpOffMs, g));
  CHECK(r.current().phase == pump_scale::Phase::POST_PUMP);

  for (uint32_t t = kPumpOffMs + 150; t < kPumpOffMs + kBackstopMs; t += 150) {
    r.update(t, pumpOff(), scaleAt(t, g));
    g += 0.04f;  // +0.04 g / 150 ms ≈ 0.27 g/s, in the TAIL band
  }
  CHECK(r.current().phase == pump_scale::Phase::POST_PUMP);
  CHECK(r.finishedSeq() == 0);

  const uint32_t after = kPumpOffMs + kBackstopMs + 150;
  r.update(after, pumpOff(), scaleAt(after, g));
  CHECK(r.finishedSeq() == 1);
  const pump_scale::Extraction& done = r.lastFinished();
  CHECK(done.endCause == pump_scale::EndCause::TIMEOUT);
  CHECK(done.decisionGainCg > 0);
}

int main() {
  testEmptyScaleOperationsDoNotCoalesceAcrossLongGap();
  testEmptyScaleOperationsCoalesceAcrossShortGap();
  testSamplesUseHostScaleTimestamp();
  testPreBeginScaleSampleIgnored();
  testPostWrapScaleSampleAccepted();
  testSamplesCaptureScaleTimerTimestamp();
  testDuplicateScaleTimestampDeduped();
  testRawSamplesStoredUntared();
  testPreYieldTareDoesNotRebaseSamples();
  testPreYieldUpwardTareHandled();
  testPreYieldSustainedTareStaysValid();
  testPostMeaningfulSustainedLiftDoesNotDisturbLiveFrame();
  testPostMeaningfulTransientDipDoesNotInvalidate();
  testSmallDipDoesNotInvalidate();
  testScaleAbsentAtStartStoresRaw();
  testCurrentYieldCgIsEndpointYield();
  testPostPumpCupLiftUsesPreDiscontinuityEndpoint();
  testSevereEndpointGainDisagreementUsesPourGain();
  testSevereDisagreementOverridesPostPumpEndpointRecovery();
  testEndpointAtThreeQuartersOfPourGainIsDisturbed();
  testEndpointAboveThreeQuartersOfPourGainRemainsOk();
  testSetTargetSnapshotRecordsTarget();
  testPumpOffConfirmationCarriesAcceptedSignalDecayOnset();
  testPumpOffConfirmationWithoutEstimateHasNoPayload();
  testEachPumpIntervalCarriesItsOwnSignalDecayEstimate();
  testUnsettledPourFinalizesOnBackstop();
  if (g_failures == 0) {
    std::printf("OK: all assertions passed\n");
    return 0;
  }
  std::printf("%d assertion(s) failed\n", g_failures);
  return 1;
}
