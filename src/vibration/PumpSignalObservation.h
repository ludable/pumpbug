// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <cstdint>

// Pump state reported by vibration analysis. DecayCandidate indicates that
// pump-signal decay may have begun, but counts as pump-on until pump-off is
// confirmed.
enum class PumpSignalState : uint8_t {
  Off,
  On,
  DecayCandidate,
};

struct PumpSignalObservation {
  PumpSignalState state = PumpSignalState::Off;
  // An accepted decay onset is available only while state is Off. It is kept
  // until the next pump-on so consumers need not read the exact frame in which
  // pump-off is confirmed.
  uint32_t acceptedDecayOnsetMs = 0;
  bool hasAcceptedDecayOnset = false;

  bool isOn() const { return state != PumpSignalState::Off; }
  bool isDecayCandidate() const {
    return state == PumpSignalState::DecayCandidate;
  }
};
