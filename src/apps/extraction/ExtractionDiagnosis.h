// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <cstdint>

#include "Extraction.h"

// Classification of finalized extraction records.
namespace pump_scale {

// Reports whether a record contains enough sustained, pour-shaped weight gain
// to be an espresso extraction.
//
// The recorder has already reduced the raw samples to two facts: how long
// pour-shaped flow lasted and how much weight it added. A normal extraction
// makes both large. Common non-shots fail for different reasons:
//
// - A grinder dose or stationary cup has weight but no sustained flow.
// - Placing or nudging a cup produces an abrupt step rather than pour flow.
// - Taring or lifting the cup translates the raw weights without adding to the
//   accumulated gain of accepted pour runs.
//
// The thresholds are specific to this device and should be validated before
// use with other machines, recipes, or scales.
inline bool isLikelyRealShot(const Extraction& e) {
  constexpr uint32_t kMinPourMs = 5000;         // sustained pour-band flow
  constexpr int32_t kMinDecisionGainCg = 1000;  // 10 g in-band gain
  return e.pourMs >= kMinPourMs && e.decisionGainCg >= kMinDecisionGainCg;
}

}  // namespace pump_scale
