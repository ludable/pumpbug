// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

class OnboardingStore {
 public:
  void load();
  bool isComplete() const { return _complete; }
  void setComplete();

 private:
  bool _complete = false;
  bool _loaded = false;
};
