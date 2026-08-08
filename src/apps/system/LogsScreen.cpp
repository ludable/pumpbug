// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#include "LogsScreen.h"

#include <cstdio>
#include <iterator>  // std::size

#include "apps/extraction/Extraction.h"  // EndCause, NO_WEIGHT sentinel
#include "diagnostics/HeapMonitor.h"
#include "diagnostics/PanicDump.h"
#include "diagnostics/PowerEventLog.h"
#include "diagnostics/RuntimeEventLog.h"
#include "diagnostics/reset_reason.h"
#include "ui/blocks.h"
#include "ui/fonts.h"
#include "ui/layout.h"
#include "ui/theme.h"
#include "util/time_format.h"

namespace {

// Leading time column for a session log row: wall-clock "HH:MM:SS" once the
// clock is set, else the boot-relative uptime so the row stays useful before
// any RTC/NTP sync.
void formatStamp(uint32_t utcSec, uint32_t ms, char* out, size_t n) {
  if (utcSec != 0) {
    timefmt::formatClock(utcSec, out, n);
  } else {
    std::snprintf(out, n, "+%lus", static_cast<unsigned long>(ms / 1000));
  }
}

// Short label for the common Xtensa EXCCAUSE values a crash reports; nullptr
// for anything else (the caller prints the raw number). The web Panic tab
// carries the full set.
const char* excCauseName(uint32_t cause) {
  switch (cause) {
    case 0:
      return "IllegalInstr";
    case 2:
      return "InstrFetch";
    case 3:
      return "LoadStore";
    case 6:
      return "DivZero";
    case 9:
      return "Alignment";
    case 20:
      return "IFetchProhib";
    case 28:
      return "LoadProhib";
    case 29:
      return "StoreProhib";
    default:
      return nullptr;
  }
}

const char* endCauseShort(uint8_t c) {
  switch (static_cast<pump_scale::EndCause>(c)) {
    case pump_scale::EndCause::STABLE:
      return "stbl";
    case pump_scale::EndCause::TIMEOUT:
      return "t/o";
    default:
      return "?";
  }
}

}  // namespace

// Page label for the header. Takes the raw page index (Page is private to
// LogsScreen) — callers pass static_cast<uint8_t>(_page).
namespace {
const char* pageNameFor(uint8_t page) {
  switch (page) {
    case 0:
      return "SHOTS";
    case 1:
      return "NET";
    case 2:
      return "POWER";
    case 3:
      return "MEMORY";
    default:
      return "CRASH";
  }
}
}  // namespace

void LogsScreen::onEnter() {
  _lastExtractionWrites = runtimeEventLog.extractionWrites();
  _lastNetWrites = runtimeEventLog.netWrites();
  _lastPowerRevision = powerEventLog.revision();
  requestDraw();
}

void LogsScreen::onLayoutChanged() { requestDraw(); }

ButtonHints LogsScreen::buttonHints() const {
  ButtonHints h{};
  h.a.tap = Hint{HintGlyph::None, "Next"};
  h.a.hold = Hint{HintGlyph::Trash, "Clear"};
  h.b.tap = Hint{HintGlyph::Back, nullptr};
  return h;
}

// Wipe the ring backing the page in view. The first two are the in-RAM
// runtime event rings; Power is the NVS-backed log, so clearing it also
// rewrites the persisted blob (clear() handles both).
void LogsScreen::clearCurrentPage() {
  switch (_page) {
    case Page::Extraction:
      runtimeEventLog.clearExtraction();
      break;
    case Page::Net:
      runtimeEventLog.clearNet();
      break;
    case Page::Power:
      powerEventLog.clear();
      break;
    case Page::Heap:
      heapMonitor.clearHistory();
      break;
    case Page::Panic:
      // Erase the stored core dump and ask the main loop to resync the
      // status-bar crash marker (it owns Chrome). Same path the web clear
      // takes, so the dot drops without a reboot regardless of where the clear
      // came from.
      diagnostics::clearLastPanicAndResync();
      break;
  }
}

ScreenResult LogsScreen::onEvent(button::Gesture event) {
  switch (event) {
    case button::Gesture::A_SHORT:
      _page = static_cast<Page>((static_cast<uint8_t>(_page) + 1) % PAGE_COUNT);
      requestDraw();
      return stay();
    case button::Gesture::A_LONG:
      clearCurrentPage();
      requestDraw();
      return stay();
    case button::Gesture::B_SHORT:
      return exit();
    default:
      return ignored();
  }
}

ScreenResult LogsScreen::tick() {
  // The Heap page shows live allocator values that drift continuously, so
  // refresh it every tick (250ms) rather than on a write counter.
  if (_page == Page::Heap) {
    requestDraw();
    return stay();
  }
  // The power revision also changes when its latest timestamp is updated.
  // Runtime logs redraw when an event is appended.
  const uint32_t e = runtimeEventLog.extractionWrites();
  const uint32_t n = runtimeEventLog.netWrites();
  const uint32_t pw = powerEventLog.revision();
  if (e != _lastExtractionWrites || n != _lastNetWrites ||
      pw != _lastPowerRevision) {
    _lastExtractionWrites = e;
    _lastNetWrites = n;
    _lastPowerRevision = pw;
    requestDraw();
  }
  return stay();
}

bool LogsScreen::onDraw(LGFX_Sprite* c) {
  constexpr int pad = 3;
  const int W = c->width();
  const int H = c->height();
  c->fillScreen(theme::bg());

  const int headerH = ui::drawViewHeader(
      c, pageNameFor(static_cast<uint8_t>(_page)), theme::accent());

  const int bx = pad;
  const int by = headerH;
  const int bw = W - 2 * pad;
  const int bh = H - headerH - pad;

  switch (_page) {
    case Page::Extraction:
      drawExtraction(c, bx, by, bw, bh);
      break;
    case Page::Net:
      drawNet(c, bx, by, bw, bh);
      break;
    case Page::Power:
      drawPower(c, bx, by, bw, bh);
      break;
    case Page::Heap:
      drawHeap(c, bx, by, bw, bh);
      break;
    case Page::Panic:
      drawPanic(c, bx, by, bw, bh);
      break;
  }
  return true;
}

void LogsScreen::drawExtraction(LGFX_Sprite* c, int x, int y, int w, int h) {
  diagnostics::ExtractionStat
      rows[diagnostics::RuntimeEventLog::EXTRACTION_CAP];
  const size_t n = runtimeEventLog.snapshotExtraction(rows, std::size(rows));

  c->setFont(font::tiny());
  c->setTextSize(1);
  const int lineH = font::metrics(font::tiny()).height + 1;

  if (n == 0) {
    c->setTextColor(theme::dim(), theme::bg());
    layout::drawTopLeft(c, "no shots yet", x, y);
    return;
  }

  int cy = y;
  for (size_t i = 0; i < n && cy + lineH <= y + h; ++i, cy += lineH) {
    const diagnostics::ExtractionStat& e = rows[i];
    char ts[12];
    timefmt::formatClock(e.startUtcSec, ts, sizeof(ts));
    const unsigned long durS = (e.endMs - e.beginMs) / 1000;
    const unsigned long pumpS = e.totalPumpOnMs / 1000;
    char wbuf[10];
    if (e.yieldCg == pump_scale::Extraction::NO_WEIGHT) {
      std::snprintf(wbuf, sizeof(wbuf), "--g");
    } else {
      std::snprintf(wbuf, sizeof(wbuf), "%.1fg", e.yieldCg / 100.0f);
    }
    char buf[56];
    std::snprintf(buf, sizeof(buf), "%s %lus p%lus %s %s%s", ts, durS, pumpS,
                  wbuf, endCauseShort(e.endCause),
                  e.isLikelyRealShot ? " R" : "");
    // Real shots in primary text; flushes/refills dimmed.
    c->setTextColor(e.isLikelyRealShot ? theme::fg() : theme::dim(),
                    theme::bg());
    layout::drawTopLeft(c, buf, x, cy);
  }
}

void LogsScreen::drawNet(LGFX_Sprite* c, int x, int y, int w, int h) {
  diagnostics::NetFailure rows[diagnostics::RuntimeEventLog::NET_CAP];
  const size_t n = runtimeEventLog.snapshotNet(rows, std::size(rows));

  c->setFont(font::tiny());
  c->setTextSize(1);
  const int lineH = font::metrics(font::tiny()).height + 1;

  if (n == 0) {
    c->setTextColor(theme::dim(), theme::bg());
    layout::drawTopLeft(c, "no failures", x, y);
    return;
  }

  c->setTextColor(theme::fg(), theme::bg());
  int cy = y;
  for (size_t i = 0; i < n && cy + lineH <= y + h; ++i, cy += lineH) {
    const diagnostics::NetFailure& e = rows[i];
    char ts[12];
    formatStamp(e.utcSec, e.ms, ts, sizeof(ts));
    const char* src = e.source == diagnostics::NetSource::Wifi ? "wifi" : "ble";
    char buf[56];
    std::snprintf(buf, sizeof(buf), "%s %s %s", ts, src, e.msg);
    layout::drawTopLeft(c, buf, x, cy);
  }
}

void LogsScreen::drawPower(LGFX_Sprite* c, int x, int y, int w, int h) {
  diagnostics::PowerEvent rows[diagnostics::PowerEventLog::CAP];
  const size_t n = powerEventLog.snapshot(rows, std::size(rows));

  c->setFont(font::tiny());
  c->setTextSize(1);
  const int lineH = font::metrics(font::tiny()).height + 1;

  if (n == 0) {
    c->setTextColor(theme::dim(), theme::bg());
    layout::drawTopLeft(c, "no power events", x, y);
    return;
  }

  c->setTextColor(theme::fg(), theme::bg());
  int cy = y;
  for (size_t i = 0; i < n && cy + lineH <= y + h; ++i, cy += lineH) {
    const diagnostics::PowerEvent& e = rows[i];
    const bool isWake =
        e.kind == static_cast<uint8_t>(diagnostics::PowerEventKind::Wake);
    char ts[18];
    timefmt::formatIsoDateTime(e.utcSec, ts, sizeof(ts));
    char batt[8];
    if (e.batteryPct < 0) {
      std::snprintf(batt, sizeof(batt), "--%%");
    } else {
      std::snprintf(batt, sizeof(batt), "%d%%", e.batteryPct);
    }
    // Wake rows carry the reset reason; sleep rows don't.
    const char* reason =
        isWake ? diagnostics::resetReasonNames(
                     static_cast<esp_reset_reason_t>(e.resetReason))
                     .shortName
               : "";
    char sleepCfg[12] = {};
    if (!isWake &&
        (e.sleep.flags & diagnostics::PowerSleepPm1I2cConfigValid) != 0) {
      std::snprintf(sleepCfg, sizeof(sleepCfg), " cfg=%02x",
                    e.sleep.pm1I2cConfig);
    }
    char buf[80];
    std::snprintf(
        buf, sizeof(buf), "%s %s %s%s%s%s%s", ts, isWake ? "wake" : "slp", batt,
        e.hasExternalPower ? " ext" : "", isWake ? " " : "", reason, sleepCfg);
    layout::drawTopLeft(c, buf, x, cy);
  }
}

// Simplified heap view for the small screen: the live numbers that matter (free
// internal, largest internal block, since-boot low-water mark, PSRAM free) plus
// the allocation-failure count. The full per-bucket history is left to the web
// screen — there's no room for a trend table here.
void LogsScreen::drawHeap(LGFX_Sprite* c, int x, int y, int w, int h) {
  diagnostics::HeapLive live{};
  heapMonitor.liveStats(live);

  c->setFont(font::tiny());
  c->setTextSize(1);
  const int lineH = font::metrics(font::tiny()).height + 1;

  // Free internal heap (in 100s of bytes precision via 1 decimal KB) is the
  // headline; an allocation failure means we ran the tank dry, so colour the
  // whole page in warning when the counter is non-zero.
  c->setTextColor(live.allocFailCount ? theme::warn() : theme::fg(),
                  theme::bg());

  char buf[48];
  int cy = y;
  auto line = [&](const char* s) {
    if (cy + lineH > y + h) return;
    layout::drawTopLeft(c, s, x, cy);
    cy += lineH;
  };

  std::snprintf(buf, sizeof(buf), "int free   %.1fk",
                live.internalFree / 1024.0f);
  line(buf);
  std::snprintf(buf, sizeof(buf), "int block  %.1fk",
                live.internalLargest / 1024.0f);
  line(buf);
  std::snprintf(buf, sizeof(buf), "int min    %.1fk",
                live.internalMinEver / 1024.0f);
  line(buf);
  std::snprintf(buf, sizeof(buf), "psram free %.0fk", live.psramFree / 1024.0f);
  line(buf);
  std::snprintf(buf, sizeof(buf), "fails %lu",
                static_cast<unsigned long>(live.allocFailCount));
  line(buf);
  if (live.allocFailCount) {
    std::snprintf(buf, sizeof(buf), "last %lub caps0x%lx",
                  static_cast<unsigned long>(live.lastFailSize),
                  static_cast<unsigned long>(live.lastFailCaps));
    line(buf);
  }
}

// Minimal crash summary decoded on demand from the coredump partition: cause /
// task / fault PC / fault address — no backtrace (the full trace is on the web
// Panic tab). A-hold deletes the report.
void LogsScreen::drawPanic(LGFX_Sprite* c, int x, int y, int w, int h) {
  diagnostics::PanicSummary p{};
  const bool present = diagnostics::readLastPanic(p);

  c->setFont(font::tiny());
  c->setTextSize(1);
  const int lineH = font::metrics(font::tiny()).height + 1;

  if (!present) {
    c->setTextColor(theme::dim(), theme::bg());
    layout::drawTopLeft(c, "no crash recorded", x, y);
    return;
  }

  int cy = y;
  auto line = [&](const char* s) {
    if (cy + lineH > y + h) return;
    layout::drawTopLeft(c, s, x, cy);
    cy += lineH;
  };

  char buf[40];
  c->setTextColor(theme::critical(), theme::bg());
  const uint32_t cause = diagnostics::xtensaExcCause(p.excCause);
  const char* name = excCauseName(cause);
  if (name)
    std::snprintf(buf, sizeof(buf), "%s", name);
  else
    std::snprintf(buf, sizeof(buf), "cause %lu",
                  static_cast<unsigned long>(cause));
  line(buf);

  c->setTextColor(theme::fg(), theme::bg());
  if (p.excCause != cause) {
    std::snprintf(buf, sizeof(buf), "raw 0x%lx mask 0x%lx",
                  static_cast<unsigned long>(p.excCause),
                  static_cast<unsigned long>(cause));
    line(buf);
  }
  std::snprintf(buf, sizeof(buf), "task %s", p.task);
  line(buf);
  std::snprintf(buf, sizeof(buf), "pc   0x%08lx",
                static_cast<unsigned long>(p.excPc));
  line(buf);
  std::snprintf(buf, sizeof(buf), "addr 0x%08lx",
                static_cast<unsigned long>(p.excVaddr));
  line(buf);

  c->setTextColor(theme::dim(), theme::bg());
  line("trace: web");
}
