// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#include "ui/gauge.h"

#include <algorithm>
#include <cstring>

#include "ui/segfont/segfont.h"
#include "ui/segfont/segfont_lgfx.h"
#include "ui/strokefont/strokefont.h"
#include "ui/strokefont/strokefont_lgfx.h"

namespace gauge {
namespace {

constexpr float kSmallHeightRatio = 0.66f;
constexpr float kUnitHeightRatio = 0.5f;
constexpr float kGapRatio = 1.0f / 8.0f;
constexpr size_t kPartBytes = 32;
constexpr int kUnitMinPadX = 3;
constexpr int kUnitMinPadY = 6;
constexpr int kFullHeightUnitMinPadY = 2;
constexpr int kMinGaugeTextHeight = 10;

const char* safeText(const char* text) { return text ? text : ""; }

// Pointer to the fractional part of a formatted number - the decimal point and
// everything after it - within `value`, or nullptr if there is no decimal
// point. The returned run is null-terminated because it points into `value`.
const char* fractionOf(const char* value) {
  return value ? std::strchr(value, '.') : nullptr;
}

struct ValueParts {
  char major[kPartBytes] = {};
  const char* fraction = "";
  bool hasFraction = false;
};

ValueParts splitValue(const char* value, bool smallFraction) {
  value = safeText(value);
  ValueParts parts;
  const char* fraction = smallFraction ? fractionOf(value) : nullptr;
  const size_t n =
      fraction ? static_cast<size_t>(fraction - value) : std::strlen(value);
  const size_t copied = std::min(n, sizeof(parts.major) - 1);
  std::memcpy(parts.major, value, copied);
  parts.major[copied] = '\0';
  parts.fraction = fraction ? fraction : "";
  parts.hasFraction = fraction && *fraction;
  return parts;
}

struct CanonicalLayout {
  segfont::Style majorStyle;
  segfont::Style smallStyle;
  strokefont::Style unitStyle;
  strokefont::Metrics unitTextMetrics;
  int gap = 0;
  int unitPadX = 0;
  int unitPadY = 0;
  int unitBoxW = 0;
  int unitBoxH = 0;
  int valueAreaW = 0;
  int refMajorW = 0;
  int refFractionW = 0;
  bool refHasFraction = false;
};

enum class ValueKind { Canonical, Uniform };
enum class GaugeMode { FixedColumn, Centered };

struct ResolvedValue {
  ValueKind kind = ValueKind::Canonical;
  segfont::Style uniformStyle;
  int width = 0;
  int height = 0;
};

struct Placement {
  int valueLeftX = 0;
  int valueRightX = 0;
  int baseline = 0;
  int unitX = 0;
  int unitTop = 0;
};

int unitPadXFor(const strokefont::Style& style) {
  return std::max(kUnitMinPadX, strokefont::stroke_diameter(style));
}

int unitPadYFor(const strokefont::Style& style) {
  return std::max(kUnitMinPadY, strokefont::stroke_diameter(style));
}

int fullHeightUnitPadYFor(const strokefont::Style& style) {
  return std::max(kFullHeightUnitMinPadY,
                  (strokefont::stroke_diameter(style) + 1) / 2);
}

void drawUnitInBox(LGFX_Sprite* c, const char* unit,
                   const strokefont::Style& style,
                   const strokefont::Metrics& metrics, layout::rect box,
                   uint32_t fg, uint32_t bg) {
  if (!unit || !*unit || box.w <= 0 || box.h <= 0) return;
  const int radius = std::max(3, std::min(box.w, box.h) / 4);
  c->fillSmoothRoundRect(box.x, box.y, box.w, box.h, radius, fg);
  const int textX = box.x + (box.w - metrics.advance) / 2;
  const int baseline = box.y + (box.h - metrics.ascent) / 2 + metrics.ascent;
  strokefont::drawString(*c, style, unit, textX, baseline, bg);
}

segfont::Style gaugeStyleForHeight(int targetH) {
  return segfont::style_for_height(std::max(kMinGaugeTextHeight, targetH));
}

CanonicalLayout layoutForMajorStyle(const segfont::Style& majorStyle,
                                    const ValueParts& ref, const char* unit,
                                    int boxW) {
  CanonicalLayout layout;
  layout.majorStyle = majorStyle;

  const int smallTargetH = std::max(
      kMinGaugeTextHeight,
      std::max(1,
               strokefont::round_px(layout.majorStyle.gh * kSmallHeightRatio)));
  layout.smallStyle = segfont::style_for_height(smallTargetH);

  unit = safeText(unit);
  if (*unit) {
    const int unitTargetH = std::max(
        1, strokefont::round_px(layout.smallStyle.gh * kUnitHeightRatio));
    layout.unitStyle = strokefont::style_for_height(unitTargetH);
    layout.unitTextMetrics = strokefont::measure(layout.unitStyle, unit);
    layout.unitPadX = unitPadXFor(layout.unitStyle);
    layout.unitPadY = unitPadYFor(layout.unitStyle);
    layout.unitBoxW = layout.unitTextMetrics.advance + 2 * layout.unitPadX;
    layout.unitBoxH = layout.unitTextMetrics.ascent + 2 * layout.unitPadY;
    layout.gap =
        std::max(1, strokefont::round_px(layout.majorStyle.gh * kGapRatio));
  }

  layout.refHasFraction = ref.hasFraction;
  layout.refMajorW = segfont::text_width(layout.majorStyle, ref.major);
  layout.refFractionW =
      ref.hasFraction ? segfont::text_width(layout.smallStyle, ref.fraction)
                      : 0;
  layout.valueAreaW = std::max(0, boxW - layout.gap - layout.unitBoxW);
  return layout;
}

int canonicalGroupWidth(const CanonicalLayout& layout) {
  return layout.refMajorW + layout.refFractionW + layout.gap + layout.unitBoxW +
         segfont::default_tracking(layout.smallStyle);
}

int canonicalRequiredHeight(const CanonicalLayout& layout) {
  return std::max(layout.majorStyle.gh,
                  std::max(layout.smallStyle.gh, layout.unitBoxH));
}

CanonicalLayout fitCanonicalLayout(const ValueParts& ref, const char* unit,
                                   int boxW, int boxH) {
  if (boxW < 1) boxW = 1;
  if (boxH < 1) boxH = 1;

  CanonicalLayout best =
      layoutForMajorStyle(gaugeStyleForHeight(1), ref, unit, boxW);

  int lo = 1;
  int hi = boxH;
  while (lo <= hi) {
    const int targetH = lo + (hi - lo) / 2;
    const segfont::Style majorStyle = gaugeStyleForHeight(targetH);
    CanonicalLayout candidate =
        layoutForMajorStyle(majorStyle, ref, unit, boxW);
    if (canonicalRequiredHeight(candidate) <= boxH &&
        canonicalGroupWidth(candidate) <= boxW) {
      best = candidate;
      lo = targetH + 1;
    } else {
      hi = targetH - 1;
    }
  }
  return best;
}

int actualFractionWidth(const CanonicalLayout& layout,
                        const ValueParts& value) {
  return value.hasFraction
             ? segfont::text_width(layout.smallStyle, value.fraction)
             : 0;
}

int fractionColumnWidth(const CanonicalLayout& layout,
                        const ValueParts& value) {
  const int refW = layout.refHasFraction ? layout.refFractionW : 0;
  return std::max(refW, actualFractionWidth(layout, value));
}

int actualCanonicalValueWidth(const CanonicalLayout& layout,
                              const ValueParts& value) {
  const int majorW = segfont::text_width(layout.majorStyle, value.major);
  const int fractionW = fractionColumnWidth(layout, value);
  const int tracking =
      value.hasFraction ? segfont::default_tracking(layout.smallStyle) : 0;
  return majorW + fractionW + tracking;
}

bool fitsCanonicalColumns(const CanonicalLayout& layout,
                          const ValueParts& value) {
  const int majorW = segfont::text_width(layout.majorStyle, value.major);
  const int fractionColumnW = fractionColumnWidth(layout, value);
  const int tracking =
      value.hasFraction ? segfont::default_tracking(layout.smallStyle) : 0;
  const int majorColumnW = layout.valueAreaW - fractionColumnW - tracking;
  return majorColumnW >= 0 && majorW <= majorColumnW;
}

segfont::Style fitUniformValueStyle(const char* value, int maxW, int maxH,
                                    int maxResolvedH) {
  value = safeText(value);
  if (maxW < 1) maxW = 1;
  if (maxH < 1) maxH = 1;
  if (maxResolvedH < 1) maxResolvedH = 1;

  segfont::Style best = segfont::style_for_height(1);
  const int topH = std::min(maxH, maxResolvedH);
  int lo = 1;
  int hi = topH;
  while (lo <= hi) {
    const int targetH = lo + (hi - lo) / 2;
    const segfont::Style style = segfont::style_for_height(targetH);
    if (style.gh <= maxH && style.gh <= maxResolvedH &&
        segfont::text_width(style, value) <= maxW) {
      best = style;
      lo = targetH + 1;
    } else {
      hi = targetH - 1;
    }
  }
  return best;
}

ResolvedValue resolveCanonicalValue(const CanonicalLayout& layout,
                                    const ValueParts& valueParts) {
  ResolvedValue resolved;
  resolved.kind = ValueKind::Canonical;
  resolved.width = actualCanonicalValueWidth(layout, valueParts);
  resolved.height = std::max(layout.majorStyle.gh, layout.smallStyle.gh);
  return resolved;
}

ResolvedValue resolveUniformValue(const CanonicalLayout& layout,
                                  const char* value, layout::rect box) {
  ResolvedValue resolved;
  resolved.kind = ValueKind::Uniform;
  resolved.uniformStyle = fitUniformValueStyle(value, layout.valueAreaW, box.h,
                                               layout.majorStyle.gh);
  resolved.width = segfont::text_width(resolved.uniformStyle, value);
  resolved.height = resolved.uniformStyle.gh;
  return resolved;
}

int topForHeight(layout::rect box, int height) {
  return box.y + (box.h - height) / 2;
}

int baselineForHeight(layout::rect box, int height) {
  return topForHeight(box, height) + height;
}

Placement placeFixedColumn(layout::rect box, const CanonicalLayout& layout,
                           const ResolvedValue& value) {
  Placement p;
  p.unitX = box.x + box.w - layout.unitBoxW;
  p.unitTop = topForHeight(box, layout.unitBoxH);
  p.valueRightX = p.unitX - layout.gap;
  p.valueLeftX = p.valueRightX - value.width;
  p.baseline = baselineForHeight(box, value.height);
  return p;
}

Placement placeCentered(layout::rect box, const CanonicalLayout& layout,
                        const ResolvedValue& value) {
  Placement p;
  const int groupW = value.width + layout.gap + layout.unitBoxW;
  p.valueLeftX = box.x + (box.w - groupW) / 2;
  p.valueRightX = p.valueLeftX + value.width;
  p.unitX = p.valueRightX + layout.gap;
  p.unitTop = topForHeight(box, layout.unitBoxH);
  p.baseline = baselineForHeight(box, value.height);
  return p;
}

void drawUnit(LGFX_Sprite* c, const CanonicalLayout& layout, const char* unit,
              int x, int y, uint32_t fg, uint32_t bg) {
  if (!unit || !*unit || layout.unitBoxW <= 0 || layout.unitBoxH <= 0) return;
  drawUnitInBox(c, unit, layout.unitStyle, layout.unitTextMetrics,
                {x, y, layout.unitBoxW, layout.unitBoxH}, fg, bg);
}

void drawSegfontLeft(LGFX_Sprite* c, const segfont::Style& style,
                     const char* text, int x, int baseline, uint32_t color) {
  segfont::drawString(*c, style, safeText(text), x, baseline - style.gh, color);
}

void drawSegfontRight(LGFX_Sprite* c, const segfont::Style& style,
                      const char* text, int rightX, int baseline,
                      uint32_t color) {
  const int w = segfont::text_width(style, safeText(text));
  drawSegfontLeft(c, style, text, rightX - w, baseline, color);
}

void drawCanonicalValue(LGFX_Sprite* c, const CanonicalLayout& layout,
                        const ValueParts& value, int valueRightX, int baseline,
                        uint32_t color) {
  if (value.hasFraction) {
    const int fractionRightX = valueRightX;
    const int majorRightX = fractionRightX -
                            fractionColumnWidth(layout, value) -
                            segfont::default_tracking(layout.smallStyle);
    drawSegfontRight(c, layout.majorStyle, value.major, majorRightX, baseline,
                     color);
    drawSegfontRight(c, layout.smallStyle, value.fraction, fractionRightX,
                     baseline, color);
  } else {
    const int majorRightX = valueRightX - fractionColumnWidth(layout, value);
    drawSegfontRight(c, layout.majorStyle, value.major, majorRightX, baseline,
                     color);
  }
}

void drawResolvedValue(LGFX_Sprite* c, const CanonicalLayout& layout,
                       const ValueParts& valueParts, const char* value,
                       const ResolvedValue& resolved, const Placement& place,
                       uint32_t color) {
  if (resolved.kind == ValueKind::Canonical) {
    drawCanonicalValue(c, layout, valueParts, place.valueRightX, place.baseline,
                       color);
    return;
  }

  drawSegfontLeft(c, resolved.uniformStyle, value, place.valueLeftX,
                  place.baseline, color);
}

void drawGauge(LGFX_Sprite* c, const char* value, const char* unit,
               layout::rect box, GaugeMode mode, const opts_t& opts) {
  value = safeText(value);
  unit = safeText(unit);
  const char* ref = opts.sizeRef ? opts.sizeRef : value;

  // Gauge callers currently pass normalized fixed-precision values ("12.3",
  // "-1.0"). If a future caller wants variable precision, normalize here or
  // add an explicit decimal-column policy before relying on sizeRef.
  const ValueParts valueParts = splitValue(value, opts.smallFraction);
  const ValueParts refParts = splitValue(ref, opts.smallFraction);
  CanonicalLayout canonical = fitCanonicalLayout(refParts, unit, box.w, box.h);
  ResolvedValue resolved;

  if (fitsCanonicalColumns(canonical, valueParts)) {
    resolved = resolveCanonicalValue(canonical, valueParts);
  } else {
    // The live value is wider than the reference-fit layout. Re-fit the whole
    // group against the live value; the fraction stays smaller via the usual
    // ratio until it hits kMinGaugeTextHeight, then the integer keeps shrinking
    // until it too would hit that limit, at which point we fall back to
    // uniform.
    CanonicalLayout liveFit =
        fitCanonicalLayout(valueParts, unit, box.w, box.h);
    if (fitsCanonicalColumns(liveFit, valueParts)) {
      canonical = liveFit;
      resolved = resolveCanonicalValue(canonical, valueParts);
    } else {
      resolved = resolveUniformValue(canonical, value, box);
    }
  }

  const Placement placement = mode == GaugeMode::Centered
                                  ? placeCentered(box, canonical, resolved)
                                  : placeFixedColumn(box, canonical, resolved);

  const uint32_t color = c->getTextStyle().fore_rgb888;
  const uint32_t bg = c->getTextStyle().back_rgb888;

  layout::ClipScope clip(c, box.x, box.y, box.w, box.h);
  drawResolvedValue(c, canonical, valueParts, value, resolved, placement,
                    color);
  drawUnit(c, canonical, unit, placement.unitX, placement.unitTop, color, bg);
}

}  // namespace

UnitBoxLayout layoutUnitBoxWithinHeight(const char* unit, int boxHeight) {
  unit = safeText(unit);
  if (!*unit) return {};
  boxHeight = std::max(1, boxHeight);

  UnitBoxLayout result;
  result.height = boxHeight;
  for (int targetHeight = boxHeight; targetHeight >= 1; --targetHeight) {
    const strokefont::Style style = strokefont::style_for_height(targetHeight);
    const strokefont::Metrics metrics = strokefont::measure(style, unit);
    if (metrics.ascent + 2 * fullHeightUnitPadYFor(style) > boxHeight) continue;
    result.style = style;
    result.metrics = metrics;
    result.width = metrics.advance + 2 * unitPadXFor(style);
    return result;
  }

  result.style = strokefont::style_for_height(1);
  result.metrics = strokefont::measure(result.style, unit);
  result.width = result.metrics.advance;
  return result;
}

void drawUnitBox(LGFX_Sprite* c, const char* unit,
                 const UnitBoxLayout& unitLayout, int x, int y, uint32_t fg,
                 uint32_t bg) {
  drawUnitInBox(c, unit, unitLayout.style, unitLayout.metrics,
                {x, y, unitLayout.width, unitLayout.height}, fg, bg);
}

void drawFixedColumn(LGFX_Sprite* c, const char* value, const char* unit, int x,
                     int y, int w, int h, opts_t opts) {
  drawGauge(c, value, unit, {x, y, w, h}, GaugeMode::FixedColumn, opts);
}

void drawCentered(LGFX_Sprite* c, const char* value, const char* unit, int x,
                  int y, int w, int h, opts_t opts) {
  drawGauge(c, value, unit, {x, y, w, h}, GaugeMode::Centered, opts);
}

}  // namespace gauge
