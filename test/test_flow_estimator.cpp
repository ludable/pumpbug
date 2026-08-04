// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

// Host-compilable unit tests for FlowEstimator (the shared windowed-slope flow
// model used by TargetAlert and shot recognition).

#include <cstdio>

#include "FlowEstimator.h"

namespace {

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

void testTooFewPointsInvalid() {
  pump_scale::FlowEstimator f;
  f.push(0, 0);
  f.push(100, 100);
  float out;
  CHECK(!f.flow(100, out));  // only 2 points (< kMinPoints)
}

void testSpanTooSmallInvalid() {
  pump_scale::FlowEstimator f;
  // 3 points but only 100 ms spread (< kMinSpanMs = 200).
  f.push(0, 0);
  f.push(50, 5);
  f.push(100, 10);
  float out;
  CHECK(!f.flow(100, out));
}

void testSteadyRiseSlope() {
  // 2 g/s = 200 cg/s, sampled every 150 ms.
  pump_scale::FlowEstimator f;
  for (int i = 0; i <= 8; ++i) {
    f.push(static_cast<uint32_t>(i * 150),
           static_cast<int16_t>(i * 30));  // +30 cg per 150 ms = 200 cg/s
  }
  float out = 0.0f;
  CHECK(f.flow(8 * 150, out));
  CHECK(near(out, 200.0f, 25.0f));  // EMA lags slightly; ~200 cg/s
}

void testFlatIsZeroFlow() {
  pump_scale::FlowEstimator f;
  for (int i = 0; i <= 8; ++i) f.push(static_cast<uint32_t>(i * 150), -8300);
  float out = 999.0f;
  CHECK(f.flow(8 * 150, out));
  CHECK(near(out, 0.0f, 5.0f));  // flat weight -> ~0 flow (caller floors it)
}

void testDropoutInvalid() {
  pump_scale::FlowEstimator f;
  for (int i = 0; i <= 8; ++i) f.push(static_cast<uint32_t>(i * 150), i * 30);
  float out;
  // newest sample at t=1200; now far past the 750 ms window -> stale.
  CHECK(!f.flow(3000, out));
}

void testWindowSurvivesMillisWrap() {
  // 2 g/s = 200 cg/s across the uint32_t millisecond wrap. Pre-wrap samples are
  // only 150/300 ms older than the post-wrap newest sample, so they must remain
  // in the trailing regression window.
  pump_scale::FlowEstimator f;
  const uint32_t t0 = UINT32_MAX - 299;  // t0 + 300 wraps to 0
  f.push(t0, 0);
  f.push(t0 + 150, 30);
  f.push(t0 + 300, 60);

  float out = 0.0f;
  CHECK(f.flow(t0 + 300, out));
  CHECK(near(out, 200.0f, 25.0f));
}

void testResetClears() {
  pump_scale::FlowEstimator f;
  for (int i = 0; i <= 8; ++i) f.push(static_cast<uint32_t>(i * 150), i * 30);
  f.reset();
  float out;
  CHECK(!f.flow(0, out));  // no data after reset
}

}  // namespace

int main() {
  testTooFewPointsInvalid();
  testSpanTooSmallInvalid();
  testSteadyRiseSlope();
  testFlatIsZeroFlow();
  testDropoutInvalid();
  testWindowSurvivesMillisWrap();
  testResetClears();
  if (g_failures == 0) {
    std::printf("OK: all assertions passed\n");
    return 0;
  }
  std::printf("%d assertion(s) failed\n", g_failures);
  return 1;
}
