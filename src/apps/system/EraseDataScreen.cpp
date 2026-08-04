// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#include "EraseDataScreen.h"

#include "apps/extraction/ShotCounter.h"
#include "diagnostics/PanicDump.h"
#include "net/WifiManager.h"
#include "ui/fonts.h"
#include "ui/layout.h"
#include "ui/message_cards.h"
#include "ui/theme.h"
#include "util/storage.h"

void EraseDataScreen::onEnter() {
  _stage = Stage::Confirm;
  requestDraw();
}

void EraseDataScreen::_performErase() {
  // A raw core dump can contain credentials and user data from RAM, so the
  // full-device erase removes it along with shot history. Shot history is
  // formatted during the following boot, before any task can access it. The
  // power-event log contains only reset metadata and survives for service
  // diagnostics.
  const bool filesystemResetRequested = storage::requestFormat();
  const bool panicErased = diagnostics::clearLastPanicAndResync();
  const bool shotCounterErased = pump_scale::shot_counter::clearPersisted();

  // The erase steps are independent and cannot be rolled back. Always finish
  // the Wi-Fi/auth wipe and reboot to clear RAM, even if a storage operation
  // failed; otherwise a partial erase would leave unrelated user data behind.
  wifiManager.requestReset(/*terminal=*/true);
  _rebootAtMs = millis() + kRebootDelayMs;
  if (filesystemResetRequested && panicErased && shotCounterErased) {
    M5_LOGI("EraseDataScreen: erase scheduled; rebooting");
    _stage = Stage::Done;
  } else {
    M5_LOGE(
        "EraseDataScreen: incomplete (filesystem request=%d, panic=%d, "
        "counter=%d); rebooting",
        filesystemResetRequested ? 1 : 0, panicErased ? 1 : 0,
        shotCounterErased ? 1 : 0);
    _stage = Stage::Incomplete;
  }
  requestDraw();
}

ScreenResult EraseDataScreen::onEvent(button::Gesture event) {
  switch (_stage) {
    case Stage::Confirm:
      switch (event) {
        case button::Gesture::A_LONG:
          _stage = Stage::Erasing;
          requestDraw();
          return stay();
        case button::Gesture::B_SHORT:
          return exit();
        default:
          return ignored();
      }
    case Stage::Erasing:
      return ignored();
    case Stage::Done:
    case Stage::Incomplete:
      // The device is about to reboot. Ignore input while the deferred wipe
      // finishes.
      return ignored();
  }
  return ignored();
}

ScreenResult EraseDataScreen::tick() {
  if (_stage == Stage::Erasing) {
    _performErase();
    return stay();
  }

  // The Wi-Fi/auth wipe is a deferred action drained by the main loop; hold
  // the reboot until it has actually run, or a slow drain (e.g. the HTTP
  // server winding down a live request) could reboot with the config intact.
  if ((_stage == Stage::Done || _stage == Stage::Incomplete) &&
      static_cast<int32_t>(millis() - _rebootAtMs) >= 0 &&
      !wifiManager.hasPendingAction()) {
    ESP.restart();
  }
  return stay();
}

void EraseDataScreen::_drawConfirm(LGFX_Sprite* c) {
  ui::drawCriticalMessageScreen(
      c, "Erase all data?",
      "Deletes ALL shot history, Wi-Fi config and pairings, then reboots. "
      "Cannot be undone.");
}

void EraseDataScreen::_drawResult(LGFX_Sprite* c, const char* msg,
                                  const char* sub, uint32_t color) {
  const int w = c->width();
  const int h = c->height();
  constexpr int margin = 4;
  c->fillScreen(theme::bg());

  // `sub` is an informational status line (e.g. "Restarting..."); reserve a
  // small strip for it when present.
  const int subH = sub ? font::metrics(font::tiny()).height + margin : 0;
  c->setTextColor(color, theme::bg());
  layout::drawCenteredInBox(c, msg, margin, 0, w - 2 * margin, h - subH,
                            font::textFamily());

  if (sub) {
    c->setFont(font::tiny());
    c->setTextSize(1);
    c->setTextColor(theme::dim(), theme::bg());
    layout::drawBottomCenter(c, sub, w / 2, h - 2);
  }
}

bool EraseDataScreen::onDraw(LGFX_Sprite* c) {
  switch (_stage) {
    case Stage::Confirm:
      _drawConfirm(c);
      break;
    case Stage::Erasing:
      _drawResult(c, "Erasing...", nullptr, theme::fg());
      break;
    case Stage::Done:
      _drawResult(c, "Erased", "Restarting...", theme::fg());
      break;
    case Stage::Incomplete:
      _drawResult(c, "Erase incomplete", "Restarting...", theme::critical());
      break;
  }
  return true;
}

ButtonHints EraseDataScreen::buttonHints() const {
  switch (_stage) {
    case Stage::Confirm:
      return {{{}, Hint{HintGlyph::Trash, "ERASE"}}, {Hint{HintGlyph::Cancel}}};
    case Stage::Erasing:
    case Stage::Incomplete:
    case Stage::Done:
    default:
      // Reboot imminent; input is swallowed, so no hints.
      return {};
  }
}
