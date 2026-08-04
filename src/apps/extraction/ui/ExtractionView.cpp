// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#include "ExtractionView.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "apps/extraction/RobustFlow.h"
#include "ui/theme.h"

namespace pump_scale {
namespace ExtractionView {

namespace {

constexpr uint32_t WINDOW_MS = 15000;  // live mode scrolling window
constexpr int MAX_COLS = 240;          // bounded by display width

// Break envelope interpolation across sample gaps larger than this. Mirrors
// the web chart's segment-splitting threshold so a true scale dropout stays
// visibly interrupted while ordinary sparse sampling draws continuously.
constexpr uint32_t GAP_MS = 500;

// Opacity for the area under the yield curve. The web chart uses ~0.18;
// LGFX's fillRectAlpha blends this over the chart background.
constexpr uint8_t YIELD_FILL_ALPHA = 46;

// Ignore only vanishing flow when deciding whether the overlay is worth
// drawing. Values beyond this are rendered as measured.
constexpr float FLOW_DISPLAY_FLOOR_G_PER_S = 0.05f;

// Pure visual breathing room at the top/bottom of the weight scale. Kept small
// so it does not recreate the old large rounded negative band below zero.
constexpr float Y_AXIS_MARGIN_FRACTION = 0.02f;

// Per-column envelope scratch, hoisted to TU-local statics so the draw paths
// don't push ~2 KB onto the loop-task stack each frame. The two draw modes
// (live, static) are mutually exclusive within a single draw() call and the
// host's draw path is single-threaded, so reusing one set of buffers is safe.
float g_colMin[MAX_COLS];
float g_colMax[MAX_COLS];
bool g_colHasData[MAX_COLS];

float niceCeilGrams(float v) {
  if (v <= 5.0f) return 5.0f;
  if (v <= 10.0f) return 10.0f;
  if (v <= 25.0f) return 25.0f;
  if (v <= 50.0f) return 50.0f;
  if (v <= 100.0f) return 100.0f;
  return std::ceil(v / 50.0f) * 50.0f;
}

struct YScale {
  float yTop;
  float yBot;
  int plotY;
  int plotH;
  float scale;  // pixels per gram

  int toY(float g) const {
    int py = plotY + plotH - 1 - static_cast<int>((g - yBot) * scale);
    if (py < plotY) py = plotY;
    if (py > plotY + plotH - 1) py = plotY + plotH - 1;
    return py;
  }
};

YScale computeYScale(float gMin, float gMax, int plotY, int plotH) {
  // Keep a nice rounded ceiling above zero so the top of the plot reads
  // cleanly. For weight itself, don't over-magnify negative excursions: use
  // the exact min value below zero before adding only a small visual margin.
  float yTop = (gMax > 0.0f) ? niceCeilGrams(gMax) : 5.0f;
  float yBot = (gMin < 0.0f) ? gMin : 0.0f;
  if (yTop <= yBot) yTop = yBot + 5.0f;
  const float margin = (yTop - yBot) * Y_AXIS_MARGIN_FRACTION;
  yTop += margin;
  yBot -= margin;
  YScale s{};
  s.yTop = yTop;
  s.yBot = yBot;
  s.plotY = plotY;
  s.plotH = plotH;
  s.scale = static_cast<float>(plotH - 1) / (yTop - yBot);
  return s;
}

float yScaleMarginGrams(const YScale& ys) {
  return (ys.yTop - ys.yBot) * Y_AXIS_MARGIN_FRACTION /
         (1.0f + 2.0f * Y_AXIS_MARGIN_FRACTION);
}

// ---------- Shared rasterisation helpers ----------

int16_t displayZeroCg(const Extraction& e) {
  return e.startRawCg != Extraction::NO_WEIGHT ? e.startRawCg : 0;
}

float displayGrams(int16_t rawCg, int16_t zeroCg) {
  return (static_cast<int32_t>(rawCg) - zeroCg) / 100.0f;
}

// Build a per-column min/max envelope by rasterising line segments between
// consecutive samples. Columns between two samples closer than GAP_MS are
// linearly interpolated, eliminating the 1-pixel gaps that appear when a
// wide landscape plot has more columns than samples.
template <typename Fn>
void buildContinuousEnvelope(const Sample* samples, size_t count, Fn accessor,
                             int plotW) {
  float (&colMin)[MAX_COLS] = g_colMin;
  float (&colMax)[MAX_COLS] = g_colMax;
  bool (&colHasData)[MAX_COLS] = g_colHasData;
  for (int i = 0; i < plotW; ++i) {
    colMin[i] = 1e30f;
    colMax[i] = -1e30f;
    colHasData[i] = false;
  }

  int prevCol = -1;
  float prevG = 0.0f;
  uint32_t prevT = 0;
  bool hasPrev = false;

  for (size_t i = 0; i < count; ++i) {
    const Sample& s = samples[i];
    int col = -1;
    float g = 0.0f;
    uint32_t t = 0;
    if (!accessor(s, col, g, t)) continue;

    if (col >= 0 && col < plotW) {
      if (g < colMin[col]) colMin[col] = g;
      if (g > colMax[col]) colMax[col] = g;
      colHasData[col] = true;
    }

    if (hasPrev && t >= prevT && (t - prevT) <= GAP_MS && prevCol != col &&
        prevCol >= 0 && prevCol < plotW && col >= 0 && col < plotW) {
      const int step = (col > prevCol) ? 1 : -1;
      for (int c = prevCol + step; c != col; c += step) {
        const float frac =
            static_cast<float>(c - prevCol) / static_cast<float>(col - prevCol);
        const float cg = prevG + (g - prevG) * frac;
        colMin[c] = colMax[c] = cg;
        colHasData[c] = true;
      }
    }

    prevCol = col;
    prevG = g;
    prevT = t;
    hasPrev = true;
  }
}

// Fill the area between the envelope and the yield zero line with a
// transparent accent-light colour, matching the web chart's weight fill.
void fillWeightEnvelope(LGFX_Sprite* c, const YScale& ys, int plotX, int plotW,
                        int zeroY) {
  float (&colMin)[MAX_COLS] = g_colMin;
  float (&colMax)[MAX_COLS] = g_colMax;
  bool (&colHasData)[MAX_COLS] = g_colHasData;
  for (int col = 0; col < plotW; ++col) {
    if (!colHasData[col]) continue;
    const int xCol = plotX + col;
    if (colMax[col] > 0.0f) {
      const int top = ys.toY(colMax[col]);
      if (zeroY >= top) {
        c->fillRectAlpha(xCol, top, 1, zeroY - top + 1, YIELD_FILL_ALPHA,
                         theme::accent_light());
      }
    }
    if (colMin[col] < 0.0f) {
      const int bot = ys.toY(colMin[col]);
      if (bot >= zeroY) {
        c->fillRectAlpha(xCol, zeroY, 1, bot - zeroY + 1, YIELD_FILL_ALPHA,
                         theme::accent_light());
      }
    }
  }
}

// Stroke the top (positive) and bottom (negative) of the envelope.
void strokeWeightEnvelope(LGFX_Sprite* c, const YScale& ys, int plotX,
                          int plotW, int zeroY) {
  float (&colMin)[MAX_COLS] = g_colMin;
  float (&colMax)[MAX_COLS] = g_colMax;
  bool (&colHasData)[MAX_COLS] = g_colHasData;

  int prevCol = -1, prevPosY = -1, prevNegY = -1;
  for (int col = 0; col < plotW; ++col) {
    if (!colHasData[col]) continue;
    const int xCol = plotX + col;
    const int posY = (colMax[col] > 0.0f) ? ys.toY(colMax[col]) : -1;
    const int negY = (colMin[col] < 0.0f) ? ys.toY(colMin[col]) : -1;
    if (prevCol >= 0) {
      const int xPrev = plotX + prevCol;
      if (prevPosY >= 0 && posY >= 0) {
        c->drawLine(xPrev, prevPosY, xCol, posY, theme::accent_light());
      } else if (posY >= 0) {
        c->drawPixel(xCol, posY, theme::accent_light());
      }
      if (prevNegY >= 0 && negY >= 0) {
        c->drawLine(xPrev, prevNegY, xCol, negY, theme::dim());
      } else if (negY >= 0) {
        c->drawPixel(xCol, negY, theme::dim());
      }
    } else {
      if (posY >= 0) c->drawPixel(xCol, posY, theme::accent_light());
      if (negY >= 0) c->drawPixel(xCol, negY, theme::dim());
    }
    prevCol = col;
    prevPosY = posY;
    prevNegY = negY;
  }
}

// ---------- Live mode ----------

// Forward declaration: defined in the static-mode section below.
bool sampleFlow(const Extraction& e, int i, int& beforeHint, float& flowOut);

struct FlowStats {
  bool has = false;
  float min = 0.0f;
  float max = 0.0f;
  float maxAbs = 0.0f;

  bool hasRenderableFlow() const {
    return has && maxAbs > FLOW_DISPLAY_FLOOR_G_PER_S;
  }
};

template <typename IncludeFn>
FlowStats computeFlowStats(const Extraction& e, IncludeFn include) {
  FlowStats stats{};
  int beforeHint = 0;
  for (size_t i = 0; i < e.sampleCount; ++i) {
    if (!include(e.samples[i])) continue;
    float f;
    if (!sampleFlow(e, static_cast<int>(i), beforeHint, f)) continue;

    if (!stats.has) {
      stats.min = f;
      stats.max = f;
      stats.has = true;
    } else {
      if (f < stats.min) stats.min = f;
      if (f > stats.max) stats.max = f;
    }
    const float absF = std::fabs(f);
    if (absF > stats.maxAbs) stats.maxAbs = absF;
  }
  return stats;
}

int flowY(float flow, const FlowStats& stats, const YScale& ys, int zeroY) {
  if (!stats.hasRenderableFlow()) return zeroY;
  const float marginG = yScaleMarginGrams(ys);
  const int flowTop = ys.toY(ys.yTop - marginG);
  const int flowBottom = ys.toY(ys.yBot + marginG);
  zeroY = std::clamp(zeroY, flowTop, flowBottom);

  const float n = std::clamp(flow / stats.maxAbs, -1.0f, 1.0f);
  int y = zeroY;
  if (n >= 0.0f) {
    const int range = std::max(0, zeroY - flowTop);
    y = zeroY - static_cast<int>(n * range);
  } else {
    const int range = std::max(0, flowBottom - zeroY);
    y = zeroY + static_cast<int>((-n) * range);
  }
  return std::clamp(y, flowTop, flowBottom);
}

template <typename IncludeFn, typename XFn>
void drawFlow(LGFX_Sprite* c, const Extraction& e, const FlowStats& stats,
              const YScale& ys, int zeroY, IncludeFn include, XFn xForSample) {
  if (!stats.hasRenderableFlow()) return;

  int prevFlowX = 0;
  int prevFlowY = 0;
  bool havePrevFlow = false;
  int beforeHint = 0;
  for (size_t i = 0; i < e.sampleCount; ++i) {
    const Sample& s = e.samples[i];
    if (!include(s)) {
      havePrevFlow = false;
      continue;
    }

    float f;
    if (!sampleFlow(e, static_cast<int>(i), beforeHint, f)) {
      havePrevFlow = false;
      continue;
    }

    const int x = xForSample(s);
    const int y = flowY(f, stats, ys, zeroY);
    if (havePrevFlow) {
      c->drawLine(prevFlowX, prevFlowY, x, y, theme::chart_flow());
    } else {
      c->drawPixel(x, y, theme::chart_flow());
      havePrevFlow = true;
    }
    prevFlowX = x;
    prevFlowY = y;
  }
}

void drawLive(LGFX_Sprite* c, const Extraction& e, uint32_t nowMs, int plotX,
              int plotY, int plotW, int plotH, bool showSamples) {
  if (plotW > MAX_COLS) plotW = MAX_COLS;
  if (plotW <= 0 || plotH <= 0) return;

  const int16_t zeroCg = displayZeroCg(e);

  // Before meaningful yield (a flush, a grinder dose, or the lead-in while
  // the cup is still empty): no trace yet. Draw just a baseline at the bottom
  // so the panel reads as "waiting", not broken.
  if (!showSamples) {
    c->drawFastHLine(plotX, plotY + plotH - 1, plotW, theme::chart_fg());
    return;
  }

  // Pass 1: scale range across the shot so far. The live chart still draws a
  // scrolling window, but sharing the same scale scope as the finished chart
  // avoids a jarring reframe when the shot flips to DONE.
  float gMin = 1e30f, gMax = -1e30f;
  for (size_t i = 0; i < e.sampleCount; ++i) {
    const Sample& s = e.samples[i];
    const float g = displayGrams(s.cg, zeroCg);
    if (g < gMin) gMin = g;
    if (g > gMax) gMax = g;
  }
  // No samples yet: draw an empty axis at zero so the box doesn't read as
  // broken.
  if (gMin > gMax) {
    gMin = 0.0f;
    gMax = 0.0f;
  }

  auto visibleSample = [&](const Sample& s) {
    return nowMs - s.tMs <= WINDOW_MS;
  };
  const FlowStats flowStats =
      computeFlowStats(e, [](const Sample&) { return true; });

  const YScale ys = computeYScale(gMin, gMax, plotY, plotH);
  const int zeroY = ys.toY(0.0f);

  if (e.sampleCount == 0) {
    c->drawFastHLine(plotX, zeroY, plotW, theme::chart_fg());
    return;
  }

  // Pass 2: continuous per-column envelope.
  buildContinuousEnvelope(
      e.samples, e.sampleCount,
      [&](const Sample& s, int& col, float& g, uint32_t& t) -> bool {
        const uint32_t age = nowMs - s.tMs;
        if (age > WINDOW_MS) return false;
        col = plotW - 1 - static_cast<int>(age * (plotW - 1) / WINDOW_MS);
        g = displayGrams(s.cg, zeroCg);
        t = s.tMs;
        return true;
      },
      plotW);

  // Paint the flow overlay above the weight stroke so near-zero/negative
  // envelope pixels do not recolour it.
  fillWeightEnvelope(c, ys, plotX, plotW, zeroY);
  c->drawFastHLine(plotX, zeroY, plotW, theme::chart_fg());
  strokeWeightEnvelope(c, ys, plotX, plotW, zeroY);

  // Flow overlay. Startup values can be slightly negative from
  // tare/settling/drift, so draw measured flow around the zero baseline.
  drawFlow(c, e, flowStats, ys, zeroY, visibleSample, [&](const Sample& s) {
    const uint32_t age = nowMs - s.tMs;
    return plotX +
           (plotW - 1 - static_cast<int>(age * (plotW - 1) / WINDOW_MS));
  });

  // Pump-off marker once in POST_PUMP, if the moment is visible. Draw it
  // dotted so it doesn't compete visually with the traces.
  if (e.phase == Phase::POST_PUMP && e.lastPumpOffMs != 0) {
    const uint32_t age = nowMs - e.lastPumpOffMs;
    if (age <= WINDOW_MS) {
      const int col =
          plotW - 1 - static_cast<int>(age * (plotW - 1) / WINDOW_MS);
      if (col >= 0 && col < plotW) {
        for (int yy = plotY; yy < plotY + plotH; yy += 3) {
          c->drawPixel(plotX + col, yy, theme::chart_fg());
        }
      }
    }
  }
}

// ---------- Static mode ----------

// Bump-robust centred-difference flow rate (g/s) at sample i. Thin adapter over
// the shared RobustFlow series (see RobustFlow.h) so the chart, flow
// statistics, and diagnostics all draw from one definition of flow, including
// its rejection of placement bumps, tares, and cup lifts.
bool sampleFlow(const Extraction& e, int i, int& beforeHint, float& flowOut) {
  if (i < 0 || i >= static_cast<int>(e.sampleCount)) return false;
  return robustSampleFlow(e.samples, e.sampleCount, static_cast<size_t>(i),
                          beforeHint, flowOut);
}

void drawStatic(LGFX_Sprite* c, const Extraction& e, int plotX, int plotY,
                int plotW, int plotH) {
  if (plotW > MAX_COLS) plotW = MAX_COLS;
  if (plotW <= 0 || plotH <= 0) return;
  // Use wrap-safe unsigned equality, not signed <=, so a shot that happens
  // to span the 32-bit millis() wrap is not rejected. (49-day wrap is
  // unreachable in practice, but this keeps the code consistent with the
  // rest of the codebase.)
  if (e.endMs == e.beginMs) return;
  const uint32_t durMs = e.endMs - e.beginMs;
  const int16_t zeroCg = displayZeroCg(e);

  // No scale data at all: just an axis line with pump-event ticks. The
  // weight headline elsewhere shows "— g" in this case.
  if (!hasSampleData(e)) {
    const int axisY = plotY + plotH / 2;
    c->drawFastHLine(plotX, axisY, plotW, theme::chart_fg());
    for (uint16_t i = 0; i < e.eventCount; ++i) {
      const Event& ev = e.events[i];
      if (ev.kind != EventKind::PUMP_ON && ev.kind != EventKind::PUMP_OFF) {
        continue;
      }
      const uint32_t off = ev.tMs - e.beginMs;
      const int col = static_cast<int>(off * (plotW - 1) / durMs);
      if (col < 0 || col >= plotW) continue;
      const uint32_t color = (ev.kind == EventKind::PUMP_ON)
                                 ? theme::accent_light()
                                 : theme::chart_fg();
      c->drawFastVLine(plotX + col, axisY - 3, 7, color);
    }
    return;
  }

  // Pass 1: gMin/gMax across all samples (displayed yield, raw - startRawCg).
  float gMin = 1e30f, gMax = -1e30f;
  for (size_t i = 0; i < e.sampleCount; ++i) {
    const Sample& s = e.samples[i];
    const float g = displayGrams(s.cg, zeroCg);
    if (g < gMin) gMin = g;
    if (g > gMax) gMax = g;
  }
  if (gMin > gMax) {
    gMin = 0.0f;
    gMax = 0.0f;
  }
  const FlowStats flowStats =
      computeFlowStats(e, [](const Sample&) { return true; });
  const YScale ys = computeYScale(gMin, gMax, plotY, plotH);
  const int zeroY = ys.toY(0.0f);

  auto sampleCol = [&](uint32_t tMs) {
    const uint32_t off = tMs - e.beginMs;
    int col = static_cast<int>(off * (plotW - 1) / durMs);
    if (col < 0) col = 0;
    if (col > plotW - 1) col = plotW - 1;
    return col;
  };

  // Pass 2: continuous per-column envelope.
  buildContinuousEnvelope(
      e.samples, e.sampleCount,
      [&](const Sample& s, int& col, float& g, uint32_t& t) -> bool {
        col = sampleCol(s.tMs);
        g = displayGrams(s.cg, zeroCg);
        t = s.tMs;
        return true;
      },
      plotW);

  // Paint the flow overlay above the weight stroke so near-zero/negative
  // envelope pixels do not recolour it.
  fillWeightEnvelope(c, ys, plotX, plotW, zeroY);
  c->drawFastHLine(plotX, zeroY, plotW, theme::chart_fg());
  strokeWeightEnvelope(c, ys, plotX, plotW, zeroY);

  // Flow overlay. Small negative values at shot start are expected from
  // tare/settling/drift, so draw measured flow around the zero baseline.
  drawFlow(
      c, e, flowStats, ys, zeroY, [](const Sample&) { return true; },
      [&](const Sample& s) { return plotX + sampleCol(s.tMs); });

  // Dashed warn-coloured horizontal across each SCALE_DISCONNECTED → next
  // SCALE_CONNECTED (or shot end) interval, drawn at the last weight
  // at-or-before the disconnect. Walks events + samples together: events
  // and samples are both time-ordered, so a single advancing sample index
  // keeps lastSampleCg current relative to the event being processed.
  auto drawDashed = [&](uint32_t fromMs, uint32_t toMs, int16_t cg) {
    const int colStart = sampleCol(fromMs);
    const int colEnd = sampleCol(toMs);
    const int gy = ys.toY(cg / 100.0f);
    for (int xc = colStart; xc <= colEnd; ++xc) {
      if ((xc / 2) & 1) continue;  // 2-on / 2-off dash pattern
      c->drawPixel(plotX + xc, gy, theme::chart_flow());
    }
  };

  int iSample = 0;
  int16_t lastSampleCg = Extraction::NO_WEIGHT;
  bool inDisconnect = false;
  uint32_t disconnectMs = 0;
  int16_t disconnectCg = 0;
  for (uint16_t iEv = 0; iEv < e.eventCount; ++iEv) {
    const Event& ev = e.events[iEv];
    // Catch lastSampleCg up through any samples that landed at-or-before this
    // event.
    while (iSample < static_cast<int>(e.sampleCount) &&
           e.samples[iSample].tMs <= ev.tMs) {
      lastSampleCg = e.samples[iSample].cg;
      ++iSample;
    }
    if (ev.kind == EventKind::SCALE_DISCONNECTED && !inDisconnect &&
        lastSampleCg != Extraction::NO_WEIGHT) {
      inDisconnect = true;
      disconnectMs = ev.tMs;
      disconnectCg = lastSampleCg;
    } else if (ev.kind == EventKind::SCALE_CONNECTED && inDisconnect) {
      drawDashed(disconnectMs, ev.tMs,
                 static_cast<int16_t>(disconnectCg - zeroCg));
      inDisconnect = false;
    }
  }
  if (inDisconnect) {
    // Shot ended while disconnected.
    drawDashed(disconnectMs, e.endMs,
               static_cast<int16_t>(disconnectCg - zeroCg));
  }

  // Vertical marker: pump-off only. Draw it dotted so it reads as a moment
  // rather than a heavy boundary. The first PUMP_ON is the shot's begin
  // (plot origin) so it needs no marker; the stable/end moment is implicit in
  // the data ending, so we don't add a second vertical line there.
  if (e.lastPumpOffMs != 0) {
    const int col = sampleCol(e.lastPumpOffMs);
    for (int yy = plotY; yy < plotY + plotH; yy += 3) {
      c->drawPixel(plotX + col, yy, theme::chart_fg());
    }
  }
}

}  // namespace

void draw(LGFX_Sprite* c, const Extraction& e, uint32_t nowMs, int x, int y,
          int w, int h, bool showLiveSamples) {
  if (w < 4 || h < 4) return;
  c->fillRect(x, y, w, h, theme::chart_bg());

  const int plotX = x + 1;
  const int plotY = y + 1;
  const int plotW = w - 2;
  const int plotH = h - 2;
  if (plotW <= 0 || plotH <= 0) return;

  if (e.phase == Phase::DONE) {
    drawStatic(c, e, plotX, plotY, plotW, plotH);
  } else {
    drawLive(c, e, nowMs, plotX, plotY, plotW, plotH, showLiveSamples);
  }
}

}  // namespace ExtractionView
}  // namespace pump_scale
