// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <esp_system.h>

namespace diagnostics {

struct ResetReasonNames {
  const char* full;
  const char* shortName;
};

void logResetReason();

inline ResetReasonNames resetReasonNames(esp_reset_reason_t reason) {
  switch (reason) {
    case ESP_RST_UNKNOWN:
      return {"unknown", "?"};
    case ESP_RST_POWERON:
      return {"power-on", "pwron"};
    case ESP_RST_EXT:
      return {"external", "ext"};
    case ESP_RST_SW:
      return {"software", "sw"};
    case ESP_RST_PANIC:
      return {"panic", "panic"};
    case ESP_RST_INT_WDT:
      return {"interrupt-watchdog", "int-wdt"};
    case ESP_RST_TASK_WDT:
      return {"task-watchdog", "task-wdt"};
    case ESP_RST_WDT:
      return {"watchdog", "wdt"};
    case ESP_RST_DEEPSLEEP:
      return {"deep-sleep", "deepslp"};
    case ESP_RST_BROWNOUT:
      return {"brownout", "brownout"};
    case ESP_RST_SDIO:
      return {"SDIO", "SDIO"};
  }
  return {"unrecognized", "?"};
}

}  // namespace diagnostics
