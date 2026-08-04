// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <cstdint>

#ifndef PB_SCALE_READING_TIMING
#define PB_SCALE_READING_TIMING 0
#endif

namespace pump_scale {

// Reports when a scale reading is consumed, displayed, and sent to a web
// client. Callbacks run inline on the main or SSE task and must return promptly.
struct ScaleReadingTimingObserver {
  void (*consumed)(uint32_t sequence, uint32_t scaleArrivalMs,
                   uint32_t consumedMs) = nullptr;
  void (*presented)(uint32_t sequence, uint32_t presentedMs) = nullptr;
  void (*webStateSent)(uint32_t sequence, uint32_t sentMs) = nullptr;
};

#if PB_SCALE_READING_TIMING
// Installs the observer used by later timing events. It must remain alive until
// it is replaced or cleared.
void setScaleReadingTimingObserver(const ScaleReadingTimingObserver* observer);
void observeScaleReadingConsumed(uint32_t sequence, uint32_t scaleArrivalMs,
                                 uint32_t consumedMs);
void observeScaleReadingPresented(uint32_t sequence, uint32_t presentedMs);
void observeScaleReadingWebStateSent(uint32_t sequence, uint32_t sentMs);
#else
inline void setScaleReadingTimingObserver(
    const ScaleReadingTimingObserver*) {}
inline void observeScaleReadingConsumed(uint32_t, uint32_t, uint32_t) {}
inline void observeScaleReadingPresented(uint32_t, uint32_t) {}
inline void observeScaleReadingWebStateSent(uint32_t, uint32_t) {}
#endif

}  // namespace pump_scale
