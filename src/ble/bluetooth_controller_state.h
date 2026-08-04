// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <cstdint>

namespace ble {

enum class ControllerState : uint8_t {
  Running,
  Stopping,
  Stopped,
  Starting,
  // A lifecycle operation failed with the controller state unknown. Recovery
  // requires a reboot because retrying setup or teardown is not safe.
  Failed,
};

}  // namespace ble
