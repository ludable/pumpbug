// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#include <cassert>
#include <cstdint>

#include "apps/extraction/ScaleReadingTimingObserver.h"

namespace {

uint32_t consumedSequence = 0;
uint32_t consumedArrivalMs = 0;
uint32_t consumedMs = 0;
uint32_t presentedSequence = 0;
uint32_t presentedMs = 0;
uint32_t webSequence = 0;
uint32_t webSentMs = 0;

void recordConsumed(uint32_t sequence, uint32_t arrivalMs,
                    uint32_t observedMs) {
  consumedSequence = sequence;
  consumedArrivalMs = arrivalMs;
  consumedMs = observedMs;
}

void recordPresented(uint32_t sequence, uint32_t observedMs) {
  presentedSequence = sequence;
  presentedMs = observedMs;
}

void recordWebState(uint32_t sequence, uint32_t observedMs) {
  webSequence = sequence;
  webSentMs = observedMs;
}

}  // namespace

int main() {
  pump_scale::observeScaleReadingConsumed(1, 2, 3);
  pump_scale::observeScaleReadingPresented(1, 4);
  pump_scale::observeScaleReadingWebStateSent(1, 5);
  assert(consumedSequence == 0);
  assert(presentedSequence == 0);
  assert(webSequence == 0);

  const pump_scale::ScaleReadingTimingObserver observer{
      recordConsumed, recordPresented, recordWebState};
  pump_scale::setScaleReadingTimingObserver(&observer);
  pump_scale::observeScaleReadingConsumed(7, 100, 108);
  pump_scale::observeScaleReadingPresented(7, 112);
  pump_scale::observeScaleReadingWebStateSent(7, 125);

  assert(consumedSequence == 7);
  assert(consumedArrivalMs == 100);
  assert(consumedMs == 108);
  assert(presentedSequence == 7);
  assert(presentedMs == 112);
  assert(webSequence == 7);
  assert(webSentMs == 125);

  pump_scale::setScaleReadingTimingObserver(nullptr);
  pump_scale::observeScaleReadingConsumed(8, 200, 210);
  assert(consumedSequence == 7);
}
