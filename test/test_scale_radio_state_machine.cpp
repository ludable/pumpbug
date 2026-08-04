// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "power/ScaleRadioStateMachine.h"

namespace {

using ControllerState = ble::ControllerState;
using Machine = power::ScaleRadioStateMachine;
using ServiceState = power::ScaleServiceState;

struct Harness {
  Machine machine;
  Machine::Inputs inputs;
  std::vector<Machine::Decision> decisions;

  Harness() { inputs.controller = ControllerState::Running; }

  void requestScale() { inputs.connectWanted = true; }

  Machine::Decision update(bool screenDimmed, uint32_t nowMs) {
    inputs.screenDimmed = screenDimmed;
    inputs.nowMs = nowMs;
    return record(machine.update(inputs));
  }

  Machine::Decision resolve(Machine::Action action, bool accepted,
                            ControllerState controller, uint32_t nowMs) {
    inputs.controller = controller;
    inputs.nowMs = nowMs;
    return record(machine.resolve(action, accepted, inputs));
  }

  unsigned eventCount(Machine::EventKind kind) const {
    unsigned count = 0;
    for (const auto& decision : decisions) {
      if (decision.event.kind == kind) ++count;
    }
    return count;
  }

  unsigned actionCount(Machine::Action action) const {
    unsigned count = 0;
    for (const auto& decision : decisions) {
      if (decision.action == action) ++count;
    }
    return count;
  }

 private:
  Machine::Decision record(Machine::Decision decision) {
    decisions.push_back(decision);
    return decision;
  }
};

void enterStandby(Harness& h, uint32_t nowMs) {
  h.requestScale();
  assert(h.update(/*screenDimmed=*/false, nowMs).action ==
         Machine::Action::None);

  h.inputs.service = ServiceState::Disconnected;
  assert(h.update(/*screenDimmed=*/true, nowMs + 1).event.kind ==
         Machine::EventKind::StopStarted);
  assert(h.machine.state() == Machine::State::Suspending);
  assert(h.machine.wantsServiceSuspended());

  h.inputs.service = ServiceState::Off;
  assert(h.update(/*screenDimmed=*/true, nowMs + 2).action ==
         Machine::Action::StopController);
  assert(h.resolve(Machine::Action::StopController, /*accepted=*/true,
                   ControllerState::Stopping, nowMs + 2)
             .action == Machine::Action::None);
  assert(h.machine.state() == Machine::State::Stopping);

  h.inputs.controller = ControllerState::Stopped;
  assert(h.update(/*screenDimmed=*/true, nowMs + 3).event.kind ==
         Machine::EventKind::ControllerStopped);
  assert(h.machine.controllerStopped());
  // The service stays suspended for as long as the radio is released.
  assert(h.machine.wantsServiceSuspended());
}

void testDisconnectedScaleEntersStandbyAndWakingRestartsIt() {
  Harness h;
  enterStandby(h, 100);
  assert(h.eventCount(Machine::EventKind::StopStarted) == 1);
  assert(h.eventCount(Machine::EventKind::ControllerStopped) == 1);

  const Machine::Decision wake = h.update(/*screenDimmed=*/false, 200);
  assert(wake.action == Machine::Action::StartController);
  assert(wake.event.kind == Machine::EventKind::WakeRequested);
  h.resolve(Machine::Action::StartController, /*accepted=*/true,
            ControllerState::Starting, 200);
  assert(h.machine.wantsServiceSuspended());

  h.inputs.controller = ControllerState::Running;
  const Machine::Decision restarted = h.update(/*screenDimmed=*/false, 220);
  assert(restarted.event.kind == Machine::EventKind::ControllerRestarted);
  assert(h.machine.state() == Machine::State::Monitoring);
  assert(!h.machine.wantsServiceSuspended());
  assert(!h.machine.controllerStopped());
}

void testReleasedDemandLeavesStoppedControllerOffWhileDimmed() {
  Harness h;
  enterStandby(h, 100);
  h.inputs.connectWanted = false;
  assert(h.update(/*screenDimmed=*/true, 200).action == Machine::Action::None);
  assert(h.machine.controllerStopped());
  assert(h.actionCount(Machine::Action::StartController) == 0);
}

// Nothing wants the radio, so it is released without waiting for a dim.
void testNoDemandReleasesTheRadio() {
  Harness h;
  h.inputs.service = ServiceState::Off;

  const Machine::Decision entry = h.update(/*screenDimmed=*/false, 100);
  assert(entry.event.kind == Machine::EventKind::StopStarted);
  assert(h.machine.wantsServiceSuspended());

  assert(h.update(/*screenDimmed=*/true, 102).action ==
         Machine::Action::StopController);
  h.resolve(Machine::Action::StopController, /*accepted=*/true,
            ControllerState::Stopping, 102);
  h.inputs.controller = ControllerState::Stopped;
  assert(h.update(/*screenDimmed=*/true, 103).event.kind ==
         Machine::EventKind::ControllerStopped);
  assert(h.machine.controllerStopped());
}

void testNewDemandRestartsControllerStoppedWithoutConsumers() {
  Harness h;
  h.inputs.service = ServiceState::Off;
  h.update(/*screenDimmed=*/true, 100);
  h.update(/*screenDimmed=*/true, 101);
  h.resolve(Machine::Action::StopController, /*accepted=*/true,
            ControllerState::Stopping, 101);
  h.inputs.controller = ControllerState::Stopped;
  h.update(/*screenDimmed=*/true, 102);
  assert(h.machine.controllerStopped());

  h.inputs.connectWanted = true;
  const Machine::Decision wake = h.update(/*screenDimmed=*/false, 200);
  assert(wake.action == Machine::Action::StartController);
  assert(wake.event.kind == Machine::EventKind::WakeRequested);
}

// Releasing the radio takes a suspend and a wait for the service to let go, so
// a short gap in demand never reaches the stop: this is the only hysteresis
// the design needs against moving between screens.
void testBriefDemandGapNeverReachesTheStop() {
  Harness h;
  h.requestScale();
  h.inputs.service = ServiceState::Ready;
  h.update(/*screenDimmed=*/false, 100);

  h.inputs.connectWanted = false;
  assert(h.update(/*screenDimmed=*/false, 101).event.kind ==
         Machine::EventKind::StopStarted);
  assert(h.machine.wantsServiceSuspended());

  // Demand returns before the service has let go of the radio.
  h.requestScale();
  h.update(/*screenDimmed=*/false, 102);
  assert(h.machine.state() == Machine::State::Monitoring);
  assert(!h.machine.wantsServiceSuspended());
  assert(h.actionCount(Machine::Action::StopController) == 0);
}

// Regression: releasing demand while a link is still up must not repeatedly
// start and abandon a release. The service takes time to reach Off, and every
// update in that window has to reach the same conclusion.
void testReleasedDemandWithLingeringLinkDoesNotFlap() {
  Harness h;
  h.requestScale();
  h.inputs.service = ServiceState::Ready;
  h.update(/*screenDimmed=*/true, 100);

  // The consumer releases the scale; the link takes a while to tear down.
  h.inputs.connectWanted = false;
  for (uint32_t nowMs = 101; nowMs < 140; ++nowMs)
    h.update(/*screenDimmed=*/true, nowMs);

  assert(h.eventCount(Machine::EventKind::StopStarted) == 1);
  assert(h.machine.state() == Machine::State::Suspending);
  assert(h.machine.wantsServiceSuspended());

  // Once the service lets go, the release completes as normal.
  h.inputs.service = ServiceState::Off;
  assert(h.update(/*screenDimmed=*/true, 140).action ==
         Machine::Action::StopController);
}

// Activity is a moment, not a state: reporting one must not leave the machine
// believing the display is awake afterwards. Demand and a live link keep the
// radio up, then activity is reported, then the device is left alone and the
// link drops. The release must still happen.
void testReportedActivityDoesNotOutliveItsMoment() {
  Harness h;
  h.requestScale();
  h.inputs.service = ServiceState::Ready;
  h.update(/*screenDimmed=*/false, 100);

  h.machine.notifyActivity();
  h.update(/*screenDimmed=*/false, 101);

  h.inputs.service = ServiceState::Disconnected;
  assert(h.update(/*screenDimmed=*/true, 102).event.kind ==
         Machine::EventKind::StopStarted);
  assert(h.machine.wantsServiceSuspended());
}

// A fixed-stop policy releases the radio even while an application wants it.
void testForceStoppedReleasesTheRadioAgainstDemand() {
  Harness h;
  h.requestScale();
  h.inputs.service = ServiceState::Ready;
  assert(h.update(/*screenDimmed=*/false, 100).action == Machine::Action::None);

  assert(h.machine.setPolicy(power::ScaleRadioPolicy::ForceStopped));
  assert(h.update(/*screenDimmed=*/false, 101).event.kind ==
         Machine::EventKind::StopStarted);
  assert(h.machine.inTransition());
  assert(h.machine.wantsServiceSuspended());

  h.inputs.service = ServiceState::Off;
  assert(h.update(/*screenDimmed=*/false, 102).action ==
         Machine::Action::StopController);
  h.resolve(Machine::Action::StopController, /*accepted=*/true,
            ControllerState::Stopping, 102);
  h.inputs.controller = ControllerState::Stopped;
  assert(h.update(/*screenDimmed=*/false, 103).event.kind ==
         Machine::EventKind::ControllerStopped);
  assert(h.machine.controllerStopped());
  assert(!h.machine.inTransition());

  // Demand returning does not lift a forced stop.
  h.inputs.service = ServiceState::Disconnected;
  for (uint32_t nowMs = 104; nowMs < 140; ++nowMs)
    h.update(/*screenDimmed=*/false, nowMs);
  assert(h.actionCount(Machine::Action::StartController) == 0);
}

// The next phase wants the radio back, with nothing asking for a scale.
void testForceRunningRestartsAReleasedRadio() {
  Harness h;
  h.inputs.service = ServiceState::Off;
  h.inputs.controller = ControllerState::Stopped;
  h.update(/*screenDimmed=*/false, 100);
  assert(h.machine.controllerStopped());

  assert(h.machine.setPolicy(power::ScaleRadioPolicy::ForceRunning));
  const Machine::Decision start = h.update(/*screenDimmed=*/false, 101);
  assert(start.action == Machine::Action::StartController);
  h.resolve(Machine::Action::StartController, /*accepted=*/true,
            ControllerState::Starting, 101);
  h.inputs.controller = ControllerState::Running;
  assert(h.update(/*screenDimmed=*/false, 102).event.kind ==
         Machine::EventKind::ControllerRestarted);
  assert(!h.machine.controllerStopped());
  assert(!h.machine.inTransition());
  assert(!h.machine.wantsServiceSuspended());

  // Nothing wants a scale, but the phase does, so the radio stays up.
  for (uint32_t nowMs = 103; nowMs < 140; ++nowMs)
    h.update(/*screenDimmed=*/true, nowMs);
  assert(h.actionCount(Machine::Action::StopController) == 0);
}

// Handing the controller over is refused while the machine is still holding
// something, because it stops asserting the moment it hands over. Refusing
// leaves the previous policy in place for the caller to report or retry.
void testExternalControlIsRefusedWhileTheMachineHoldsSomething() {
  Harness h;
  h.inputs.service = ServiceState::Ready;
  assert(h.update(/*screenDimmed=*/false, 100).event.kind ==
         Machine::EventKind::StopStarted);
  assert(h.machine.state() == Machine::State::Suspending);
  assert(h.machine.wantsServiceSuspended());

  assert(!h.machine.setPolicy(power::ScaleRadioPolicy::ExternalControl));
  assert(h.machine.policy() == power::ScaleRadioPolicy::Demand);
  // Refusing changed nothing: the release carries on to its own conclusion.
  assert(h.machine.wantsServiceSuspended());
  h.inputs.service = ServiceState::Off;
  assert(h.update(/*screenDimmed=*/false, 101).action ==
         Machine::Action::StopController);
}

// The same refusal from a quiet machine that has released the radio: the
// suspension it is holding would be stranded by a handover.
void testExternalControlIsRefusedWhileTheRadioIsReleased() {
  Harness h;
  enterStandby(h, 100);
  assert(h.machine.controllerStopped());
  assert(h.machine.wantsServiceSuspended());

  assert(!h.machine.setPolicy(power::ScaleRadioPolicy::ExternalControl));
  assert(h.machine.policy() == power::ScaleRadioPolicy::Demand);
}

// Accepted from where the stress build actually selects it: a fresh machine,
// settled, with the controller running and nothing suspended.
void testExternalControlIsAcceptedFromASettledMachine() {
  Harness h;
  assert(!h.machine.inTransition());
  assert(!h.machine.wantsServiceSuspended());
  assert(h.machine.setPolicy(power::ScaleRadioPolicy::ExternalControl));
  assert(h.machine.policy() == power::ScaleRadioPolicy::ExternalControl);
}

// The stress build drives the controller itself, so the machine asks for
// nothing at all under ExternalControl whatever the conditions say.
void testExternalControlMakesNoRequests() {
  Harness h;
  assert(h.machine.setPolicy(power::ScaleRadioPolicy::ExternalControl));
  h.requestScale();

  for (uint32_t nowMs = 100; nowMs < 140; ++nowMs) {
    h.inputs.service = nowMs % 2 ? ServiceState::Off : ServiceState::Ready;
    h.inputs.controller =
        nowMs % 3 ? ControllerState::Running : ControllerState::Stopped;
    h.update(/*screenDimmed=*/nowMs % 2 == 0, nowMs);
  }
  assert(h.actionCount(Machine::Action::StopController) == 0);
  assert(h.actionCount(Machine::Action::StartController) == 0);
  assert(!h.machine.wantsServiceSuspended());
}

void testExternalControlCannotBeRelinquished() {
  Harness h;
  assert(h.machine.setPolicy(power::ScaleRadioPolicy::ExternalControl));

  assert(!h.machine.setPolicy(power::ScaleRadioPolicy::Demand));
  assert(!h.machine.setPolicy(power::ScaleRadioPolicy::ForceRunning));
  assert(!h.machine.setPolicy(power::ScaleRadioPolicy::ForceStopped));
  assert(h.machine.policy() == power::ScaleRadioPolicy::ExternalControl);
}

// A phase change is a deliberate request, so it permits the retry that a
// failure otherwise waits for. A diagnostic has no user activity to request it.
void testPolicyChangeRetriesAfterAFailure() {
  Harness h;
  h.requestScale();
  h.inputs.service = ServiceState::Off;
  h.update(/*screenDimmed=*/true, 100);
  h.update(/*screenDimmed=*/true, 101);
  const Machine::Decision rejected =
      h.resolve(Machine::Action::StopController, /*accepted=*/false,
                ControllerState::Running, 101);
  assert(rejected.event.kind == Machine::EventKind::Failure);
  assert(h.machine.hasFailed());

  // Held in Failed: the observation does not satisfy the policy and no retry
  // has been earned.
  for (uint32_t nowMs = 102; nowMs < 140; ++nowMs)
    h.update(/*screenDimmed=*/true, nowMs);
  assert(h.machine.hasFailed());

  assert(h.machine.setPolicy(power::ScaleRadioPolicy::ForceRunning));
  assert(h.update(/*screenDimmed=*/true, 140).action == Machine::Action::None);
  assert(h.machine.state() == Machine::State::Monitoring);
  assert(!h.machine.hasFailed());
  assert(!h.machine.inTransition());
}

// A controller that reports Failed cannot be stopped or started, so the machine
// says so once and then leaves it alone rather than parking silently.
void testUnusableControllerIsReportedOnceAndLeftAlone() {
  Harness h;
  h.requestScale();
  h.inputs.service = ServiceState::Ready;
  h.inputs.controller = ControllerState::Failed;

  const Machine::Decision reported = h.update(/*screenDimmed=*/false, 100);
  assert(reported.event.kind == Machine::EventKind::Failure);
  assert(reported.event.failureArea ==
         Machine::FailureArea::ControllerUnusable);
  assert(h.machine.state() == Machine::State::Failed);

  h.machine.notifyActivity();
  for (uint32_t nowMs = 101; nowMs < 140; ++nowMs)
    h.update(/*screenDimmed=*/false, nowMs);
  assert(h.eventCount(Machine::EventKind::Failure) == 1);
  assert(h.actionCount(Machine::Action::StartController) == 0);
  assert(h.actionCount(Machine::Action::StopController) == 0);
}

void testConnectedScaleDoesNotEnterStandby() {
  Harness h;
  h.requestScale();
  h.inputs.service = ServiceState::Ready;
  h.update(/*screenDimmed=*/true, 100);

  assert(h.machine.state() == Machine::State::Monitoring);
  assert(!h.machine.wantsServiceSuspended());
  assert(h.actionCount(Machine::Action::StopController) == 0);
}

void testControllerDemandStartsStoppedControllerWhileAwake() {
  Harness h;
  h.inputs.controller = ControllerState::Stopped;
  h.inputs.diagnosticScanWanted = true;
  // A machine that starts out looking at a stopped controller spends its first
  // update recognising that, and acts on the next one.
  assert(h.update(/*screenDimmed=*/false, 100).action == Machine::Action::None);
  assert(h.machine.state() == Machine::State::ControllerStopped);
  const Machine::Decision decision = h.update(/*screenDimmed=*/false, 101);

  assert(decision.action == Machine::Action::StartController);
  h.resolve(Machine::Action::StartController, /*accepted=*/true,
            ControllerState::Starting, 100);
  assert(h.machine.state() == Machine::State::Starting);
}

// Demand arriving while the display stays dim is enough on its own.
void testDemandWakesStandbyWhileDisplayRemainsDimmed() {
  Harness h;
  enterStandby(h, 100);
  h.inputs.diagnosticScanWanted = true;
  const Machine::Decision decision = h.update(/*screenDimmed=*/true, 200);

  assert(decision.action == Machine::Action::StartController);
  assert(decision.event.kind == Machine::EventKind::WakeRequested);
}

void testRejectedStartWaitsForNewActivityBeforeRetrying() {
  Harness h;
  enterStandby(h, 100);
  assert(h.update(/*screenDimmed=*/false, 200).action ==
         Machine::Action::StartController);
  const Machine::Decision rejected =
      h.resolve(Machine::Action::StartController, /*accepted=*/false,
                ControllerState::Stopped, 200);
  assert(rejected.event.kind == Machine::EventKind::Failure);
  assert(h.machine.state() == Machine::State::Failed);
  assert(!h.machine.wantsServiceSuspended());

  for (uint32_t nowMs = 201; nowMs < 250; ++nowMs) {
    assert(h.update(/*screenDimmed=*/false, nowMs).action ==
           Machine::Action::None);
  }
  assert(h.actionCount(Machine::Action::StartController) == 1);
  assert(h.eventCount(Machine::EventKind::Failure) == 1);

  // The nudge lets the machine leave Failed; the retry follows on the next
  // update, like every other step.
  h.machine.notifyActivity();
  assert(h.update(/*screenDimmed=*/false, 300).action == Machine::Action::None);
  assert(h.machine.state() == Machine::State::ControllerStopped);
  assert(h.update(/*screenDimmed=*/false, 301).action ==
         Machine::Action::StartController);
}

void testServiceStopTimeoutIsReportedOnceAndReleasesSuspension() {
  Harness h;
  h.requestScale();
  h.inputs.service = ServiceState::Disconnected;
  h.update(/*screenDimmed=*/true, 100);
  assert(h.machine.state() == Machine::State::Suspending);

  const Machine::Decision timeout = h.update(/*screenDimmed=*/true, 5100);
  assert(timeout.event.kind == Machine::EventKind::Failure);
  assert(!h.machine.wantsServiceSuspended());

  for (uint32_t nowMs = 5101; nowMs < 5200; ++nowMs) {
    h.update(/*screenDimmed=*/true, nowMs);
  }
  assert(h.eventCount(Machine::EventKind::StopStarted) == 1);
  assert(h.eventCount(Machine::EventKind::Failure) == 1);
}

// A failed release parks in Failed rather than timing out again and again:
// the controller is still running and unwanted, so there is something to
// retry, but only a fresh activity event permits the attempt. The retry, when
// it comes, is exactly one new release.
void testServiceStopTimeoutRetriesTheReleaseOnceAfterActivity() {
  Harness h;
  h.requestScale();
  h.inputs.service = ServiceState::Disconnected;  // never reaches Off
  h.update(/*screenDimmed=*/true, 100);
  assert(h.machine.state() == Machine::State::Suspending);

  assert(h.update(/*screenDimmed=*/true, 5100).event.kind ==
         Machine::EventKind::Failure);
  assert(h.machine.state() == Machine::State::Failed);

  // Without activity, a full second timeout window passes with no new release
  // and no repeated failure.
  for (uint32_t nowMs = 5101; nowMs < 10300; ++nowMs) {
    h.update(/*screenDimmed=*/true, nowMs);
  }
  assert(h.eventCount(Machine::EventKind::StopStarted) == 1);
  assert(h.eventCount(Machine::EventKind::Failure) == 1);

  // The nudge lets the machine leave Failed; the retried release follows on
  // the next update, like every other step.
  h.machine.notifyActivity();
  assert(h.update(/*screenDimmed=*/true, 10300).action ==
         Machine::Action::None);
  assert(h.machine.state() == Machine::State::Monitoring);
  assert(h.update(/*screenDimmed=*/true, 10301).event.kind ==
         Machine::EventKind::StopStarted);
  assert(h.machine.state() == Machine::State::Suspending);
  assert(h.eventCount(Machine::EventKind::StopStarted) == 2);
  assert(h.eventCount(Machine::EventKind::Failure) == 1);
}

// Failed also exits on its own when the radio is already where the policy
// wants it: there is nothing to retry, so no activity is asked for.
void testFailedSettlesAloneWhenThereIsNothingToRetry() {
  // Wanted and Running: a service-stop failure, then the display wakes.
  Harness wanted;
  wanted.requestScale();
  wanted.inputs.service = ServiceState::Disconnected;
  wanted.update(/*screenDimmed=*/true, 100);
  wanted.update(/*screenDimmed=*/true, 5100);
  assert(wanted.machine.state() == Machine::State::Failed);
  wanted.update(/*screenDimmed=*/false, 5101);
  assert(wanted.machine.state() == Machine::State::Monitoring);
  assert(wanted.actionCount(Machine::Action::StopController) == 0);

  // Unwanted and Stopped: a rejected start whose demand has gone.
  Harness unwanted;
  enterStandby(unwanted, 100);
  unwanted.update(/*screenDimmed=*/false, 200);
  unwanted.resolve(Machine::Action::StartController, /*accepted=*/false,
                   ControllerState::Stopped, 200);
  assert(unwanted.machine.state() == Machine::State::Failed);
  unwanted.inputs.connectWanted = false;
  unwanted.update(/*screenDimmed=*/true, 201);
  assert(unwanted.machine.controllerStopped());
}

void testControllerStopTimeoutDoesNotRepeat() {
  Harness h;
  h.requestScale();
  h.inputs.service = ServiceState::Disconnected;
  h.update(/*screenDimmed=*/true, 100);
  h.inputs.service = ServiceState::Off;
  h.update(/*screenDimmed=*/true, 101);
  h.resolve(Machine::Action::StopController, /*accepted=*/true,
            ControllerState::Stopping, 101);

  const Machine::Decision timeout = h.update(/*screenDimmed=*/true, 5101);
  assert(timeout.event.kind == Machine::EventKind::Failure);
  assert(h.machine.state() == Machine::State::Failed);
  assert(!h.machine.wantsServiceSuspended());

  for (uint32_t nowMs = 5102; nowMs < 5200; ++nowMs) {
    h.update(/*screenDimmed=*/true, nowMs);
  }
  assert(h.eventCount(Machine::EventKind::Failure) == 1);
}

void testReleasedDemandClearsWakePendingDuringControllerStop() {
  Harness h;
  h.requestScale();
  h.inputs.service = ServiceState::Disconnected;
  h.update(/*screenDimmed=*/true, 100);
  h.inputs.service = ServiceState::Off;
  h.update(/*screenDimmed=*/true, 101);
  h.resolve(Machine::Action::StopController, /*accepted=*/true,
            ControllerState::Stopping, 101);

  h.inputs.connectWanted = false;
  h.inputs.controller = ControllerState::Stopped;
  h.update(/*screenDimmed=*/true, 120);
  assert(h.machine.controllerStopped());

  h.inputs.connectWanted = true;
  const Machine::Decision wake = h.update(/*screenDimmed=*/false, 1000);
  assert(wake.action == Machine::Action::StartController);
  assert(wake.event.kind == Machine::EventKind::WakeRequested);
  h.resolve(Machine::Action::StartController, /*accepted=*/true,
            ControllerState::Starting, 1000);
  h.inputs.controller = ControllerState::Running;
  const Machine::Decision restarted = h.update(/*screenDimmed=*/false, 1020);
  assert(restarted.event.kind == Machine::EventKind::ControllerRestarted);
}

// The radio becomes wanted again while the stop is in flight. The stop cannot
// be abandoned, so it lands first and the restart is issued in the same
// decision that reports the landing.
void testDemandDuringControllerStopRestartsAfterStopCompletes() {
  Harness h;
  h.requestScale();
  h.inputs.service = ServiceState::Disconnected;
  h.update(/*screenDimmed=*/true, 100);
  h.inputs.service = ServiceState::Off;
  h.update(/*screenDimmed=*/true, 101);
  h.resolve(Machine::Action::StopController, /*accepted=*/true,
            ControllerState::Stopping, 101);

  // The radio becomes wanted again mid-stop; the stop still runs to completion.
  h.inputs.diagnosticScanWanted = true;
  const Machine::Decision waiting = h.update(/*screenDimmed=*/true, 110);
  assert(waiting.action == Machine::Action::None);
  assert(h.machine.state() == Machine::State::Stopping);

  h.inputs.controller = ControllerState::Stopped;
  const Machine::Decision stopped = h.update(/*screenDimmed=*/true, 120);
  assert(stopped.event.kind == Machine::EventKind::ControllerStopped);

  // The next update reconciles against conditions that moved on while the stop
  // was in flight.
  const Machine::Decision restart = h.update(/*screenDimmed=*/true, 121);
  assert(restart.action == Machine::Action::StartController);
  assert(restart.event.kind == Machine::EventKind::WakeRequested);
}

// Demand can disappear while the radio is still coming up. With the display
// dimmed the policy does not want the radio, so the start runs to completion
// and the radio is released again straight away.
void testDemandReleaseDuringRestartReleasesTheRadioAgain() {
  Harness h;
  enterStandby(h, 100);
  h.update(/*screenDimmed=*/false, 200);
  h.resolve(Machine::Action::StartController, /*accepted=*/true,
            ControllerState::Starting, 200);

  h.inputs.connectWanted = false;
  h.inputs.controller = ControllerState::Running;
  const Machine::Decision restarted = h.update(/*screenDimmed=*/true, 220);
  assert(restarted.event.kind == Machine::EventKind::ControllerRestarted);

  // Nothing wants it now, so the next update starts releasing it again.
  assert(h.update(/*screenDimmed=*/true, 221).event.kind ==
         Machine::EventKind::StopStarted);
  assert(h.machine.wantsServiceSuspended());
}

void testDiagnosticScanDemandResumesWithoutWaitingForScale() {
  Harness h;
  enterStandby(h, 100);
  h.inputs.connectWanted = false;
  h.inputs.diagnosticScanWanted = true;
  h.update(/*screenDimmed=*/true, 200);
  h.resolve(Machine::Action::StartController, /*accepted=*/true,
            ControllerState::Starting, 200);

  h.inputs.controller = ControllerState::Running;
  const Machine::Decision restarted = h.update(/*screenDimmed=*/true, 220);
  assert(restarted.event.kind == Machine::EventKind::ControllerRestarted);
  assert(!h.machine.wantsServiceSuspended());

  h.inputs.service = ServiceState::Ready;
  assert(h.update(/*screenDimmed=*/true, 240).event.kind ==
         Machine::EventKind::None);
}

void testConnectionWonWhileReleasingKeepsTheRadio() {
  for (const ServiceState state :
       {ServiceState::Connecting, ServiceState::Ready}) {
    Harness h;
    h.requestScale();
    h.inputs.service = ServiceState::Disconnected;
    assert(h.update(/*screenDimmed=*/true, 100).event.kind ==
           Machine::EventKind::StopStarted);
    assert(h.machine.wantsServiceSuspended());

    h.inputs.service = state;
    h.update(/*screenDimmed=*/true, 101);
    assert(h.machine.state() == Machine::State::Monitoring);
    assert(!h.machine.wantsServiceSuspended());
    assert(h.actionCount(Machine::Action::StopController) == 0);
  }
}

void testDiagnosticDemandPreventsAndCancelsStandby() {
  Harness activeBeforeEntry;
  activeBeforeEntry.requestScale();
  activeBeforeEntry.inputs.diagnosticScanWanted = true;
  activeBeforeEntry.inputs.service = ServiceState::Disconnected;
  assert(activeBeforeEntry.update(/*screenDimmed=*/true, 100).action ==
         Machine::Action::None);
  assert(activeBeforeEntry.machine.state() == Machine::State::Monitoring);

  Harness arrivesDuringEntry;
  arrivesDuringEntry.requestScale();
  arrivesDuringEntry.inputs.service = ServiceState::Disconnected;
  assert(arrivesDuringEntry.update(/*screenDimmed=*/true, 100).event.kind ==
         Machine::EventKind::StopStarted);
  arrivesDuringEntry.inputs.diagnosticScanWanted = true;
  arrivesDuringEntry.update(/*screenDimmed=*/true, 101);
  assert(arrivesDuringEntry.machine.state() == Machine::State::Monitoring);
  assert(!arrivesDuringEntry.machine.wantsServiceSuspended());
}

void testAbandonedStopAlwaysReleasesSuspension() {
  Harness h;
  h.requestScale();
  h.inputs.service = ServiceState::Disconnected;
  assert(h.update(/*screenDimmed=*/true, 100).event.kind ==
         Machine::EventKind::StopStarted);

  h.inputs.service = ServiceState::Off;
  h.inputs.controller = ControllerState::Starting;
  const Machine::Decision abandoned = h.update(/*screenDimmed=*/true, 101);
  assert(abandoned.event.kind == Machine::EventKind::Failure);
  assert(h.machine.state() == Machine::State::Failed);
  assert(!h.machine.wantsServiceSuspended());
}

}  // namespace

int main() {
  testDisconnectedScaleEntersStandbyAndWakingRestartsIt();
  testReleasedDemandLeavesStoppedControllerOffWhileDimmed();
  testNoDemandReleasesTheRadio();
  testNewDemandRestartsControllerStoppedWithoutConsumers();
  testBriefDemandGapNeverReachesTheStop();
  testReleasedDemandWithLingeringLinkDoesNotFlap();
  testReportedActivityDoesNotOutliveItsMoment();
  testForceStoppedReleasesTheRadioAgainstDemand();
  testForceRunningRestartsAReleasedRadio();
  testExternalControlIsRefusedWhileTheMachineHoldsSomething();
  testExternalControlIsRefusedWhileTheRadioIsReleased();
  testExternalControlIsAcceptedFromASettledMachine();
  testExternalControlMakesNoRequests();
  testExternalControlCannotBeRelinquished();
  testPolicyChangeRetriesAfterAFailure();
  testUnusableControllerIsReportedOnceAndLeftAlone();
  testConnectedScaleDoesNotEnterStandby();
  testControllerDemandStartsStoppedControllerWhileAwake();
  testDemandWakesStandbyWhileDisplayRemainsDimmed();
  testRejectedStartWaitsForNewActivityBeforeRetrying();
  testServiceStopTimeoutIsReportedOnceAndReleasesSuspension();
  testServiceStopTimeoutRetriesTheReleaseOnceAfterActivity();
  testFailedSettlesAloneWhenThereIsNothingToRetry();
  testControllerStopTimeoutDoesNotRepeat();
  testReleasedDemandClearsWakePendingDuringControllerStop();
  testDemandDuringControllerStopRestartsAfterStopCompletes();
  testDemandReleaseDuringRestartReleasesTheRadioAgain();
  testDiagnosticScanDemandResumesWithoutWaitingForScale();
  testConnectionWonWhileReleasingKeepsTheRadio();
  testDiagnosticDemandPreventsAndCancelsStandby();
  testAbandonedStopAlwaysReleasesSuspension();
  std::puts("OK: all assertions passed");
  return 0;
}
