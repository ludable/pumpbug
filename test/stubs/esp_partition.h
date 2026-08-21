// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <cstddef>

using esp_err_t = int;

constexpr esp_err_t ESP_OK = 0;
constexpr int ESP_PARTITION_TYPE_DATA = 1;
constexpr int ESP_PARTITION_SUBTYPE_DATA_SPIFFS = 2;

struct esp_partition_t {
  size_t size = 0;
};

const esp_partition_t* esp_partition_find_first(int type, int subtype,
                                                const char* label);
esp_err_t esp_partition_read(const esp_partition_t* partition, size_t offset,
                             void* destination, size_t size);
