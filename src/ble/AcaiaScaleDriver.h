// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <string>

class NimBLEClient;
class NimBLERemoteCharacteristic;

// Identifies supported Acaia devices and locates their GATT characteristics.
// BleScaleService owns discovery and connection state.
namespace AcaiaScaleDriver {

struct ResolvedGatt {
  const char* profileLabel = nullptr;
  NimBLERemoteCharacteristic* writeChar = nullptr;
  NimBLERemoteCharacteristic* notifyChar = nullptr;
};

bool matchesAdvertisedName(const std::string& name);
bool resolveGatt(NimBLEClient* client, ResolvedGatt* out);

}  // namespace AcaiaScaleDriver
