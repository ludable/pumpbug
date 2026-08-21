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
  _formatPresented = false;
  requestDraw();
}

ScreenResult StorageRecoveryScreen::onEvent(button::Gesture event) {
  switch (_stage) {
    case Stage::Notice:
      if (event == button::Gesture::A_SHORT) {
        if (storage::retryMount() == storage::MountState::Ready) {
          restartSoon();
          return stay();
        }
        _stage = Stage::ActionFailed;
        requestDraw();
        return stay();
      }
      if (event == button::Gesture::A_LONG) {
        _stage = Stage::Confirm;
        requestDraw();
        return stay();
      }
      if (event == button::Gesture::B_SHORT) return exit();
      return ignored();

    case Stage::Confirm:
      if (event == button::Gesture::B_SHORT) {
        _stage = Stage::Notice;
        requestDraw();
        return stay();
      }
      if (event == button::Gesture::A_LONG) {
        _stage = Stage::Formatting;
        _formatPresented = false;
        requestDraw();
        return stay();
      }
      return ignored();

    case Stage::Formatting:
      return ignored();

    case Stage::ActionFailed:
      if (event == button::Gesture::B_SHORT) {
        _stage = Stage::Notice;
        requestDraw();
        return stay();
      }
      return ignored();

    case Stage::Restarting:
      return ignored();
  }
  return ignored();
}

void StorageRecoveryScreen::restartSoon() {
  _stage = Stage::Restarting;
  _rebootAtMs = millis() + kRebootDelayMs;
  requestDraw();
}

ScreenResult StorageRecoveryScreen::tick() {
  if (_stage == Stage::Formatting && _formatPresented) {
    _formatPresented = false;
    if (storage::format() == storage::MountState::Ready) {
      restartSoon();
    } else {
      _stage = Stage::ActionFailed;
      requestDraw();
    }
    return stay();
  }

  if (_stage == Stage::Restarting &&
      static_cast<int32_t>(millis() - _rebootAtMs) >= 0) {
    ESP.restart();
  }
  return stay();
}

void StorageRecoveryScreen::onPresented() {
  if (_stage == Stage::Formatting) _formatPresented = true;
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
      ui::drawCriticalMessageScreen(
          c, "Shot storage unavailable",
          "Retry, erase saved shots, or continue without recording.");
      break;
    case Stage::Confirm:
      ui::drawCriticalMessageScreen(
          c, "Erase saved shots?",
          "Formats shot storage. Wi-Fi and settings are kept.");
      break;
    case Stage::Formatting:
      drawStatus(c, "Erasing...", theme::fg());
      break;
    case Stage::ActionFailed:
      drawStatus(c, "Recovery failed", theme::critical());
      break;
    case Stage::Restarting:
      drawStatus(c, "Restarting...", theme::fg());
      break;
  }
  return true;
}

ButtonHints StorageRecoveryScreen::buttonHints() const {
  ButtonHints hints{};
  switch (_stage) {
    case Stage::Notice:
      hints.a.tap = Hint{HintGlyph::None, "RETRY"};
      hints.a.hold = Hint{HintGlyph::Trash, "ERASE"};
      hints.b.tap = Hint{HintGlyph::None, "SKIP"};
      break;
    case Stage::Confirm:
      hints.a.hold = Hint{HintGlyph::Trash, "ERASE"};
      hints.b.tap = Hint{HintGlyph::Back};
      break;
    case Stage::Formatting:
      break;
    case Stage::ActionFailed:
      hints.b.tap = Hint{HintGlyph::Back, "BACK"};
      break;
    case Stage::Restarting:
      break;
  }
  return hints;
}
