// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#include <cassert>
#include <cstdio>

#include "power/ScaleRadioStateMachine.h"

using power::controllerShouldRun;
using power::radioTarget;
using power::ScaleRadioConditions;
using power::ScaleRadioPolicy;
using power::ScaleServiceState;

namespace {

constexpr ScaleServiceState kStates[] = {
    ScaleServiceState::Off,
    ScaleServiceState::Disconnected,
    ScaleServiceState::Connecting,
    ScaleServiceState::Ready,
};
constexpr int kStateCount = sizeof(kStates) / sizeof(kStates[0]);

const char* stateName(ScaleServiceState state) {
  switch (state) {
    case ScaleServiceState::Off:
      return "Off";
    case ScaleServiceState::Disconnected:
      return "Disconnected";
    case ScaleServiceState::Connecting:
      return "Connecting";
    case ScaleServiceState::Ready:
      return "Ready";
  }
  return "?";
}

struct Row {
  bool connectWanted;
  bool diagnosticScanWanted;
  bool screenDimmed;
  bool expected[kStateCount];  // one per kStates entry, in order
};

// Every combination of the three flags against every service state. The
// expected column is written out rather than computed so that it states the
// intended policy independently of the implementation.
constexpr Row kTable[] = {
    // connect  diag   dimmed          Off    Disc   Conn   Ready
    // No demand: the radio serves nobody, awake or not.
    {false, false, false, {false, false, false, false}},
    {false, false, true, {false, false, false, false}},
    // A diagnostic scan lease outranks power saving in every condition.
    {false, true, false, {true, true, true, true}},
    {false, true, true, {true, true, true, true}},
    {true, true, false, {true, true, true, true}},
    {true, true, true, {true, true, true, true}},
    // Connect demand while the display is awake: keep searching whatever the
    // link state.
    {true, false, false, {true, true, true, true}},
    // Connect demand once dimmed: only an established link is worth the radio.
    {true, false, true, {false, false, true, true}},
};

}  // namespace

int main() {
  unsigned checks = 0;
  for (const Row& row : kTable) {
    for (int i = 0; i < kStateCount; ++i) {
      ScaleRadioConditions conditions;
      conditions.service = kStates[i];
      conditions.screenDimmed = row.screenDimmed;
      conditions.connectWanted = row.connectWanted;
      conditions.diagnosticScanWanted = row.diagnosticScanWanted;

      const bool actual = controllerShouldRun(conditions);
      if (actual != row.expected[i]) {
        std::printf(
            "FAIL connect=%d diagnostic=%d dimmed=%d service=%s: "
            "expected %d, got %d\n",
            row.connectWanted, row.diagnosticScanWanted, row.screenDimmed,
            stateName(kStates[i]), row.expected[i], actual);
        assert(false);
      }
      ++checks;
    }
  }

  // The table is exhaustive over the three flags and every service state.
  assert(checks == 8 * kStateCount);

  // Fixed policies hold the radio in one state regardless of demand.
  for (const Row& row : kTable) {
    for (int i = 0; i < kStateCount; ++i) {
      ScaleRadioConditions conditions;
      conditions.service = kStates[i];
      conditions.screenDimmed = row.screenDimmed;
      conditions.connectWanted = row.connectWanted;
      conditions.diagnosticScanWanted = row.diagnosticScanWanted;

      assert(radioTarget(ScaleRadioPolicy::ForceRunning, conditions));
      assert(!radioTarget(ScaleRadioPolicy::ForceStopped, conditions));
      // Demand dispatches to the condition-dependent policy and nothing else.
      assert(radioTarget(ScaleRadioPolicy::Demand, conditions) ==
             controllerShouldRun(conditions));
      ++checks;
    }
  }
  assert(checks == 2 * 8 * kStateCount);

  std::puts("OK: all assertions passed");
  return 0;
}
