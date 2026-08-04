// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <cstdint>

namespace wifi {

enum class PowerSave : uint8_t { None, MinimumModem, MaximumModem };

constexpr PowerSave kDefaultStaPowerSave = PowerSave::MaximumModem;

constexpr const char* powerSaveName(PowerSave mode) {
  switch (mode) {
    case PowerSave::None:
      return "none";
    case PowerSave::MinimumModem:
      return "minimum_modem";
    case PowerSave::MaximumModem:
      return "maximum_modem";
  }
  return "unknown";
}

}  // namespace wifi
