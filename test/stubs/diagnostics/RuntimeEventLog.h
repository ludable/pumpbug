// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <cstdint>

namespace diagnostics {

enum class NetSource : uint8_t { Wifi, Ble };

enum class BleFailureCode : uint16_t {
  Connect = 0,
  Handshake,
  Disconnect,
  ScaleServiceStop,
  ScaleControllerStop,
  ScaleControllerStart,
  ScaleControllerUnusable,
};

class RuntimeEventLog {
 public:
  void pushNetFailure(NetSource source, uint16_t code, const char* message) {
    ++netWrites;
    lastSource = source;
    lastCode = code;
    lastMessage = message;
  }

  unsigned netWrites = 0;
  NetSource lastSource = NetSource::Wifi;
  uint16_t lastCode = 0;
  const char* lastMessage = nullptr;
};

}  // namespace diagnostics

extern diagnostics::RuntimeEventLog runtimeEventLog;
