// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#include "EraseDataScreen.h"

#include <nvs_flash.h>

#include "diagnostics/PanicDump.h"
#include "net/NetworkServicesHost.h"
#include "ui/fonts.h"
#include "ui/layout.h"
#include "ui/message_cards.h"
#include "ui/theme.h"
#include "util/storage.h"

void EraseDataScreen::onEnter() {
  _stage = Stage::Confirm;
  _erasePresented = false;
  _rebootAtMs = 0;
  requestDraw();
}

void EraseDataScreen::_finishErase(Stage result) {
  _stage = result;
  _rebootAtMs = millis() + kRebootDelayMs;
  requestDraw();
}

void EraseDataScreen::_performErase() {
  if (!_networkServices.stopForDataErase()) {
    M5_LOGE("EraseDataScreen: network shutdown failed; rebooting");
    _finishErase(Stage::Incomplete);
    return;
  }

  // Credentials and RAM dumps are erased before shot history so an
  // interruption during the slower format cannot leave sensitive data behind
  // after destroying the user's shots.
  // Erasing the default partition also deinitializes it. Code running before
  // reboot must not reinitialize NVS or retry writes after initializing it,
  // because that could recreate state after the wipe.
  const esp_err_t nvsResult = nvs_flash_erase();
  const bool nvsErased = nvsResult == ESP_OK;
  const bool panicErased = diagnostics::clearLastPanicAndResync();
  if (!nvsErased || !panicErased) {
    M5_LOGE("EraseDataScreen: incomplete (nvs=%d, panic=%d); rebooting",
            static_cast<int>(nvsResult), panicErased ? 1 : 0);
    _finishErase(Stage::Incomplete);
    return;
  }

  if (storage::format() == storage::MountState::Ready) {
    M5_LOGI(
        "EraseDataScreen: NVS, core dump, and shot history erased; rebooting");
    _finishErase(Stage::Done);
  } else {
    M5_LOGE("EraseDataScreen: shot-history format failed; rebooting");
    _finishErase(Stage::Incomplete);
  }
}

ScreenResult EraseDataScreen::onEvent(button::Gesture event) {
  switch (_stage) {
    case Stage::Confirm:
      switch (event) {
        case button::Gesture::A_LONG:
          _stage = Stage::Erasing;
          _erasePresented = false;
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
      // The device is about to reboot. Ignore input until then.
      return ignored();
  }
  return ignored();
}

ScreenResult EraseDataScreen::tick() {
  if (_stage == Stage::Erasing && _erasePresented) {
    _erasePresented = false;
    _performErase();
    return stay();
  }

  if ((_stage == Stage::Done || _stage == Stage::Incomplete) &&
      static_cast<int32_t>(millis() - _rebootAtMs) >= 0) {
    ESP.restart();
  }
  return stay();
}

void EraseDataScreen::onPresented() {
  if (_stage == Stage::Erasing) _erasePresented = true;
}

void EraseDataScreen::_drawConfirm(LGFX_Sprite* c) {
  ui::drawCriticalMessageScreen(
      c, "Erase all data?",
      "Deletes shots, settings, pairings, counters, and diagnostics, then "
      "reboots. Cannot be undone.");
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
