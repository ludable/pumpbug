// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#include <LittleFS.h>
#include <esp_partition.h>
#include <esp_spi_flash.h>

#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>

#include "util/storage.h"

TestLittleFS LittleFS;

namespace {

esp_partition_t partition{2 * SPI_FLASH_SEC_SIZE};
std::array<uint8_t, 2 * SPI_FLASH_SEC_SIZE> partitionBytes;
bool partitionReadable = true;

void reset(bool mountSucceeds = true) {
  LittleFS.reset();
  LittleFS.beginSucceeds = mountSucceeds;
  partitionBytes.fill(0xff);
  partitionReadable = true;
}

}  // namespace

const esp_partition_t* esp_partition_find_first(int type, int subtype,
                                                const char* label) {
  if (type != ESP_PARTITION_TYPE_DATA ||
      subtype != ESP_PARTITION_SUBTYPE_DATA_SPIFFS ||
      std::strcmp(label, "history") != 0) {
    return nullptr;
  }
  return &partition;
}

esp_err_t esp_partition_read(const esp_partition_t*, size_t offset,
                             void* destination, size_t size) {
  if (!partitionReadable || offset + size > partitionBytes.size()) return -1;
  std::memcpy(destination, partitionBytes.data() + offset, size);
  return ESP_OK;
}

void testM5LogInfo(const char*, ...) {}
void testM5LogWarning(const char*, ...) {}
void testM5LogError(const char*, ...) {}

int main() {
  reset();
  assert(storage::mount() == storage::MountState::Ready);
  assert(LittleFS.formatCalls == 0);

  reset(false);
  assert(storage::mount() == storage::MountState::Ready);
  assert(LittleFS.formatCalls == 1);

  reset(false);
  partitionBytes[0] = 0;
  assert(storage::mount() == storage::MountState::Unavailable);
  assert(LittleFS.formatCalls == 0);

  reset(false);
  partitionBytes.back() = 0;
  assert(storage::mount() == storage::MountState::Unavailable);
  assert(LittleFS.formatCalls == 0);

  reset(false);
  partitionReadable = false;
  assert(storage::mount() == storage::MountState::Unavailable);
  assert(LittleFS.formatCalls == 0);

  reset(false);
  partitionBytes[0] = 0;
  assert(storage::mount() == storage::MountState::Unavailable);
  LittleFS.beginSucceeds = true;
  assert(storage::retryMount() == storage::MountState::Ready);
  assert(LittleFS.formatCalls == 0);

  reset(false);
  partitionBytes[0] = 0;
  assert(storage::mount() == storage::MountState::Unavailable);
  assert(storage::retryMount() == storage::MountState::Unavailable);
  assert(LittleFS.formatCalls == 0);

  reset(false);
  partitionBytes[0] = 0;
  assert(storage::mount() == storage::MountState::Unavailable);
  assert(storage::format() == storage::MountState::Ready);
  assert(LittleFS.formatCalls == 1);

  reset(false);
  partitionBytes[0] = 0;
  assert(storage::mount() == storage::MountState::Unavailable);
  LittleFS.formatSucceeds = false;
  assert(storage::format() == storage::MountState::Unavailable);
  assert(LittleFS.formatCalls == 1);

  reset(false);
  partitionBytes[0] = 0;
  assert(storage::mount() == storage::MountState::Unavailable);
  LittleFS.formatMakesMountable = false;
  assert(storage::format() == storage::MountState::Unavailable);
  assert(LittleFS.formatCalls == 1);

  std::puts("OK: storage mount and recovery policy");
  return 0;
}
