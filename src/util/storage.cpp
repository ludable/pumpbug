// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#include "storage.h"

#include <LittleFS.h>
#include <M5Unified.h>
#include <Preferences.h>

#include <atomic>

namespace storage {
namespace {
constexpr char kNvsNamespace[] = "storage";
constexpr char kInitializedKey[] = "initialized";
constexpr char kFormatRequestedKey[] = "format_req";
constexpr const char* kPartitionLabel = "history";
constexpr const char* kBasePath = "/littlefs";
constexpr uint8_t kMaxOpenFiles = 10;

// ESP-IDF permits 15 visible characters plus the null terminator.
constexpr size_t kNvsNameMaxBytes = 16;
static_assert(sizeof(kNvsNamespace) <= kNvsNameMaxBytes);
static_assert(sizeof(kInitializedKey) <= kNvsNameMaxBytes);
static_assert(sizeof(kFormatRequestedKey) <= kNvsNameMaxBytes);

std::atomic<MountState> currentState{MountState::Unavailable};
std::atomic<FailureReason> currentFailure{FailureReason::Filesystem};

enum class MarkerState : uint8_t { Absent, Present, Unreadable };

MarkerState readBool(const char* key) {
  Preferences preferences;
  // Read-write mode creates the namespace on a fresh NVS. A read-only open
  // cannot distinguish ordinary absence from failure to open the namespace.
  if (!preferences.begin(kNvsNamespace, /*readOnly=*/false)) {
    return MarkerState::Unreadable;
  }
  const bool present = preferences.getBool(key, false);
  preferences.end();
  return present ? MarkerState::Present : MarkerState::Absent;
}

bool writeBool(const char* key, bool value) {
  Preferences preferences;
  if (!preferences.begin(kNvsNamespace, /*readOnly=*/false)) return false;
  const bool ok = preferences.putBool(key, value) > 0;
  preferences.end();
  return ok;
}

bool rememberInitialized() {
  if (readBool(kInitializedKey) == MarkerState::Present) return true;
  if (writeBool(kInitializedKey, true)) return true;
  M5_LOGW("storage: cannot save initialization state");
  return false;
}

MountState publish(FailureReason failure) {
  const MountState state = failure == FailureReason::None
                               ? MountState::Ready
                               : MountState::Unavailable;
  currentFailure.store(failure, std::memory_order_release);
  currentState.store(state, std::memory_order_release);
  return state;
}

bool beginHistory() {
  return LittleFS.begin(/*formatOnFail=*/false, kBasePath, kMaxOpenFiles,
                        kPartitionLabel);
}

// beginHistory() must run first, even when a reset was requested, because the
// Arduino wrapper retains that partition label for format().
FailureReason formatAndMount(bool clearPendingRequest) {
  const bool formatted = LittleFS.format();
  // A requested reset is cleared only after the format attempt completes. A
  // power interruption before this write leaves it set for the next boot.
  // First-use initialization has no reset request and does not touch this key.
  const bool requestCleared =
      !clearPendingRequest || writeBool(kFormatRequestedKey, false);
  if (!requestCleared) M5_LOGE("storage: cannot clear format request");
  if (!formatted) M5_LOGE("storage: LittleFS format failed");
  // A reset that still appears pending must not be followed by recording new
  // history: the next boot could format it.
  if (!requestCleared) return FailureReason::SettingsUnavailable;
  if (!formatted) return FailureReason::Filesystem;
  if (!beginHistory()) {
    M5_LOGE("storage: LittleFS mount after format failed");
    return FailureReason::Filesystem;
  }
  // This marker prevents a later mount failure from being mistaken for first
  // use. Do not record history until that protection is durable.
  if (!rememberInitialized()) {
    LittleFS.end();
    return FailureReason::SettingsUnavailable;
  }
  M5_LOGI("storage: initialized and mounted");
  return FailureReason::None;
}
}  // namespace

MountState mount() {
  const MarkerState requested = readBool(kFormatRequestedKey);
  // Besides attempting the mount, this supplies the non-default partition
  // label used by LittleFS.format().
  const bool mounted = beginHistory();

  if (requested == MarkerState::Unreadable) {
    if (mounted) LittleFS.end();
    M5_LOGE("storage: cannot read pending format request");
    return publish(FailureReason::SettingsUnavailable);
  }

  if (requested == MarkerState::Present) {
    if (mounted) LittleFS.end();
    return publish(formatAndMount(/*clearPendingRequest=*/true));
  }

  if (mounted) {
    if (rememberInitialized()) return publish(FailureReason::None);
    LittleFS.end();
    return publish(FailureReason::SettingsUnavailable);
  }

  const MarkerState initialized = readBool(kInitializedKey);
  if (initialized == MarkerState::Unreadable) {
    M5_LOGE("storage: cannot read initialization state");
    return publish(FailureReason::SettingsUnavailable);
  }
  if (initialized == MarkerState::Absent) {
    M5_LOGI("storage: preparing uninitialized shot history");
    return publish(formatAndMount(/*clearPendingRequest=*/false));
  }

  M5_LOGE("storage: previously initialized LittleFS cannot be mounted");
  return publish(FailureReason::Filesystem);
}

MountState mountState() { return currentState.load(std::memory_order_acquire); }

FailureReason failureReason() {
  return currentFailure.load(std::memory_order_acquire);
}

bool requestFormat() {
  if (writeBool(kFormatRequestedKey, true)) return true;
  M5_LOGE("storage: cannot save format request");
  return false;
}

const char* mountStateName(MountState state) {
  return state == MountState::Ready ? "ready" : "unavailable";
}

}  // namespace storage
