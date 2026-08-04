// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <M5Unified.h>

#include "util/debounce.h"
#include "util/ema.h"
#include "util/i2c_lock.h"

class DeviceOrientation {
 public:
  enum OrientationValue : uint8_t {
    PORTRAIT = 0,           // front button on the bottom
    LANDSCAPE = 1,          // front button on the right
    PORTRAIT_FLIPPED = 2,   // front button on the top
    LANDSCAPE_FLIPPED = 3,  // front button on the left
    UNKNOWN = 128,
  };

 private:
  // Low-pass coefficient applied to accel.x/y each sample. Smaller = more
  // smoothing. At a 50ms sample period, 0.2 has a soft time constant of ~5
  // samples (~250ms), which absorbs typical bump transients.
  static constexpr float kAccelAlpha = 0.2f;

  // Hysteresis: once an orientation is committed, it takes a larger tilt to
  // enter a new one than to stay in the current one. This prevents the
  // detector from flip-flopping when the device is held near a boundary.
  static constexpr float kEnterThreshold = 0.75f;
  static constexpr float kExitThreshold = 0.55f;

  // A candidate orientation must hold for this long before we commit. Real
  // rotations easily exceed this; bumps and brief tilts do not.
  static constexpr unsigned long kDwellMs = 400;

  OrientationValue _orientation = UNKNOWN;
  OrientationValue _candidate = UNKNOWN;
  unsigned long _candidateSince = 0;
  EMA _fax{kAccelAlpha};
  EMA _fay{kAccelAlpha};
  Debounce<50> _sampleRate;
#if PB_UI_DEBUG_REMOTE
  bool _override = false;
  bool _overrideChanged = false;
#endif
  OrientationValue classify(float ax, float ay,
                            OrientationValue current) const {
    // On StickS3 held upright (USB at bottom): ay ~ -1.0
    // Rotated 90° clockwise: ax ~ +1.0
    // Upside down: ay ~ +1.0
    // Rotated 90° counter-clockwise: ax ~ -1.0

    // Prefer the current orientation with a looser "exit" threshold so it
    // stays sticky; require a stronger "enter" threshold for a new one.
    if (current == LANDSCAPE_FLIPPED && ay < -kExitThreshold)
      return LANDSCAPE_FLIPPED;
    if (current == PORTRAIT_FLIPPED && ax > kExitThreshold)
      return PORTRAIT_FLIPPED;
    if (current == LANDSCAPE && ay > kExitThreshold) return LANDSCAPE;
    if (current == PORTRAIT && ax < -kExitThreshold) return PORTRAIT;

    if (ay < -kEnterThreshold) return LANDSCAPE_FLIPPED;
    if (ax > kEnterThreshold) return PORTRAIT_FLIPPED;
    if (ay > kEnterThreshold) return LANDSCAPE;
    if (ax < -kEnterThreshold) return PORTRAIT;
    return UNKNOWN;  // no dominant axis
  }

 public:
  bool update() {
#if PB_UI_DEBUG_REMOTE
    if (_override) {
      const bool changed = _overrideChanged;
      _overrideChanged = false;
      return changed;
    }
#endif
    if (!_sampleRate()) return false;
    // M5.Imu.update() reads the BMI270 over the shared bus; take the lock so it
    // can't collide with the FIFO drain task. See util/i2c_lock.h.
    bool updated;
    m5::imu_3d_t accel;
    {
      I2cLock lock;
      updated = M5.Imu.update();
      if (updated) accel = M5.Imu.getImuData().accel;
    }
    if (!updated) return false;

    const OrientationValue observed =
        classify(_fax.update(accel.x), _fay.update(accel.y), _orientation);
    // Ambiguous tilt: don't reset the dwell timer — a brief swing through the
    // dead zone shouldn't invalidate an in-progress candidate.
    if (observed == UNKNOWN) return false;

    const unsigned long now = millis();
    if (observed != _candidate) {
      _candidate = observed;
      _candidateSince = now;
      return false;
    }
    if (observed == _orientation) return false;
    if (now - _candidateSince < kDwellMs) return false;

    _orientation = observed;
    return true;
  }

  OrientationValue getOrientation() const { return _orientation; }

#if PB_UI_DEBUG_REMOTE
  // Selects the next orientation and ignores IMU readings until released.
  void cycleOverride() {
    _override = true;
    _orientation = static_cast<OrientationValue>(((_orientation & 3) + 1) & 3);
    _overrideChanged = true;
  }

  void clearOverride() { _override = false; }
#endif

  bool isPortrait() { return (_orientation & 1) == 0; }
  bool isLandscape() { return (_orientation & 1) != 0; }

  void rotateDisplay() {
    if (_orientation != UNKNOWN) M5.Display.setRotation(_orientation);
  }
};
