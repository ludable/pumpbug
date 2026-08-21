// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#include "storage.h"

#include <LittleFS.h>
#include <M5Unified.h>
#include <esp_partition.h>
#include <esp_spi_flash.h>

#include <array>
#include <atomic>

namespace storage {
namespace {

constexpr const char* kPartitionLabel = "history";
constexpr const char* kBasePath = "/littlefs";
constexpr uint8_t kMaxOpenFiles = 10;
constexpr size_t kLittleFsBlockBytes = SPI_FLASH_SEC_SIZE;

std::atomic<MountState> currentState{MountState::Unavailable};

enum class ErasedState : uint8_t { Erased, Programmed, Unreadable };

bool beginHistory() {
  return LittleFS.begin(/*formatOnFail=*/false, kBasePath, kMaxOpenFiles,
                        kPartitionLabel);
}

ErasedState initialBlocksErased() {
  const esp_partition_t* partition = esp_partition_find_first(
      ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS,
      kPartitionLabel);
  if (!partition || partition->size < 2 * kLittleFsBlockBytes) {
    return ErasedState::Unreadable;
  }

  std::array<uint8_t, 256> buffer;
  for (size_t offset = 0; offset < 2 * kLittleFsBlockBytes;
       offset += buffer.size()) {
    if (esp_partition_read(partition, offset, buffer.data(), buffer.size()) !=
        ESP_OK) {
      return ErasedState::Unreadable;
    }
    for (uint8_t byte : buffer) {
      if (byte != 0xff) return ErasedState::Programmed;
    }
  }
  return ErasedState::Erased;
}

MountState publish(bool ready) {
  const MountState state = ready ? MountState::Ready : MountState::Unavailable;
  currentState.store(state, std::memory_order_release);
  return state;
}

// beginHistory() runs first so the Arduino wrapper retains the non-default
// partition label used by format().
bool formatAndMount() {
  if (!LittleFS.format()) {
    M5_LOGE("storage: LittleFS format failed");
    return false;
  }
  if (!beginHistory()) {
    M5_LOGE("storage: LittleFS mount after format failed");
    return false;
  }
  M5_LOGI("storage: formatted and mounted");
  return true;
}

}  // namespace

MountState mount() {
  // Besides attempting the mount, this supplies the non-default partition
  // label used by LittleFS.format().
  const bool mounted = beginHistory();

  if (mounted) return publish(true);

  const ErasedState erased = initialBlocksErased();
  if (erased == ErasedState::Erased) {
    M5_LOGI("storage: preparing erased history partition");
    return publish(formatAndMount());
  }
  if (erased == ErasedState::Unreadable) {
    M5_LOGE("storage: cannot inspect history partition");
  } else {
    M5_LOGE("storage: history partition is not blank and cannot be mounted");
  }
  return publish(false);
}

MountState retryMount() { return publish(beginHistory()); }

MountState format() {
  if (mountState() == MountState::Ready) {
    LittleFS.end();
  } else {
    const bool mounted = beginHistory();
    if (mounted) LittleFS.end();
  }
  return publish(formatAndMount());
}

MountState mountState() { return currentState.load(std::memory_order_acquire); }

const char* mountStateName(MountState state) {
  return state == MountState::Ready ? "ready" : "unavailable";
}

}  // namespace storage
