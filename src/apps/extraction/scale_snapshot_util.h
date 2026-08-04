// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include "Extraction.h"
#include "ble/BleScaleService.h"

namespace pump_scale {

inline ScaleSnapshot scaleSnapshotFromBle(
    const BleScaleService::Snapshot& snap) {
  ScaleSnapshot out{};
  if (snap.state == BleScaleService::State::READY &&
      snap.weight.timestampMs != 0) {
    out.present = true;
    out.grams = snap.weight.grams;
    out.timestampMs = snap.weight.timestampMs;
    out.scaleTimerMs = snap.weight.scaleTimerMs;
    out.sequence = snap.weight.sequence;
  }
  return out;
}

}  // namespace pump_scale
