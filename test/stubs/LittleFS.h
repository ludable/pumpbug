// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <FS.h>

class TestLittleFS : public fs::FS {
 public:
  bool begin(bool, const char*, unsigned, const char*) {
    ++beginCalls;
    mounted = beginSucceeds;
    return mounted;
  }

  void end() {
    ++endCalls;
    mounted = false;
  }

  bool format() {
    ++formatCalls;
    if (!formatSucceeds) return false;
    if (formatMakesMountable) beginSucceeds = true;
    return true;
  }

  void reset() {
    beginSucceeds = true;
    formatSucceeds = true;
    formatMakesMountable = true;
    mounted = false;
    beginCalls = 0;
    endCalls = 0;
    formatCalls = 0;
  }

  bool beginSucceeds = true;
  bool formatSucceeds = true;
  bool formatMakesMountable = true;
  bool mounted = false;
  unsigned beginCalls = 0;
  unsigned endCalls = 0;
  unsigned formatCalls = 0;
};

extern TestLittleFS LittleFS;
