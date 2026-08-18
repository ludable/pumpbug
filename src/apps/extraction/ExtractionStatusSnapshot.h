// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <cstdint>

#include "Extraction.h"
#include "ble/BleScaleService.h"
#include "util/storage.h"
#include "vibration/PumpSignalObservation.h"

namespace pump_scale {

// Lightweight, copy-cheap snapshot of live extraction status. ExtractionScreen
// populates it under the recorder mutex where needed; HTTP /state and the SSE
// stream can safely inspect it after the lock is released.
struct ExtractionStatusSnapshot {
  bool active = false;
  Phase currentPhase = Phase::IDLE;
  uint32_t acceptedSeq = 0;
  uint32_t savedSeq = 0;  // last shot persisted to LittleFS
  storage::MountState storageState = storage::MountState::Unavailable;
  uint32_t currentBeginMs = 0;  // 0 when phase is IDLE
  uint32_t currentElapsedMs = 0;
  uint16_t currentSampleCount = 0;
  BleScaleService::State scaleState = BleScaleService::State::OFF;
  bool hasCurrentWeight = false;
  int16_t currentWeightCg = 0;
  uint32_t currentWeightSequence = 0;
  bool hasCurrentYield = false;
  int16_t currentYieldCg = 0;
  bool currentPouring = false;
  PumpSignalState pumpSignalState = PumpSignalState::Off;
  bool hasDisplayShot = false;
  uint32_t displayShotSeq = 0;
};

}  // namespace pump_scale
