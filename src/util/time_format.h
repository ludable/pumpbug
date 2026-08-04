// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <ctime>

namespace timefmt {

// Writes "MM:SS" (zero-padded, both fields exactly two digits) into `out`.
// Minutes clamp to 99 so the field never widens past the 5-char visual
// template the digit-family layout helpers size for.
inline void formatMMSS(uint32_t totalSec, char* out, size_t n) {
  uint32_t m = totalSec / 60;
  const uint32_t s = totalSec % 60;
  if (m > 99) m = 99;
  std::snprintf(out, n, "%02u:%02u", static_cast<unsigned>(m),
                static_cast<unsigned>(s));
}

// Wall-clock "HH:MM:SS" from Unix epoch seconds, or "--:--:--" when `utcSec`
// is 0 (the wall clock isn't usable yet — no RTC and no NTP since boot).
// Rendered in UTC unless a TZ has been configured in the environment.
inline void formatClock(uint32_t utcSec, char* out, size_t n) {
  if (utcSec == 0) {
    std::snprintf(out, n, "--:--:--");
    return;
  }
  const time_t t = static_cast<time_t>(utcSec);
  struct tm tmv;
  localtime_r(&t, &tmv);
  std::snprintf(out, n, "%02d:%02d:%02d", tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
}

// Wall-clock "D Mon HH:MM" from Unix epoch seconds, or "--" when unknown. The
// month name avoids ambiguous US/EU numeric dates while staying compact enough
// for on-device logs and metadata strips.
inline void formatDateTime(uint32_t utcSec, char* out, size_t n) {
  if (utcSec == 0) {
    std::snprintf(out, n, "--");
    return;
  }
  const time_t t = static_cast<time_t>(utcSec);
  struct tm tmv;
  localtime_r(&t, &tmv);
  static constexpr const char* kMonth[] = {"Jan", "Feb", "Mar", "Apr",
                                           "May", "Jun", "Jul", "Aug",
                                           "Sep", "Oct", "Nov", "Dec"};
  const int mon = (tmv.tm_mon >= 0 && tmv.tm_mon < 12) ? tmv.tm_mon : 0;
  std::snprintf(out, n, "%d %s %02d:%02d", tmv.tm_mday, kMonth[mon],
                tmv.tm_hour, tmv.tm_min);
}

// Wall-clock "YYYY-MM-DD HH:MM" from Unix epoch seconds, or "--" when unknown.
// Use for logs/diagnostics where lexical sorting and copy/paste clarity matter
// more than fitting into a tiny metadata strip.
inline void formatIsoDateTime(uint32_t utcSec, char* out, size_t n) {
  if (utcSec == 0) {
    std::snprintf(out, n, "--");
    return;
  }
  const time_t t = static_cast<time_t>(utcSec);
  struct tm tmv;
  localtime_r(&t, &tmv);
  std::snprintf(out, n, "%04d-%02d-%02d %02d:%02d", tmv.tm_year + 1900,
                tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min);
}

}  // namespace timefmt
