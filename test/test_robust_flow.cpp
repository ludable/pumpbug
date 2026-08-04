// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

// Unit tests for the bump-robust flow series shared by device and browser
// charts and by flow statistics.

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "RobustFlow.h"

namespace {

using pump_scale::computeFlowSeriesStats;
using pump_scale::robustSampleFlow;
using pump_scale::Sample;

int g_failures = 0;

#define CHECK(cond)                                               \
  do {                                                            \
    if (!(cond)) {                                                \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                               \
    }                                                             \
  } while (0)

bool near(float a, float b, float tol) {
  return (a - b < tol) && (b - a < tol);
}

struct ReferenceRow {
  Sample sample;
  bool hasFlow;
  float flow;
};

struct ReferenceVector {
  std::vector<ReferenceRow> rows;
  bool statsHave = false;
  float sustainedPeakGPerS = 0.0f;
};

ReferenceVector loadReferenceVector(const char* path) {
  ReferenceVector out;
  std::ifstream in(path);
  if (!in) {
    std::printf("FAIL could not open robust-flow reference data: %s\n", path);
    ++g_failures;
    return out;
  }

  std::string line;
  while (std::getline(in, line)) {
    if (line.empty()) continue;
    if (line[0] == '#') {
      std::istringstream meta(line.substr(1));
      std::string key;
      meta >> key;
      if (key == "stats_have") {
        int v = 0;
        meta >> v;
        out.statsHave = v != 0;
      } else if (key == "sustained_peak_g_s") {
        meta >> out.sustainedPeakGPerS;
      }
      continue;
    }
    if (line.rfind("t_ms", 0) == 0) continue;

    std::istringstream row(line);
    std::string tMsS, cgS, scaleTimerS, flowS;
    if (!std::getline(row, tMsS, '\t') || !std::getline(row, cgS, '\t') ||
        !std::getline(row, scaleTimerS, '\t') ||
        !std::getline(row, flowS, '\t')) {
      std::printf("FAIL malformed robust-flow reference row: %s\n",
                  line.c_str());
      ++g_failures;
      continue;
    }
    ReferenceRow r;
    r.sample.tMs =
        static_cast<uint32_t>(std::strtoul(tMsS.c_str(), nullptr, 10));
    r.sample.cg = static_cast<int16_t>(std::strtol(cgS.c_str(), nullptr, 10));
    r.sample.scaleTimerMs = scaleTimerS == "-"
                                ? scale_time::UNKNOWN_MS
                                : static_cast<uint32_t>(std::strtoul(
                                      scaleTimerS.c_str(), nullptr, 10));
    r.hasFlow = flowS != "null";
    r.flow = r.hasFlow ? std::strtof(flowS.c_str(), nullptr) : 0.0f;
    out.rows.push_back(r);
  }
  return out;
}

void testSharedReferenceVector(const char* path) {
  const ReferenceVector reference = loadReferenceVector(path);
  CHECK(!reference.rows.empty());
  std::vector<Sample> samples;
  for (const ReferenceRow& r : reference.rows) samples.push_back(r.sample);

  int hint = 0;
  for (size_t i = 0; i < reference.rows.size(); ++i) {
    float f = 0.0f;
    const bool gotFlow =
        robustSampleFlow(samples.data(), samples.size(), i, hint, f);
    CHECK(gotFlow == reference.rows[i].hasFlow);
    if (gotFlow && reference.rows[i].hasFlow) {
      CHECK(near(f, reference.rows[i].flow, 0.0001f));
    }
  }

  const auto stats = computeFlowSeriesStats(samples.data(), samples.size());
  CHECK(stats.have == reference.statsHave);
  if (stats.have && reference.statsHave) {
    CHECK(
        near(stats.sustainedPeakGPerS, reference.sustainedPeakGPerS, 0.0001f));
  }
}

std::vector<Sample> bumpShotSamples() {
  std::vector<Sample> out{
      {0, 0},     {100, 2300}, {200, 2050}, {300, 1600},
      {400, 800}, {500, 0},
  };
  for (uint32_t t = 600; t <= 2200; t += 100) out.push_back({t, 0});

  int16_t cg = 0;
  for (uint32_t t = 2300; t <= 16300; t += 100) {
    cg += 30;
    out.push_back({t, cg});
  }
  for (uint32_t t = 16400; t <= 18400; t += 100) out.push_back({t, cg});
  return out;
}

// A large bump collapses to zero before a steady pour. The collapse must not
// appear as negative flow.
void testCupBumpNotRenderedAsFlow() {
  const std::vector<Sample> s = bumpShotSamples();
  int hint = 0;
  float worstNeg = 0.0f;
  for (size_t i = 0; i < s.size(); ++i) {
    float f;
    if (!robustSampleFlow(s.data(), s.size(), i, hint, f)) continue;
    if (f < worstNeg) worstNeg = f;
  }
  CHECK(worstNeg > -0.5f);

  int h2 = 0;
  float f;
  CHECK(!robustSampleFlow(s.data(), s.size(), 0, h2, f));
  CHECK(!robustSampleFlow(s.data(), s.size(), 2, h2, f));
  CHECK(!robustSampleFlow(s.data(), s.size(), 5, h2, f));
}

// The sustained peak must describe the pour rather than the initial bump.
void testSustainedPeakIsPourPlateauNotBump() {
  const std::vector<Sample> s = bumpShotSamples();
  const auto stats = computeFlowSeriesStats(s.data(), s.size());
  CHECK(stats.have);
  CHECK(stats.sustainedPeakGPerS < 5.0f);
  CHECK(near(stats.sustainedPeakGPerS, 3.0f, 0.1f));
}

// A lone flow spike on an otherwise steady pour must not set the sustained
// peak: the hold window's minimum trims it, so the reported peak stays at the
// plateau.
void testLoneSpikeDoesNotSetPeak() {
  std::vector<Sample> s;
  int cg = 0;
  for (int i = 0; i < 40; ++i) {
    // Steady ~1 g/s ramp (15 cg / 150 ms), with one 1-sample jump partway that
    // briefly reads as fast flow but is small enough not to be a non-pour step.
    int step = (i == 20) ? 40 : 15;
    cg += step;
    s.push_back(
        Sample{static_cast<uint32_t>(i * 150), static_cast<int16_t>(cg)});
  }
  const auto stats = computeFlowSeriesStats(s.data(), s.size());
  CHECK(stats.have);
  // The plateau is ~1 g/s; the lone fast sample must not drag the peak up near
  // the spike's instantaneous rate.
  CHECK(stats.sustainedPeakGPerS < 1.4f);
}

// A burst shorter than the hold window must NOT set the sustained peak, even
// when the sample stream starts at tMs 0 (a relative/normalized timebase, as
// replay and the test fixtures use). Regression for the cutoff-clamp bug: with
// t < kPeakHoldMs the eviction cutoff floors to 0, so a `runStartT <= cutoff`
// guard would fire for a run starting at t==0 and score the partial window.
void testSubHoldBurstAtTimeZeroNotScored() {
  std::vector<Sample> s;
  int cg = 0;
  for (int i = 0; i < 6; ++i) {  // ~4 g/s burst over the first ~750 ms
    cg += 60;
    s.push_back(
        Sample{static_cast<uint32_t>(i * 150), static_cast<int16_t>(cg)});
  }
  for (int i = 6; i < 40; ++i) {  // ~0.67 g/s for the rest
    cg += 10;
    s.push_back(
        Sample{static_cast<uint32_t>(i * 150), static_cast<int16_t>(cg)});
  }
  const auto stats = computeFlowSeriesStats(s.data(), s.size());
  CHECK(stats.have);
  // The 4 g/s burst lasts under the 700 ms hold, so it must not be the peak.
  CHECK(stats.sustainedPeakGPerS < 3.0f);
}

// A clean steady ramp is normal pour flow and must pass through untouched.
void testCleanRampEstimable() {
  std::vector<Sample> s;
  for (int i = 0; i < 20; ++i) {
    s.push_back(
        Sample{static_cast<uint32_t>(i * 150),
               static_cast<int16_t>(i * 15)});  // 0.15 g / 150 ms = 1 g/s
  }
  int hint = 0;
  float f;
  CHECK(robustSampleFlow(s.data(), s.size(), 10, hint, f));
  CHECK(near(f, 1.0f, 0.05f));
}

// A placement step (a cup set on the scale) is a non-pour transient: windows
// that span it read "no flow", while the flat stretches on either side don't.
void testPlacementStepSuppressed() {
  std::vector<Sample> s;
  for (int i = 0; i < 10; ++i)
    s.push_back(Sample{static_cast<uint32_t>(i * 150), 0});
  for (int i = 10; i < 20; ++i)
    s.push_back(
        Sample{static_cast<uint32_t>(i * 150), 800});  // +8 g step at i=10
  int hint = 0;
  float f;
  // A sample whose ±500 ms window contains the step edge: suppressed.
  CHECK(!robustSampleFlow(s.data(), s.size(), 10, hint, f));
  // Flat well after the step: estimable and ~0 flow.
  int h2 = 0;
  CHECK(robustSampleFlow(s.data(), s.size(), 18, h2, f));
  CHECK(near(f, 0.0f, 0.05f));
}

// A bump collapse that arrives as a run of fast SMALL drops — each below the 3
// g single-sample magnitude, but together a slope steeper than -5 g/s — must
// still be rejected. This is the case the fixed-magnitude downward test alone
// misses; the steep-slope half of the break predicate covers it.
void testFastSubThreeGramCollapseSuppressed() {
  // Each step is -1 g over 150 ms = -6.7 g/s: steeper than the discontinuity
  // rate, but only 1 g, so isNonPourStep on its own would not flag it.
  CHECK(!pump_scale::isNonPourStep(-100, 150));
  CHECK(pump_scale::isFlowBreakEdge(-100, 150));

  std::vector<Sample> s;
  int cg = 1500;
  for (int i = 0; i < 8; ++i)  // flat
    s.push_back(Sample{static_cast<uint32_t>(i * 150), 1500});
  for (int i = 8; i < 16; ++i) {  // steep small-step collapse
    cg -= 100;
    s.push_back(
        Sample{static_cast<uint32_t>(i * 150), static_cast<int16_t>(cg)});
  }
  for (int i = 16; i < 24; ++i)  // flat
    s.push_back(
        Sample{static_cast<uint32_t>(i * 150), static_cast<int16_t>(cg)});
  int hint = 0;
  float f;
  // A sample whose window spans the collapse reads no flow rather than a large
  // negative.
  CHECK(!robustSampleFlow(s.data(), s.size(), 11, hint, f));
}

// A tare / cup lift is a sudden downward step and is likewise rejected.
void testTareStepSuppressed() {
  std::vector<Sample> s;
  for (int i = 0; i < 10; ++i)
    s.push_back(Sample{static_cast<uint32_t>(i * 150), 1000});
  for (int i = 10; i < 20; ++i)
    s.push_back(
        Sample{static_cast<uint32_t>(i * 150), 200});  // -8 g drop at i=10
  int hint = 0;
  float f;
  CHECK(!robustSampleFlow(s.data(), s.size(), 10, hint, f));
}

// The per-edge break test must use the scale timer (when present), not raw host
// arrival time, so BLE burst delivery can't fake a steep transient. Regression
// for the flowDeltaMs fix.
void testBreakTestUsesScaleTimerNotHostJitter() {
  // Acaia-style: two readings 100 ms apart in scale time, delivered ~5 ms apart
  // in host time (a BLE burst). A benign -0.15 g (-15 cg) drip edge.
  Sample s[2];
  s[0] = Sample{0, 1000, 0};   // tMs, cg, scaleTimerMs
  s[1] = Sample{5, 985, 100};  // host +5 ms, scale +100 ms
  // Over the 100 ms scale dt this is -1.5 g/s, well shy of the -5 g/s break, so
  // the window must NOT read a break. Over the 5 ms host gap it would look like
  // -300 g/s and be wrongly flagged.
  CHECK(!pump_scale::windowCrossesBreak(s, 0, 1));
  CHECK(pump_scale::isSteepDownwardSlope(-15, 5));     // host gap: looks steep
  CHECK(!pump_scale::isSteepDownwardSlope(-15, 100));  // scale dt: not steep
}

}  // namespace

int main(int argc, char** argv) {
  const char* referencePath =
      argc > 1 ? argv[1] : "test/robust_flow_reference.tsv";
  testSharedReferenceVector(referencePath);
  testCupBumpNotRenderedAsFlow();
  testSustainedPeakIsPourPlateauNotBump();
  testLoneSpikeDoesNotSetPeak();
  testSubHoldBurstAtTimeZeroNotScored();
  testCleanRampEstimable();
  testPlacementStepSuppressed();
  testFastSubThreeGramCollapseSuppressed();
  testTareStepSuppressed();
  testBreakTestUsesScaleTimerNotHostJitter();
  if (g_failures == 0) {
    std::printf("OK: all assertions passed\n");
    return 0;
  }
  std::printf("%d assertion(s) failed\n", g_failures);
  return 1;
}
