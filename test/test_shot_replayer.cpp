// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

// Host-compilable unit tests for ShotReplayer.

#include <cstdint>
#include <cstdio>

#include "ShotReplayer.h"

namespace {

int g_failures = 0;

#define CHECK(cond)                                               \
  do {                                                            \
    if (!(cond)) {                                                \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                               \
    }                                                             \
  } while (0)

using pump_scale::EventKind;
using pump_scale::Extraction;
using pump_scale::ScaleSnapshot;
using pump_scale::ShotReplayer;

// beginMs=1000, pump 1000..5000, end 6000; three raw samples; 15 g cup.
Extraction makeShot() {
  Extraction e{};
  e.beginMs = 1000;
  e.lastPumpOffMs = 5000;
  e.endMs = 6000;
  e.startRawCg = 1500;
  e.events[0] = {1000, EventKind::BEGIN};
  e.events[1] = {1000, EventKind::PUMP_ON};
  e.events[2] = {5000, EventKind::PUMP_OFF};
  e.events[3] = {6000, EventKind::END};
  e.eventCount = 4;
  e.samples[0] = {1500, 1500, 0};  // 15 g raw
  e.samples[1] = {2000, 1700, 0};  // 17 g raw
  e.samples[2] = {3000, 2500, 0};  // 25 g raw
  e.sampleCount = 3;
  return e;
}

void testEmitsPumpAndRawWeight() {
  const Extraction e = makeShot();
  ShotReplayer r;
  r.start(e, 10'000);

  bool pumpOn = false, newSample = false;
  ScaleSnapshot snap{};

  // t=begin: pump already on; no sample due yet (first is at vMs 1500).
  CHECK(r.tick(10'000, pumpOn, snap, newSample));
  CHECK(pumpOn);
  CHECK(!newSample);
  CHECK(snap.timestampMs == 0);  // nothing reported yet
  CHECK(snap.grams == 15.0f);    // cup sitting on the scale

  // t=+500 -> vMs 1500: first sample lands; raw = baseline + yield = 15 g.
  CHECK(r.tick(10'500, pumpOn, snap, newSample));
  CHECK(newSample);
  CHECK(snap.timestampMs == 1500);
  CHECK(snap.grams == 15.0f);

  // t=+1000 -> vMs 2000: 2 g yield -> raw 17 g.
  CHECK(r.tick(11'000, pumpOn, snap, newSample));
  CHECK(newSample);
  CHECK(snap.grams == 17.0f);

  // t=+2000 -> vMs 3000: 10 g yield -> raw 25 g, pump still on.
  CHECK(r.tick(12'000, pumpOn, snap, newSample));
  CHECK(pumpOn);
  CHECK(snap.grams == 25.0f);

  // t=+4000 -> vMs 5000: PUMP_OFF reached; no new sample (none past 3000).
  CHECK(r.tick(14'000, pumpOn, snap, newSample));
  CHECK(!pumpOn);
  CHECK(!newSample);
  CHECK(snap.grams == 25.0f);  // holds the last reading
}

void testStopsPastEnd() {
  const Extraction e = makeShot();
  ShotReplayer r;
  r.start(e, 0);
  bool pumpOn = false, newSample = false;
  ScaleSnapshot snap{};
  CHECK(r.tick(5000, pumpOn, snap, newSample));   // vMs 6000 == end: last frame
  CHECK(!r.tick(5001, pumpOn, snap, newSample));  // vMs 6001 > end: done
}

void testPauseFreezesReplayPosition() {
  const Extraction e = makeShot();
  ShotReplayer r;
  r.start(e, 0);
  bool pumpOn = false, newSample = false;
  ScaleSnapshot snap{};
  r.tick(1000, pumpOn, snap, newSample);  // vMs 2000
  CHECK(r.virtualNowMs() == 2000);
  r.setPaused(true, 1000);  // pause at this instant -> frozen at vMs 2000
  // Wall time advances while paused, but the replay position does not.
  r.tick(4500, pumpOn, snap, newSample);
  CHECK(r.virtualNowMs() == 2000);
  // Resume: time advances normally again from where it froze.
  r.setPaused(false, 4500);
  r.tick(5000, pumpOn, snap, newSample);  // +500 -> vMs 2500
  CHECK(r.virtualNowMs() == 2500);
}

void testResumeWithoutPausedTicksPreservesPosition() {
  const Extraction e = makeShot();
  ShotReplayer r;
  r.start(e, 0);
  bool pumpOn = false, newSample = false;
  ScaleSnapshot snap{};
  r.tick(1000, pumpOn, snap, newSample);  // vMs 2000

  r.setPaused(true, 1000);
  // ExtractionScreen does not tick the replayer while paused. Resuming must
  // still discard the whole paused wall-time span.
  r.setPaused(false, 4500);
  CHECK(r.virtualNowMs() == 2000);
  r.tick(5000, pumpOn, snap, newSample);  // +500 -> vMs 2500
  CHECK(r.virtualNowMs() == 2500);
}

}  // namespace

int main() {
  testEmitsPumpAndRawWeight();
  testStopsPastEnd();
  testPauseFreezesReplayPosition();
  testResumeWithoutPausedTicksPreservesPosition();
  if (g_failures == 0) {
    std::printf("OK: all assertions passed\n");
    return 0;
  }
  std::printf("%d assertion(s) failed\n", g_failures);
  return 1;
}
