// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <cstdint>

#include "ble/ControllerTransition.h"

namespace power {

enum class ScaleServiceState : uint8_t {
  Off,
  Disconnected,
  Connecting,
  Ready,
};

// Everything the radio policy decides from: what wants the radio, and how far
// the link has got.
struct ScaleRadioConditions {
  ScaleServiceState service = ScaleServiceState::Off;
  bool screenDimmed = false;
  bool connectWanted = false;
  bool diagnosticScanWanted = false;
};

// Selects how the state machine manages the Bluetooth controller.
enum class ScaleRadioPolicy : uint8_t {
  // Releases a disconnected controller after display dimming.
  Demand,
  // Hold the controller running or stopped while the policy is selected.
  ForceRunning,
  ForceStopped,
  // Transfers responsibility for controller changes to the application.
  //
  // Selecting it is checked rather than assumed: it may only be entered while
  // the machine is settled and holds no service suspension, because the machine
  // stops asserting either the moment it hands over. PowerManager also requires
  // the controller to be running. Nothing leaves ExternalControl again, because
  // the machine cannot rebuild state after another owner has changed the radio.
  ExternalControl,
};

// The demand-driven policy releases a disconnected radio once the display
// dims, while preserving a connection or a diagnostic scan request.
inline bool controllerShouldRun(const ScaleRadioConditions& conditions) {
  if (conditions.diagnosticScanWanted) return true;
  if (!conditions.connectWanted) return false;
  if (!conditions.screenDimmed) return true;
  return conditions.service == ScaleServiceState::Ready ||
         conditions.service == ScaleServiceState::Connecting;
}

// What the selected policy wants the controller to be doing. Only Demand
// consults the conditions; the rest are constants.
inline bool radioTarget(ScaleRadioPolicy policy,
                        const ScaleRadioConditions& conditions) {
  switch (policy) {
    case ScaleRadioPolicy::ForceRunning:
      return true;
    case ScaleRadioPolicy::ForceStopped:
    // ExternalControl never reaches here; update() makes no decision under it.
    case ScaleRadioPolicy::ExternalControl:
      return false;
    case ScaleRadioPolicy::Demand:
      break;
  }
  return controllerShouldRun(conditions);
}

// One coherent answer about the radio, built from a single observation so that
// its fields cannot disagree with each other. Diagnostics wait on `settled`,
// report `failed`, and check `shouldRun` against the state their phase asked
// for, without reading the machine's internals.
struct ScaleRadioStatus {
  ScaleRadioPolicy policy = ScaleRadioPolicy::Demand;
  ble::ControllerState controller = ble::ControllerState::Stopped;
  bool shouldRun = false;
  bool settled = false;
  bool failed = false;
};

// Keeps the Bluetooth controller in the state the selected policy asks for.
//
// This class contains no hardware or logging code. PowerManager supplies one
// set of current observations on each update(), performs the returned Action,
// applies wantsServiceSuspended(), and records the optional Event. Keeping
// those effects outside makes timing, failure, and wake races reproducible in
// host tests.
//
// Connect wanted means an application has asked BleScaleService for live scale
// data. Suspending the service does not erase this demand.
//
// =============================================================================
//   State diagram
// =============================================================================
//
// Every update() asks radioTarget() what the radio should be doing and takes at
// most one step toward that answer, reporting at most one event. There is no
// second set of conditions for releasing or restoring the radio, so the two
// cannot disagree; a change of mind is a different answer on the next update().
//
//   Monitoring --- radio unwanted --> Suspending --- service Off --> Stopping
//       ^                                 |                              |
//       |                                 |                controller Stopped
//       |        radio wanted again       |                              |
//       + <-------------------------------+                              v
//       |                                                  ControllerStopped
//       |                                                                |
//       |                                                     radio wanted
//       |                                                                v
//       + <----------- controller Running ------------------------- Starting
//
// Releasing the radio takes two steps because the scale service has to let go
// of it first. wantsServiceSuspended() is true from Suspending all the way
// round to Starting, so the service stays out of the way until a restart has
// actually landed. If the radio becomes wanted again during Suspending, the
// machine returns to Monitoring and the suspension lifts with it; nothing has
// been torn down at that point.
//
// A transition already handed to the controller always runs to completion,
// because the controller cannot abandon one part-way. If the radio becomes
// wanted while it is stopping, the stop finishes and the next update() starts
// it again.
//
// One edge is not part of the cycle. A machine that boots finding the
// controller already stopped moves to ControllerStopped without reporting
// anything and decides what to do on the next update.
//
//   Monitoring --- controller already stopped ---> ControllerStopped
//
//   Monitoring  (controller reports Failed) --+
//   Suspending  (service stop timed out) -----+
//   Stopping    (stop failed or rejected) ----+--> Failed
//   Starting    (start failed or rejected) ---+
//
// Failed leaves the radio where it lies, because a transition that did not land
// makes the controller's state uncertain. It exits to Monitoring
// or ControllerStopped, whichever the controller is observed in, on either of
// two triggers: there is nothing to retry because the observation already
// satisfies the policy, or a new activity event asks for one more attempt. A
// controller that reports ControllerState::Failed satisfies neither and is left
// alone, which is correct — that state needs a reboot, not a retry.
//
// Except under ExternalControl, the machine performs every start and stop and
// observes its result. Fixed policies use ScaleRadioStatus::settled to report
// completion. A policy change permits one retry after a previous failure.
//
// =============================================================================
//   Calling protocol
// =============================================================================
//
//   setPolicy(policy)         Select what the controller is asked to do.
//                             Returns false if an ExternalControl handover is
//                             refused or if another policy tries to reclaim an
//                             externally controlled radio.
//   notifyActivity()          Report one activity event.
//   update(inputs)            Advance from observed service/controller state.
//   resolve(...)              Report the immediate result of a returned
//                             StopController or StartController request.
//   wantsServiceSuspended()   Apply after every update() and resolve().
//
// Each Decision carries at most one action and one event, and both may be
// present. The caller must process both fields.
class ScaleRadioStateMachine {
 public:
  enum class State : uint8_t {
    Monitoring,
    Suspending,
    Stopping,
    ControllerStopped,
    Starting,
    Failed,
  };

  enum class Action : uint8_t {
    None,
    StopController,
    StartController,
  };

  enum class EventKind : uint8_t {
    None,
    StopStarted,
    ControllerStopped,
    WakeRequested,
    ControllerRestarted,
    Failure,
  };

  enum class FailureArea : uint8_t {
    ServiceStop,
    ControllerStop,
    ControllerStart,
    ControllerUnusable,
    Count,
  };

  struct Inputs {
    ScaleServiceState service = ScaleServiceState::Off;
    ble::ControllerState controller = ble::ControllerState::Stopped;
    uint32_t nowMs = 0;
    bool screenDimmed = false;
    bool connectWanted = false;
    bool diagnosticScanWanted = false;
  };

  struct Event {
    EventKind kind = EventKind::None;
    FailureArea failureArea = FailureArea::ServiceStop;
    const char* reason = nullptr;
  };

  struct Decision {
    Action action = Action::None;
    Event event;
  };

  // Permits one more controller start attempt after a failed one. A button
  // press does not make the controller more startable; it is a rate limit on
  // retries that happens to cost nothing when nobody is there.
  void notifyActivity();
  // Selects what the controller is asked to do. The machine keeps running
  // across the change and converges on the new answer.
  //
  // Returns false, leaving the previous policy in place, only when
  // ExternalControl is requested while the machine is mid-transition, failed,
  // or holding the scale service suspended — the states in which handing over
  // would strand something the machine is about to stop asserting. The caller
  // is expected to surface the refusal rather than continue.
  [[nodiscard]] bool setPolicy(ScaleRadioPolicy policy);
  ScaleRadioPolicy policy() const { return _policy; }

  Decision update(const Inputs& inputs);
  Decision resolve(Action action, bool accepted, const Inputs& inputs);

  // Projects the observation onto what the policy decides from.
  static ScaleRadioConditions conditionsOf(const Inputs& inputs);

  State state() const { return _state; }
  // The scale service stays suspended for as long as the radio is not the
  // service's to use: while it is being released, while it is released, and
  // until a restart has actually landed. Failed lifts the suspension so
  // retained demand can resume if the controller turns out to be usable.
  bool wantsServiceSuspended() const {
    switch (_state) {
      case State::Suspending:
      case State::Stopping:
      case State::ControllerStopped:
      case State::Starting:
        return true;
      case State::Monitoring:
      case State::Failed:
        return false;
    }
    return false;
  }
  // A transition is in flight, so the controller is on its way somewhere rather
  // than where the policy asked. Whether it has arrived is a comparison against
  // the current observation, which PowerManager makes when it builds a
  // ScaleRadioStatus; the machine deliberately caches nothing to compare with.
  bool inTransition() const {
    return _state == State::Suspending || _state == State::Stopping ||
           _state == State::Starting;
  }
  bool hasFailed() const { return _state == State::Failed; }
  // The machine stopped the controller. Whether that reads as the product's
  // standby depends on the selected policy, which is the caller's business.
  bool controllerStopped() const { return _state == State::ControllerStopped; }

 private:
  static constexpr uint32_t kServiceStopTimeoutMs = 5000;

  ble::ControllerTransition _controllerTransition;
  State _state = State::Monitoring;
  ScaleRadioPolicy _policy = ScaleRadioPolicy::Demand;
  bool _activityPending = false;
  uint32_t _stageStartedMs = 0;

  Decision advanceTransition(const Inputs& inputs);
  Decision updateMonitoring(const Inputs& inputs, bool shouldRun);
  Decision updateSuspending(const Inputs& inputs, bool shouldRun);
  Decision updateControllerStopped(const Inputs& inputs, bool shouldRun);
  Decision updateFailed(const Inputs& inputs, bool shouldRun);
  Decision beginControllerStop(const Inputs& inputs);
  Decision beginControllerStart(const Inputs& inputs);
  Decision fail(FailureArea area, const char* reason);

  static Decision action(Action action);
  static Decision event(EventKind kind);
  static Decision failure(FailureArea area, const char* reason);
};

}  // namespace power
