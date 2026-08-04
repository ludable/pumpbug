// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <cstdint>

#include "apps/extraction/Extraction.h"

// NVS-backed persistence for the extraction target-weight alert.
//
// Stores the user-facing target and enabled state together with the prediction
// coefficients used by TargetAlert.
//
// Threading: this store is mutated only on the UI task (the on-device overlay
// and the tick() apply path for web POSTs). HTTP handlers should never call it
// directly but stage desired values through atomics.
class TargetStore {
 public:
  // Factory prediction coefficients. These remain user-configurable through
  // the stored settings and are captured in each extraction record.
  static constexpr uint16_t kDefaultTauMs = 1000;
  static constexpr int16_t kDefaultCCg = 0;
  static constexpr uint16_t kDefaultReactionLeadMs = 0;

  // Loads config + coefficients from NVS. Idempotent.
  void load();

  // Current coefficients, shaped for TargetAlert::setCoeffs().
  pump_scale::TargetCoeffs coeffs() const;

  uint16_t targetCg() const { return _targetCg; }
  bool armed() const { return _armed; }
  uint16_t tauMs() const { return _tauMs; }
  int16_t cCg() const { return _cCg; }
  uint16_t reactionLeadMs() const { return _reactionLeadMs; }

  // Persisted setters. No-op (no NVS write) when the value is unchanged.
  void setTarget(uint16_t targetCg);
  void setArmed(bool armed);
  // Set both in one NVS transaction (the web POST carries both).
  void setTargetAndArmed(uint16_t targetCg, bool armed);

 private:
  uint16_t _targetCg = 0;
  bool _armed = false;
  uint16_t _tauMs = kDefaultTauMs;
  int16_t _cCg = kDefaultCCg;
  uint16_t _reactionLeadMs = kDefaultReactionLeadMs;
  bool _loaded = false;
};
