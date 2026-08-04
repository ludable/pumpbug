// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#include "UiDebugRemote.h"

#if PB_UI_DEBUG_REMOTE

#include <Arduino.h>
#include <M5Unified.h>

#include <cstdio>
#include <cstdlib>

#include "UiHost.h"

namespace {
constexpr char kEnableSequence[] = "\x02UIDBG\n";
constexpr uint8_t kEnableLength = sizeof(kEnableSequence) - 1;
}  // namespace

UiDebugRemote::UiDebugRemote(UiHost& host) : _host(host) {}

void UiDebugRemote::begin() { Serial.begin(115200); }

button::Gesture UiDebugRemote::poll() {
  while (Serial.available() > 0) {
    const int byte = Serial.read();
    if (_enableMatch < kEnableLength) {
      if (byte == kEnableSequence[_enableMatch]) {
        if (++_enableMatch == kEnableLength)
          M5_LOGI("UiDebugRemote: enabled");
      } else {
        _enableMatch = byte == kEnableSequence[0] ? 1 : 0;
      }
      continue;
    }
    switch (byte) {
      case 'a':
        return button::Gesture::A_SHORT;
      case 'A':
        return button::Gesture::A_LONG;
      case 'b':
        return button::Gesture::B_SHORT;
      case 'B':
        return button::Gesture::B_LONG;
      case 'r':
        _host.debugCycleOrientation();
        break;
      case 'R':
        _host.debugReleaseOrientation();
        break;
      case 'P':
        dumpDisplay(Serial);
        break;
      default:
        break;
    }
  }
  return button::Gesture::NONE;
}

void UiDebugRemote::dumpDisplay(Stream& out) {
  const int width = M5.Display.width();
  const int height = M5.Display.height();

  // Heap buffers keep the loop-task stack small and consume memory only while
  // a screenshot is being transferred.
  auto* line =
      static_cast<lgfx::rgb888_t*>(malloc(width * sizeof(lgfx::rgb888_t)));
  auto* hex = static_cast<char*>(malloc(width * 6 + 1));
  if (!line || !hex) {
    free(line);
    free(hex);
    out.println("SNAP ERR no mem");
    return;
  }

  out.printf("SNAP BEGIN %d %d\n", width, height);
  for (int y = 0; y < height; ++y) {
    M5.Display.readRect(0, y, width, 1, line);
    for (int x = 0; x < width; ++x) {
      std::snprintf(&hex[x * 6], 7, "%02x%02x%02x", line[x].r, line[x].g,
                    line[x].b);
    }
    out.print("SNAP ");
    out.println(hex);
  }
  out.println("SNAP END");
  free(hex);
  free(line);
}

#else

UiDebugRemote::UiDebugRemote(UiHost&) {}

void UiDebugRemote::begin() {}

button::Gesture UiDebugRemote::poll() { return button::Gesture::NONE; }

#endif
