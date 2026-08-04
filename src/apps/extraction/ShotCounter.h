// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <cstdint>

namespace pump_scale {
namespace shot_counter {

// Persistent count of accepted shots. This is a user-facing lifetime counter,
// independent of the bounded shot-history archive and its retention cycle.
// Main-task only.
// Load once during setup before reading the current value.
bool load();
uint64_t value();

// Advances the in-memory counter for the current shot and persists it. Returns
// false when NVS could not be updated; the visible count still advances for
// the current session.
bool increment();

// Reset only the counter. The in-memory value changes after NVS confirms the
// write, so a failed reset can be retried without misleading the UI.
bool reset();

// Used by the full-device erase path, which reboots immediately and does not
// need to update the in-memory value.
bool clearPersisted();

}  // namespace shot_counter
}  // namespace pump_scale
