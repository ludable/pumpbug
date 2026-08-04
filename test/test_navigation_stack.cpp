// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#include <cassert>

#include "ui/Navigation.h"

int main() {
  int home = 1;
  int onboarding = 2;
  int diagnostics = 3;
  int logs = 4;
  int wifiStatus = 5;
  int wifiSetup = 6;

  NavigationStack<int> stack;
  stack.begin(onboarding);
  assert(stack.size() == 1);
  assert(stack.top() == &onboarding);

  assert(!stack.pop());
  assert(stack.top() == &onboarding);
  stack.replace(home);
  assert(stack.top() == &home);
  stack.push(home);
  assert(stack.top() == &home);

  assert(stack.pop());
  stack.push(diagnostics);
  stack.push(logs);
  assert(stack.top() == &logs);
  assert(stack.pop());
  assert(stack.top() == &diagnostics);
  assert(stack.pop());
  assert(stack.top() == &home);

  stack.push(wifiStatus);
  stack.replace(wifiSetup);
  assert(stack.top() == &wifiSetup);
  stack.replace(wifiStatus);
  assert(stack.top() == &wifiStatus);
  assert(stack.pop());
  assert(stack.top() == &home);

  stack.push(onboarding);
  assert(stack.top() == &onboarding);
  assert(stack.pop());
  assert(stack.top() == &home);

  stack.begin(home);
  assert(stack.size() == 1);
  assert(stack.top() == &home);
}
