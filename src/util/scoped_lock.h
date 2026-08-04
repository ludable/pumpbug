// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// RAII guard for a plain (non-recursive) FreeRTOS mutex passed by handle.
//
// Blocks until the mutex is taken, releases on scope exit. A null handle is a
// no-op so call sites don't have to guard against a mutex that failed to
// create. Each owner keeps its own SemaphoreHandle_t and passes it in; for the
// shared I2C bus use I2cLock instead, which owns its own recursive mutex.
class ScopedLock {
 public:
  explicit ScopedLock(SemaphoreHandle_t mutex) : _mutex(mutex) {
    if (_mutex) xSemaphoreTake(_mutex, portMAX_DELAY);
  }
  ~ScopedLock() {
    if (_mutex) xSemaphoreGive(_mutex);
  }
  ScopedLock(const ScopedLock&) = delete;
  ScopedLock& operator=(const ScopedLock&) = delete;

 private:
  SemaphoreHandle_t _mutex;
};
