// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#include "Menu.h"

#include <cmath>

#include "blocks.h"
#include "fonts.h"
#include "theme.h"

Menu::Menu(const char* title, const std::vector<Item>& items,
           uint8_t selectedIndex) {
  init(title, items, selectedIndex);
}

void Menu::init(const char* title, const std::vector<Item>& items,
                uint8_t selectedIndex) {
  _title = std::string(title);
  _items = items;
  _selectedIndex = selectedIndex;

  _itemFontHeight = font::metrics(_itemFont).height;
}

ScreenResult Menu::onEvent(button::Gesture event) {
  switch (update(event)) {
    case SELECTION_CHANGED:
      requestDraw();
      return stay();
    case SELECTION_CONFIRMED:
      if (Screen* destination = selectedDestination()) {
        return push(*destination);
      }
      return stay();
    case MENU_CLOSED:
      return exit();
    case NONE:
      return ignored();
  }
  return ignored();
}

bool Menu::onDraw(LGFX_Sprite* canvas) {
  draw(canvas);
  return true;
}

uint16_t Menu::getItemHeight() {
  return _itemFontHeight + MENU_ITEM_MARGIN * 2;
}

void Menu::drawItem(LGFX_Sprite* canvas, uint16_t y, uint16_t index,
                    bool selected) {
  const uint16_t w = canvas->width();
  const uint16_t itemHeight = getItemHeight();

  canvas->setCursor(6, y + MENU_ITEM_MARGIN);

  if (selected) {
    // Highlight bar
    canvas->fillRect(0, y, w, itemHeight, theme::highlight());
    canvas->setTextColor(theme::highlight_fg(), theme::highlight());

  } else {
    canvas->setTextColor(theme::fg(), theme::bg());
  }
  canvas->printf("%s", _items[index].label.c_str());
}

void Menu::draw(LGFX_Sprite* canvas) {
  const uint16_t canvasWidth = canvas->width();
  const uint16_t canvasHeight = canvas->height();

  canvas->fillRect(0, 0, canvasWidth, canvasHeight, theme::bg());

  canvas->setTextDatum(top_left);

  const uint16_t headerH =
      ui::drawViewHeader(canvas, _title.c_str(), theme::accent());
  // The divider distinguishes the persistent menu title from its first item.
  canvas->drawFastHLine(0, headerH - 1, canvasWidth, theme::accent());

  int y = headerH + MENU_TOP_MARGIN;

  const uint16_t itemHeight = getItemHeight();
  const uint16_t availableHeight = canvasHeight - y;
  const uint16_t maxVisible = availableHeight / itemHeight;
  const uint16_t totalItems = _items.size();

  int16_t scrollOffset = 0;
  if (totalItems > maxVisible) {
    // Place the current item at the center if possible
    scrollOffset = _selectedIndex - (maxVisible / 2);
    // Clamp offset to valid range
    if (scrollOffset < 0) scrollOffset = 0;
    if (scrollOffset + maxVisible > totalItems) {
      scrollOffset = totalItems - maxVisible;
    }
  }

  canvas->setFont(_itemFont);
  canvas->setTextSize(1);

  // Draw one more in case it's partially visible
  uint16_t endIdx = scrollOffset + (maxVisible + 1);
  if (endIdx > totalItems) endIdx = totalItems;

  const uint16_t itemsTop = y;

  for (int pos = 0, i = scrollOffset; i < endIdx; i++) {
    drawItem(canvas, y, i, (i == _selectedIndex));
    y += itemHeight;
  }

  canvas->setTextColor(theme::accent(), theme::bg());

  if (scrollOffset > 0) {
    // Arrow up indicator
    const uint16_t arrowHeight = std::floor(_itemFontHeight * 0.5);
    const uint16_t arrowTop = itemsTop + 2;
    const uint16_t arrowBottom = arrowTop + arrowHeight;
    const uint16_t arrowLeft = canvasWidth - arrowHeight - MENU_ITEM_MARGIN;
    const uint16_t arrowCenter = arrowLeft + std::round(arrowHeight / 2);
    const uint16_t arrowRight = arrowLeft + arrowHeight;
    canvas->fillTriangle(arrowCenter, arrowTop, arrowLeft, arrowBottom,
                         arrowRight, arrowBottom, theme::accent());
  }

  if (scrollOffset + maxVisible < totalItems) {
    // Arrow down indicator
    const uint16_t arrowHeight = std::floor(_itemFontHeight * 0.5);
    const uint16_t arrowBottom = canvasHeight - MENU_ITEM_MARGIN;
    const uint16_t arrowTop = arrowBottom - arrowHeight;
    const uint16_t arrowLeft = canvasWidth - arrowHeight - MENU_ITEM_MARGIN;
    const uint16_t arrowCenter = arrowLeft + std::round(arrowHeight / 2);
    const uint16_t arrowRight = arrowLeft + arrowHeight;
    canvas->fillTriangle(arrowCenter, arrowBottom, arrowLeft, arrowTop,
                         arrowRight, arrowTop, theme::accent());
  }
}

Menu::MenuAction Menu::update(button::Gesture event) {
  if (event == button::Gesture::NONE) return NONE;

  switch (event) {
    case button::Gesture::A_SHORT:
      if (_items.empty()) return NONE;
      _selectedIndex = ++_selectedIndex % _items.size();
      return SELECTION_CHANGED;

    case button::Gesture::A_LONG:
      if (_items.empty()) return NONE;
      return SELECTION_CONFIRMED;

    case button::Gesture::B_SHORT:
      return MENU_CLOSED;

    default:
      break;
  }
  return NONE;
}
