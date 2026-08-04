// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#include "diagnostics/RuntimeEventLog.h"

#include <Arduino.h>  // millis()

#include <cstring>

#include "util/wallclock.h"

diagnostics::RuntimeEventLog runtimeEventLog;

namespace diagnostics {

void RuntimeEventLog::pushExtractionStat(const ExtractionStat& e) {
  _extraction.push(e);
}

void RuntimeEventLog::pushNetFailure(NetSource source, uint16_t code,
                                     const char* msg) {
  NetFailure e{};
  e.ms = millis();
  e.utcSec = wallclock::utcNow();
  e.source = source;
  e.code = code;
  if (msg) {
    std::strncpy(e.msg, msg, sizeof(e.msg) - 1);
    e.msg[sizeof(e.msg) - 1] = '\0';
  }
  _net.push(e);
}

size_t RuntimeEventLog::snapshotExtraction(ExtractionStat* out, size_t max,
                                           uint32_t* outWrites) const {
  return _extraction.snapshot(out, max, outWrites);
}

size_t RuntimeEventLog::snapshotNet(NetFailure* out, size_t max,
                                    uint32_t* outWrites) const {
  return _net.snapshot(out, max, outWrites);
}

}  // namespace diagnostics
