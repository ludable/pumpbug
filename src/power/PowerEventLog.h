// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <cstddef>
#include <cstdint>

// Persistent power-event log.  The log stores its ring as a single versioned
// NVS blob, persists it after each change, and reloads it at boot.
//
// The main task records Wake during setup and Sleep immediately before
// powerOff(). The HTTP task can copy or clear the log at the same time. A
// FreeRTOS mutex prevents record(), backfillLatestWakeTimestamp(), clear(), and
// snapshot() from accessing the entries at the same time.
//
// Keep the mutex held while writing NVS so another record or clear cannot
// change RAM during the write. A portMUX spinlock cannot protect flash I/O
// because it disables interrupts. NVS writes are infrequent, so briefly
// delaying an HTTP read is acceptable.
namespace power {

enum class PowerEventKind : uint8_t { Wake = 0, Sleep = 1 };

enum PowerBootFlag : uint16_t {
  PowerBootPrewakeBusReady = 1u << 0,
  PowerBootPrewakeTransactionStarted = 1u << 1,
  PowerBootPrewakeTransactionSucceeded = 1u << 2,
  PowerBootDisplayDetected = 1u << 3,
  PowerBootPm1Ready = 1u << 4,
  PowerBootWakeSourceValid = 1u << 5,
  PowerBootGpioIrqValid = 1u << 6,
  PowerBootLcdRailRead = 1u << 7,
  PowerBootLcdRailOn = 1u << 8,
  PowerBootI2cConfigRead = 1u << 9,
  PowerBootI2cSleepDisabled = 1u << 10,
};

struct PowerBootDiagnostics {
  uint16_t flags = 0;
  uint8_t pm1WakeSource = 0;
  uint8_t pm1GpioIrq = 0;
  uint8_t pm1I2cConfig = 0;
};

enum PowerSleepFlag : uint8_t {
  PowerSleepPm1I2cConfigValid = 1u << 0,
};

struct PowerSleepDiagnostics {
  uint8_t flags = 0;
  // Raw PM1 I2C_CFG (0x09). Bits 3:0 are SLP_TO; 0 disables idle sleep.
  uint8_t pm1I2cConfig = 0;
};

struct PowerEvent {
  uint32_t utcSec;    // wall-clock epoch seconds, or 0 if clock was unset
  uint8_t kind;       // PowerEventKind
  int8_t batteryPct;  // 0-100, or -1 if the reading failed
  uint8_t hasExternalPower;  // bool: external power (VIN) present at the event
  uint8_t resetReason;  // esp_reset_reason() for Wake; 0 (UNKNOWN) for Sleep
  PowerBootDiagnostics boot;    // Wake boot path; zeroed for Sleep
  PowerSleepDiagnostics sleep;  // Pre-shutdown PM1 state; zeroed for Wake
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
              uint32_t utcSec, uint8_t resetReason = 0,
              PowerBootDiagnostics boot = {}, PowerSleepDiagnostics sleep = {});

  // Set the current boot's Wake timestamp after SNTP makes UTC available.
  // Returns true only after the updated log is persisted; false leaves the Wake
  // untimed so a later synchronization can retry.
  bool backfillLatestWakeTimestamp(uint32_t utcSec);

  // Wipe the in-RAM ring and the persisted NVS blob. Returns false if the NVS
  // namespace couldn't be opened or cleared (the in-RAM ring is reset either
  // way).
  bool clear();

  // Newest-first copy into `out` (capacity `max`); returns the count written.
  // If `outWrites` is non-null, the write counter is copied under the same
  // mutex as the entries so paged responses use one sequence value.
  size_t snapshot(PowerEvent* out, size_t max,
                  uint32_t* outWrites = nullptr) const;

  // In-memory content revision for the on-device Logs screen. It includes
  // timestamp updates and need not persist because the screen reads its current
  // value when it opens. No mutex required as it's an aligned 32-bit read.
  uint32_t revision() const { return _revision; }

 private:
  bool persist();

  PowerEvent _items[CAP] = {};
  uint32_t _writes = 0;
  uint32_t _revision = 0;
  // Serializes every public operation (RAM + any NVS work) so the log is safe
  // to call from the main and WebServer tasks. The handle is only ever passed
  // by value to xSemaphore*, never mutated, so const snapshot() can take it
  // without `mutable`.
  SemaphoreHandle_t _mutex = nullptr;
};

}  // namespace power
