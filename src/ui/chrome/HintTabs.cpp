// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#include "HintTabs.h"

#include <M5Unified.h>

#include <algorithm>

#include "ui/font_glyphs.h"
#include "ui/fonts.h"
#include "ui/layout.h"
#include "ui/theme.h"

namespace {
// ---------------------------------------------------------------------------
// Hint-bar geometry + action glyphs.
// ---------------------------------------------------------------------------
constexpr int HINT_GLYPH = 13;  // action-glyph box side (odd: clean center)
constexpr int HINT_PAD = 5;     // inner padding inside a tab
constexpr int HINT_GAP = 3;     // glyph↔label gap, and gap between tabs
constexpr int HINT_STRIPE = 3;  // accent stripe along the button-facing edge
constexpr int HINT_STRIPE_LONG = 9;  // thicker stripe = long-press cue
constexpr int HINT_RADIUS = 2;    // corner radius, on the stripe-opposite side
constexpr int HINT_TAB_MIN = 22;  // smallest a tab shrinks to
constexpr int HINT_MIN_GAP = 12;  // min gap kept between the two tabs

// Rendered width of a hint's content: glyph box (if any) + gap + label text.
// `c` supplies the (already-selected) hint font for measuring.
int slotWidth(LGFX_Sprite& c, const Hint& h) {
  int w = 0;
  if (h.glyph != HintGlyph::None) w += HINT_GLYPH;
  if (h.label && h.label[0]) {
    if (w) w += HINT_GAP;
    w += c.textWidth(h.label);
  }
  return w;
}

// Draw a built-in action glyph centered on (cx, cy), using the montserrat
// symbol font (see ui/font_glyphs.h). The canvas text color is used as-is, so
// set it before calling.
void drawHintGlyph(LGFX_Sprite& c, HintGlyph g, int cx, int cy) {
  switch (g) {
    case HintGlyph::Power:
      layout::drawCentered(&c, font::glyph::POWER, cx, cy);
      break;
    case HintGlyph::Ok:  // check mark
      layout::drawCentered(&c, font::glyph::OK, cx, cy);
      break;
    case HintGlyph::Cancel:  // cross
      layout::drawCentered(&c, font::glyph::CLOSE, cx, cy);
      break;
    case HintGlyph::Back:  // left arrow
      layout::drawCentered(&c, font::glyph::LEFT, cx, cy);
      break;
    case HintGlyph::Forth:  // right arrow
      layout::drawCentered(&c, font::glyph::RIGHT, cx, cy);
      break;
    case HintGlyph::Edit:
      layout::drawCentered(&c, font::glyph::EDIT, cx, cy);
      break;
    case HintGlyph::Trash:  // trash can
      layout::drawCentered(&c, font::glyph::TRASH, cx, cy);
      break;
    case HintGlyph::None:
    default:
      break;
  }
}
}  // namespace

int HintTabs::stripeSpan(Stripe s) {
  // Hold and Double share one span so a tab's reserved width is
  // phase-independent.
  return s == Stripe::Tap ? HINT_STRIPE : HINT_STRIPE_LONG;
}

HintTabs::TabPlan HintTabs::plan(const ButtonHint& bh) {
  // Measure against the hint font on the metrics sprite (no buffer needed for
  // textWidth); draw() selects the same font before rendering.
  _measure.setFont(font::button_hint());
  _measure.setTextSize(1);

  TabPlan p;
  auto add = [&](const Hint& h, Stripe st) {
    if (h.empty()) return;
    Slot& s = p.slots[p.count++];
    s = {h, st, slotWidth(_measure, h)};
    p.tabWidth = std::max(p.tabWidth, s.width);
    p.stripeSpan = std::max(p.stripeSpan, stripeSpan(st));
  };
  add(bh.tap, Stripe::Tap);
  add(bh.hold, Stripe::Hold);
  add(bh.doubleTap, Stripe::Double);
  // Tab width from the widest slot, so cycling press types never reflows the
  // tab (or shifts its neighbour). The current slot is centered within it.
  if (p.count) p.tabWidth = std::max(p.tabWidth + 2 * HINT_PAD, HINT_TAB_MIN);
  return p;
}

void HintTabs::setHints(const ButtonHints& h) {
  // Hints are glyph enums + pointers to string literals (see Hint::operator==),
  // so a field compare short-circuits unchanged frames (the host calls this
  // every loop).
  if (_hints.a == h.a && _hints.b == h.b && _hints.power == h.power) return;
  _hints = h;
  _aPlan = plan(h.a);
  _bPlan = plan(h.b);
  _powerPlan = plan(h.power);
  _phase = 0;    // restart the cycle on each button's primary (tap) action
  _alt.reset();  // give the primary action a full dwell before cycling, so
                 // entry always starts the same sequence rather than picking
                 // up mid-cycle from the previous page's free-running timer
  _contentDirty = true;
}

bool HintTabs::poll() {
  bool repaint = _contentDirty;
  // Advance the cycle only when a button actually has more than one press type
  // to show; a single-slot tab renders identically each tick, so ticking it
  // would repaint for nothing.
  if (_alt() &&
      (_aPlan.count > 1 || _bPlan.count > 1 || _powerPlan.count > 1)) {
    ++_phase;
    repaint = true;
  }
  return repaint;
}

void HintTabs::draw(LGFX_Sprite& canvas, ChromeEdge edge) {
  // Logical frame: the tabs are laid out flat (length × T) and Chrome rotates
  // the canvas onto the edge, so the layout stays orientation-agnostic — with
  // one exception. Text must read upright, and Chrome draws horizontal edges
  // unrotated; on the Top edge that would leave the layout pointing the wrong
  // way, so we mirror it (B tab at the start, accent stripe along the top)
  // rather than flip glyphs. The accent stripe sits on the button-facing edge:
  // logical v=0 (top) for the Top edge, v=T (bottom) otherwise.
  const int length = canvas.width();
  const int T = canvas.height();
  const bool topEdge = edge == ChromeEdge::Top;
  const bool bMirror = topEdge;    // B tab at the start (u=0) vs the far end
  const bool stripeTop = topEdge;  // accent stripe at logical v=0 vs v=T

  LGFX_Sprite& hb = canvas;
  hb.setFont(font::button_hint());
  hb.setTextSize(1);

  // The press type shown this frame for each tab, cycling with _phase. The
  // plans (which slots, their widths, the tab width) were resolved in setHints.
  const Slot empty{};
  const Slot& a = _aPlan.count ? _aPlan.slots[_phase % _aPlan.count] : empty;
  const Slot& b = _bPlan.count ? _bPlan.slots[_phase % _bPlan.count] : empty;
  const Slot& power =
      _powerPlan.count ? _powerPlan.slots[_phase % _powerPlan.count] : empty;
  int aW = _aPlan.tabWidth;
  // The side tabs' stripes consume width. Reserve their widest spans so the
  // tabs keep fixed widths as their press types cycle.
  int bW = _bPlan.count ? _bPlan.tabWidth + _bPlan.stripeSpan : 0;
  int powerW =
      _powerPlan.count ? _powerPlan.tabWidth + _powerPlan.stripeSpan : 0;

  // When Power is absent, both tabs keep their natural width, with B near its
  // corner and bar background between them. Only shrink proportionally if
  // they cannot both fit.
  if (!powerW && aW && bW && aW + HINT_GAP + bW > length) {
    const int avail = length - HINT_GAP;
    bW = std::max(HINT_TAB_MIN / 2, avail * bW / (aW + bW));
    aW = avail - bW;
  } else if (!powerW) {
    aW = std::min(aW, length);
    bW = std::min(bW, length);
  }

  // B stays at its corner (the bar end the B button sits by). A starts at the
  // opposite end, then moves toward centre by splitting the slack onto both
  // sides of A while keeping at least HINT_MIN_GAP between the tabs. With no B
  // tab, A simply centres in the whole bar.
  int aX = 0, bX = 0, powerX = 0;
  if (!powerW && bMirror) {
    bX = 0;
    aX = length - aW;
  } else if (!powerW) {
    aX = 0;
    bX = length - bW;
  }
  if (!powerW && aW > 0) {
    const int slack = length - aW - bW;  // bW==0 ⇒ slack spans the whole bar
    // With B present, keep at least HINT_MIN_GAP between the tabs; with B
    // absent there's nothing to clear, so just centre A in the slack.
    const int shift =
        bW > 0 ? std::min(slack / 2, slack - HINT_MIN_GAP) : slack / 2;
    if (shift > 0) aX += bMirror ? -shift : shift;
  }

  if (powerW) {
    const int gaps = (aW ? HINT_GAP : 0) + (bW ? HINT_GAP : 0);
    const int total = powerW + aW + bW + gaps;
    if (total > length) {
      const int avail = std::max(0, length - gaps);
      const int natural = powerW + aW + bW;
      powerW = natural ? avail * powerW / natural : 0;
      bW = natural ? avail * bW / natural : 0;
      aW = avail - powerW - bW;
    }

    const int startW = bMirror ? bW : powerW;
    const int endW = bMirror ? powerW : bW;
    if (bMirror) {
      bX = 0;
      powerX = length - powerW;
    } else {
      powerX = 0;
      bX = length - bW;
    }
    if (aW) {
      const int low = startW + HINT_GAP;
      const int high = length - endW - HINT_GAP - aW;
      aX = std::max(low, std::min((length - aW) / 2, high));
    }
  }

  enum class TabSide : uint8_t { A, Start, End };
  auto renderTab = [&](int x, int w, const Slot& s, TabSide side) {
    if (w <= 0) return;  // absent tab (no slots) or shrunk away
    hb.setClipRect(x, 0, w, T);
    hb.fillRoundRect(x, 0, w, T, HINT_RADIUS, theme::bg_alt());
    hb.setTextColor(theme::fg(), theme::bg_alt());

    // Accent stripe on the edge facing the physical button; its style marks the
    // press type — a thin line (tap), a thick line (hold) or a split pair of
    // thin lines (double), all within the same span. A runs along the bar's
    // outer edge, so its stripe spans the tab on that edge. Power and B sit
    // around the two corners, so their stripes run down the outer sides. We
    // square the two corners on the stripe edge first (bg_alt over the rounded
    // notch) so only the side opposite the stripe stays rounded.
    const int span = stripeSpan(s.stripe);
    struct StripeBand {
      int d0, d1;  // accent fill, as depth [d0, d1) inward from the tab edge
    };
    StripeBand band[2];
    int nb;
    if (s.stripe == Stripe::Double) {  // two thin lines with a thin gap
      band[0] = {0, HINT_STRIPE};
      band[1] = {span - HINT_STRIPE, span};
      nb = 2;
    } else {  // tap (thin) or hold (thick): one solid line
      band[0] = {0, span};
      nb = 1;
    }
    int boxX0 = x + HINT_PAD, boxX1 = x + w - HINT_PAD, cy = T / 2;
    if (side == TabSide::A) {
      if (stripeTop) {
        hb.fillRect(x, 0, w, HINT_RADIUS, theme::bg_alt());
        for (int i = 0; i < nb; ++i)
          hb.fillRect(x, band[i].d0, w, band[i].d1 - band[i].d0,
                      theme::accent());
        cy = span + (T - span) / 2;
      } else {
        hb.fillRect(x, T - HINT_RADIUS, w, HINT_RADIUS, theme::bg_alt());
        for (int i = 0; i < nb; ++i)
          hb.fillRect(x, T - band[i].d1, w, band[i].d1 - band[i].d0,
                      theme::accent());
        cy = (T - span) / 2;
      }
    } else if (side == TabSide::Start) {
      hb.fillRect(x, 0, HINT_RADIUS, T, theme::bg_alt());
      for (int i = 0; i < nb; ++i)
        hb.fillRect(x + band[i].d0, 0, band[i].d1 - band[i].d0, T,
                    theme::accent());
      boxX0 = x + span + HINT_PAD;
    } else {
      hb.fillRect(x + w - HINT_RADIUS, 0, HINT_RADIUS, T, theme::bg_alt());
      for (int i = 0; i < nb; ++i)
        hb.fillRect(x + w - band[i].d1, 0, band[i].d1 - band[i].d0, T,
                    theme::accent());
      boxX1 = x + w - span - HINT_PAD;
    }

    int gx = boxX0 + ((boxX1 - boxX0) - s.width) / 2;
    if (gx < boxX0) gx = boxX0;
    if (s.hint.glyph != HintGlyph::None) {
      drawHintGlyph(hb, s.hint.glyph, gx + HINT_GLYPH / 2, cy);
      gx += HINT_GLYPH + (s.hint.label && s.hint.label[0] ? HINT_GAP : 0);
    }
    if (s.hint.label && s.hint.label[0]) {
      layout::drawMiddleLeft(&hb, s.hint.label, gx, cy);
    }
    hb.clearClipRect();
  };
  renderTab(aX, aW, a, TabSide::A);
  renderTab(bX, bW, b, bMirror ? TabSide::Start : TabSide::End);
  renderTab(powerX, powerW, power, bMirror ? TabSide::End : TabSide::Start);

  _contentDirty = false;
}
