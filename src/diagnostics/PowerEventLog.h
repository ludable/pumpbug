// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <cstddef>
#include <cstdint>

// Persistent power-event log. Unlike RuntimeEventLog (in-RAM, ephemeral), the
// power log must survive the device's power cycles: this hardware shuts down
// via a full PMIC power-off (see PowerManager::update), so "wake" is a cold
// boot and RAM — including RTC slow memory — is gone. The only medium that
// bridges a shutdown is flash, so the ring is mirrored to NVS (a single
// versioned blob under one key), rewritten on each event and reloaded at boot.
//
// Events are recorded at controlled, self-initiated moments — a Wake early in
// setup() and a Sleep right before we call powerOff() — both on the main task.
// But snapshot() and clear() are also served over HTTP from the WebServer task
// (the diagnostics routes), so the log is accessed cross-task. A single
// FreeRTOS mutex makes that boundary the class's own concern: every public
// operation takes it and runs to completion as one atomic unit — record() =
// update RAM + persist to NVS, clear() = wipe RAM + clear NVS, snapshot() =
// copy RAM. So a caller never has to know which task it's on or which parts
// touch flash; the contract is simply "safe to call from any task."
//
// The mutex is held across the NVS write, deliberately. A portMUX spinlock
// would be wrong here — it disables interrupts and must not wrap flash I/O —
// and a non-blocking read path isn't worth the complexity: record() runs only
// ~twice per power cycle (Wake before the web server is up, Sleep as we power
// off), so a web read essentially never contends, and when it could (at
// shutdown) blocking it briefly is harmless.
namespace diagnostics {

enum class PowerEventKind : uint8_t { Wake = 0, Sleep = 1 };

struct PowerEvent {
  uint32_t utcSec;    // wall-clock epoch seconds, or 0 if clock was unset
  uint8_t kind;       // PowerEventKind
  int8_t batteryPct;  // 0-100, or -1 if the reading failed
  uint8_t hasExternalPower;  // bool: external power (VIN) present at the event
  uint8_t resetReason;  // esp_reset_reason() for Wake; 0 (UNKNOWN) for Sleep
};

class PowerEventLog {
 public:
  static constexpr size_t CAP = 32;

  PowerEventLog();

  // Load the persisted ring from NVS. Call once in setup() before record().
  // A missing key or a version/size mismatch leaves the log empty.
  void begin();

  // Append an event and rewrite the NVS blob. batteryPct follows
  // power::BatteryStatus::percent (-1 on read error); clamped to [-1, 100].
  // resetReason is the raw esp_reset_reason() value, meaningful only for Wake
  // events (it tells a clean power-on apart from a crash/brownout/watchdog
  // reboot); pass 0 for Sleep.
  void record(PowerEventKind kind, int batteryPct, bool hasExternalPower,
              uint32_t utcSec, uint8_t resetReason = 0);

  // Wipe the in-RAM ring and the persisted NVS blob. Returns false if the NVS
  // namespace couldn't be opened or cleared (the in-RAM ring is reset either
  // way).
  bool clear();

  // Newest-first copy into `out` (capacity `max`); returns the count written.
  // If `outWrites` is non-null it receives the write counter captured
  // atomically with the entries, so a reader can ETag/label the response
  // consistently.
  size_t snapshot(PowerEvent* out, size_t max,
                  uint32_t* outWrites = nullptr) const;

  // Total events ever recorded this lifetime, for change-detection. A plain
  // aligned 32-bit read — it can't tear, so it needs no lock (worst case a
  // caller sees the pre- or post-write value, both fine for ETag/redraw).
  uint32_t writes() const { return _writes; }

 private:
  void persist();

  PowerEvent _items[CAP] = {};
  uint32_t _writes = 0;
  // Serializes every public operation (RAM + any NVS work) so the log is safe
  // to call from the main and WebServer tasks. The handle is only ever passed
  // by value to xSemaphore*, never mutated, so const snapshot() can take it
  // without `mutable`.
  SemaphoreHandle_t _mutex = nullptr;
};

}  // namespace diagnostics

extern diagnostics::PowerEventLog powerEventLog;
