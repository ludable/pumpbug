// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// Serializes access to the shared M5.In_I2C bus.
//
// The BMI270 FIFO drains on its own task (core 0) while battery and orientation
// reads run on the main loop (core 1). Both talk to M5.In_I2C, and a collision
// mid-transaction corrupts the transfer (a stray START during another device's
// transaction). Every code path that touches M5.In_I2C must hold this lock for
// the duration of its transaction; each transaction is bounded (the largest is
// the ~15 ms FIFO batch read at 1 MHz), so contention is brief.
//
// The mutex is recursive so a guarded function that calls another guarded
// helper on the same task doesn't deadlock.
inline SemaphoreHandle_t i2cMutex() {
  // C++11 guarantees this runs exactly once, even when two cores race here.
  static SemaphoreHandle_t m = xSemaphoreCreateRecursiveMutex();
  return m;
}

// Releases the shared-bus mutex on scope exit. A timed acquisition must be
// checked with acquired() before accessing the bus.
class I2cLock {
 public:
  I2cLock() { xSemaphoreTakeRecursive(i2cMutex(), portMAX_DELAY); }
  explicit I2cLock(uint32_t timeoutMs)
      : _acquired(xSemaphoreTakeRecursive(
                      i2cMutex(), pdMS_TO_TICKS(timeoutMs)) == pdTRUE) {}
  ~I2cLock() {
    if (_acquired) xSemaphoreGiveRecursive(i2cMutex());
  }
  I2cLock(const I2cLock&) = delete;
  I2cLock& operator=(const I2cLock&) = delete;

  [[nodiscard]] bool acquired() const { return _acquired; }

 private:
  bool _acquired = true;
};
