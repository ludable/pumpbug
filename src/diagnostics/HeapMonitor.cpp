// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#include "diagnostics/HeapMonitor.h"

#include <Arduino.h>  // millis()
#include <esp_heap_caps.h>
#include <freertos/task.h>

#include <atomic>

#include "util/wallclock.h"

diagnostics::HeapMonitor heapMonitor;

namespace diagnostics {

namespace {
// The allocator invokes the callback in the task whose allocation failed, so
// numeric fields use lock-free atomics and the fixed-size diagnostic strings
// are best-effort volatile snapshots. A reader can observe strings from
// adjacent failures, which is acceptable for this diagnostic record.
std::atomic<uint32_t> gAllocFailCount{0};
std::atomic<uint32_t> gLastAllocFailSize{0};
std::atomic<uint32_t> gLastAllocFailCaps{0};
volatile char gLastAllocFailTask[16] = "";
volatile char gLastAllocFailFn[32] = "";

template <size_t N>
void copyVolatile(volatile char (&dst)[N], const char* src) {
  dst[0] = '\0';
  if (!src) return;
  size_t i = 0;
  for (; i + 1 < N && src[i]; ++i) dst[i] = src[i];
  dst[i] = '\0';
}

template <size_t N>
void readVolatile(char (&dst)[N], const volatile char (&src)[N]) {
  size_t i = 0;
  for (; i + 1 < N; ++i) {
    const char c = src[i];
    dst[i] = c;
    if (c == '\0') return;
  }
  dst[i] = '\0';
}

void onHeapAllocFailed(size_t size, uint32_t caps, const char* fn) {
  gLastAllocFailSize.store(static_cast<uint32_t>(size),
                           std::memory_order_relaxed);
  gLastAllocFailCaps.store(caps, std::memory_order_relaxed);
  copyVolatile(gLastAllocFailTask, pcTaskGetName(nullptr));
  copyVolatile(gLastAllocFailFn, fn);
  gAllocFailCount.fetch_add(1, std::memory_order_relaxed);
}

constexpr uint32_t kInternalCaps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
constexpr uint32_t kDmaCaps = MALLOC_CAP_DMA | MALLOC_CAP_8BIT;
constexpr uint32_t kPsramCaps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
}  // namespace

void HeapMonitor::begin() {
  static bool installed = false;
  if (installed) return;
  heap_caps_register_failed_alloc_callback(onHeapAllocFailed);
  installed = true;
}

void HeapMonitor::liveStats(HeapLive& out) const {
  out.internalFree = heap_caps_get_free_size(kInternalCaps);
  out.internalLargest = heap_caps_get_largest_free_block(kInternalCaps);
  out.internalMinEver = heap_caps_get_minimum_free_size(kInternalCaps);
  out.dmaLargest = heap_caps_get_largest_free_block(kDmaCaps);
  out.psramFree = heap_caps_get_free_size(kPsramCaps);
  out.psramLargest = heap_caps_get_largest_free_block(kPsramCaps);
  out.allocFailCount = gAllocFailCount.load(std::memory_order_relaxed);
  out.lastFailSize = gLastAllocFailSize.load(std::memory_order_relaxed);
  out.lastFailCaps = gLastAllocFailCaps.load(std::memory_order_relaxed);
  readVolatile(out.lastFailTask, gLastAllocFailTask);
  readVolatile(out.lastFailFn, gLastAllocFailFn);
}

void HeapMonitor::resetBucket(uint32_t now) {
  _bucketStartMs = now;
  _accInternalFree = UINT32_MAX;
  _accInternalLargest = UINT32_MAX;
  _accDmaLargest = UINT32_MAX;
  _accPsramFree = UINT32_MAX;
}

void HeapMonitor::tick() {
  const uint32_t now = millis();

  // Consume a deferred clear before sampling so the open bucket can't carry
  // pre-clear minima into a post-clear entry. Test-and-clear is atomic; the
  // actual reset stays on this (the owning) task. Checked every call, ahead of
  // the rate limit, so a clear takes effect within one loop iteration.
  if (_clearBucket.exchange(false, std::memory_order_acquire)) resetBucket(now);

  // Rate-limit the heap walk (largest_free_block scans the heap) to SAMPLE_MS.
  // The very first tick samples immediately (_lastSampleMs == 0).
  if (_lastSampleMs != 0 && now - _lastSampleMs < SAMPLE_MS) return;
  _lastSampleMs = now;

  // Fold this sample into the open window's running minima. The accumulators
  // start at UINT32_MAX (the min identity), so the first sample of a window
  // sets them outright — no first-sample special case, no ordering hazard.
  const uint32_t internalFree = heap_caps_get_free_size(kInternalCaps);
  const uint32_t internalLargest =
      heap_caps_get_largest_free_block(kInternalCaps);
  const uint32_t dmaLargest = heap_caps_get_largest_free_block(kDmaCaps);
  const uint32_t psramFree = heap_caps_get_free_size(kPsramCaps);
  if (internalFree < _accInternalFree) _accInternalFree = internalFree;
  if (internalLargest < _accInternalLargest)
    _accInternalLargest = internalLargest;
  if (dmaLargest < _accDmaLargest) _accDmaLargest = dmaLargest;
  if (psramFree < _accPsramFree) _accPsramFree = psramFree;

  // Close the window once it has spanned BUCKET_MS: push its worst case, then
  // restart the window and reset the accumulators to the min identity.
  if (now - _bucketStartMs >= BUCKET_MS) {
    HeapSample s{};
    s.ms = now;
    s.utcSec = wallclock::utcNow();
    s.minInternalFree = _accInternalFree;
    s.minInternalLargest = _accInternalLargest;
    s.minDmaLargest = _accDmaLargest;
    s.minPsramFree = _accPsramFree;
    _ring.push(s);
    resetBucket(now);
  }
}

size_t HeapMonitor::snapshot(HeapSample* out, size_t max,
                             uint32_t* outWrites) const {
  return _ring.snapshot(out, max, outWrites);
}

void HeapMonitor::clearHistory() {
  // Clear both halves of the history: ask tick() to reset the open bucket, then
  // clear the closed ring. Ordering matters: if tick() races before the flag it
  // may close the old bucket, but this ring clear will remove that row; if it
  // races after the flag it resets before sampling. Release pairs with tick()'s
  // acquire on the flag.
  _clearBucket.store(true, std::memory_order_release);
  _ring.clear();
}

}  // namespace diagnostics
