// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#include "StorageRecoveryScreen.h"

#include "ui/fonts.h"
#include "ui/layout.h"
#include "ui/message_cards.h"
#include "ui/theme.h"

bool StorageRecoveryScreen::shouldPresent() const {
  return storage::mountState() != storage::MountState::Ready;
}

void StorageRecoveryScreen::onEnter() {
  _stage = Stage::Notice;
  requestDraw();
}

ScreenResult StorageRecoveryScreen::onEvent(button::Gesture event) {
  switch (_stage) {
    case Stage::Notice:
      if (event == button::Gesture::A_SHORT) {
        _stage = Stage::Confirm;
        requestDraw();
        return stay();
      }
      // Startup has no previous screen. B continues without shot history; when
      // opened from Diagnostics it retains its usual back behavior.
      if (event == button::Gesture::B_SHORT) return exit();
      return ignored();

    case Stage::Confirm:
      if (event == button::Gesture::B_SHORT) {
        _stage = Stage::Notice;
        requestDraw();
        return stay();
      }
      if (event == button::Gesture::A_LONG) {
        if (storage::requestFormat()) {
          _stage = Stage::Restarting;
          _rebootAtMs = millis() + kRebootDelayMs;
        } else {
          _stage = Stage::RequestFailed;
        }
        requestDraw();
        return stay();
      }
      return ignored();

    case Stage::RequestFailed:
      if (event == button::Gesture::A_SHORT) {
        _stage = Stage::Notice;
        requestDraw();
        return stay();
      }
      if (event == button::Gesture::B_SHORT) return exit();
      return ignored();

    case Stage::Restarting:
      return ignored();
  }
  return ignored();
}

ScreenResult StorageRecoveryScreen::tick() {
  if (_stage == Stage::Restarting &&
      static_cast<int32_t>(millis() - _rebootAtMs) >= 0) {
    ESP.restart();
  }
  return stay();
}

void StorageRecoveryScreen::drawNotice(LGFX_Sprite* c) const {
  const bool settingsUnavailable =
      storage::failureReason() == storage::FailureReason::SettingsUnavailable;
  const char* title = settingsUnavailable ? "Settings error" : "Storage error";
  const char* body =
      settingsUnavailable
          ? "Cannot read or save device settings. Shots will not be recorded."
          : "Cannot read flash storage. Shots will not be recorded.";
  ui::drawCriticalMessageScreen(c, title, body);
}

void StorageRecoveryScreen::drawStatus(LGFX_Sprite* c, const char* message,
                                       uint32_t color) const {
  c->fillScreen(theme::bg());
  c->setTextColor(color, theme::bg());
  layout::drawCenteredInBox(c, message, 4, 0, c->width() - 8, c->height(),
                            font::textFamily());
}

bool StorageRecoveryScreen::onDraw(LGFX_Sprite* c) {
  switch (_stage) {
    case Stage::Notice:
      drawNotice(c);
      break;
    case Stage::Confirm:
      ui::drawCriticalMessageScreen(
          c, "Reset shot history?",
          "Deletes all shot history. Wi-Fi and settings are kept.");
      break;
    case Stage::Restarting:
      drawStatus(c, "Restarting...", theme::fg());
      break;
    case Stage::RequestFailed:
      drawStatus(c, "Settings write failed", theme::critical());
      break;
  }
  return true;
}

ButtonHints StorageRecoveryScreen::buttonHints() const {
  switch (_stage) {
    case Stage::Notice:
      return {{Hint{HintGlyph::None, "RESET"}},
              {Hint{HintGlyph::None, "SKIP"}}};
    case Stage::Confirm:
      return {{{}, Hint{HintGlyph::None, "CONFIRM"}}, {Hint{HintGlyph::Back}}};
    case Stage::RequestFailed:
      return {{Hint{HintGlyph::Back, "BACK"}},
              {Hint{HintGlyph::None, "CONTINUE"}}};
    case Stage::Restarting:
    default:
      return {};
  }
}
