// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#include "ScaleReadingTimingObserver.h"

#if PB_SCALE_READING_TIMING

#include <atomic>

namespace pump_scale {
namespace {

std::atomic<const ScaleReadingTimingObserver*> activeObserver{nullptr};

}  // namespace

void setScaleReadingTimingObserver(const ScaleReadingTimingObserver* observer) {
  activeObserver.store(observer, std::memory_order_release);
}

void observeScaleReadingConsumed(uint32_t sequence, uint32_t scaleArrivalMs,
                                 uint32_t consumedMs) {
  const ScaleReadingTimingObserver* observer =
      activeObserver.load(std::memory_order_acquire);
  if (observer && observer->consumed) {
    observer->consumed(sequence, scaleArrivalMs, consumedMs);
  }
}

void observeScaleReadingPresented(uint32_t sequence, uint32_t presentedMs) {
  const ScaleReadingTimingObserver* observer =
      activeObserver.load(std::memory_order_acquire);
  if (observer && observer->presented) {
    observer->presented(sequence, presentedMs);
  }
}

void observeScaleReadingWebStateSent(uint32_t sequence, uint32_t sentMs) {
  const ScaleReadingTimingObserver* observer =
      activeObserver.load(std::memory_order_acquire);
  if (observer && observer->webStateSent) {
    observer->webStateSent(sequence, sentMs);
  }
}

}  // namespace pump_scale

#endif
