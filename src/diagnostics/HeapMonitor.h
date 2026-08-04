// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <freertos/FreeRTOS.h>

#include <atomic>
#include <cstddef>
#include <cstdint>

// Heap diagnostics: the live state of the allocator plus a short, time-bucketed
// history of worst-case pressure, on a device that runs with very little free
// internal SRAM (single-digit KB at times). Two things matter for catching a
// leak or a fragmentation creep:
//
//   1) Current state + the all-time low-water mark. "How close are we to the
//      edge right now, and how close have we ever been." These are read on
//      demand straight from the heap allocator; the low-water mark is the
//      allocator's own minimum-free tracker (survives since boot).
//
//   2) A history that doesn't lie about transients. A bare periodic sample
//      misses the brief dips that actually cause allocation failures, so each
//      history entry is a *bucket*: the worst case (minimum free / minimum
//      largest block) seen across a fixed window. A slowly-falling sequence of
//      per-bucket minima is the signature of a leak; a falling largest-block
//      with steady free is fragmentation.
//
// On top of that, the allocator's failed-allocation callback is installed so a
// malloc that returns null is counted (with its size and caps) even when the
// caller swallows the failure. That callback fires in whatever task context hit
// the failure, so it only bumps a few plain counters — no locking, no heap
// walking — and the rest is read on the main loop / web task.
//
// tick() is called every main-loop iteration; it rate-limits the actual heap
// walk to SAMPLE_MS and closes a bucket every BUCKET_MS, so the per-loop cost
// is a couple of millis() comparisons. snapshot*() are reader-side (web task).
namespace diagnostics {

// One closed history bucket: the worst case observed over the window ending at
// (ms, utcSec). "Worst" = smallest free / smallest largest-free-block, i.e. the
// moment of peak usage and peak fragmentation within the window.
struct HeapSample {
  uint32_t ms;                  // millis() at bucket close (uptime; always set)
  uint32_t utcSec;              // wall-clock epoch seconds, or 0 if clock unset
  uint32_t minInternalFree;     // min free internal 8-bit heap over the window
  uint32_t minInternalLargest;  // min largest free internal block (frag. proxy)
  uint32_t minDmaLargest;       // min largest free DMA-capable block
  uint32_t minPsramFree;        // min free PSRAM over the window
};

// Live, read-on-demand allocator state plus the persistent (since-boot)
// counters. Filled by liveStats(); the history ring is read separately.
struct HeapLive {
  uint32_t internalFree;     // free internal 8-bit heap
  uint32_t internalLargest;  // largest free internal block
  uint32_t internalMinEver;  // all-time low of internal free since boot
  uint32_t dmaLargest;       // largest free DMA-capable block
  uint32_t psramFree;        // free PSRAM
  uint32_t psramLargest;     // largest free PSRAM block
  uint32_t allocFailCount;   // total failed allocations since boot
  uint32_t lastFailSize;     // size (bytes) of the most recent failed alloc
  uint32_t lastFailCaps;     // caps bitmask of the most recent failed alloc
  char lastFailTask[16];     // FreeRTOS task that saw the latest failed alloc
  char lastFailFn[32];       // allocator call site label from ESP-IDF
};

class HeapMonitor {
 public:
  // History depth and cadence. CAP buckets of BUCKET_MS each ≈ the window of
  // history retained; SAMPLE_MS bounds how often the (heap-walking) sample
  // runs.
  static constexpr size_t CAP = 30;             // ~30 min at 1-min buckets
  static constexpr uint32_t SAMPLE_MS = 1000;   // heap walk at most once a sec
  static constexpr uint32_t BUCKET_MS = 60000;  // close a bucket each minute

  // Register the allocator's failed-alloc callback. Idempotent; call once after
  // M5.begin() (the heap component must be up). Safe to call again — the
  // registration only takes the first time.
  void begin();

  // Drive sampling and bucketing. Call every main-loop iteration; internally
  // rate-limited, so this is cheap on the loop that doesn't cross a boundary.
  void tick();

  // Read-on-demand live state. Walks the heap, so call from a normal task
  // context (main loop / web task), not an ISR.
  void liveStats(HeapLive& out) const;

  // Newest-first copy of the history ring into `out` (capacity `max`); returns
  // the count written. If `outWrites` is non-null it receives the bucket-write
  // counter captured under the same lock, for a consistent (count, writes)
  // pair.
  size_t snapshot(HeapSample* out, size_t max,
                  uint32_t* outWrites = nullptr) const;

  // Drop all recorded history: both the closed ring and the in-progress bucket,
  // so no pre-clear sample can resurface when that bucket later closes. The
  // live state and since-boot counters are untouched. Used by the web "clear"
  // and the on-device long-press on the Heap page; safe to call from any task —
  // the ring clears immediately, and the open bucket is reset by tick() on the
  // owning (main) task via a deferred flag, keeping bucket state single-writer.
  void clearHistory();

  // Total buckets ever closed, for change-detection / ETag. Plain aligned read.
  uint32_t writes() const { return _ring.writes; }

 private:
  // Same Ring shape as RuntimeEventLog: a portMUX-guarded circular buffer so a
  // push on the main loop and a snapshot on the web task can't tear.
  struct Ring {
    HeapSample items[CAP] = {};
    uint32_t writes = 0;
    mutable portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;

    void push(const HeapSample& e) {
      portENTER_CRITICAL(&mux);
      items[writes % CAP] = e;
      ++writes;
      portEXIT_CRITICAL(&mux);
    }
    size_t snapshot(HeapSample* out, size_t max, uint32_t* outWrites) const {
      portENTER_CRITICAL(&mux);
      const uint32_t w = writes;
      const size_t count = (w < CAP) ? static_cast<size_t>(w) : CAP;
      const size_t n = (count < max) ? count : max;
      for (size_t i = 0; i < n; ++i) out[i] = items[(w - 1 - i) % CAP];
      if (outWrites) *outWrites = w;
      portEXIT_CRITICAL(&mux);
      return n;
    }
    void clear() {
      portENTER_CRITICAL(&mux);
      writes = 0;
      portEXIT_CRITICAL(&mux);
    }
  };

  // Start a fresh window at `now`: reset the minima to the min identity and the
  // window-start timestamp. The single place the "fresh bucket" invariant is
  // established — called on bucket close and on a deferred clear. Main-loop
  // only.
  void resetBucket(uint32_t now);

  Ring _ring;

  // Open-bucket state (main-loop only; not shared cross-task). The minima start
  // at UINT32_MAX — the identity for min — so the first sample of each window
  // sets them with no special-case seeding, and a reset just restores the
  // identity. _bucketStartMs marks when the current window opened.
  uint32_t _bucketStartMs = 0;
  uint32_t _lastSampleMs = 0;
  uint32_t _accInternalFree = UINT32_MAX;
  uint32_t _accInternalLargest = UINT32_MAX;
  uint32_t _accDmaLargest = UINT32_MAX;
  uint32_t _accPsramFree = UINT32_MAX;

  // Deferred clear: clearHistory() (any task) sets this; tick() (main loop)
  // consumes it and resets the open bucket. Keeps the bucket accumulators
  // single-writer — the web task never touches them directly.
  std::atomic<bool> _clearBucket{false};
};

}  // namespace diagnostics

extern diagnostics::HeapMonitor heapMonitor;
