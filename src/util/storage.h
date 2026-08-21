// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <cstdint>

namespace storage {

// Serialized in the extraction STATE packet. Keep the numeric values aligned
// with SHOT_STORAGE_STATUS in web-src/app/extraction/constants.js.
enum class MountState : uint8_t {
  Ready = 0,
  Unavailable = 1,
};

// Prepares shot-history storage before its consumers start. If mounting fails,
// the volume is formatted only when every byte in the initial filesystem blocks
// is 0xff. Otherwise, the volume is left unchanged for explicit recovery.
MountState mount();
MountState mountState();

// Attempts the mount again without formatting.
MountState retryMount();

// Formats the history partition after explicit confirmation. During the call,
// filesystem access must be prevented either by keeping MountState unavailable
// or by preventing every LittleFS consumer from running.
MountState format();

const char* mountStateName(MountState state);

}  // namespace storage
