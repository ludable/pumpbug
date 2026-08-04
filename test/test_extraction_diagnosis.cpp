// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

// Host-compilable unit tests for ExtractionDiagnosis::isLikelyRealShot.
//
// A real extraction has sustained pour-band flow and enough accumulated gain.
// The recorder computes those facts from raw weight; this file tests only the
// final thresholds. Recorder tests cover the flow classification itself.

#include <cstdio>

#include "ExtractionDiagnosis.h"

namespace {

int g_failures = 0;

#define CHECK(cond)                                               \
  do {                                                            \
    if (!(cond)) {                                                \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                               \
    }                                                             \
  } while (0)

pump_scale::Extraction plausibleShot() {
  pump_scale::Extraction e{};
  e.beginMs = 1000;
  e.lastPumpOffMs = 9000;
  e.endMs = 12000;
  e.totalPumpOnMs = 8000;
  e.observedSampleCount = 40;
  e.pourMs = 10000;         // 10 s of sustained pour-band flow
  e.decisionGainCg = 4000;  // 40 g accumulated while flowing
  return e;
}

void testSustainedPourAccepted() {
  CHECK(pump_scale::isLikelyRealShot(plausibleShot()));
}

void testNoFlowGrinderRejected() {
  // A grinder: flat weight -> the flow never entered the pour band, so the
  // recorder accumulated no in-band time or yield.
  pump_scale::Extraction e = plausibleShot();
  e.pourMs = 0;
  e.decisionGainCg = 0;
  CHECK(!pump_scale::isLikelyRealShot(e));
}

void testBriefInBandPlacementRejected() {
  // A cup of grounds placed on the scale: the step itself is over-ceiling
  // (excluded from yield), leaving only a brief in-band settle. Not sustained.
  pump_scale::Extraction e = plausibleShot();
  e.pourMs = 1500;          // ~1.5 s, below the sustained floor
  e.decisionGainCg = 1800;  // even with weight present
  CHECK(!pump_scale::isLikelyRealShot(e));
}

void testTooLittleYieldRejected() {
  pump_scale::Extraction e = plausibleShot();
  e.decisionGainCg = 500;  // 5 g, below the yield floor
  CHECK(!pump_scale::isLikelyRealShot(e));
}

void testBoundaryAccepted() {
  // Exactly at both thresholds graduates (guards against accidental
  // tightening).
  pump_scale::Extraction e = plausibleShot();
  e.pourMs = 5000;
  e.decisionGainCg = 1000;
  CHECK(pump_scale::isLikelyRealShot(e));
}

}  // namespace

int main() {
  testSustainedPourAccepted();
  testNoFlowGrinderRejected();
  testBriefInBandPlacementRejected();
  testTooLittleYieldRejected();
  testBoundaryAccepted();
  if (g_failures == 0) {
    std::printf("OK: all assertions passed\n");
    return 0;
  }
  std::printf("%d assertion(s) failed\n", g_failures);
  return 1;
}
