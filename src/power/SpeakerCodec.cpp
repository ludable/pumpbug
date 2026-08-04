// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#include "power/SpeakerCodec.h"

#include <M5Unified.h>

#include "util/i2c_lock.h"

namespace power {

SpeakerCodec speakerCodec;

bool SpeakerCodec::keepUntil(uint32_t deadlineMs) {
  if (!ensure()) return false;
  _keepUntilMs = deadlineMs;
  _keepActive = true;
  return true;
}

void SpeakerCodec::tick(uint32_t nowMs) {
  if (!_speakerRunning) return;
  if (demandPresent(nowMs)) return;
  if (M5.Speaker.isPlaying()) return;
  release();
}

// Whether any demand wants the codec powered at nowMs. Subtraction is wrap-safe
// across the millis() rollover.
bool SpeakerCodec::demandPresent(uint32_t nowMs) const {
  return _keepActive && static_cast<int32_t>(_keepUntilMs - nowMs) > 0;
}

bool SpeakerCodec::ensure() {
  if (_speakerRunning) return true;

  I2cLock lock;
  if (!M5.Speaker.begin()) {
    // begin() enables the ES8311 rail before it sets up I2S, and does not undo
    // that when the I2S step fails. Without this end() the codec would keep
    // drawing current with nothing tracking it: the speaker remains stopped,
    // so tick() returns early and never releases it.
    M5.Speaker.end();
    M5_LOGE("Audio: speaker amplifier start failed");
    return false;
  }

  _speakerRunning = true;
  M5.Speaker.setVolume(255);
  M5_LOGI("Audio: speaker amplifier powered");
  return true;
}

void SpeakerCodec::release() {
  I2cLock lock;
  M5.Speaker.end();
  _speakerRunning = false;
  M5_LOGI("Audio: speaker amplifier released");
}

}  // namespace power
