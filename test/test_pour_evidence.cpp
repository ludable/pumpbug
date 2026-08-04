// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

// Host-compilable unit tests for batch pour evidence reduced from RobustFlow.

#include <cstdio>
#include <vector>

#include "RobustFlow.h"

namespace {

using pump_scale::computePourEvidence;
using pump_scale::Sample;

int g_failures = 0;

#define CHECK(cond)                                               \
  do {                                                            \
    if (!(cond)) {                                                \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                               \
    }                                                             \
  } while (0)

void ramp(std::vector<Sample>& s, uint32_t& t, double& cg, double rateCgPerS,
          uint32_t durMs, uint32_t stepMs = 150) {
  const uint32_t end = t + durMs;
  while (t <= end) {
    s.push_back(Sample{t, static_cast<int16_t>(cg)});
    t += stepMs;
    cg += rateCgPerS * stepMs / 1000.0;
  }
}

void flat(std::vector<Sample>& s, uint32_t& t, double cg, uint32_t durMs,
          uint32_t stepMs = 150) {
  const uint32_t end = t + durMs;
  while (t <= end) {
    s.push_back(Sample{t, static_cast<int16_t>(cg)});
    t += stepMs;
  }
}

void testSustainedPourRecognized() {
  std::vector<Sample> s;
  uint32_t t = 0;
  double cg = 0;
  ramp(s, t, cg, 200.0, 8000);
  const auto e = computePourEvidence(s.data(), s.size());
  CHECK(e.hasPour);
  CHECK(e.pourMs >= 6500);
  CHECK(e.gainCg >= 1300);
  CHECK(e.gainCg <= 1800);
}

void testStartWeightIsLastFlatSampleBeforePour() {
  std::vector<Sample> s;
  uint32_t t = 0;
  double cg = 0;
  flat(s, t, 0.0, 3000);
  ramp(s, t, cg, 200.0, 8000);
  flat(s, t, cg, 3000);
  const auto e = computePourEvidence(s.data(), s.size());
  CHECK(e.hasStartRawCg);
  CHECK(e.startRawCg == 0);
  CHECK(e.gainCg >= 1550);
  CHECK(e.gainCg <= 1650);
}

void testSmoothedSettledLagDoesNotMoveStartWeightIntoPour() {
  std::vector<Sample> s;
  uint32_t t = 0;
  flat(s, t, 0.0, 2200, 100);
  double cg = 25;
  ramp(s, t, cg, 600.0, 2000, 100);
  const auto e = computePourEvidence(s.data(), s.size());
  CHECK(e.hasPour);
  CHECK(e.hasStartRawCg);
  CHECK(e.startRawCg == 0);
}

void testTransientThenFlatPlateauSeedsPourStart() {
  // A large transient decays into a flat zero before the pour. The flat samples
  // are the trustworthy starting weight even while smoothed flow is settling.
  std::vector<Sample> s{
      {0, 2800},   {100, 4000}, {200, 3600}, {300, 3000},
      {400, 1000}, {500, 300},  {600, 50},   {700, 0},
  };
  uint32_t t = 800;
  flat(s, t, 0.0, 2000, 100);
  double cg = 0;
  ramp(s, t, cg, 400.0, 1500, 100);
  const auto e = computePourEvidence(s.data(), s.size());
  CHECK(e.hasPour);
  CHECK(e.hasStartRawCg);
  CHECK(e.startRawCg == 0);
}

void testLiftFreezesGainNearPreLiftPeak() {
  // A 25 g pour, then the cup comes off over 1 s. The evidence must freeze
  // near the pre-lift weight instead of following the collapse partway down
  // while the smoothed flow transits Settled/Tail on its way to
  // Discontinuity.
  std::vector<Sample> s;
  uint32_t t = 0;
  double cg = 0;
  flat(s, t, 0.0, 2000);
  ramp(s, t, cg, 178.6, 14000);   // ~25 g over 14 s
  ramp(s, t, cg, -2500.0, 1000);  // lift: -25 g/s
  flat(s, t, 0.0, 5000);
  const auto e = computePourEvidence(s.data(), s.size());
  CHECK(e.hasPour);
  CHECK(e.gainCg >= 2350);
  CHECK(e.gainCg <= 2600);
}

void testPostPumpDiscontinuitySelectsPrecedingEndpoint() {
  std::vector<Sample> s;
  uint32_t t = 0;
  double cg = 0;
  flat(s, t, 0.0, 2000);
  ramp(s, t, cg, 390.0, 12000);  // pour to about 47 g
  flat(s, t, cg, 1200);

  // Some scale modes alternate between positive and negative tare readings
  // after the cup is lifted. End on the positive phase: its sign alone would
  // otherwise make the invalid 71.4 g endpoint look plausible.
  for (int cycle = 0; cycle < 3; ++cycle) {
    flat(s, t, 7140.0, 450);
    flat(s, t, -7140.0, 450);
  }
  flat(s, t, 7140.0, 600);

  int16_t endpoint = 0;
  CHECK(pump_scale::endpointBeforePostPumpDiscontinuity(s.data(), s.size(),
                                                        15000, endpoint));
  CHECK(endpoint >= 4600 && endpoint <= 4800);
}

void testFastPostPumpTailMovementKeepsFinalEndpoint() {
  // The steep-slope flow break catches this 0.8 g drop, but endpoint selection
  // requires a material raw step so ordinary tail motion remains.
  const Sample s[] = {{900, 4000}, {1000, 4000}, {1100, 3920}};
  int16_t endpoint = 0;
  CHECK(!pump_scale::endpointBeforePostPumpDiscontinuity(s, 3, 1000, endpoint));
}

void testUpwardNudgeDoesNotInflateGain() {
  // A one-sample +24 g spike mid-pour (springing back over the next samples)
  // must not ratchet the run's peak: the spike step is untrusted.
  std::vector<Sample> s;
  uint32_t t = 0;
  double cg = 0;
  flat(s, t, 0.0, 2000);
  ramp(s, t, cg, 180.0, 10000);  // pour to ~18 g
  const int16_t base = static_cast<int16_t>(cg);
  s.push_back(Sample{t, static_cast<int16_t>(base + 2400)});
  t += 150;
  s.push_back(Sample{t, static_cast<int16_t>(base + 1200)});
  t += 150;
  ramp(s, t, cg, 180.0, 11000);  // pour on to ~38 g
  flat(s, t, cg, 3000);
  const auto e = computePourEvidence(s.data(), s.size());
  CHECK(e.hasPour);
  CHECK(e.gainCg >= 3600);
  CHECK(e.gainCg <= 4100);  // the +24 g spike must not be counted
}

void testTailJitterDoesNotRatchetGain() {
  // Small alternating jitter after the pour must not ratchet the peak by more
  // than the jitter amplitude.
  std::vector<Sample> s;
  uint32_t t = 0;
  double cg = 0;
  ramp(s, t, cg, 300.0, 10000);  // pour to ~30 g
  const int16_t base = static_cast<int16_t>(cg);
  for (int i = 0; i < 20; ++i) {
    s.push_back(Sample{t, static_cast<int16_t>(base + ((i & 1) ? 2 : -2))});
    t += 150;
  }
  const auto e = computePourEvidence(s.data(), s.size());
  CHECK(e.hasPour);
  CHECK(e.gainCg >= 2900);
  CHECK(e.gainCg <= 3040);
}

void testNoFlowRejected() {
  std::vector<Sample> s;
  uint32_t t = 0;
  flat(s, t, -8300.0, 20000);
  const auto e = computePourEvidence(s.data(), s.size());
  CHECK(!e.hasPour);
  CHECK(!e.hasStartRawCg);
  CHECK(e.pourMs == 0);
  CHECK(e.gainCg == 0);
}

void testDripTailIncludedInGain() {
  std::vector<Sample> s;
  uint32_t t = 0;
  double cg = 0;
  ramp(s, t, cg, 500.0, 6000);
  ramp(s, t, cg, 15.0, 2000);
  flat(s, t, cg, 3000);
  const auto e = computePourEvidence(s.data(), s.size());
  CHECK(e.hasPour);
  CHECK(e.gainCg >= 2900);
  CHECK(e.gainCg <= 3300);
}

void testTransientBumpDoesNotCorruptGain() {
  std::vector<Sample> s;
  uint32_t t = 0;
  double cg = 0;
  ramp(s, t, cg, 200.0, 4000);
  s.push_back(Sample{t, static_cast<int16_t>(cg - 700)});
  t += 150;
  ramp(s, t, cg, 200.0, 4000);
  flat(s, t, cg, 3000);
  const auto e = computePourEvidence(s.data(), s.size());
  CHECK(e.hasPour);
  CHECK(e.pourMs >= 6500);
  CHECK(e.gainCg >= 1400);
  CHECK(e.gainCg <= 1800);
}

void testSplitPourSumsRuns() {
  std::vector<Sample> s;
  uint32_t t = 0;
  double cg = 0;
  ramp(s, t, cg, 500.0, 5000);
  flat(s, t, cg, 4000);
  ramp(s, t, cg, 500.0, 5000);
  flat(s, t, cg, 3000);
  const auto e = computePourEvidence(s.data(), s.size());
  CHECK(e.hasPour);
  CHECK(e.gainCg >= 4400);
  CHECK(e.pourMs >= 8000);
}

}  // namespace

int main() {
  testSustainedPourRecognized();
  testStartWeightIsLastFlatSampleBeforePour();
  testSmoothedSettledLagDoesNotMoveStartWeightIntoPour();
  testTransientThenFlatPlateauSeedsPourStart();
  testLiftFreezesGainNearPreLiftPeak();
  testPostPumpDiscontinuitySelectsPrecedingEndpoint();
  testFastPostPumpTailMovementKeepsFinalEndpoint();
  testUpwardNudgeDoesNotInflateGain();
  testTailJitterDoesNotRatchetGain();
  testNoFlowRejected();
  testDripTailIncludedInGain();
  testTransientBumpDoesNotCorruptGain();
  testSplitPourSumsRuns();
  if (g_failures == 0) {
    std::printf("OK: all assertions passed\n");
    return 0;
  }
  std::printf("%d assertion(s) failed\n", g_failures);
  return 1;
}
