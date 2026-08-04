// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include "apps/onboarding/OnboardingScreen.h"
#include "apps/system/StorageRecoveryScreen.h"
#include "ui/Menu.h"
#include "ui/Navigation.h"
#include "ui/Screen.h"

class MainNavigation : public RootNavigation {
 public:
  MainNavigation(Menu& mainMenu, Screen& mainScreen,
                 OnboardingScreen& onboardingScreen,
                 StorageRecoveryScreen& storageRecoveryScreen)
      : _mainMenu(mainMenu),
        _mainScreen(mainScreen),
        _onboardingScreen(onboardingScreen),
        _storageRecoveryScreen(storageRecoveryScreen) {}

  Screen& initialScreen() override {
    if (_storageRecoveryScreen.shouldPresent()) return _storageRecoveryScreen;
    return _ordinaryInitialScreen();
  }

  Screen& screenAfterExit(Screen& exiting) override {
    if (&exiting == &_storageRecoveryScreen) return _ordinaryInitialScreen();
    if (&exiting == &_onboardingScreen &&
        _onboardingScreen.completedThisVisit()) {
      return _mainMenu;
    }
    // At the root of navigation, toggles between the main screen and the main
    // menu.
    if (&exiting == &_mainScreen) return _mainMenu;
    return _mainScreen;
  }

  Screen* shortcutDestination(Screen& active,
                              button::Gesture gesture) override {
    // B-hold is an unhinted shortcut back to the product's primary view. A
    // tap keeps its local Back/Menu meaning, and holding on the main screen is
    // a no-op.
    if (gesture == button::Gesture::B_LONG && &active != &_mainScreen) {
      return &_mainScreen;
    }
    return nullptr;
  }

 private:
  Menu& _mainMenu;
  Screen& _mainScreen;
  OnboardingScreen& _onboardingScreen;
  StorageRecoveryScreen& _storageRecoveryScreen;

  Screen& _ordinaryInitialScreen() {
    if (_onboardingScreen.begin()) return _onboardingScreen;
    return _mainScreen;
  }
};
