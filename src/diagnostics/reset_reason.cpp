// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#include "reset_reason.h"

#include <M5Unified.h>

namespace diagnostics {

void logResetReason() {
  const esp_reset_reason_t reason = esp_reset_reason();
  const ResetReasonNames names = resetReasonNames(reason);
  M5_LOGI("Reset reason: %s (%u)", names.full, static_cast<unsigned>(reason));
}

}  // namespace diagnostics
