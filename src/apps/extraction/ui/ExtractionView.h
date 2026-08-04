// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <M5Unified.h>

#include <cstdint>

#include "apps/extraction/Extraction.h"

namespace pump_scale {

// Chart for an Extraction.
//
// Live mode (IDLE / RUNNING / POST_PUMP): 30s scrolling window, per-pixel
// min/max envelope of weight samples. Once in POST_PUMP, a thin vertical
// marker at the pump-off moment scrolls left across the trace.
//
// Static mode (DONE): non-scrolling, fits the whole extraction. Direct
// annotations on the plot: pump-on / pump-off / stable markers, a faint
// horizontal "destination line" at the final weight, and a dimmed
// flow-rate (g/s) overlay computed from a 1-second centred finite difference.
// Scale-gap sample runs are drawn as a dashed warn-coloured horizontal at
// the last known weight.
//
// Stateless: all data lives in the passed Extraction. nowMs is only consulted
// for live-mode scrolling.
//
// showLiveSamples gates the live (scrolling) trace: when false the live panel
// draws only its baseline axis, so a shot that hasn't yet crossed the
// meaningful-yield bar (a flush, a grinder dose, the pre-infusion lead-in) is
// not plotted. It has no effect in static (DONE) mode, which always renders the
// finished record in full.
namespace ExtractionView {

void draw(LGFX_Sprite* canvas, const Extraction& e, uint32_t nowMs, int x,
          int y, int w, int h, bool showLiveSamples);

}  // namespace ExtractionView

}  // namespace pump_scale
