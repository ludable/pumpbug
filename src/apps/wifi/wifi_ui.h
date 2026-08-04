// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <M5Unified.h>

#include <cstdio>

#include "net/Auth.h"
#include "net/NetworkStatus.h"
#include "net/WifiManager.h"
#include "ui/blocks.h"
#include "ui/icons.h"
#include "ui/theme.h"

// Drawing and formatting shared by the Wi-Fi setup wizard and status screen.
namespace wifi_ui {

// Size reference for info-block labels: the widest label any Wi-Fi screen
// uses, so stacked labels render at one size across the module's screens.
// Landscape rows get their uniformity from the group itself instead
// (ui::drawInfoScreen fits one label size and column per screen), so the
// cross-screen uniformity this buys is portrait-only.
constexpr const char* kLabelSizeRef = "ERROR";

// Header band with the Wi-Fi glyph in the right slot — text annotations
// there clip in portrait. `activeBands` animates the glyph while
// connecting (see ui::ConnectingSweep).
inline int drawWifiHeader(LGFX_Sprite* c, const char* title,
                          NetworkStatus status, int activeBands = 3) {
  const int headerH = ui::drawViewHeader(c, title, theme::accent());
  constexpr int pad = 4;
  ui::drawWifiIcon(c,
                   {c->width() - pad - 24,
                    (headerH - 20) / 2 + ui::kViewHeaderContentOffsetY, 24, 20},
                   status, theme::bg(), activeBands);
  return headerH;
}

// The pairing URL every Wi-Fi QR encodes: the device's durable .local origin
// (so the cookie keeps working after the device moves networks) plus the
// current one-time pair token. `to` is the query-encoded post-pair destination;
// the encoded config destination preserves the fragment web-src/config/app.js
// reads.
inline void formatPairUrl(char* buf, size_t n, const char* to) {
  std::snprintf(buf, n, "http://%s.local/auth/pair?token=%s&to=%s",
                wifiManager.hostname(), auth.currentPairToken().c_str(), to);
}

// The "type it instead" screen behind every pairing QR: one guidance line,
// the device URL(s), and the PIN inline. `host` may be nullptr when only
// one address is worth typing (AP mode, where the numeric IP is the short
// durable one). Draws below `headerH`.
inline void drawPairDetails(LGFX_Sprite* c, int headerH, const char* host,
                            const char* ip, const char* pin) {
  // "http://" as the label splits a URL across the block shape: the scheme
  // still marks the value as a URL without stealing width from the part the
  // user actually types. "PIN dddd" inlines to one full-width line.
  char pinLine[16];
  std::snprintf(pinLine, sizeof(pinLine), "PIN %s", pin);
  ui::InfoBlock blocks[3];
  int count = 0;
  if (host && *host) blocks[count++] = {"http://", host, theme::fg(), 26};
  blocks[count++] = {"http://", ip, theme::fg(), 26};
  blocks[count++] = {"", pinLine, theme::fg(), 26};
  ui::drawInfoScreen(c, headerH, "Or browse here and enter the PIN:",
                     "Or browse here:", blocks, count, theme::bg(),
                     kLabelSizeRef);
}

}  // namespace wifi_ui
