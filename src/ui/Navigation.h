// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <cstddef>
#include <vector>

#include "button.h"

class Screen;

template <typename Entry>
class NavigationStack {
 public:
  void begin(Entry& initial) {
    _entries.clear();
    _entries.reserve(kInitialCapacity);
    _entries.push_back(&initial);
  }

  Entry* top() const { return _entries.empty() ? nullptr : _entries.back(); }

  void push(Entry& entry) { _entries.push_back(&entry); }

  void replace(Entry& entry) {
    if (_entries.empty()) {
      _entries.push_back(&entry);
    } else {
      _entries.back() = &entry;
    }
  }

  bool pop() {
    if (_entries.size() <= 1) return false;
    _entries.pop_back();
    return true;
  }

  std::size_t size() const { return _entries.size(); }

 private:
  // Covers the initial entry and the first levels of interactive navigation
  // before another allocation is required.
  static constexpr std::size_t kInitialCapacity = 4;
  std::vector<Entry*> _entries;
};

// Selects the screen shown at startup and after the bottom screen exits.
class RootNavigation {
 public:
  virtual ~RootNavigation() = default;

  virtual Screen& initialScreen() = 0;
  virtual Screen& screenAfterExit(Screen& exiting) = 0;

  // Resolve a product-wide gesture that the foreground screen did not handle.
  // Returning a destination resets the navigation stack to that screen.
  virtual Screen* shortcutDestination(Screen&, button::Gesture) {
    return nullptr;
  }
};
