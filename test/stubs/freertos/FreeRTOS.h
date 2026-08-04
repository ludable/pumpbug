// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

// Host stub for the FreeRTOS types util/i2c_lock.h names. The tests are
// single-threaded and have no shared bus, so the lock is a no-op here.

#include <cstdint>

using TickType_t = uint32_t;

#define portMAX_DELAY 0xFFFFFFFFU
#define pdTRUE 1
#define pdFALSE 0
#define pdMS_TO_TICKS(ms) (static_cast<TickType_t>(ms))
