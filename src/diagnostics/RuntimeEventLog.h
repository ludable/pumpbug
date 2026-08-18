// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <freertos/FreeRTOS.h>

#include <cstddef>
#include <cstdint>

// In-RAM, non-persistent runtime event log. Producers across several tasks can
// push into this always-on facility, and the logs are rendered in the
// Diagnostics -> Logs screen.
//
// Each log consists of a spinlock-guarded ring buffer. Doesn't persist across
// reboots; the rings just show the most recent events since power-on.
namespace diagnostics {

enum class PumpDetectionEventKind : uint8_t { On = 1, Off };

// One transition from the production vibration trigger. The raw and frequency
// measurements describe the transition frame; smoothedSnrDb is the detector's
// carried EMA state. Off events also report the full detected interval and
// every failed condition across the consecutive frames that closed it.
struct PumpDetectionEvent {
  uint32_t ms;
  uint32_t utcSec;
  uint32_t detectedForMs;
  float rawSnrDb;
  float smoothedSnrDb;
  float peakHz;
  float dominantPeakHz;
  float spectralFlux;
  uint8_t closeFailureMask;
  bool stationary;
  PumpDetectionEventKind kind;
};

// One finished extraction: snapshot of the recorder's finalize edge
// (Phase::DONE). Mirrors the summary fields of pump_scale::Extraction; weights
// are centigrams and may be the NO_WEIGHT sentinel (INT16_MIN).
struct ExtractionStat {
  uint32_t startUtcSec;  // pump_scale::Extraction.startUtcSec (0 if unknown)
  uint32_t beginMs;
  uint32_t lastPumpOffConfirmedMs;
  uint32_t stableMs;
  uint32_t endMs;
  uint32_t totalPumpOnMs;
  int16_t yieldCg;
  int16_t startRawCg;
  int16_t settledRawCg;
  uint32_t pourMs;
  int32_t decisionGainCg;
  uint16_t observedSampleCount;
  uint8_t endCause;       // pump_scale::EndCause
  uint8_t yieldStatus;    // pump_scale::YieldStatus
  bool isLikelyRealShot;  // pump_scale::isLikelyRealShot() verdict
};

enum class NetSource : uint8_t { Wifi, Ble };

// ESP-IDF Wi-Fi disconnect reasons occupy one byte. Application failures start
// above that range so a numeric log entry cannot be mistaken for a driver
// reason.
enum class WifiFailureCode : uint16_t {
  BluetoothConditioning = 0x100,
};

enum class BleFailureCode : uint16_t {
  Connect = 0,
  Handshake,
  Disconnect,
  ScaleServiceStop,
  ScaleControllerStop,
  ScaleControllerStart,
  ScaleControllerUnusable,
};

// One network failure. `code` is source-specific: a Wi-Fi disconnect reason,
// a `WifiFailureCode`, or a `BleFailureCode`. `msg` is the short human label
// the Logs view shows.
struct NetFailure {
  uint32_t ms;      // millis() at the failure (uptime; always present)
  uint32_t utcSec;  // wall-clock epoch seconds, or 0 if the clock is unset
  NetSource source;
  uint16_t code;
  char msg[24];
};

class RuntimeEventLog {
 public:
  // Ring capacities — "the last few" of each kind. Tunable.
  // Twelve transitions retain about six pump-detection intervals while keeping
  // the complete JSON response within JsonStream's safe response-size budget.
  static constexpr size_t PUMP_DETECTION_CAP = 12;
  static constexpr size_t EXTRACTION_CAP = 8;
  static constexpr size_t NET_CAP = 16;

  // Describes an atomic snapshot: `writes` reports appends since the most
  // recent clear, `revision` identifies any content change, including a clear.
  struct SnapshotState {
    uint32_t writes = 0;
    uint32_t revision = 0;
  };

  void pushPumpDetection(const PumpDetectionEvent& e);
  void pushExtractionStat(const ExtractionStat& e);
  // `msg` is copied (truncated to NetFailure::msg); null is treated as "".
  void pushNetFailure(NetSource source, uint16_t code, const char* msg);

  // Copy each ring (newest entries first) into `out` (capacity `max`); returns
  // the count written (<= min(max, CAP)).
  size_t snapshotPumpDetection(PumpDetectionEvent* out, size_t max,
                               SnapshotState* outState = nullptr) const;
  size_t snapshotExtraction(ExtractionStat* out, size_t max,
                            SnapshotState* outState = nullptr) const;
  size_t snapshotNet(NetFailure* out, size_t max,
                     SnapshotState* outState = nullptr) const;

  // Empty a ring (the Logs screen clears the page in view on a long A-press).
  void clearPumpDetection() { _pumpDetection.clear(); }
  void clearExtraction() { _extraction.clear(); }
  void clearNet() { _net.clear(); }

  // Aligned 32-bit reads and writes are atomic on ESP32, so the on-device Logs
  // screen can read these revisions without taking each ring's lock.
  uint32_t pumpDetectionRevision() const { return _pumpDetection.revision; }
  uint32_t extractionRevision() const { return _extraction.revision; }
  uint32_t netRevision() const { return _net.revision; }

 private:
  template <typename T, size_t N>
  struct Ring {
    T items[N] = {};
    uint32_t writes = 0;    // entries appended since the most recent clear
    uint32_t revision = 0;  // updates since boot, including appends and clears
    mutable portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;

    void push(const T& e) {
      portENTER_CRITICAL(&mux);
      items[writes % N] = e;
      ++writes;
      ++revision;
      portEXIT_CRITICAL(&mux);
    }

    // Atomic copy of the newest entries and their corresponding counters.
    size_t snapshot(T* out, size_t max,
                    SnapshotState* outState = nullptr) const {
      portENTER_CRITICAL(&mux);
      const uint32_t w = writes;
      const size_t count = (w < N) ? static_cast<size_t>(w) : N;
      const size_t n = (count < max) ? count : max;
      for (size_t i = 0; i < n; ++i) {
        out[i] = items[(w - 1 - i) % N];
      }
      if (outState) {
        outState->writes = w;
        outState->revision = revision;
      }
      portEXIT_CRITICAL(&mux);
      return n;
    }

    void clear() {
      portENTER_CRITICAL(&mux);
      writes = 0;  // snapshot()/count keys off writes, so the ring reads empty
      ++revision;
      portEXIT_CRITICAL(&mux);
    }
  };

  Ring<PumpDetectionEvent, PUMP_DETECTION_CAP> _pumpDetection;
  Ring<ExtractionStat, EXTRACTION_CAP> _extraction;
  Ring<NetFailure, NET_CAP> _net;
};

}  // namespace diagnostics

extern diagnostics::RuntimeEventLog runtimeEventLog;
