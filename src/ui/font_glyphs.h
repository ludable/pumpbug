// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

// Icon/symbol glyphs available in the lv_font_montserrat_* family.
//
// WHICH FONTS: every lv_font_montserrat_* size that M5GFX vendors
// (lgfx::fonts::lv_font_montserrat_8 .. _48, see ui/fonts.h). All sizes share
// the same glyph set; only the rendered point size differs. These glyphs are
// NOT present in the bitmap/TTF fonts (Font2, DejaVu*, Roboto_Thin_24), so
// only draw them while a montserrat font is selected (e.g. font::body(),
// font::button_hint()).
//
// WHY THESE EXIST HERE: M5GFX vendors only the font glyph *data*
// (lv_font_montserrat_*.c); it does NOT ship LVGL's lv_symbol_def.h, so there
// is no lgfx::LV_SYMBOL_* / lgfx::fonts::LV_SYMBOL_* constant to reference.
// The glyphs themselves are reachable only by their raw UTF-8 codepoint, so we
// name them here, mirroring LVGL's LV_SYMBOL_* names.
//
// REFERENCE:
//   - Glyph coverage comes from the cmap in each font file, e.g.
//     .pio/libdeps/<env>/M5GFX/src/lgfx/Fonts/lvgl/lv_font_montserrat_24.c
//     (printable ASCII U+0020..U+007E, plus the U+00B0/U+2022 and the
//      FontAwesome PUA codepoints listed below).
//   - Canonical names + codepoints: LVGL's lv_symbol_def.h
//     https://github.com/lvgl/lvgl/blob/master/src/font/lv_symbol_def.h
//
// USAGE:
//   canvas->setFont(font::body());
//   canvas->drawString(font::glyph::WIFI, x, y);
//
// The literals are plain UTF-8 byte sequences; concatenate with adjacent
// string literals like any other ("battery " font::glyph::BATTERY_FULL).

namespace font {
namespace glyph {

// Latin-1 / punctuation that also lives in these fonts (not FontAwesome).
constexpr const char* DEGREE = "\xC2\xB0";      // U+00B0  °
constexpr const char* BULLET = "\xE2\x80\xA2";  // U+2022  •

// FontAwesome symbol glyphs (Unicode Private Use Area).
constexpr const char* AUDIO = "\xEF\x80\x81";          // U+F001
constexpr const char* VIDEO = "\xEF\x80\x88";          // U+F008
constexpr const char* LIST = "\xEF\x80\x8B";           // U+F00B
constexpr const char* OK = "\xEF\x80\x8C";             // U+F00C
constexpr const char* CLOSE = "\xEF\x80\x8D";          // U+F00D
constexpr const char* POWER = "\xEF\x80\x91";          // U+F011
constexpr const char* SETTINGS = "\xEF\x80\x93";       // U+F013
constexpr const char* HOME = "\xEF\x80\x95";           // U+F015
constexpr const char* DOWNLOAD = "\xEF\x80\x99";       // U+F019
constexpr const char* DRIVE = "\xEF\x80\x9C";          // U+F01C
constexpr const char* REFRESH = "\xEF\x80\xA1";        // U+F021
constexpr const char* MUTE = "\xEF\x80\xA6";           // U+F026
constexpr const char* VOLUME_MID = "\xEF\x80\xA7";     // U+F027
constexpr const char* VOLUME_MAX = "\xEF\x80\xA8";     // U+F028
constexpr const char* IMAGE = "\xEF\x80\xBE";          // U+F03E
constexpr const char* TINT = "\xEF\x81\x83";           // U+F043
constexpr const char* PREV = "\xEF\x81\x88";           // U+F048
constexpr const char* PLAY = "\xEF\x81\x8B";           // U+F04B
constexpr const char* PAUSE = "\xEF\x81\x8C";          // U+F04C
constexpr const char* STOP = "\xEF\x81\x8D";           // U+F04D
constexpr const char* NEXT = "\xEF\x81\x91";           // U+F051
constexpr const char* EJECT = "\xEF\x81\x92";          // U+F052
constexpr const char* LEFT = "\xEF\x81\x93";           // U+F053
constexpr const char* RIGHT = "\xEF\x81\x94";          // U+F054
constexpr const char* PLUS = "\xEF\x81\xA7";           // U+F067
constexpr const char* MINUS = "\xEF\x81\xA8";          // U+F068
constexpr const char* EYE_OPEN = "\xEF\x81\xAE";       // U+F06E
constexpr const char* EYE_CLOSE = "\xEF\x81\xB0";      // U+F070
constexpr const char* WARNING = "\xEF\x81\xB1";        // U+F071
constexpr const char* SHUFFLE = "\xEF\x81\xB4";        // U+F074
constexpr const char* UP = "\xEF\x81\xB7";             // U+F077
constexpr const char* DOWN = "\xEF\x81\xB8";           // U+F078
constexpr const char* LOOP = "\xEF\x81\xB9";           // U+F079
constexpr const char* DIRECTORY = "\xEF\x81\xBB";      // U+F07B
constexpr const char* UPLOAD = "\xEF\x82\x93";         // U+F093
constexpr const char* CALL = "\xEF\x82\x95";           // U+F095
constexpr const char* CUT = "\xEF\x83\x84";            // U+F0C4
constexpr const char* COPY = "\xEF\x83\x85";           // U+F0C5
constexpr const char* SAVE = "\xEF\x83\x87";           // U+F0C7
constexpr const char* BARS = "\xEF\x83\x89";           // U+F0C9
constexpr const char* ENVELOPE = "\xEF\x83\xA0";       // U+F0E0
constexpr const char* CHARGE = "\xEF\x83\xA7";         // U+F0E7
constexpr const char* PASTE = "\xEF\x83\xAA";          // U+F0EA
constexpr const char* BELL = "\xEF\x83\xB3";           // U+F0F3
constexpr const char* KEYBOARD = "\xEF\x84\x9C";       // U+F11C
constexpr const char* GPS = "\xEF\x84\xA4";            // U+F124
constexpr const char* FILE = "\xEF\x85\x9B";           // U+F15B
constexpr const char* WIFI = "\xEF\x87\xAB";           // U+F1EB
constexpr const char* BATTERY_FULL = "\xEF\x89\x80";   // U+F240
constexpr const char* BATTERY_3 = "\xEF\x89\x81";      // U+F241
constexpr const char* BATTERY_2 = "\xEF\x89\x82";      // U+F242
constexpr const char* BATTERY_1 = "\xEF\x89\x83";      // U+F243
constexpr const char* BATTERY_EMPTY = "\xEF\x89\x84";  // U+F244
constexpr const char* USB = "\xEF\x8A\x87";            // U+F287
constexpr const char* BLUETOOTH = "\xEF\x8A\x93";      // U+F293
constexpr const char* TRASH = "\xEF\x8B\xAD";          // U+F2ED
constexpr const char* EDIT = "\xEF\x8C\x84";           // U+F304
constexpr const char* BACKSPACE = "\xEF\x95\x9A";      // U+F55A
constexpr const char* SD_CARD = "\xEF\x9F\x82";        // U+F7C2
constexpr const char* NEW_LINE = "\xEF\xA2\xA2";       // U+F8A2

}  // namespace glyph
}  // namespace font
