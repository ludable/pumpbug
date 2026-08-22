// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <cstdint>

#include "apps/extraction/Extraction.h"

// NVS-backed persistence for the user-configurable target weight and whether
// the alert is enabled.
//
// Threading: this store is mutated only on the UI task (the on-device overlay
// and the tick() apply path for web POSTs). HTTP handlers should never call it
// directly but stage desired values through atomics.
class TargetStore {
 public:
  // Loads the target settings from NVS. Idempotent.
  void load();

  // Combines the stored user settings with the predictor's built-in parameters.
  pump_scale::TargetCoeffs coeffs() const;

  uint16_t targetCg() const { return _targetCg; }
  bool armed() const { return _armed; }

  // Persisted setters. No-op (no NVS write) when the value is unchanged.
  void setTarget(uint16_t targetCg);
  void setArmed(bool armed);
  // Set both in one NVS transaction (the web POST carries both).
  void setTargetAndArmed(uint16_t targetCg, bool armed);

 private:
  uint16_t _targetCg = 0;
  bool _armed = false;
  bool _loaded = false;
};
