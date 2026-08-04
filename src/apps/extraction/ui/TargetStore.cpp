// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#include "TargetStore.h"

#include <Preferences.h>

namespace {
constexpr const char* NS = "target";
constexpr const char* K_TARGET = "targetCg";
constexpr const char* K_ARMED = "armed";
constexpr const char* K_TAU = "tauMs";
constexpr const char* K_C = "cCg";
constexpr const char* K_REACTION = "reactMs";
}  // namespace

void TargetStore::load() {
  if (_loaded) return;

  Preferences p;
  p.begin(NS, /*readOnly=*/true);
  _targetCg = p.getUShort(K_TARGET, 0);
  _armed = p.getBool(K_ARMED, false);
  _tauMs = p.getUShort(K_TAU, kDefaultTauMs);
  _cCg = static_cast<int16_t>(p.getShort(K_C, kDefaultCCg));
  _reactionLeadMs = p.getUShort(K_REACTION, kDefaultReactionLeadMs);
  p.end();

  // A target below the floor is not a valid alert target: 0 is unset, and a
  // smaller nonzero value can only come from older firmware. Read it back as no
  // target and disarm, since arming without a target is meaningless.
  if (_targetCg < pump_scale::kMinTargetCg) {
    _targetCg = 0;
    _armed = false;
  }

  _loaded = true;
}

pump_scale::TargetCoeffs TargetStore::coeffs() const {
  pump_scale::TargetCoeffs c;
  c.targetCg = static_cast<int16_t>(_targetCg);
  c.armed = _armed;
  c.tauMs = _tauMs;
  c.cCg = _cCg;
  c.reactionLeadMs = _reactionLeadMs;
  return c;
}

void TargetStore::setTarget(uint16_t targetCg) {
  if (_targetCg == targetCg) return;
  _targetCg = targetCg;
  Preferences p;
  p.begin(NS, /*readOnly=*/false);
  p.putUShort(K_TARGET, _targetCg);
  p.end();
}

void TargetStore::setArmed(bool armed) {
  if (_armed == armed) return;
  _armed = armed;
  Preferences p;
  p.begin(NS, /*readOnly=*/false);
  p.putBool(K_ARMED, _armed);
  p.end();
}

void TargetStore::setTargetAndArmed(uint16_t targetCg, bool armed) {
  if (_targetCg == targetCg && _armed == armed) return;
  _targetCg = targetCg;
  _armed = armed;
  Preferences p;
  p.begin(NS, /*readOnly=*/false);
  p.putUShort(K_TARGET, _targetCg);
  p.putBool(K_ARMED, _armed);
  p.end();
}
