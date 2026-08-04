// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

// Host-compilable unit tests for LivePourTracker.

#include <cstdio>

#include "LivePourTracker.h"

namespace {

int g_failures = 0;

#define CHECK(cond)                                               \
  do {                                                            \
    if (!(cond)) {                                                \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                               \
    }                                                             \
  } while (0)

void ramp(pump_scale::LivePourTracker& tkr, uint32_t& t, double& cg,
          double rateCgPerS, uint32_t durMs, uint32_t stepMs = 150) {
  const uint32_t end = t + durMs;
  while (t <= end) {
    tkr.update(t, static_cast<int16_t>(cg));
    t += stepMs;
    cg += rateCgPerS * stepMs / 1000.0;
  }
}

void flat(pump_scale::LivePourTracker& tkr, uint32_t& t, double cg,
          uint32_t durMs, uint32_t stepMs = 150) {
  const uint32_t end = t + durMs;
  while (t <= end) {
    tkr.update(t, static_cast<int16_t>(cg));
    t += stepMs;
  }
}

void testPourStartAndMeaningfulYield() {
  pump_scale::LivePourTracker tkr;
  uint32_t t = 0;
  double cg = 0;
  flat(tkr, t, 0.0, 3000);
  ramp(tkr, t, cg, 200.0, 8000);
  CHECK(tkr.hasPourStartWeight());
  CHECK(tkr.firstPourStartRawCg() >= 0);
  CHECK(tkr.firstPourStartRawCg() <= 50);
  CHECK(tkr.hasMeaningfulYield());
}

void testTrustedSampleFreezesAcrossKnock() {
  pump_scale::LivePourTracker tkr;
  uint32_t t = 0;
  double cg = 0;
  ramp(tkr, t, cg, 200.0, 4000);
  CHECK(tkr.lastSampleWasTrusted());
  const int16_t before = tkr.lastTrustedRawCg();
  CHECK(before > 700);

  tkr.update(t, before + 2000);
  t += 120;
  CHECK(!tkr.lastSampleWasTrusted());
  CHECK(tkr.lastTrustedRawCg() == before);

  tkr.update(t, before);
  t += 120;
  CHECK(!tkr.lastSampleWasTrusted());
  tkr.update(t, before + 20);
  CHECK(tkr.lastSampleWasTrusted());
  CHECK(tkr.lastTrustedRawCg() == before + 20);
}

void testKnockDoesNotSetMeaningfulYield() {
  pump_scale::LivePourTracker tkr;
  uint32_t t = 0;
  double cg = 0;
  flat(tkr, t, 0.0, 3000);
  ramp(tkr, t, cg, 200.0, 900);
  CHECK(tkr.hasPourStartWeight());
  CHECK(!tkr.hasMeaningfulYield());

  const int16_t before = tkr.lastTrustedRawCg();
  tkr.update(t, before + 500);
  t += 150;
  CHECK(!tkr.lastSampleWasTrusted());
  CHECK(!tkr.hasMeaningfulYield());

  tkr.update(t, before);
  t += 150;
  CHECK(!tkr.lastSampleWasTrusted());
  CHECK(!tkr.hasMeaningfulYield());
}

void testPrePourKnockDoesNotSetPourStart() {
  pump_scale::LivePourTracker tkr;
  uint32_t t = 0;
  flat(tkr, t, 0.0, 2000);
  tkr.update(t, 1800);
  t += 150;
  tkr.update(t, 900);
  t += 150;
  flat(tkr, t, 0.0, 2000);

  double cg = 0;
  ramp(tkr, t, cg, 200.0, 8000);
  CHECK(tkr.hasPourStartWeight());
  CHECK(tkr.firstPourStartRawCg() == 0);
}

void testSettledCountReachedAfterPour() {
  pump_scale::LivePourTracker tkr;
  uint32_t t = 0;
  double cg = 0;
  ramp(tkr, t, cg, 500.0, 4000);
  CHECK(tkr.hasMeaningfulYield());
  CHECK(tkr.hasUnsettledPour());
  flat(tkr, t, cg, 3000);
  CHECK(tkr.settledCountReached());
  CHECK(!tkr.hasUnsettledPour());
}

}  // namespace

int main() {
  testPourStartAndMeaningfulYield();
  testTrustedSampleFreezesAcrossKnock();
  testKnockDoesNotSetMeaningfulYield();
  testPrePourKnockDoesNotSetPourStart();
  testSettledCountReachedAfterPour();
  if (g_failures == 0) {
    std::printf("OK: all assertions passed\n");
    return 0;
  }
  std::printf("%d assertion(s) failed\n", g_failures);
  return 1;
}
