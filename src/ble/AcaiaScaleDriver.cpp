// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#include "AcaiaScaleDriver.h"

#include <NimBLEDevice.h>

#include <string>

#include "AcaiaV2Codec.h"

namespace AcaiaScaleDriver {
namespace {

// One Acaia protocol can appear behind multiple GATT layouts. Current
// Lunar/Pyxis firmware uses a UART-like split write/notify profile; earlier
// Lunar/Pearl firmware uses one characteristic for both directions.
struct AcaiaGattProfile {
  const char* label;
  const char* serviceUuid;
  const char* writeUuid;
  const char* notifyUuid;
  bool singleCharacteristic;
};

constexpr const char* DEVICE_NAME_PREFIXES[] = {"LUNAR", "ACAIA", "PYXIS",
                                                "PROCH"};

const AcaiaGattProfile GATT_PROFILES[] = {
    {"uart", AcaiaV2::SERVICE_UUID, AcaiaV2::CHAR_WRITE_UUID,
     AcaiaV2::CHAR_NOTIFY_UUID, false},
    {"legacy", AcaiaV2::LEGACY_SERVICE_UUID, AcaiaV2::LEGACY_CHAR_UUID,
     AcaiaV2::LEGACY_CHAR_UUID, true},
};

char upperAscii(char c) {
  return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
}

bool startsWithAsciiCaseInsensitive(const std::string& s, const char* prefix) {
  if (!prefix) return false;
  size_t i = 0;
  for (; prefix[i] != '\0'; ++i) {
    if (i >= s.size() || upperAscii(s[i]) != upperAscii(prefix[i])) {
      return false;
    }
  }
  return true;
}

NimBLERemoteCharacteristic* findCharacteristicInServices(NimBLEClient* client,
                                                         const char* uuid) {
  if (!client || !uuid) return nullptr;
  const auto& svcs = client->getServices(true);
  const NimBLEUUID target(uuid);
  for (NimBLERemoteService* svc : svcs) {
    if (!svc) continue;
    const auto& chars = svc->getCharacteristics(true);
    for (NimBLERemoteCharacteristic* ch : chars) {
      if (!ch) continue;
      if (ch->getUUID() == target) return ch;
    }
  }
  return nullptr;
}

bool resolveGattProfile(NimBLEClient* client, const AcaiaGattProfile& profile,
                        ResolvedGatt* out) {
  if (!client || !out) return false;
  NimBLERemoteCharacteristic* writeChar = nullptr;
  NimBLERemoteCharacteristic* notifyChar = nullptr;

  NimBLERemoteService* svc = client->getService(profile.serviceUuid);
  if (svc) {
    writeChar = svc->getCharacteristic(profile.writeUuid);
    notifyChar = profile.singleCharacteristic
                     ? writeChar
                     : svc->getCharacteristic(profile.notifyUuid);
  }

  // Some legacy stacks are easier to identify by characteristic than by
  // service UUID. pyacaia discovers the old profile this way, so keep the same
  // fallback for earlier Lunar/Pearl firmware.
  if ((!writeChar || !notifyChar) && profile.singleCharacteristic) {
    NimBLERemoteCharacteristic* ch =
        findCharacteristicInServices(client, profile.writeUuid);
    if (ch) {
      writeChar = ch;
      notifyChar = ch;
    }
  }

  if (!writeChar || !notifyChar) return false;
  out->profileLabel = profile.label;
  out->writeChar = writeChar;
  out->notifyChar = notifyChar;
  return true;
}

}  // namespace

bool matchesAdvertisedName(const std::string& name) {
  for (const char* prefix : DEVICE_NAME_PREFIXES) {
    if (startsWithAsciiCaseInsensitive(name, prefix)) return true;
  }
  return false;
}

bool resolveGatt(NimBLEClient* client, ResolvedGatt* out) {
  if (!out) return false;
  *out = ResolvedGatt{};
  for (const AcaiaGattProfile& profile : GATT_PROFILES) {
    if (resolveGattProfile(client, profile, out)) return true;
  }
  return false;
}

}  // namespace AcaiaScaleDriver
