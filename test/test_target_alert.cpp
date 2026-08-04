// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

// TargetAlert consumes yield samples that advance only when weight changes look
// like a pour. These tests verify that STOP_NOW fires on a genuine approach,
// remains active for the rest of the extraction, and ignores rejected samples.

#include <cstdio>

#include "apps/extraction/TargetAlert.h"

namespace {

int g_failures = 0;

#define CHECK(cond)                                               \
  do {                                                            \
    if (!(cond)) {                                                \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                               \
    }                                                             \
  } while (0)

using pump_scale::TargetAlert;
using pump_scale::TrustedYieldSample;

// TargetAlert updates more often than the recorder accepts a new weight. A
// repeated sequence number therefore means that no new sample is available.
struct Feeder {
  TargetAlert& a;
  uint32_t t = 0;
  uint32_t seq = 0;
  int16_t lastYield = 0;

  // Records when STOP_NOW first appears and how often its one-shot notification
  // occurs.
  int stopEdges = 0;
  bool sawEdge = false, sawStop = false;
  uint32_t firstEdgeT = 0, firstStopT = 0;

  void observe(const TargetAlert::State& s, uint32_t tickT) {
    if (s.stopNowEdge) {
      ++stopEdges;
      if (!sawEdge) {
        sawEdge = true;
        firstEdgeT = tickT;
      }
    }
    if (s.level == TargetAlert::Level::STOP_NOW && !sawStop) {
      sawStop = true;
      firstStopT = tickT;
    }
  }

  TargetAlert::Level sample(int16_t yieldCg, bool running = true) {
    ++seq;
    lastYield = yieldCg;
    const uint32_t sampleT = t;
    TargetAlert::Level level = TargetAlert::Level::OFF;
    for (int i = 0; i < 4; ++i) {  // ~132 ms dwell across 30 fps ticks
      TrustedYieldSample smp{true, yieldCg, sampleT, seq};
      const TargetAlert::State& s = a.update(t, running, smp);
      observe(s, t);
      level = s.level;
      t += 33;
    }
    return level;
  }

  // Repeats the last reading without advancing its sequence number.
  TargetAlert::Level hold(uint32_t durMs, bool running = true) {
    TargetAlert::Level level = TargetAlert::Level::OFF;
    const uint32_t sampleT = t > 0 ? t - 33 : 0;
    for (uint32_t e = 0; e < durMs; e += 33) {
      TrustedYieldSample smp{true, lastYield, sampleT, seq};
      const TargetAlert::State& s = a.update(t, running, smp);
      observe(s, t);
      level = s.level;
      t += 33;
    }
    return level;
  }

  // Raises the accepted yield to `toCg` at about 3 g/s.
  TargetAlert::Level rampTo(int16_t toCg) {
    TargetAlert::Level level = TargetAlert::Level::OFF;
    for (int16_t y = lastYield; y <= toCg; y += 36) level = sample(y);
    return level;
  }
};

pump_scale::TargetCoeffs coeffs(int16_t targetCg) {
  pump_scale::TargetCoeffs c;
  c.targetCg = targetCg;
  c.armed = true;
  c.tauMs = 750;
  c.cCg = 0;
  c.reactionLeadMs = 0;
  return c;
}

void testReachingTargetFiresStopNow() {
  // Trusted yield climbing past the target turns STOP_NOW on.
  TargetAlert a;
  a.setCoeffs(coeffs(2000));
  a.onRunningEntry();
  Feeder f{a};
  CHECK(f.rampTo(2100) == TargetAlert::Level::STOP_NOW);
}

void testWithheldKnockDoesNotTrigger() {
  // A rejected weight spike repeats the last accepted sample below the target.
  TargetAlert a;
  a.setCoeffs(coeffs(2000));
  a.onRunningEntry();
  Feeder f{a};
  f.rampTo(1000);  // pour to half target, projection well short
  CHECK(a.state().level != TargetAlert::Level::STOP_NOW);
  CHECK(f.hold(2000) != TargetAlert::Level::STOP_NOW);
}

void testPredictiveCutLeadsTheTarget() {
  // A steady, plausible pour rate fires the predictive cut BEFORE the trusted
  // yield reaches the target (the flow*tau lead).
  TargetAlert a;
  a.setCoeffs(coeffs(2000));
  a.onRunningEntry();
  Feeder f{a};
  TargetAlert::Level level = TargetAlert::Level::OFF;
  int16_t y = 0;
  for (; y <= 1980 && level != TargetAlert::Level::STOP_NOW; y += 36)
    level = f.sample(y);
  CHECK(level == TargetAlert::Level::STOP_NOW);
  CHECK(f.lastYield < 2000);  // led the target, not merely crossed it
}

void testPredictiveStopNowSurvivesFlowStall() {
  // A stale scale reading must not retract an alert that already fired.
  TargetAlert a;
  a.setCoeffs(coeffs(3000));
  a.onRunningEntry();
  Feeder f{a};
  TargetAlert::Level level = TargetAlert::Level::OFF;
  int16_t y = 0;
  for (; y <= 2950 && level != TargetAlert::Level::STOP_NOW; y += 36)
    level = f.sample(y);
  CHECK(level == TargetAlert::Level::STOP_NOW);
  CHECK(f.lastYield < 3000);
  CHECK(f.hold(2000) == TargetAlert::Level::STOP_NOW);
}

void testStopNowIsOneWay() {
  // Once STOP_NOW is on, a later lower trusted reading (drip settle / mild dip)
  // must not retract STOP_NOW.
  TargetAlert a;
  a.setCoeffs(coeffs(2000));
  a.onRunningEntry();
  Feeder f{a};
  CHECK(f.rampTo(2100) == TargetAlert::Level::STOP_NOW);
  CHECK(f.sample(1950) == TargetAlert::Level::STOP_NOW);  // dipped below target
  CHECK(f.sample(1850) == TargetAlert::Level::STOP_NOW);
}

void testNewPullClearsStopNow() {
  // onRunningEntry (a fresh pull) clears STOP_NOW.
  TargetAlert a;
  a.setCoeffs(coeffs(2000));
  a.onRunningEntry();
  Feeder f{a};
  CHECK(f.rampTo(2100) == TargetAlert::Level::STOP_NOW);
  a.onRunningEntry();
  Feeder f2{a};
  CHECK(f2.sample(500) == TargetAlert::Level::OFF);
}

void testLevelReentersStopNowAfterCoalesce() {
  // A brief pump-off interval hides the alert output without resetting the
  // decision. It reappears as soon as the same extraction resumes.
  TargetAlert a;
  a.setCoeffs(coeffs(2000));
  a.onRunningEntry();
  Feeder f{a};
  CHECK(f.rampTo(2100) == TargetAlert::Level::STOP_NOW);  // still on
  CHECK(f.hold(300, /*running=*/false) !=
        TargetAlert::Level::STOP_NOW);  // pump off
  CHECK(f.sample(1900, /*running=*/true) ==
        TargetAlert::Level::STOP_NOW);  // re-entry
}

void testStopNowEdgeRaisedOnlyOnce() {
  // stopNowEdge is true only when STOP_NOW first appears.
  TargetAlert a;
  a.setCoeffs(coeffs(2000));
  a.onRunningEntry();
  Feeder f{a};
  f.rampTo(2100);  // crosses target → STOP_NOW
  f.sample(1950);  // dip below target — STOP_NOW holds
  f.hold(2000);    // flow stalls — STOP_NOW holds
  CHECK(f.stopEdges == 1);
  CHECK(f.sawEdge && f.sawStop);
  CHECK(f.firstEdgeT == f.firstStopT);  // edge marks the first STOP_NOW tick
}

void testStopNowEdgeNotRefiredOnCoalesce() {
  // The case the raw level signal gets "wrong": after a pump-off coalesce the
  // level re-enters STOP_NOW, but the edge stays silent — the alarm is recorded
  // once per pull. This is the equivalence the refactor relies on.
  TargetAlert a;
  a.setCoeffs(coeffs(2000));
  a.onRunningEntry();
  Feeder f{a};
  f.rampTo(2100);
  CHECK(f.stopEdges == 1);
  f.hold(300, /*running=*/false);    // pump off: level leaves STOP_NOW
  f.sample(1900, /*running=*/true);  // coalesce back: level STOP_NOW again
  CHECK(f.stopEdges == 1);           // but no second edge
}

void testStopNowEdgeReArmsOnNewPull() {
  // onRunningEntry clears STOP_NOW, so a genuinely new pull records a new
  // alarm: the edge fires again.
  TargetAlert a;
  a.setCoeffs(coeffs(2000));
  a.onRunningEntry();
  Feeder f{a};
  f.rampTo(2100);
  CHECK(f.stopEdges == 1);
  a.onRunningEntry();
  Feeder f2{a};
  f2.rampTo(2100);
  CHECK(f2.stopEdges == 1);
}

}  // namespace

int main() {
  testReachingTargetFiresStopNow();
  testWithheldKnockDoesNotTrigger();
  testPredictiveCutLeadsTheTarget();
  testPredictiveStopNowSurvivesFlowStall();
  testStopNowIsOneWay();
  testNewPullClearsStopNow();
  testLevelReentersStopNowAfterCoalesce();
  testStopNowEdgeRaisedOnlyOnce();
  testStopNowEdgeNotRefiredOnCoalesce();
  testStopNowEdgeReArmsOnNewPull();
  if (g_failures == 0) {
    std::printf("OK: all assertions passed\n");
    return 0;
  }
  std::printf("%d assertion(s) failed\n", g_failures);
  return 1;
}
