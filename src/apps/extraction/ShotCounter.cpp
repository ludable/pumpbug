// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#include "ShotCounter.h"

#include <M5Unified.h>
#include <nvs.h>

#include <limits>

namespace pump_scale {
namespace {
constexpr char kNvsNamespace[] = "shotcount";
constexpr char kCountKey[] = "count";
constexpr size_t kNvsNameMaxBytes = 16;

static_assert(sizeof(kNvsNamespace) <= kNvsNameMaxBytes);
static_assert(sizeof(kCountKey) <= kNvsNameMaxBytes);

bool writeCount(uint64_t value) {
  nvs_handle_t handle = 0;
  esp_err_t error = nvs_open(kNvsNamespace, NVS_READWRITE, &handle);
  if (error != ESP_OK) {
    M5_LOGE("ShotCounter: NVS open failed while saving: %d",
            static_cast<int>(error));
    return false;
  }

  error = nvs_set_u64(handle, kCountKey, value);
  if (error == ESP_OK) error = nvs_commit(handle);
  nvs_close(handle);
  if (error != ESP_OK) {
    M5_LOGE("ShotCounter: NVS write failed: %d", static_cast<int>(error));
    return false;
  }
  return true;
}
}  // namespace

bool ShotCounter::load() {
  if (_loaded) return true;

  nvs_handle_t handle = 0;
  esp_err_t error = nvs_open(kNvsNamespace, NVS_READONLY, &handle);
  if (error == ESP_ERR_NVS_NOT_FOUND) {
    _value = 0;
    _loaded = true;
    return true;
  }
  if (error != ESP_OK) {
    M5_LOGE("ShotCounter: NVS open failed during load: %d",
            static_cast<int>(error));
    return false;
  }

  uint64_t value = 0;
  error = nvs_get_u64(handle, kCountKey, &value);
  nvs_close(handle);
  if (error == ESP_ERR_NVS_NOT_FOUND) {
    _value = 0;
    _loaded = true;
    return true;
  }
  if (error != ESP_OK) {
    M5_LOGE("ShotCounter: NVS read failed: %d", static_cast<int>(error));
    return false;
  }

  _value = value;
  _loaded = true;
  return true;
}

bool ShotCounter::increment() {
  if (!load()) return false;
  if (_value == std::numeric_limits<uint64_t>::max()) return true;

  ++_value;
  return writeCount(_value);
}

bool ShotCounter::reset() {
  if (!writeCount(0)) return false;
  _value = 0;
  _loaded = true;
  return true;
}

}  // namespace pump_scale
