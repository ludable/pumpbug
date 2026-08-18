// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#include <cstdio>
#include <limits>

#include "vibration/PumpDecayTracker.h"

namespace {

int failures = 0;

#define CHECK(cond)                                               \
  do {                                                            \
    if (!(cond)) {                                                \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++failures;                                                 \
    }                                                             \
  } while (0)

void establishReference(PumpDecayTracker& tracker, uint32_t& nowMs) {
  tracker.step(nowMs, false, true, true, 20.0f);
  for (size_t i = 1; i < PumpDecayTracker::MIN_REFERENCE_FRAMES; ++i) {
    nowMs += 160;
    tracker.step(nowMs, true, true, true, 20.0f);
  }
}

void testCarriesOnsetThroughConfirmation() {
  PumpDecayTracker tracker;
  uint32_t nowMs = 1000;
  establishReference(tracker, nowMs);

  nowMs += 160;
  auto result = tracker.step(nowMs, true, true, true, 15.0f);
  CHECK(result.state == PumpSignalState::DecayCandidate);
  CHECK(result.isOn());
  CHECK(!result.hasAcceptedDecayOnset);
  const uint32_t onsetMs = nowMs;

  nowMs += 640;
  result = tracker.step(nowMs, true, false, false, 0.0f);
  CHECK(result.state == PumpSignalState::Off);
  CHECK(!result.isOn());
  CHECK(result.hasAcceptedDecayOnset);
  CHECK(result.acceptedDecayOnsetMs == onsetMs);

  nowMs += 160;
  result = tracker.step(nowMs, false, false, false, 0.0f);
  CHECK(result.state == PumpSignalState::Off);
  CHECK(result.hasAcceptedDecayOnset);
  CHECK(result.acceptedDecayOnsetMs == onsetMs);

  nowMs += 160;
  result = tracker.step(nowMs, false, true, true, 20.0f);
  CHECK(result.state == PumpSignalState::On);
  CHECK(!result.hasAcceptedDecayOnset);
  CHECK(result.acceptedDecayOnsetMs == 0);
}

void testRecoveryClearsCandidate() {
  PumpDecayTracker tracker;
  uint32_t nowMs = 2000;
  establishReference(tracker, nowMs);

  nowMs += 160;
  CHECK(tracker.step(nowMs, true, true, false, 0.0f).isDecayCandidate());
  nowMs += 160;
  const auto result = tracker.step(nowMs, true, true, true, 18.0f);
  CHECK(result.state == PumpSignalState::On);
  CHECK(!result.hasAcceptedDecayOnset);
}

void testExpiryClearsCandidate() {
  PumpDecayTracker tracker;
  uint32_t nowMs = 3000;
  establishReference(tracker, nowMs);

  nowMs += 160;
  CHECK(tracker.step(nowMs, true, true, true, 15.0f).isDecayCandidate());
  nowMs += PumpDecayTracker::MAX_CARRY_MS + 1;
  const auto result = tracker.step(nowMs, true, true, false, 0.0f);
  CHECK(result.state == PumpSignalState::On);
  CHECK(!result.hasAcceptedDecayOnset);
}

void testShortReferenceCannotStartCandidate() {
  PumpDecayTracker tracker;
  uint32_t nowMs = 4000;
  tracker.step(nowMs, false, true, true, 20.0f);
  for (size_t i = 1; i < PumpDecayTracker::MIN_REFERENCE_FRAMES - 1; ++i) {
    nowMs += 160;
    tracker.step(nowMs, true, true, true, 20.0f);
  }
  nowMs += 160;
  const auto result = tracker.step(nowMs, true, true, false, 0.0f);
  CHECK(result.state == PumpSignalState::On);
  CHECK(!result.hasAcceptedDecayOnset);
}

void testMillisRolloverExpiry() {
  PumpDecayTracker tracker;
  uint32_t nowMs = std::numeric_limits<uint32_t>::max() - 2600;
  establishReference(tracker, nowMs);
  nowMs += 160;
  const uint32_t onsetMs = nowMs;
  CHECK(tracker.step(onsetMs, true, true, false, 0.0f).isDecayCandidate());
  nowMs += PumpDecayTracker::MAX_CARRY_MS + 1;
  CHECK(nowMs < onsetMs);
  CHECK(!tracker.step(nowMs, true, true, false, 0.0f).isDecayCandidate());
}

}  // namespace

int main() {
  testCarriesOnsetThroughConfirmation();
  testRecoveryClearsCandidate();
  testExpiryClearsCandidate();
  testShortReferenceCannotStartCandidate();
  testMillisRolloverExpiry();
  if (failures == 0) {
    std::printf("OK: all assertions passed\n");
    return 0;
  }
  std::printf("%d assertion(s) failed\n", failures);
  return 1;
}
