// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <M5Unified.h>

#include <algorithm>
#include <cstdint>

#include "ui/fonts.h"
#include "ui/layout.h"
#include "ui/theme.h"

// Themed drawing composites shared across screens: the view header band,
// labeled fitted-value info blocks, instruction text, guide cards, and a
// text spinner. One layer above layout.h: layout stays pure geometry and
// text with no theme or font-role opinions; the policy (which font family,
// what dims, what label proportions) lives here, so every screen renders
// the same idiom.
namespace ui {

// Options for drawFittedText. Fields are ordered for designated
// initializers, e.g. {.sizeRef = "00:00"}.
struct FitOpts {
  // Pick the font size by fitting this string instead of the text, so a
  // group of values renders at one size. Pass the widest representative.
  const char* sizeRef = nullptr;
  // Anchor to the box's left edge instead of centering horizontally.
  bool leftAlign = false;
};

// A value auto-sized to its box, in the standard text family. Leaves text
// size reset to 1.
inline void drawFittedText(LGFX_Sprite* c, const char* text, layout::rect box,
                           uint32_t color, uint32_t bg, FitOpts opts = {}) {
  if (box.w <= 0 || box.h <= 0) return;
  layout::draw_centered_opts_t inner;
  inner.sizeRef = opts.sizeRef;
  inner.leftAlign = opts.leftAlign;
  c->setTextColor(color, bg);
  layout::drawCenteredInBox(c, text, box, font::textFamily(), inner);
  c->setTextSize(1);
}

// A uniformly-sized label paired with a colored value. `labelSizeRef` keeps
// every label in a group the same height regardless of its width.
inline void drawLabeledValue(LGFX_Sprite* c, const char* label,
                             layout::rect labelBox, const char* value,
                             layout::rect valueBox, uint32_t valueColor,
                             uint32_t bg, const char* labelSizeRef,
                             uint32_t labelColor = theme::dim()) {
  drawFittedText(c, label, labelBox, labelColor, bg, {.sizeRef = labelSizeRef});
  drawFittedText(c, value, valueBox, valueColor, bg);
}

// Label strip height for stacked blocks. A constant, not a share of the
// cell: measureInfoBlock and drawInfoBlock must agree on it for the packer
// to allocate exactly what a block renders.
constexpr int kInfoLabelH = 16;
// Vertical padding a block's value carries within its cell.
constexpr int kInfoCellPad = 4;

// Label stacked above value.
// An empty label gives the value the whole box; used for self-describing values
// (e.g. URLs) whose label would only steal space.
inline void drawInfoBlock(LGFX_Sprite* c, layout::rect box, const char* label,
                          const char* value, uint32_t valueColor, uint32_t bg,
                          const char* labelSizeRef,
                          uint32_t labelColor = theme::dim()) {
  if (!label || !*label) {
    drawFittedText(c, value, box, valueColor, bg);
    return;
  }
  const auto split = layout::splitVFixed(box, kInfoLabelH, 1);
  drawLabeledValue(c, label, split.first, value, split.second, valueColor, bg,
                   labelSizeRef, labelColor);
}

struct InfoRowLabelLayout {
  font::sized_font_t font{};
  int columnWidth = -1;
};

// Chooses one font and one measured column for a group of row labels. Null and
// empty entries do not participate, which lets an icon occupy the resulting
// column without influencing the text size.
inline InfoRowLabelLayout measureInfoRowLabels(LGFX_Sprite* c,
                                               const char* const* labels,
                                               int count, int maxLabelW,
                                               int maxLabelH,
                                               int columnPad = 4) {
  InfoRowLabelLayout result;
  int chosenH = maxLabelH + 1;
  bool found = false;
  for (int i = 0; i < count; ++i) {
    if (!labels[i] || !*labels[i]) continue;
    const font::sized_font_t candidate =
        font::fit(c, labels[i], std::max(1, maxLabelW), std::max(1, maxLabelH),
                  font::textFamily());
    if (!found || c->fontHeight() < chosenH) {
      found = true;
      chosenH = c->fontHeight();
      result.font = candidate;
    }
  }
  if (!found) return result;

  font::apply(c, result.font);
  result.columnWidth = 0;
  for (int i = 0; i < count; ++i) {
    if (labels[i] && *labels[i]) {
      result.columnWidth = std::max(result.columnWidth,
                                    static_cast<int>(c->textWidth(labels[i])));
    }
  }
  result.columnWidth += columnPad;
  c->setTextSize(1);
  return result;
}

// Label beside value.
// An empty label (or a box too narrow for one) gives the value the whole width.
// Standalone (labelColW < 0), the label sizes itself to the row, its column
// takes only the width it needs, and the value centers in the remainder. As
// part of a group (drawInfoScreen passes the shared column width and label
// font it precomputed), label and value draw left-aligned in fixed columns so
// the rows read as one table. Label-less rows center in both modes: a
// self-describing line ("PIN 1234") is a caption, not a table row.
inline void drawInfoRow(LGFX_Sprite* c, layout::rect box, const char* label,
                        const char* value, uint32_t valueColor, uint32_t bg,
                        int labelColW = -1, font::sized_font_t labelFont = {},
                        uint32_t labelColor = theme::dim()) {
  if (!label || !*label || box.w < 48) {
    drawFittedText(c, value, box, valueColor, bg);
    return;
  }
  if (labelColW >= 0) {
    font::apply(c, labelFont);
    c->setTextColor(labelColor, bg);
    layout::drawMiddleLeft(c, label, box.x, box.y + box.h / 2);
    c->setTextSize(1);
    const auto split = layout::splitHFixed(box, labelColW, 2);
    drawFittedText(c, value, split.second, valueColor, bg,
                   {.sizeRef = nullptr, .leftAlign = true});
    return;
  }
  font::fit(c, label, box.w / 3, std::max(4, box.h - 4), font::textFamily());
  const int labelW = c->textWidth(label) + 4;
  const auto split = layout::splitHFixed(box, labelW, 2);
  drawFittedText(c, label, split.first, labelColor, bg);
  drawFittedText(c, value, split.second, valueColor, bg);
}

using InfoIconRenderer = void (*)(LGFX_Sprite*, layout::rect, uint32_t);

inline void drawInfoIconRow(LGFX_Sprite* c, layout::rect box,
                            InfoIconRenderer drawIcon, uint32_t iconColor,
                            const char* value, uint32_t valueColor, uint32_t bg,
                            int leadingColW = -1) {
  const int iconColW = leadingColW >= 0 ? std::min(leadingColW, box.w)
                                        : std::min(box.h, box.w / 3);
  const auto split = layout::splitHFixed(box, iconColW, 2);
  const int iconW = std::min(split.first.w, split.first.h);
  const layout::rect iconBox = {
      split.first.x + (split.first.w - iconW) / 2,
      split.first.y + (split.first.h - iconW) / 2,
      iconW,
      iconW,
  };
  const int iconPad =
      std::min(std::max(4, iconW / 5), std::max(0, (iconW - 3) / 2));
  drawIcon(c, layout::inset(iconBox, iconPad), iconColor);
  drawFittedText(c, value, split.second, valueColor, bg,
                 {.sizeRef = nullptr, .leftAlign = true});
}

struct InfoBlock {
  const char* label;  // empty/nullptr: the value fills the whole cell
  const char* value;
  uint32_t valueColor;
  // Largest useful rendered height for the value, in pixels. The packer
  // never allocates more height than the value can use, so a short value
  // (a 4-digit PIN) stops growing here and cedes the rest.
  int maxValuePx = 32;
};

// Height at which `b`'s value reaches its width-limited or capped size at
// body width `w` — the cell the block wants, and all it can use.
// `labelColW` is the shared label column of a grouped row (see drawInfoRow);
// -1 falls back to the standalone reservation of a third of the width.
inline int measureInfoBlock(LGFX_Sprite* c, const InfoBlock& b, int w, bool row,
                            int labelColW = -1) {
  const bool hasLabel = b.label && *b.label;
  // Rows reserve the label column; stacked blocks a label strip on top.
  const int valueW =
      (row && hasLabel) ? w - (labelColW >= 0 ? labelColW : w / 3) : w;
  font::fit(c, b.value, valueW - kInfoCellPad, b.maxValuePx,
            font::textFamily());
  const int valueH = c->fontHeight();
  c->setTextSize(1);
  return valueH + ((!row && hasLabel) ? kInfoLabelH : 0) + kInfoCellPad;
}

inline constexpr int kViewHeaderMinHeight = 24;
inline constexpr int kViewHeaderPadX = 4;
inline constexpr int kViewHeaderPadY = 1;
inline constexpr int kViewHeaderContentOffsetY = -1;

// Draws the standard header for an ordinary screen. The optional right
// annotation uses the title font and defaults to the secondary text color.
// The band shades from bgTop down toward bgBottom at the body edge; pass both
// equal for a flat band.
inline int drawViewHeader(LGFX_Sprite* c, const char* title,
                          uint32_t titleColor, const char* right = nullptr,
                          uint32_t rightColor = 0, uint32_t bgTop = theme::bg(),
                          uint32_t bgBottom = theme::bg_alt()) {
  c->setFont(font::view_title());
  c->setTextSize(1);
  const int headerH =
      std::max(kViewHeaderMinHeight, c->fontHeight() + 2 * kViewHeaderPadY);
  // A gradient between equal endpoints is just a flat fill, so the solid-band
  // case needs no separate path.
  const int gradientY = headerH / 2;
  c->fillRect(0, 0, c->width(), gradientY, bgTop);
  c->fillGradientRect(0, gradientY, c->width(), headerH - gradientY, bgTop,
                      bgBottom, lgfx::VLINEAR);
  const int contentCenterY = headerH / 2 + kViewHeaderContentOffsetY;
  // Transparent LVGL glyph edges blend against the sprite's base color. Match
  // the solid upper half without painting opaque rectangles over the gradient.
  const uint32_t previousBaseColor = c->getBaseColor();
  c->setBaseColor(bgTop);
  c->setTextColor(titleColor);
  layout::drawMiddleLeft(c, title, kViewHeaderPadX, contentCenterY);
  if (right && *right) {
    c->setTextColor(rightColor ? rightColor : theme::dim());
    layout::drawMiddleRight(c, right, c->width() - kViewHeaderPadX,
                            contentCenterY);
  }
  c->setBaseColor(previousBaseColor);
  return headerH;
}

// Font choices for guidance text: capped well below the value fonts so
// instructions read as a caption, not a headline.
inline font::family_t instructionFamily() {
  static const font::sized_font_t kFamily[] = {
      {&lgfx::fonts::lv_font_montserrat_22, 1.0f},
      {&lgfx::fonts::lv_font_montserrat_20, 1.0f},
      {&lgfx::fonts::lv_font_montserrat_18, 1.0f},
      {&lgfx::fonts::lv_font_montserrat_16, 1.0f},
      {&lgfx::fonts::lv_font_montserrat_14, 1.0f},
      {&lgfx::fonts::lv_font_montserrat_12, 1.0f},
      {&lgfx::fonts::lv_font_montserrat_10, 1.0f},
  };
  return {kFamily, sizeof(kFamily) / sizeof(kFamily[0])};
}

// A whole info screen below the header: optional guidance text, then the
// fields, laid out by measure→pack→distribute. Every field takes exactly
// the height it can use (measureInfoBlock), so slack becomes breathing
// between elements instead of air trapped inside oversized cells; the
// guidance is elastic, taking whatever the fields leave over. When the
// full guidance can't render at a readable size, `guidanceCompact` (when
// given) replaces it; only after that do the fields shrink below their
// desired heights. Fields render as stacked label-over-value blocks in
// portrait and, in landscape, as one table of label-beside-value rows: one
// label size for the group and one shared label column, everything
// left-aligned.
inline void drawInfoScreen(LGFX_Sprite* c, int headerH, const char* guidance,
                           const char* guidanceCompact, const InfoBlock* blocks,
                           int count, uint32_t bg, const char* labelSizeRef) {
  constexpr int kMaxBlocks = 8;
  constexpr int kMinGuidanceBudget = 18;  // height the caption always gets
  constexpr int kMinGuidanceLineH = 14;   // below this the caption is noise
  if (count <= 0 || count > kMaxBlocks) return;
  const bool rows = c->width() > c->height();
  const layout::rect body =
      layout::inset({0, headerH, c->width(), c->height() - headerH}, 2);

  InfoRowLabelLayout rowLabels;
  if (rows) {
    constexpr int kRowLabelMaxH = 20;  // readable on the panel, chosen by eye
    const char* labels[kMaxBlocks] = {};
    for (int i = 0; i < count; ++i) {
      labels[i] = blocks[i].label;
    }
    rowLabels =
        measureInfoRowLabels(c, labels, count, body.w / 2, kRowLabelMaxH);
  }

  int desired[kMaxBlocks];
  int sumDesired = 0;
  for (int i = 0; i < count; ++i) {
    desired[i] =
        measureInfoBlock(c, blocks[i], body.w, rows, rowLabels.columnWidth);
    sumDesired += desired[i];
  }

  // Guidance gets what the fields leave over (at most half the body).
  int guidH = 0;
  const char* guidText = guidance;
  if (guidText && *guidText) {
    const int budget =
        std::max(kMinGuidanceBudget,
                 std::min(body.h / 2, body.h - sumDesired - 4 * count));
    guidH = layout::measureWrapped(c, guidText, body.w - 2, budget,
                                   instructionFamily());
    if (c->fontHeight() < kMinGuidanceLineH && guidanceCompact) {
      guidText = guidanceCompact;
      guidH = layout::measureWrapped(c, guidText, body.w - 2, budget,
                                     instructionFamily());
    }
    c->setTextSize(1);
    guidH += 4;
  }

  // Fields keep their desired heights when they fit; otherwise they shrink
  // proportionally (the panel is genuinely too small at that point).
  const int fieldAvail = body.h - guidH;
  int cells[kMaxBlocks];
  int sumCells = 0;
  for (int i = 0; i < count; ++i) {
    cells[i] = (sumDesired > fieldAvail && sumDesired > 0)
                   ? desired[i] * fieldAvail / sumDesired
                   : desired[i];
    sumCells += cells[i];
  }

  // Slack becomes even gaps between fields (capped so the group stays
  // cohesive); the remainder centers the group under the guidance.
  int slack = fieldAvail - sumCells;
  const int gap =
      count > 1 ? std::min(12, std::max(0, slack) / (count + 1)) : 0;
  slack -= gap * (count - 1);

  if (guidText && guidH > 0) {
    c->setTextColor(theme::dim(), bg);
    layout::drawWrappedCentered(c, guidText,
                                {body.x + 1, body.y, body.w - 2, guidH - 4},
                                instructionFamily());
  }
  int y = body.y + guidH + std::max(0, slack) / 2;
  for (int i = 0; i < count; ++i) {
    const layout::rect cell = {body.x, y, body.w, cells[i]};
    if (rows) {
      drawInfoRow(c, cell, blocks[i].label, blocks[i].value,
                  blocks[i].valueColor, bg, rowLabels.columnWidth,
                  rowLabels.font);
    } else {
      drawInfoBlock(c, cell, blocks[i].label, blocks[i].value,
                    blocks[i].valueColor, bg, labelSizeRef);
    }
    y += cells[i] + gap;
  }
}

// One large wrapped message filling the body below a header — drawGuideCard's
// content half, callable on its own when the header needs something
// drawViewHeader can't do (e.g. the animated Wi-Fi glyph).
inline void drawCardBody(LGFX_Sprite* c, int headerH, const char* text) {
  const layout::rect body =
      layout::inset({0, headerH, c->width(), c->height() - headerH}, 6);
  c->setTextColor(theme::fg(), theme::bg());
  layout::drawWrappedCentered(c, text, body, font::textFamily());
}

// Header and message for a full-screen card. The caller supplies the cleared
// background so a screen can compose this with its own drawing lifecycle.
inline void drawGuideCard(LGFX_Sprite* c, const char* title,
                          uint32_t titleColor, const char* right,
                          const char* text) {
  drawCardBody(c, drawViewHeader(c, title, titleColor, right), text);
}

// QR panel with its guidance text. The QR sits on a white field (phone
// cameras want true black-on-white regardless of theme, and the field
// doubles as the quiet zone) and takes the largest square that fits; the
// text gets whatever the QR doesn't need — the band above it in portrait,
// the column beside it in landscape. The human-readable equivalent belongs
// on a separate details screen, not in captions, to avoid making the QR or
// the caption too small to read.
inline void drawQrWithInstruction(LGFX_Sprite* c, int headerH, const char* text,
                                  const char* payload) {
  const int w = c->width();
  const int h = c->height();
  c->setTextColor(theme::dim(), theme::bg());
  // qrcode() picks the smallest QR version that fits the payload and pads
  // the remainder of the square white.
  if (w > h) {
    const int box = h - headerH;
    const layout::rect field = {w - box - 8, headerH, box + 8, h - headerH};
    layout::drawWrappedCentered(
        c, text, layout::inset({0, headerH, field.x, h - headerH}, 4),
        instructionFamily());
    c->fillRect(field.x, field.y, field.w, field.h, TFT_WHITE);
    c->qrcode(payload, field.x + (field.w - box) / 2, field.y, box);
    return;
  }
  const int box = std::min(w, h - headerH);
  const int bandH = h - headerH - box;
  layout::drawWrappedCentered(c, text, layout::inset({0, headerH, w, bandH}, 4),
                              instructionFamily());
  c->fillRect(0, headerH + bandH, w, box, TFT_WHITE);
  c->qrcode(payload, (w - box) / 2, headerH + bandH, box);
}

}  // namespace ui
