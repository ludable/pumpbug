// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#include "ShotCounter.h"

#include <M5Unified.h>
#include <Preferences.h>

#include <limits>

namespace pump_scale {
namespace shot_counter {
namespace {
constexpr char kNvsNamespace[] = "shotcount";
constexpr char kCountKey[] = "count";
constexpr size_t kNvsNameMaxBytes = 16;

static_assert(sizeof(kNvsNamespace) <= kNvsNameMaxBytes);
static_assert(sizeof(kCountKey) <= kNvsNameMaxBytes);

struct State {
  uint64_t value = 0;
  bool loaded = false;
};

State& state() {
  static State instance;
  return instance;
}

bool writeCount(uint64_t value) {
  Preferences preferences;
  if (!preferences.begin(kNvsNamespace, /*readOnly=*/false)) {
    M5_LOGE("Shot counter: NVS open failed");
    return false;
  }
  const size_t written = preferences.putULong64(kCountKey, value);
  preferences.end();
  if (written != sizeof(value)) {
    M5_LOGE("Shot counter: NVS write failed");
    return false;
  }
  return true;
}
}  // namespace

bool load() {
  State& current = state();
  if (current.loaded) return true;

  Preferences preferences;
  // Read-write mode creates the namespace on a fresh device; a read-only open
  // would report ordinary first use as an NVS error.
  if (!preferences.begin(kNvsNamespace, /*readOnly=*/false)) {
    M5_LOGE("Shot counter: NVS open failed during load");
    return false;
  }
  current.value = preferences.getULong64(kCountKey, 0);
  preferences.end();
  current.loaded = true;
  return true;
}

uint64_t value() { return state().value; }

bool increment() {
  // Do not replace an unreadable persisted count with a new value based on
  // zero. A later accepted shot will retry the load.
  if (!load()) return false;
  State& current = state();
  if (current.value == std::numeric_limits<uint64_t>::max()) return true;

  ++current.value;
  return writeCount(current.value);
}

bool reset() {
  if (!writeCount(0)) return false;
  State& current = state();
  current.value = 0;
  current.loaded = true;
  return true;
}

bool clearPersisted() { return writeCount(0); }

}  // namespace shot_counter
}  // namespace pump_scale
