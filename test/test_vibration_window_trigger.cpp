// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#include <cassert>
#include <cmath>
#include <cstdio>
#include <limits>

#include "vibration/VibrationWindowTrigger.h"

namespace {

constexpr float kPumpHz = 110.0f;

void opensAfterConsecutiveEntryFrames() {
  VibrationWindowTrigger trigger;

  assert(!trigger.step(true, VibrationWindowTrigger::SNR_ON_DB, kPumpHz));
  assert(trigger.step(true, VibrationWindowTrigger::SNR_ON_DB, kPumpHz));
  assert(trigger.active());
}

void entryFailuresRestartTheCount() {
  VibrationWindowTrigger trigger;

  assert(!trigger.step(true, VibrationWindowTrigger::SNR_ON_DB, kPumpHz));
  assert(!trigger.step(false, VibrationWindowTrigger::SNR_ON_DB, kPumpHz));
  assert(!trigger.step(true, VibrationWindowTrigger::SNR_ON_DB, kPumpHz));
  assert(trigger.step(true, VibrationWindowTrigger::SNR_ON_DB, kPumpHz));
}

void staysOpenAtTheLowerThreshold() {
  VibrationWindowTrigger trigger;
  trigger.step(true, VibrationWindowTrigger::SNR_ON_DB, kPumpHz);
  trigger.step(true, VibrationWindowTrigger::SNR_ON_DB, kPumpHz);

  for (uint8_t i = 0; i < VibrationWindowTrigger::N_OFF - 1; ++i) {
    assert(trigger.step(false, VibrationWindowTrigger::SNR_STAY_DB, kPumpHz));
  }
  assert(trigger.step(true, VibrationWindowTrigger::SNR_STAY_DB, kPumpHz));
  for (uint8_t i = 0; i < VibrationWindowTrigger::N_OFF - 1; ++i) {
    assert(trigger.step(true, VibrationWindowTrigger::SNR_STAY_DB - 0.1f,
                        kPumpHz));
  }
  assert(!trigger.step(true, VibrationWindowTrigger::SNR_STAY_DB - 0.1f,
                       kPumpHz));
}

void rejectsInvalidMeasurements() {
  const float nan = std::numeric_limits<float>::quiet_NaN();
  const float infinity = std::numeric_limits<float>::infinity();

  VibrationWindowTrigger trigger;
  assert(!trigger.step(true, nan, kPumpHz));
  assert(!trigger.step(true, infinity, kPumpHz));
  assert(!trigger.step(true, VibrationWindowTrigger::SNR_ON_DB,
                       VibrationWindowTrigger::PEAK_MIN_HZ - 0.1f));
  assert(!trigger.step(true, VibrationWindowTrigger::SNR_ON_DB,
                       VibrationWindowTrigger::PEAK_MAX_HZ + 0.1f));

  assert(!trigger.step(true, VibrationWindowTrigger::SNR_ON_DB,
                       VibrationWindowTrigger::PEAK_MIN_HZ));
  assert(trigger.step(true, VibrationWindowTrigger::SNR_ON_DB,
                      VibrationWindowTrigger::PEAK_MAX_HZ));
}

void resetClearsAllState() {
  VibrationWindowTrigger trigger;
  trigger.step(true, VibrationWindowTrigger::SNR_ON_DB, kPumpHz);
  assert(trigger.step(true, VibrationWindowTrigger::SNR_ON_DB, kPumpHz));

  trigger.reset();

  assert(!trigger.active());
  assert(!trigger.step(true, VibrationWindowTrigger::SNR_ON_DB, kPumpHz));
}

}  // namespace

int main() {
  opensAfterConsecutiveEntryFrames();
  entryFailuresRestartTheCount();
  staysOpenAtTheLowerThreshold();
  rejectsInvalidMeasurements();
  resetClearsAllState();
  std::puts("OK: all assertions passed");
}
