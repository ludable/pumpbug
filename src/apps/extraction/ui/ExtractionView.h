// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <M5Unified.h>

#include <cstdint>

#include "apps/extraction/Extraction.h"

namespace pump_scale {

// Draws live and finished extraction charts.
//
// The live chart scrolls recent weight and flow data. The finished chart fits
// the whole shot. Both mark pump-signal decay onset and confirmed pump-off,
// shading the time between them. Finished charts also mark scale
// disconnections.
//
// `draw()` retains no state. `nowMs` controls the live scroll position.
//
// `showLiveSamples` limits the live chart to its baseline until a pour is
// established. It does not affect finished charts.
namespace ExtractionView {

void draw(LGFX_Sprite* canvas, const Extraction& e, uint32_t nowMs, int x,
          int y, int w, int h, bool showLiveSamples);

}  // namespace ExtractionView

}  // namespace pump_scale
