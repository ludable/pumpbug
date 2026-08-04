// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <cstdint>

namespace power {

// Owns the StickS3 speaker-amplifier lifecycle. Starting the speaker has a
// noticeable power cost, so nearby cues extend one powered interval instead of
// repeatedly starting and stopping the hardware. Release is deferred until
// playback finishes so a cue is never cut short.
class SpeakerCodec {
 public:
  // Power the speaker now and keep it powered at least until deadlineMs.
  // Returns false if the speaker could not start.
  bool keepUntil(uint32_t deadlineMs);
  // Release the speaker once no demand remains and no tone is playing.
  void tick(uint32_t nowMs);
  bool isSpeakerRunning() const { return _speakerRunning; }

 private:
  bool ensure();
  void release();
  bool demandPresent(uint32_t nowMs) const;

  uint32_t _keepUntilMs = 0;
  // Distinguishes "no keep-alive was ever set" from a deadline that
  // legitimately falls at tick 0 (millis() wrap-around).
  bool _keepActive = false;
  bool _speakerRunning = false;
};

extern SpeakerCodec speakerCodec;

}  // namespace power
