// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#include "theme.h"

namespace theme {

namespace {

constexpr Palette kDark = {
    .bg = 0x000000,
    .bg_alt = 0x181818,
    .surface = 0x303030,
    .muted = 0x404040,
    .fg = 0xFFFFFF,
    .dim = 0x808080,
    .highlight = 0xC0C0C0,
    .highlight_fg = 0x000000,
    .accent = 0x2080C0,
    .accent_light = 0xA0C0FF,
    .ok = 0x00C000,
    .warn = 0xFFFF00,
    .critical = 0xFF0000,
    .alarm = 0xDC2828,
    .ok_fill = 0x00C000,
    .warn_fill = 0xFFFF00,
    .critical_fill = 0xFF0000,
    .critical_fill_fg = 0xFFFFFF,
    .chart_bg = 0x000000,
    .chart_fg = 0xFFFFFF,
    .chart_flow = 0xFFFF00,
};

constexpr Palette kLight = {
    .bg = 0xFFFFFF,
    .bg_alt = 0xE0E0E0,
    .surface = 0xE0E0E0,
    .muted = 0xB0B0B0,
    .fg = 0x101010,
    .dim = 0x606060,
    .highlight = 0x0077CC,
    .highlight_fg = 0xFFFFFF,
    .accent = 0x0077CC,
    .accent_light = 0x80C0FF,
    .ok = 0x1B7A46,
    .warn = 0x996600,
    .critical = 0xB00020,
    .alarm = 0xE02020,
    .ok_fill = 0x00C000,
    .warn_fill = 0xFFFF00,
    .critical_fill = 0xFF0000,
    .critical_fill_fg = 0xFFFFFF,
    .chart_bg = 0x000000,
    .chart_fg = 0xFFFFFF,
    .chart_flow = 0xFFFF00,
};

const Palette* gActive = &kDark;
}  // namespace

const Palette& palette() { return *gActive; }

void setLightMode(bool light) { gActive = light ? &kLight : &kDark; }

bool isLightMode() { return gActive == &kLight; }

}  // namespace theme
