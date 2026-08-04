// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#include "OnboardingStore.h"

#include <Preferences.h>

namespace {
constexpr const char* kNamespace = "onboard";
constexpr const char* kComplete = "complete";
}  // namespace

void OnboardingStore::load() {
  if (_loaded) return;

  Preferences preferences;
  preferences.begin(kNamespace, /*readOnly=*/true);
  _complete = preferences.getBool(kComplete, false);
  preferences.end();
  _loaded = true;
}

void OnboardingStore::setComplete() {
  if (_complete) return;

  Preferences preferences;
  preferences.begin(kNamespace, /*readOnly=*/false);
  preferences.putBool(kComplete, true);
  preferences.end();
  _complete = true;
  _loaded = true;
}
