// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#include "shot_id_reservation.h"

#include <M5Unified.h>
#include <nvs.h>

#include <algorithm>

namespace pump_scale::shot_store_detail {
namespace {
constexpr char kNvsNamespace[] = "shots";
constexpr char kLastIdKey[] = "last_id";
constexpr size_t kNvsNameMaxBytes = 16;

static_assert(sizeof(kNvsNamespace) <= kNvsNameMaxBytes);
static_assert(sizeof(kLastIdKey) <= kNvsNameMaxBytes);
}  // namespace

uint32_t reserveNextShotId(uint32_t greatestExistingShotId) {
  nvs_handle_t handle = 0;
  esp_err_t error = nvs_open(kNvsNamespace, NVS_READWRITE, &handle);
  if (error != ESP_OK) {
    M5_LOGE("ShotStore: NVS open failed while reserving an ID: %d",
            static_cast<int>(error));
    return 0;
  }

  uint32_t storedLastId = 0;
  error = nvs_get_u32(handle, kLastIdKey, &storedLastId);
  if (error != ESP_OK && error != ESP_ERR_NVS_NOT_FOUND) {
    nvs_close(handle);
    M5_LOGE("ShotStore: NVS read failed while reserving an ID: %d",
            static_cast<int>(error));
    return 0;
  }

  const uint32_t last = std::max(storedLastId, greatestExistingShotId);
  if (last >= kMaxShotId) {
    nvs_close(handle);
    M5_LOGE("ShotStore: shot ID space exhausted");
    return 0;
  }
  const uint32_t next = last + 1;

  error = nvs_set_u32(handle, kLastIdKey, next);
  if (error == ESP_OK) error = nvs_commit(handle);
  nvs_close(handle);
  if (error != ESP_OK) {
    M5_LOGE("ShotStore: NVS write failed while reserving an ID: %d",
            static_cast<int>(error));
    return 0;
  }
  return next;
}

}  // namespace pump_scale::shot_store_detail
