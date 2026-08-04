// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <freertos/FreeRTOS.h>

// Host stub: acquisition always succeeds, since nothing else can hold the bus.

using SemaphoreHandle_t = void*;

inline SemaphoreHandle_t xSemaphoreCreateRecursiveMutex() { return nullptr; }
inline int xSemaphoreTakeRecursive(SemaphoreHandle_t, TickType_t) {
  return pdTRUE;
}
inline int xSemaphoreGiveRecursive(SemaphoreHandle_t) { return pdTRUE; }
