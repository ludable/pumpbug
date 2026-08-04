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

// One finished extraction: snapshot of the recorder's finalize edge
// (Phase::DONE). Mirrors the summary fields of pump_scale::Extraction; weights
// are centigrams and may be the NO_WEIGHT sentinel (INT16_MIN).
struct ExtractionStat {
  uint32_t startUtcSec;  // pump_scale::Extraction.startUtcSec (0 if unknown)
  uint32_t beginMs;
  uint32_t lastPumpOffMs;
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
  static constexpr size_t EXTRACTION_CAP = 8;
  static constexpr size_t NET_CAP = 16;

  void pushExtractionStat(const ExtractionStat& e);
  // `msg` is copied (truncated to NetFailure::msg); null is treated as "".
  void pushNetFailure(NetSource source, uint16_t code, const char* msg);

  // Copy each ring newest-first into `out` (capacity `max`); returns the count
  // written (<= min(max, CAP)). Reader-side; safe to call from the UI task. If
  // `outWrites` is non-null it receives the write counter captured atomically
  // with the entries — use it (not a separate writes()) when a reader needs the
  // count and the entries to describe the same instant.
  size_t snapshotExtraction(ExtractionStat* out, size_t max,
                            uint32_t* outWrites = nullptr) const;
  size_t snapshotNet(NetFailure* out, size_t max,
                     uint32_t* outWrites = nullptr) const;

  // Empty a ring (the Logs screen clears the page in view on a long A-press).
  // Resets the write counter to 0, which also trips the screen's change
  // detection so the now-empty page redraws.
  void clearExtraction() { _extraction.clear(); }
  void clearNet() { _net.clear(); }

  // Total pushes ever per ring, for cheap change-detection in tick(). A plain
  // aligned 32-bit read; monotonic, so a torn read is impossible here.
  uint32_t extractionWrites() const { return _extraction.writes; }
  uint32_t netWrites() const { return _net.writes; }

 private:
  template <typename T, size_t N>
  struct Ring {
    T items[N] = {};
    uint32_t writes = 0;  // total pushes ever (monotonic)
    mutable portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;

    void push(const T& e) {
      portENTER_CRITICAL(&mux);
      items[writes % N] = e;
      ++writes;
      portEXIT_CRITICAL(&mux);
    }

    // Newest-first copy. Returns count written; if `outWrites` is non-null, the
    // write counter is captured under the same lock, so the caller gets a
    // consistent (count, writes) pair even if a push/clear races the read.
    size_t snapshot(T* out, size_t max, uint32_t* outWrites = nullptr) const {
      portENTER_CRITICAL(&mux);
      const uint32_t w = writes;
      const size_t count = (w < N) ? static_cast<size_t>(w) : N;
      const size_t n = (count < max) ? count : max;
      for (size_t i = 0; i < n; ++i) {
        out[i] = items[(w - 1 - i) % N];
      }
      if (outWrites) *outWrites = w;
      portEXIT_CRITICAL(&mux);
      return n;
    }

    void clear() {
      portENTER_CRITICAL(&mux);
      writes = 0;  // snapshot()/count keys off writes, so the ring reads empty
      portEXIT_CRITICAL(&mux);
    }
  };

  Ring<ExtractionStat, EXTRACTION_CAP> _extraction;
  Ring<NetFailure, NET_CAP> _net;
};

}  // namespace diagnostics

extern diagnostics::RuntimeEventLog runtimeEventLog;
