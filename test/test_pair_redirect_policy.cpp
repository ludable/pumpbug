// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#include <cassert>
#include <cstdio>
#include <string_view>

#include "net/PairRedirectPolicy.h"

int main() {
  using net::pairRedirectDestination;

  assert(std::string_view(pairRedirectDestination(true, "/")) == "/");
  assert(std::string_view(pairRedirectDestination(true, "/config/#paired")) ==
         "/config/#paired");

  assert(std::string_view(pairRedirectDestination(false, "/")) == "/config/");
  assert(std::string_view(pairRedirectDestination(false, "/config/#paired")) ==
         "/config/");

  assert(std::string_view(pairRedirectDestination(true, "")) == "/");
  assert(std::string_view(pairRedirectDestination(true, "/config/")) == "/");
  assert(std::string_view(pairRedirectDestination(true, "//evil.example")) ==
         "/");
  assert(std::string_view(pairRedirectDestination(true, "/\\evil.example")) ==
         "/");
  // WebServer decodes query arguments before applying this policy, so the
  // encoded form is not itself an allowed destination.
  assert(std::string_view(pairRedirectDestination(true, "/config/%23paired")) ==
         "/");
  assert(std::string_view(
             pairRedirectDestination(true, "https://evil.example")) == "/");

  std::puts("OK: all assertions passed");
  return 0;
}
