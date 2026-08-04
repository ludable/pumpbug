// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include "ble/bluetooth_controller_state.h"

class BleScaleService {
 public:
  enum class State {
    OFF,
    SCANNING,
    CONNECTING,
    READY,
    RECONNECTING,
  };

  struct Snapshot {
    State state = State::OFF;
  };

  struct DemandSnapshot {
    bool connectWanted = false;
    bool diagnosticScanWanted = false;
  };

  Snapshot snapshot() const { return {state}; }
  DemandSnapshot demandSnapshot() const {
    return {connectWanted, diagnosticScanWanted};
  }
  ble::ControllerState controllerState() const { return controller; }
  bool isStarted() const { return started; }

  void enable() { connectWanted = true; }
  void disable() { connectWanted = false; }
  void setPowerSuspended(bool suspended) {
    powerSuspended = suspended;
    if (suspended) {
      ++suspendRequests;
    } else {
      ++resumeRequests;
    }
  }

  bool requestControllerStop() {
    ++stopRequests;
    if (acceptStop && stopCompletesImmediately) {
      controller = ble::ControllerState::Stopped;
    }
    return acceptStop;
  }

  bool requestControllerStart() {
    ++startRequests;
    if (acceptStart && startCompletesImmediately) {
      controller = ble::ControllerState::Running;
    }
    if (dropDemandOnStart) {
      connectWanted = false;
      diagnosticScanWanted = false;
    }
    return acceptStart;
  }

  State state = State::OFF;
  ble::ControllerState controller = ble::ControllerState::Running;
  bool started = true;
  bool acceptStop = true;
  bool acceptStart = true;
  bool stopCompletesImmediately = false;
  bool startCompletesImmediately = false;
  bool dropDemandOnStart = false;
  bool connectWanted = false;
  bool diagnosticScanWanted = false;
  bool powerSuspended = false;
  unsigned suspendRequests = 0;
  unsigned resumeRequests = 0;
  unsigned stopRequests = 0;
  unsigned startRequests = 0;
};
