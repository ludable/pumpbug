// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <M5Unified.h>

#include "ChromeContent.h"
#include "util/debounce.h"

// Chrome content painter: button-help tabs that point at the physical controls
// and hint at their function. The Power and B tabs sit at opposite sides, with
// A in the front. The hints for a button with more than one press type (tap,
// hold, double-tap) cycle over time, the current one marked by the tab's stripe
// style (thin = tap, thick = hold, split = double). Tabs are laid out in a
// logical horizontal scratch buffer and rotated onto the strip's edge, so the
// layout stays orientation-agnostic.
//
// setHints resolves and *measures* the hints once (which press types are
// present, their content widths, the tab width); draw() — called only on
// repaint — just picks the current press type by phase and lays it out.
class HintTabs : public ChromeContent {
 public:
  void setHints(const ButtonHints& h);

  ChromeFrame frame() const override { return ChromeFrame::Logical; }
  bool poll() override;
  void draw(LGFX_Sprite& canvas, ChromeEdge edge) override;

 private:
  // Which press type a resolved slot is; selects the stripe style.
  enum class Stripe : uint8_t { Tap, Hold, Double };
  static int stripeSpan(Stripe s);  // overall stripe thickness for a style

  // A resolved hint ready to render: the hint, its stripe style, and its
  // content width (glyph + label), measured once so draw() needn't re-measure.
  struct Slot {
    Hint hint;
    Stripe stripe = Stripe::Tap;
    int width = 0;
  };

  // The plan for one button's hints: one slot per press type in cycle order
  // (tap, hold, double), plus precomputed layout: tabWidth is the fixed extent
  // along the strip, stripeSpan is the widest accent stripe across its slots.
  // Side tabs reserve that stripe width so they do not reflow as they cycle.
  struct TabPlan {
    Slot slots[3];
    int count = 0;
    int tabWidth = 0;
    int stripeSpan = 0;
  };

  TabPlan plan(const ButtonHint& bh);  // resolve + measure one button's hints

  ButtonHints _hints;                  // last hints, for change detection
  TabPlan _aPlan, _bPlan, _powerPlan;  // rebuilt when hints change
  // Font-metrics scratch for plan()/slotWidth() — textWidth() reads only font
  // metrics, so this is never allocated a buffer; draw() renders into the
  // canvas Chrome hands it.
  LGFX_Sprite _measure{&M5.Display};
  Debounce<2500> _alt;        // press-type cycling cadence
  uint32_t _phase = 0;        // index into each tab's present press types
  bool _contentDirty = true;  // hints changed since last paint
};
