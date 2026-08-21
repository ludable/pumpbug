// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#include "power/PowerEventLog.h"

#include <M5Unified.h>
#include <Preferences.h>

#include <cstring>

namespace power {

// Arduino-ESP32 starts the scheduler before global constructors run, so the
// mutex can be created during static initialization.
PowerEventLog::PowerEventLog() : _mutex(xSemaphoreCreateMutex()) {}

namespace {
constexpr char kNamespace[] = "powerlog";
constexpr char kKey[] = "ring";
constexpr uint8_t kVersion = 4;

// Stored representation of the ring. Preferences writes its bytes directly,
// so it must remain trivially copyable. begin() starts a fresh log when the
// stored version does not match this layout.
struct Persisted {
  uint8_t version;
  uint32_t writes;
  PowerEvent items[PowerEventLog::CAP];
};
}  // namespace

void PowerEventLog::begin() {
  Preferences prefs;
  if (!prefs.begin(kNamespace, /*readOnly=*/true)) return;  // nothing stored
  Persisted p{};
  const size_t got = prefs.getBytes(kKey, &p, sizeof(p));
  prefs.end();
  if (got != sizeof(p) || p.version != kVersion) return;  // absent/incompatible
  // No other task can access the log during setup. Take the mutex here too so
  // every access to the entries follows the same rule.
  if (_mutex) xSemaphoreTake(_mutex, portMAX_DELAY);
  _writes = p.writes;
  std::memcpy(_items, p.items, sizeof(_items));
  if (_mutex) xSemaphoreGive(_mutex);
}

void PowerEventLog::record(PowerEventKind kind, int batteryPct,
                           bool hasExternalPower, uint32_t utcSec,
                           uint8_t resetReason, PowerBootDiagnostics boot,
                           PowerSleepDiagnostics sleep) {
  // Keep the RAM update and NVS write under one mutex. Otherwise an HTTP clear
  // could run between them and a later write could restore cleared data.
  if (_mutex) xSemaphoreTake(_mutex, portMAX_DELAY);
  PowerEvent& slot = _items[_writes % CAP];
  slot.utcSec = utcSec;
  slot.kind = static_cast<uint8_t>(kind);
  slot.batteryPct =
      (batteryPct < 0)
          ? -1
          : static_cast<int8_t>(batteryPct > 100 ? 100 : batteryPct);
  slot.hasExternalPower = hasExternalPower ? 1 : 0;
  slot.resetReason = resetReason;
  slot.boot = kind == PowerEventKind::Wake ? boot : PowerBootDiagnostics{};
  slot.sleep = kind == PowerEventKind::Sleep ? sleep : PowerSleepDiagnostics{};
  ++_writes;
  ++_revision;
  persist();  // caller holds _mutex
  if (_mutex) xSemaphoreGive(_mutex);
}

bool PowerEventLog::backfillLatestWakeTimestamp(uint32_t utcSec) {
  if (utcSec == 0) return false;
  if (_mutex) xSemaphoreTake(_mutex, portMAX_DELAY);

  bool updated = false;
  if (_writes != 0) {
    PowerEvent& latest = _items[(_writes - 1) % CAP];
    if (latest.kind == static_cast<uint8_t>(PowerEventKind::Wake) &&
        latest.utcSec == 0) {
      latest.utcSec = utcSec;
      if (persist()) {
        ++_revision;
        updated = true;
      } else {
        latest.utcSec = 0;
      }
    }
  }

  if (_mutex) xSemaphoreGive(_mutex);
  return updated;
}

bool PowerEventLog::clear() {
  // Clear RAM and NVS under one mutex so a concurrent record cannot rewrite the
  // stored value while the clear is in progress.
  if (_mutex) xSemaphoreTake(_mutex, portMAX_DELAY);
  _writes = 0;
  std::memset(_items, 0, sizeof(_items));
  ++_revision;

  bool ok = false;
  Preferences prefs;
  if (prefs.begin(kNamespace, /*readOnly=*/false)) {
    ok = prefs.clear();  // wipe all keys in the namespace
    prefs.end();
    if (!ok)
      M5_LOGW("PowerEventLog: NVS clear failed; persisted log not cleared");
  } else {
    M5_LOGW("PowerEventLog: NVS open failed; persisted log not cleared");
  }

  if (_mutex) xSemaphoreGive(_mutex);
  return ok;
}

size_t PowerEventLog::snapshot(PowerEvent* out, size_t max,
                               uint32_t* outWrites) const {
  if (_mutex) xSemaphoreTake(_mutex, portMAX_DELAY);
  const uint32_t w = _writes;
  const size_t count = (w < CAP) ? static_cast<size_t>(w) : CAP;
  const size_t n = (count < max) ? count : max;
  for (size_t i = 0; i < n; ++i) {
    out[i] = _items[(w - 1 - i) % CAP];
  }
  if (outWrites) *outWrites = w;
  if (_mutex) xSemaphoreGive(_mutex);
  return n;
}

bool PowerEventLog::persist() {
  // The caller holds _mutex, so the entries cannot change while they are copied
  // and written to NVS.
  Persisted p{};
  p.version = kVersion;
  p.writes = _writes;
  std::memcpy(p.items, _items, sizeof(_items));

  Preferences prefs;
  if (!prefs.begin(kNamespace, /*readOnly=*/false)) {
    M5_LOGW("PowerEventLog: NVS open failed; log not persisted");
    return false;
  }
  const bool ok = prefs.putBytes(kKey, &p, sizeof(p)) == sizeof(p);
  prefs.end();
  if (!ok) M5_LOGW("PowerEventLog: NVS write failed; log not persisted");
  return ok;
}

}  // namespace power
