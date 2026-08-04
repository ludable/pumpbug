// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#include <M5Unified.h>

#include <cassert>
#include <cstdint>
#include <cstdio>

#include "power/SpeakerCodec.h"

TestM5 M5;

namespace {

void reset() { M5.Speaker = TestM5Speaker{}; }

}  // namespace

// The lifecycle logs are not part of what this test asserts.
void testM5LogInfo(const char*, ...) {}
void testM5LogWarning(const char*, ...) {}
void testM5LogError(const char*, ...) {}

int main() {
  // The first demand starts the speaker synchronously.
  {
    reset();
    power::SpeakerCodec codec;
    assert(codec.keepUntil(1000));
    assert(codec.isSpeakerRunning());
    assert(M5.Speaker.begins == 1);
    assert(M5.Speaker.volume == 255);
  }

  // A failed start reports failure so the caller skips its cue, and undoes the
  // rail that begin() left enabled.
  {
    reset();
    M5.Speaker.beginSucceeds = false;
    power::SpeakerCodec codec;
    assert(!codec.keepUntil(1000));
    assert(!codec.isSpeakerRunning());
    assert(M5.Speaker.ends == 1);
  }

  // Keep-alive: held until the deadline, released once it passes.
  {
    reset();
    power::SpeakerCodec codec;
    assert(codec.keepUntil(1000));
    codec.tick(999);
    assert(codec.isSpeakerRunning());
    codec.tick(1000);
    assert(!codec.isSpeakerRunning());
    assert(M5.Speaker.ends == 1);
  }

  // A tone still playing outlives its keep-alive: releasing mid-cue would cut
  // it, so the release waits for playback to finish.
  {
    reset();
    power::SpeakerCodec codec;
    assert(codec.keepUntil(1000));
    M5.Speaker.playing = true;
    codec.tick(5000);
    assert(codec.isSpeakerRunning());
    M5.Speaker.playing = false;
    codec.tick(5000);
    assert(!codec.isSpeakerRunning());
  }

  // Keep-alive expiration remains correct across the millis() rollover.
  {
    constexpr uint32_t nearWrap = UINT32_MAX - 100;

    reset();
    power::SpeakerCodec codec;
    assert(
        codec.keepUntil(nearWrap + 200));  // deadline 100 ticks past the wrap
    codec.tick(nearWrap);
    assert(codec.isSpeakerRunning());
    codec.tick(nearWrap + 300);
    assert(!codec.isSpeakerRunning());
  }

  std::puts("OK: all assertions passed");
  return 0;
}
