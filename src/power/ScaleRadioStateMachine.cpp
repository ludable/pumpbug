// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#include "power/ScaleRadioStateMachine.h"

namespace power {

namespace {
using ControllerState = ble::ControllerState;
using Transition = ble::ControllerTransition;
}  // namespace

ScaleRadioStateMachine::Decision ScaleRadioStateMachine::action(
    Action requestedAction) {
  Decision decision;
  decision.action = requestedAction;
  return decision;
}

ScaleRadioStateMachine::Decision ScaleRadioStateMachine::event(EventKind kind) {
  Decision decision;
  decision.event.kind = kind;
  return decision;
}

ScaleRadioStateMachine::Decision ScaleRadioStateMachine::failure(
    FailureArea area, const char* reason) {
  Decision decision;
  decision.event.kind = EventKind::Failure;
  decision.event.failureArea = area;
  decision.event.reason = reason;
  return decision;
}

void ScaleRadioStateMachine::notifyActivity() { _activityPending = true; }

bool ScaleRadioStateMachine::setPolicy(ScaleRadioPolicy policy) {
  if (policy == _policy) return true;

  // The lifecycle stress diagnostic owns the controller after this handover.
  // Reacquiring it would require rebuilding state from controller and service
  // observations that may have changed independently.
  if (_policy == ScaleRadioPolicy::ExternalControl) return false;

  // Handing the controller over means the machine stops asserting anything,
  // including a transition it is waiting on and a service suspension it is
  // holding. Refuse rather than abandon either: the caller is in a position to
  // wait or to report, and the machine is not.
  if (policy == ScaleRadioPolicy::ExternalControl &&
      (inTransition() || hasFailed() || wantsServiceSuspended())) {
    return false;
  }

  _policy = policy;
  // A deliberate change of what is being asked for earns the same single retry
  // an activity event does, so a failure does not remain after its conditions
  // have changed.
  _activityPending = true;
  return true;
}

ScaleRadioConditions ScaleRadioStateMachine::conditionsOf(
    const Inputs& inputs) {
  ScaleRadioConditions conditions;
  conditions.service = inputs.service;
  conditions.screenDimmed = inputs.screenDimmed;
  conditions.connectWanted = inputs.connectWanted;
  conditions.diagnosticScanWanted = inputs.diagnosticScanWanted;
  return conditions;
}

ScaleRadioStateMachine::Decision ScaleRadioStateMachine::fail(
    FailureArea area, const char* reason) {
  _controllerTransition.reset();
  _state = State::Failed;
  _activityPending = false;
  return failure(area, reason);
}

ScaleRadioStateMachine::Decision ScaleRadioStateMachine::beginControllerStop(
    const Inputs& inputs) {
  if (inputs.controller == ControllerState::Stopped) {
    _state = State::ControllerStopped;
    return event(EventKind::ControllerStopped);
  }
  if (inputs.controller == ControllerState::Failed)
    return fail(FailureArea::ControllerStop, "stop unavailable");
  if (inputs.controller == ControllerState::Starting)
    return fail(FailureArea::ControllerStop, "stop busy");
  if (inputs.controller == ControllerState::Running) {
    _state = State::Stopping;
    return action(Action::StopController);
  }
  // Already stopping under someone else's request: wait for it to land.
  if (!_controllerTransition.begin(Transition::Target::Stopped, inputs.nowMs))
    return fail(FailureArea::ControllerStop, "stop wait busy");
  _state = State::Stopping;
  return {};
}

ScaleRadioStateMachine::Decision ScaleRadioStateMachine::beginControllerStart(
    const Inputs& inputs) {
  _activityPending = false;

  if (inputs.controller == ControllerState::Failed)
    return fail(FailureArea::ControllerStart, "start unavailable");

  // Already starting under someone else's request: wait for it to land.
  if (inputs.controller == ControllerState::Starting) {
    if (!_controllerTransition.begin(Transition::Target::Running, inputs.nowMs))
      return fail(FailureArea::ControllerStart, "start wait busy");
    _state = State::Starting;
    return event(EventKind::WakeRequested);
  }

  _state = State::Starting;
  Decision decision = event(EventKind::WakeRequested);
  decision.action = Action::StartController;
  return decision;
}

ScaleRadioStateMachine::Decision ScaleRadioStateMachine::advanceTransition(
    const Inputs& inputs) {
  const bool starting = _state == State::Starting;
  const Transition::Result result =
      _controllerTransition.update(inputs.controller, inputs.nowMs);

  if (result == Transition::Result::Succeeded) {
    _controllerTransition.reset();
    if (starting) {
      _state = State::Monitoring;
      _activityPending = false;
      return event(EventKind::ControllerRestarted);
    }
    _state = State::ControllerStopped;
    return event(EventKind::ControllerStopped);
  }
  if (result == Transition::Result::Failed) {
    return fail(
        starting ? FailureArea::ControllerStart : FailureArea::ControllerStop,
        starting ? "start failed" : "stop failed");
  }
  if (result == Transition::Result::TimedOut) {
    return fail(
        starting ? FailureArea::ControllerStart : FailureArea::ControllerStop,
        starting ? "start timed out" : "stop timed out");
  }
  return {};
}

ScaleRadioStateMachine::Decision ScaleRadioStateMachine::updateMonitoring(
    const Inputs& inputs, bool shouldRun) {
  // Nothing else moves the controller, so a state other than Running is one the
  // machine has yet to observe settling rather than someone else's doing.
  if (inputs.controller == ControllerState::Stopped) {
    _state = State::ControllerStopped;
    return {};
  }
  if (inputs.controller == ControllerState::Failed)
    return fail(FailureArea::ControllerUnusable, "controller unusable");
  if (shouldRun || inputs.controller != ControllerState::Running) return {};

  _state = State::Suspending;
  _stageStartedMs = inputs.nowMs;
  return event(EventKind::StopStarted);
}

ScaleRadioStateMachine::Decision ScaleRadioStateMachine::updateSuspending(
    const Inputs& inputs, bool shouldRun) {
  // Wanted again before anything was torn down, so the suspension lifts with
  // the state.
  if (shouldRun) {
    _state = State::Monitoring;
    return {};
  }
  if (inputs.service == ScaleServiceState::Off)
    return beginControllerStop(inputs);
  if (inputs.nowMs - _stageStartedMs >= kServiceStopTimeoutMs)
    return fail(FailureArea::ServiceStop, "service stop timed out");
  return {};
}

ScaleRadioStateMachine::Decision
ScaleRadioStateMachine::updateControllerStopped(const Inputs& inputs,
                                                bool shouldRun) {
  if (inputs.controller == ControllerState::Running) {
    _state = State::Monitoring;
    return {};
  }
  if (!shouldRun) return {};
  return beginControllerStart(inputs);
}

ScaleRadioStateMachine::Decision ScaleRadioStateMachine::updateFailed(
    const Inputs& inputs, bool shouldRun) {
  const bool running = inputs.controller == ControllerState::Running;
  const bool stopped = inputs.controller == ControllerState::Stopped;
  // A controller mid-transition, or one that reported Failed, is not somewhere
  // this machine can resume from.
  if (!running && !stopped) return {};

  const bool nothingToRetry = running == shouldRun;
  if (!nothingToRetry && !_activityPending) return {};

  _state = running ? State::Monitoring : State::ControllerStopped;
  return {};
}

ScaleRadioStateMachine::Decision ScaleRadioStateMachine::update(
    const Inputs& inputs) {
  // The application drives the controller itself under this policy.
  if (_policy == ScaleRadioPolicy::ExternalControl) return {};

  if (_state == State::Stopping || _state == State::Starting) {
    if (_controllerTransition.pending()) return advanceTransition(inputs);
    return {};
  }

  const bool shouldRun = radioTarget(_policy, conditionsOf(inputs));
  switch (_state) {
    case State::Monitoring:
      return updateMonitoring(inputs, shouldRun);
    case State::Suspending:
      return updateSuspending(inputs, shouldRun);
    case State::ControllerStopped:
      return updateControllerStopped(inputs, shouldRun);
    case State::Failed:
      return updateFailed(inputs, shouldRun);
    default:
      return {};
  }
}

ScaleRadioStateMachine::Decision ScaleRadioStateMachine::resolve(
    Action resolvedAction, bool accepted, const Inputs& inputs) {
  if (resolvedAction == Action::StopController) {
    if (inputs.controller == ControllerState::Stopped) {
      _state = State::ControllerStopped;
      return event(EventKind::ControllerStopped);
    }
    if (inputs.controller == ControllerState::Failed)
      return fail(FailureArea::ControllerStop, "stop failed");
    if (!accepted && inputs.controller != ControllerState::Stopping)
      return fail(FailureArea::ControllerStop, "stop rejected");
    if (!_controllerTransition.begin(Transition::Target::Stopped, inputs.nowMs))
      return fail(FailureArea::ControllerStop, "stop wait busy");
    _state = State::Stopping;
    return {};
  }

  if (resolvedAction == Action::StartController) {
    if (inputs.controller == ControllerState::Running) {
      _state = State::Monitoring;
      return event(EventKind::ControllerRestarted);
    }
    if (inputs.controller == ControllerState::Failed)
      return fail(FailureArea::ControllerStart, "start failed");
    if (!accepted && inputs.controller != ControllerState::Starting)
      return fail(FailureArea::ControllerStart, "start rejected");
    if (!_controllerTransition.begin(Transition::Target::Running, inputs.nowMs))
      return fail(FailureArea::ControllerStart, "start wait busy");
    _state = State::Starting;
  }
  return {};
}

}  // namespace power
