// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

// Drives the radio state machine against a simulated controller and scale
// service and asserts the property the design rests on: with the inputs held
// still, the radio settles into the state controllerShouldRun() asks for, and
// settles there once rather than cycling.
//
// The scenario tests next door check named sequences. This checks every
// starting position, which is what catches two decision points disagreeing.

#include <cassert>
#include <cstdint>
#include <cstdio>

#include "power/ScaleRadioStateMachine.h"

namespace {

using ControllerState = ble::ControllerState;
using Machine = power::ScaleRadioStateMachine;
using ServiceState = power::ScaleServiceState;

// Stands in for the controller and the scale service, applying each requested
// action and letting the hardware settle one step later.
struct Simulation {
  Machine machine;
  Machine::Inputs inputs;
  unsigned releasesStarted = 0;
  unsigned startsRequested = 0;
  unsigned stopsRequested = 0;
  bool reportActivity = false;
  bool externalControl = false;
  bool startedDimmed = false;

  void step(uint32_t nowMs) {
    // Activity is reported the way the product reports it: it wakes the
    // display, and the machine is told separately. The display then dims again
    // later, which is the sequence that catches an activity event being kept
    // past its moment.
    if (reportActivity && nowMs == 5) {
      inputs.screenDimmed = false;
      machine.notifyActivity();
    }
    if (reportActivity && nowMs == 15) inputs.screenDimmed = startedDimmed;
    // The application driving the controller itself stops it mid-run.
    if (externalControl && nowMs == 7)
      inputs.controller = ControllerState::Stopped;
    inputs.nowMs = nowMs;
    Machine::Decision decision = machine.update(inputs);
    if (decision.event.kind == Machine::EventKind::StopStarted)
      ++releasesStarted;

    // Mirrors the caller's decision loop, which resolves each request at once.
    for (int guard = 0; guard < 4; ++guard) {
      if (decision.action == Machine::Action::StopController) {
        ++stopsRequested;
        inputs.controller = ControllerState::Stopping;
        decision = machine.resolve(Machine::Action::StopController,
                                   /*accepted=*/true, inputs);
      } else if (decision.action == Machine::Action::StartController) {
        ++startsRequested;
        inputs.controller = ControllerState::Starting;
        decision = machine.resolve(Machine::Action::StartController,
                                   /*accepted=*/true, inputs);
      } else {
        break;
      }
    }

    if (inputs.controller == ControllerState::Stopping)
      inputs.controller = ControllerState::Stopped;
    else if (inputs.controller == ControllerState::Starting)
      inputs.controller = ControllerState::Running;

    // The service releases the radio when suspended, and searches again once
    // it is allowed to and something wants a scale.
    if (machine.wantsServiceSuspended()) {
      inputs.service = ServiceState::Off;
    } else if (inputs.connectWanted && inputs.service == ServiceState::Off) {
      inputs.service = ServiceState::Disconnected;
    }
  }

  bool radioRunning() const {
    return inputs.controller == ControllerState::Running;
  }

  bool policyWantsRadio() const {
    power::ScaleRadioConditions conditions;
    conditions.service = inputs.service;
    conditions.screenDimmed = inputs.screenDimmed;
    conditions.connectWanted = inputs.connectWanted;
    conditions.diagnosticScanWanted = inputs.diagnosticScanWanted;
    return power::radioTarget(machine.policy(), conditions);
  }
};

constexpr uint32_t kStepsToSettle = 60;

}  // namespace

int main() {
  unsigned scenarios = 0;
  for (int flags = 0; flags < 8; ++flags) {
    for (int perturbation = 0; perturbation < 4; ++perturbation) {
      for (int radioRunning = 0; radioRunning < 2; ++radioRunning) {
        for (int serviceState = 0; serviceState < 4; ++serviceState) {
          Simulation sim;
          sim.reportActivity = perturbation & 1;
          sim.externalControl = perturbation & 2;
          // An application that drives the controller itself takes it with
          // ExternalControl, and the machine must then leave it entirely alone.
          // A fresh machine is settled and unsuspended, so this is accepted.
          if (sim.externalControl) {
            const bool accepted =
                sim.machine.setPolicy(power::ScaleRadioPolicy::ExternalControl);
            assert(accepted);
          }
          sim.inputs.connectWanted = flags & 1;
          sim.inputs.diagnosticScanWanted = flags & 2;
          sim.inputs.screenDimmed = flags & 4;
          sim.startedDimmed = flags & 4;
          sim.inputs.controller = radioRunning ? ControllerState::Running
                                               : ControllerState::Stopped;
          sim.inputs.service = static_cast<ServiceState>(serviceState);
          // A stopped radio cannot be carrying a link, so skip states the
          // hardware cannot be in.
          if (!radioRunning) sim.inputs.service = ServiceState::Off;

          for (uint32_t nowMs = 1; nowMs <= kStepsToSettle; ++nowMs)
            sim.step(nowMs);

          if (sim.externalControl) {
            // Handed over: no requests at all, whatever the conditions say.
            if (sim.startsRequested || sim.stopsRequested ||
                sim.releasesStarted) {
              std::printf(
                  "FAIL handed-over machine acted: connect=%d diagnostic=%d "
                  "dimmed=%d -> releases=%u starts=%u stops=%u\n",
                  sim.inputs.connectWanted, sim.inputs.diagnosticScanWanted,
                  sim.inputs.screenDimmed, sim.releasesStarted,
                  sim.startsRequested, sim.stopsRequested);
              assert(false);
            }
            ++scenarios;
            continue;
          }

          if (sim.policyWantsRadio() != sim.radioRunning()) {
            std::printf(
                "FAIL did not settle: connect=%d diagnostic=%d dimmed=%d "
                "startedRunning=%d service=%d -> wanted=%d running=%d\n",
                sim.inputs.connectWanted, sim.inputs.diagnosticScanWanted,
                sim.inputs.screenDimmed, radioRunning, serviceState,
                sim.policyWantsRadio(), sim.radioRunning());
            assert(false);
          }
          // Settling is a one-way trip. A reported activity event wakes the
          // display and so legitimately costs one further round trip; nothing
          // else may.
          const unsigned allowed = sim.reportActivity ? 2u : 1u;
          if (sim.releasesStarted > allowed || sim.startsRequested > allowed ||
              sim.stopsRequested > allowed) {
            std::printf(
                "FAIL cycled: connect=%d diagnostic=%d dimmed=%d "
                "startedRunning=%d service=%d -> releases=%u starts=%u "
                "stops=%u\n",
                sim.inputs.connectWanted, sim.inputs.diagnosticScanWanted,
                sim.inputs.screenDimmed, radioRunning, serviceState,
                sim.releasesStarted, sim.startsRequested, sim.stopsRequested);
            assert(false);
          }
          ++scenarios;
        }
      }
    }
  }

  assert(scenarios == 8 * 4 * 2 * 4);

  std::puts("OK: all assertions passed");
  return 0;
}
