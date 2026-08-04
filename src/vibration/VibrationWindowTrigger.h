// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <cmath>
#include <cstdint>

// Detects a sustained, stationary vibration in the pump's frequency range.
// Detection starts after N_ON consecutive frames meet SNR_ON_DB and ends after
// N_OFF consecutive frames fail the lower SNR_STAY_DB threshold.
//
// VibrationSensor uses this detection to open and close the extraction
// recorder's candidate window. The class has no hardware dependencies and can
// be tested directly with feature values.
class VibrationWindowTrigger {
 public:
  // Entry requires stronger evidence than continuation so small changes near
  // the threshold do not repeatedly open and close the window.
  static constexpr float SNR_ON_DB = 14.0f;    // entry SNR floor
  static constexpr float SNR_STAY_DB = 11.0f;  // hysteresis stay floor
  static constexpr float PEAK_MIN_HZ = 90.0f;
  static constexpr float PEAK_MAX_HZ = 130.0f;
  static constexpr uint8_t N_ON = 2;   // consecutive frames to open
  static constexpr uint8_t N_OFF = 4;  // consecutive frames to close

  // Process one set of vibration features and return the current detection.
  bool step(bool stationary, float snrDb, float peakHz) {
    const bool enter =
        stationary && std::isfinite(snrDb) && snrDb >= SNR_ON_DB &&
        std::isfinite(peakHz) && peakHz >= PEAK_MIN_HZ && peakHz <= PEAK_MAX_HZ;
    const bool stay =
        stationary && std::isfinite(snrDb) && snrDb >= SNR_STAY_DB &&
        std::isfinite(peakHz) && peakHz >= PEAK_MIN_HZ && peakHz <= PEAK_MAX_HZ;

    if (!_active) {
      if (enter) {
        if (++_onCount >= N_ON) {
          _active = true;
          _offCount = 0;
        }
      } else {
        _onCount = 0;
      }
    } else if (stay) {
      _offCount = 0;
    } else if (++_offCount >= N_OFF) {
      _active = false;
      _onCount = 0;
    }
    return _active;
  }

  bool active() const { return _active; }

  void reset() {
    _active = false;
    _onCount = 0;
    _offCount = 0;
  }

 private:
  bool _active = false;
  uint8_t _onCount = 0;
  uint8_t _offCount = 0;
};
