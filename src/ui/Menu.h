// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <M5Unified.h>

#include <string>
#include <vector>

#include "Screen.h"
#include "button.h"
#include "fonts.h"

class Menu : public Screen {
 public:
  // A menu entry. Pointer payloads are caller-owned; Menu only stores and
  // returns them.
  struct Item {
    std::string label;
    enum Type : uint8_t { NONE, DESTINATION, ACTION } type = NONE;
    union {
      Screen* destination = nullptr;
      int32_t value;  // ACTION: caller-defined code
    };

    static Item open(std::string label, Screen& destination) {
      Item i;
      i.label = std::move(label);
      i.type = DESTINATION;
      i.destination = &destination;
      return i;
    }
    static Item action(std::string label, int32_t value) {
      Item i;
      i.label = std::move(label);
      i.type = ACTION;
      i.value = value;
      return i;
    }
  };

  enum MenuAction : uint8_t {
    NONE,
    SELECTION_CHANGED,
    SELECTION_CONFIRMED,
    MENU_CLOSED,
  };

  Menu() = default;
  Menu(const char* title, const std::vector<Item>& items,
       uint8_t selectedIndex = 0);

  void init(const char* title, const std::vector<Item>& items,
            uint8_t selectedIndex = 0);

  void onEnter() override {}
  ScreenResult onEvent(button::Gesture event) override;
  bool onDraw(LGFX_Sprite* canvas) override;

  // Process a button event. Only mutates state; the caller is responsible for
  // requesting a redraw when the action is SELECTION_CHANGED.
  MenuAction update(button::Gesture event);

  // Render into the supplied canvas. Canvas is assumed to be sized to the
  // menu's available area; Menu reads its dimensions and draws full-bleed.
  void draw(LGFX_Sprite* canvas);

  // Typed payload of the selected item. Each returns its empty value
  // (nullptr / 0) when the menu is empty or the selection is another kind,
  // so callers can probe without checking the type first.
  Screen* selectedDestination() const {
    return _selectedIs(Item::DESTINATION) ? _items[_selectedIndex].destination
                                          : nullptr;
  }
  int32_t selectedValue() const {
    return _selectedIs(Item::ACTION) ? _items[_selectedIndex].value : 0;
  }

 private:
  bool _selectedIs(Item::Type t) const {
    return !_items.empty() && _items[_selectedIndex].type == t;
  }
  uint16_t getItemHeight();
  void drawItem(LGFX_Sprite* canvas, uint16_t y, uint16_t index, bool selected);

  std::string _title;
  std::vector<Item> _items;
  uint8_t _selectedIndex = 0;
  uint16_t _itemFontHeight = 0;

  font::font_t _itemFont = font::menu_item();

  static const uint16_t MENU_TOP_MARGIN = 4;
  static const uint16_t MENU_ITEM_MARGIN = 2;
};
