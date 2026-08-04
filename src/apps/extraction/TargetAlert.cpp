// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#include "TargetAlert.h"

#include <cmath>

namespace pump_scale {

namespace {
int16_t clampToInt16(float v) {
  if (v > 32767.0f) return 32767;
  if (v < -32767.0f) return -32767;
  return static_cast<int16_t>(std::lround(v));
}
}  // namespace

void TargetAlert::onRunningEntry() {
  _flow.reset();
  _stopNowFired = false;
  _haveLastSeq = false;
  _haveSample = false;
  _lastYieldCg = 0;
  _state = State{};
}

void TargetAlert::reset() { onRunningEntry(); }

const TargetAlert::State& TargetAlert::update(
    uint32_t nowMs, bool phaseRunning, const TrustedYieldSample& sample) {
  // Ingest a new trusted sample (deduplicated by seq), but only while the
  // pump is running. While it is off the flow estimate and the held yield keep
  // their pump-on values to preserve the alert across a same-shot
  // POST_PUMP→RUNNING coalesce (a profiling or pre-infusion pulse): if the
  // off-pump drip samples were pushed, they would fill the estimator's window
  // with near-zero flow and soften or delay the alert when the pump resumes,
  // and _lastYieldCg would drift off the pump-off yield that snapshotAtStop()
  // reports. On resume, the samples still inside the estimator's window are by
  // then older than the window spans, so the estimate re-primes from fresh
  // pump-on samples while the EMA smoothing carries over. A withheld knock
  // arrives as a repeated seq and is dropped here, so it never reaches the flow
  // estimate. Reaching the target while the pump is off does not trigger an
  // alarm because there is no running extraction to stop.
  if (phaseRunning && sample.have) {
    _lastYieldCg = sample.yieldCg;
    _haveSample = true;
    if (!_haveLastSeq || sample.seq != _lastSeq) {
      _lastSeq = sample.seq;
      _haveLastSeq = true;
      _flow.push(sample.tMs, sample.yieldCg);
    }
  }

  // Current flow validity: estimable, not stale (FlowEstimator), above the
  // floor (TargetAlert policy).
  float flow = 0.0f;
  bool flowValid = false;
  {
    float raw;
    if (_haveSample && _flow.flow(nowMs, raw)) {
      flow = raw;
      flowValid = flow >= kFlowFloorCgPerS;
    }
  }

  const bool alreadyFired = _stopNowFired;

  State s;
  s.flowValid = flowValid;
  s.flowCgPerS = flowValid ? clampToInt16(flow) : 0;

  // Estimate how much more coffee will reach the cup after the pump stops
  // (`overshoot`). Add it to the current yield to predict the settled yield if
  // the pump stopped now (`projected`), and subtract it from the remaining
  // target weight when calculating the approach countdown (`tRemaining`).
  int16_t projected = _lastYieldCg;
  int32_t tRemaining = 0;
  if (_haveSample) {
    float ov = flowValid ? flow * (_coeffs.tauMs / 1000.0f) + _coeffs.cCg
                         : static_cast<float>(_coeffs.cCg);
    if (ov < 0.0f) ov = 0.0f;
    if (ov > kMaxOvershootCg) ov = kMaxOvershootCg;
    const int16_t overshoot = static_cast<int16_t>(std::lround(ov));
    projected = clampToInt16(static_cast<float>(_lastYieldCg) + overshoot);
    if (flowValid) {
      const float roomCg =
          static_cast<float>(_coeffs.targetCg) - _lastYieldCg - overshoot;
      tRemaining = static_cast<int32_t>(std::lround(roomCg * 1000.0f / flow)) -
                   static_cast<int32_t>(_coeffs.reactionLeadMs);
    }
  }
  s.projectedFinalCg = projected;
  s.tRemainingMs = tRemaining;

  // Once the projection reaches the target, STOP_NOW stays on until the next
  // shot. Keeping it on prevents a temporary flow stall from retracting a
  // warning the user may already be acting on. Abrupt knocks cannot trigger it
  // because the recorder withholds them from the trusted yield stream. Slow
  // pressure on the scale can pass that filter and may trigger the alert near
  // the target.
  const bool alertEnabled =
      _coeffs.armed && _coeffs.targetCg > 0 && phaseRunning;
  if (alertEnabled && _haveSample) {
    if (projected >= _coeffs.targetCg) _stopNowFired = true;
    if (_stopNowFired) {
      s.level = Level::STOP_NOW;
    } else if (flowValid && tRemaining > 0 &&
               tRemaining <= static_cast<int32_t>(kApproachWindowMs)) {
      s.level = Level::APPROACHING;
    }
  }

  // stopNowEdge marks the tick STOP_NOW first turned on. The recorder
  // consumes this edge to emit exactly one ALARM_TRIGGERED event per shot;
  // stopNowFired itself stays true until the next onRunningEntry().
  s.stopNowFired = _stopNowFired;
  s.stopNowEdge = !alreadyFired && _stopNowFired;

  _state = s;
  return _state;
}

AlarmContext TargetAlert::snapshotAtStop() const {
  AlarmContext ctx;
  ctx.yieldCg = _lastYieldCg;
  // The flow here is already over the trusted yield, so a knock never reaches
  // it; floor-only validity is enough (a dropout leaves it false).
  ctx.flowValid = _state.flowValid;
  ctx.flowCgPerS = _state.flowCgPerS;
  ctx.projectedFinalCg = _state.projectedFinalCg;
  return ctx;
}

}  // namespace pump_scale
