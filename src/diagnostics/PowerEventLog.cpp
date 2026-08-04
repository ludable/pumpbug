// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#include "diagnostics/PowerEventLog.h"

#include <M5Unified.h>
#include <Preferences.h>

#include <cstring>

diagnostics::PowerEventLog powerEventLog;

namespace diagnostics {

// xSemaphoreCreateMutex() at static-init time is fine on arduino-esp32 (the
// scheduler is already up when global ctors run; see WifiManager, which does
// the same).
PowerEventLog::PowerEventLog() : _mutex(xSemaphoreCreateMutex()) {}

namespace {
constexpr char kNamespace[] = "powerlog";
constexpr char kKey[] = "ring";
constexpr uint8_t kVersion = 2;  // v2 repurposed the pad byte as resetReason

// On-flash layout. Same firmware reads and writes it, so a raw struct blob is
// safe; the version byte guards against a future field change (mismatch ->
// discard and start fresh). Keep this POD and trivially copyable.
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
  // Runs in setup() before the web server is up, so uncontended; lock anyway to
  // keep the "every operation goes through the mutex" contract uniform.
  if (_mutex) xSemaphoreTake(_mutex, portMAX_DELAY);
  _writes = p.writes;
  std::memcpy(_items, p.items, sizeof(_items));
  if (_mutex) xSemaphoreGive(_mutex);
}

void PowerEventLog::record(PowerEventKind kind, int batteryPct,
                           bool hasExternalPower, uint32_t utcSec,
                           uint8_t resetReason) {
  // One atomic operation: RAM update + the persist() flash write, so a
  // concurrent web clear() can't slip its NVS wipe between our RAM update and
  // our putBytes() and leave flash inconsistent with RAM.
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
  ++_writes;
  persist();  // caller holds _mutex
  if (_mutex) xSemaphoreGive(_mutex);
}

bool PowerEventLog::clear() {
  // One atomic operation: the RAM wipe and the NVS wipe can't interleave with a
  // record() (which would otherwise rewrite the blob we just cleared).
  if (_mutex) xSemaphoreTake(_mutex, portMAX_DELAY);
  _writes = 0;
  std::memset(_items, 0, sizeof(_items));

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

void PowerEventLog::persist() {
  // Private helper; the caller (record()) holds _mutex, so reading the ring and
  // doing the NVS write here is already serialized.
  Persisted p{};
  p.version = kVersion;
  p.writes = _writes;
  std::memcpy(p.items, _items, sizeof(_items));

  Preferences prefs;
  if (!prefs.begin(kNamespace, /*readOnly=*/false)) {
    M5_LOGW("PowerEventLog: NVS open failed; event not persisted");
    return;
  }
  prefs.putBytes(kKey, &p, sizeof(p));
  prefs.end();
}

}  // namespace diagnostics
