// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#include <cassert>
#include <string_view>

#include "net/wifi_power_save.h"

int main() {
  static_assert(wifi::kDefaultStaPowerSave == wifi::PowerSave::MaximumModem);
  assert(std::string_view(wifi::powerSaveName(wifi::PowerSave::None)) ==
         "none");
  assert(std::string_view(wifi::powerSaveName(wifi::PowerSave::MinimumModem)) ==
         "minimum_modem");
  assert(std::string_view(wifi::powerSaveName(wifi::PowerSave::MaximumModem)) ==
         "maximum_modem");
}
