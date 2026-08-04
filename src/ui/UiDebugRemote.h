// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <cstdint>

#include "button.h"

#ifndef PB_UI_DEBUG_REMOTE
#define PB_UI_DEBUG_REMOTE 0
#endif

class Stream;
class UiHost;

// Accepts button events and screenshot requests over USB serial. The ordinary
// build uses the same interface as a no-op so UI input call sites do not depend
// on the debugging facility.
//
// Send "\x02UIDBG\n" before using the protocol:
//   a/A/b/B  inject a button tap or hold
//   r        select the next display orientation
//   R        resume orientation readings from the IMU
//   P        write the composed display as RGB888 rows
class UiDebugRemote {
 public:
  explicit UiDebugRemote(UiHost& host);

  // Starts serial input when the facility is enabled.
  void begin();

  // Returns the next injected gesture, or NONE when no command is available.
  button::Gesture poll();

 private:
#if PB_UI_DEBUG_REMOTE
  void dumpDisplay(Stream& out);

  UiHost& _host;
  uint8_t _enableMatch = 0;
#endif
};
