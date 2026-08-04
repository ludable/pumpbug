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

// Boot-time reason shot history was left unavailable. This is recovery-screen
// detail, not part of the extraction wire protocol.
enum class FailureReason : uint8_t {
  None,
  Filesystem,
  SettingsUnavailable,
};

// Mount shot-history storage before network and UI tasks start. A requested
// reset is completed here. A volume with no record of prior initialization is
// prepared automatically; a previously initialized volume that cannot mount
// is left untouched for explicit recovery.
MountState mount();
MountState mountState();
FailureReason failureReason();

// Request a format on the next boot. Runtime code never replaces a mounted
// filesystem because HTTP and UI tasks may be reading it. The request survives
// an interrupted format and is cleared after a completed attempt.
bool requestFormat();

const char* mountStateName(MountState state);

}  // namespace storage
