// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#include "ScaleMsgScreen.h"

#include <cstdio>

#include "ui/blocks.h"
#include "ui/fonts.h"
#include "ui/layout.h"
#include "ui/theme.h"

void ScaleMsgScreen::onEnter() {
  bleScale.enable();         // Connect mode — messages only flow when connected
  bleScale.armMessageLog();  // start the passive capture tap (resets counts)
  _state = BleScaleService::State::OFF;
  _log = BleScaleService::MsgLogSnapshot{};
  requestDraw();
}

void ScaleMsgScreen::onExit() {
  bleScale.disarmMessageLog();
  bleScale.disable();
}

void ScaleMsgScreen::onLayoutChanged() { requestDraw(); }

ScreenResult ScaleMsgScreen::tick() {
  _state = bleScale.snapshot().state;
  _log = bleScale.messageLogSnapshot();
  requestDraw();
  return stay();
}

ScreenResult ScaleMsgScreen::onEvent(button::Gesture event) {
  switch (event) {
    case button::Gesture::B_SHORT:
      return exit();  // onExit disarms and disconnects.
    default:
      return ignored();
  }
}

namespace {
const char* stateLabel(BleScaleService::State s) {
  switch (s) {
    case BleScaleService::State::OFF:
      return "off";
    case BleScaleService::State::SCANNING:
      return "scanning";
    case BleScaleService::State::CONNECTING:
      return "connecting";
    case BleScaleService::State::READY:
      return "connected";
    case BleScaleService::State::RECONNECTING:
      return "reconnecting";
  }
  return "?";
}
}  // namespace

bool ScaleMsgScreen::onDraw(LGFX_Sprite* c) {
  constexpr int pad = 3;
  c->fillScreen(theme::bg());

  int y = ui::drawViewHeader(c, "SCALE MSGS", theme::accent());

  // Connection state — readable, since "no messages" usually means "not
  // connected yet".
  const bool ready = _state == BleScaleService::State::READY;
  c->setFont(font::tiny());
  c->setTextColor(ready ? theme::ok() : theme::dim(), theme::bg());
  layout::drawTopLeft(c, stateLabel(_state), pad, y);
  y += font::metrics(font::tiny()).height + 2;

  const int lineH = font::metrics(font::tiny()).height + 1;
  auto count = [&](BleScaleService::MsgTag t) {
    return _log.byTag[static_cast<int>(t)];
  };
  using T = BleScaleService::MsgTag;

  char buf[64];
  // RX breakdown: common decoded events first, then diagnostic/error buckets.
  std::snprintf(
      buf, sizeof(buf), "RX W%lu B%lu T%lu K%lu",
      (unsigned long)count(T::RxWeight), (unsigned long)count(T::RxBattery),
      (unsigned long)count(T::RxTimer), (unsigned long)count(T::RxKey));
  c->setTextColor(theme::fg(), theme::bg());
  layout::drawTopLeft(c, buf, pad, y);
  y += lineH;

  std::snprintf(
      buf, sizeof(buf), "RX ?%lu M%lu X%lu S%lu",
      (unsigned long)count(T::RxUnknown), (unsigned long)count(T::RxMixed),
      (unsigned long)count(T::RxRejected), (unsigned long)count(T::RxSettings));
  layout::drawTopLeft(c, buf, pad, y);
  y += lineH;

  // TX breakdown: heartbeat/tare/subscribe/identify/timer/other.
  std::snprintf(
      buf, sizeof(buf), "TX hb%lu tr%lu sb%lu id%lu tm%lu o%lu",
      (unsigned long)count(T::TxHeartbeat), (unsigned long)count(T::TxTare),
      (unsigned long)count(T::TxSubscribe), (unsigned long)count(T::TxIdentify),
      (unsigned long)count(T::TxTimer), (unsigned long)count(T::TxOther));
  layout::drawTopLeft(c, buf, pad, y);
  y += lineH;

  // Total + hint to the web view for the full timeline. Sum the per-tag counts
  // (the true packet total) — not _log.writes, which is the ETag change counter
  // and also ticks on arm/disarm.
  unsigned long total = 0;
  for (int i = 0; i < BleScaleService::MSG_TAG_COUNT; ++i) {
    total += _log.byTag[i];
  }
  std::snprintf(buf, sizeof(buf), "%lu msgs · web for detail", total);
  c->setTextColor(theme::dim(), theme::bg());
  layout::drawTopLeft(c, buf, pad, y);
  return true;
}
