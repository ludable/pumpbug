// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#include "diagnostics/diagnostics_routes.h"

#include <WebServer.h>
#include <esp_system.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "apps/extraction/Extraction.h"  // EndCause values, NO_WEIGHT sentinel
#include "ble/BleScaleService.h"         // bleScale.scanSnapshot() for BLE scan
#include "diagnostics/HeapMonitor.h"
#include "diagnostics/PanicDump.h"
#include "diagnostics/RuntimeEventLog.h"
#include "net/HttpServer.h"
#include "net/JsonStream.h"
#include "net/WifiManager.h"
#include "power/PowerEventLog.h"
#include "util/power.h"  // power::getBatteryStatus() for the live block

namespace {

uint32_t gLogBootNonce = 0;
// Serializing the full power ring produces about 6 KB, while responses should
// stay near 2-3 KB to protect the HTTP task's heap. Page the ring accordingly.
constexpr size_t kPowerPageCap = 12;

// Fields are emitted raw (epoch seconds, uptime ms, numeric enums); labels and
// formatting live in the frontend, so there's no enum-string table duplicated
// from the on-device view.

// A centigram weight, or JSON null for the NO_WEIGHT sentinel.
void weight(JsonStream& j, int16_t cg) {
  if (cg == pump_scale::Extraction::NO_WEIGHT)
    j.null_();
  else
    j.i(cg);
}

// --- Per-ring object writers ------------------------------------------------
// Each writes {"cap":N,"writes":N,"entries":[...]} newest-first. The writer
// auto-flushes its application buffer, while each ring's capacity keeps the
// complete response within JsonStream's lwIP heap budget.

void writePumpDetection(JsonStream& j,
                        const diagnostics::PumpDetectionEvent* rows, size_t n,
                        uint32_t writes) {
  j.open()
      .key("cap")
      .u(diagnostics::RuntimeEventLog::PUMP_DETECTION_CAP)
      .key("writes")
      .u(writes)
      .key("entries")
      .arrayOpen();
  for (size_t k = 0; k < n; ++k) {
    const diagnostics::PumpDetectionEvent& e = rows[k];
    if (k) j.comma();
    j.open()
        .key("ms")
        .u(e.ms)
        .key("utcSec")
        .u(e.utcSec)
        .key("event")
        .str(e.kind == diagnostics::PumpDetectionEventKind::On ? "on" : "off")
        .key("detectedForMs")
        .u(e.detectedForMs)
        .key("rawSnrDb")
        .f(e.rawSnrDb)
        .key("smoothedSnrDb")
        .f(e.smoothedSnrDb)
        .key("peakHz")
        .f(e.peakHz)
        .key("dominantPeakHz")
        .f(e.dominantPeakHz)
        .key("spectralFlux")
        .f(e.spectralFlux)
        .key("stationary")
        .boolean(e.stationary)
        .key("closeFailureMask")
        .u(e.closeFailureMask)
        .close();
  }
  j.arrayClose().close();
}

void writeExtraction(JsonStream& j, const diagnostics::ExtractionStat* rows,
                     size_t n, uint32_t writes) {
  j.open()
      .key("cap")
      .u(diagnostics::RuntimeEventLog::EXTRACTION_CAP)
      .key("writes")
      .u(writes)
      .key("entries")
      .arrayOpen();
  for (size_t k = 0; k < n; ++k) {
    const diagnostics::ExtractionStat& e = rows[k];
    if (k) j.comma();
    j.open()
        .key("startUtcSec")
        .u(e.startUtcSec)
        .key("beginMs")
        .u(e.beginMs)
        .key("lastPumpOffConfirmedMs")
        .u(e.lastPumpOffConfirmedMs)
        .key("stableMs")
        .u(e.stableMs)
        .key("endMs")
        .u(e.endMs)
        .key("totalPumpOnMs")
        .u(e.totalPumpOnMs)
        .key("yieldCg");
    weight(j, e.yieldCg);
    j.key("startRawCg");
    weight(j, e.startRawCg);
    j.key("settledRawCg");
    weight(j, e.settledRawCg);
    j.key("pourMs")
        .u(e.pourMs)
        .key("decisionGainCg")
        .i(e.decisionGainCg)
        .key("observedSampleCount")
        .u(e.observedSampleCount)
        .key("endCause")
        .u(e.endCause)
        .key("yieldStatus")
        .u(e.yieldStatus)
        .key("isLikelyRealShot")
        .boolean(e.isLikelyRealShot)
        .close();
  }
  j.arrayClose().close();
}

void writeNet(JsonStream& j, const diagnostics::NetFailure* rows, size_t n,
              uint32_t writes) {
  j.open()
      .key("cap")
      .u(diagnostics::RuntimeEventLog::NET_CAP)
      .key("writes")
      .u(writes)
      .key("entries")
      .arrayOpen();
  for (size_t k = 0; k < n; ++k) {
    const diagnostics::NetFailure& e = rows[k];
    if (k) j.comma();
    j.open()
        .key("ms")
        .u(e.ms)
        .key("utcSec")
        .u(e.utcSec)
        .key("source")
        .str(e.source == diagnostics::NetSource::Wifi ? "wifi" : "ble")
        .key("code")
        .u(e.code)
        .key("msg")
        .str(e.msg)
        .close();
  }
  j.arrayClose().close();
}

void writePower(JsonStream& j, const power::BatteryStatus* live,
                const power::PowerEvent* rows, size_t n, uint32_t writes,
                size_t total, size_t offset) {
  const size_t nextOffset = offset + n;
  j.open()
      .key("cap")
      .u(power::PowerEventLog::CAP)
      .key("writes")
      .u(writes)
      .key("total")
      .u(total)
      .key("offset")
      .u(offset)
      .key("nextOffset");
  if (nextOffset < total)
    j.u(nextOffset);
  else
    j.null_();
  if (live) {
    j.key("live").open().key("batteryPct");
    // Current charge, or null when the reading failed.
    if (live->percent < 0)
      j.null_();
    else
      j.i(live->percent);
    j.key("batteryMv");
    if (live->voltageMv < 0)
      j.null_();
    else
      j.i(live->voltageMv);
    j.key("hasExternalPower")
        .boolean(live->hasExternalPower)
        .key("bluetoothConditioned")
        .boolean(wifiManager.isBluetoothConditioned())
        .close();
  }
  j.key("entries").arrayOpen();
  for (size_t k = 0; k < n; ++k) {
    const power::PowerEvent& e = rows[k];
    const bool wake =
        e.kind == static_cast<uint8_t>(power::PowerEventKind::Wake);
    if (k) j.comma();
    j.open()
        .key("utcSec")
        .u(e.utcSec)
        .key("kind")
        .str(wake ? "wake" : "sleep")
        .key("batteryPct");
    if (e.batteryPct < 0)
      j.null_();
    else
      j.i(e.batteryPct);
    // resetReason is only meaningful on wake rows (raw esp_reset_reason()).
    j.key("hasExternalPower")
        .boolean(e.hasExternalPower)
        .key("resetReason")
        .u(wake ? e.resetReason : 0)
        .key("bootFlags")
        .u(wake ? e.boot.flags : 0)
        .key("pm1WakeSource");
    if (wake && (e.boot.flags & power::PowerBootWakeSourceValid) != 0)
      j.u(e.boot.pm1WakeSource);
    else
      j.null_();
    j.key("pm1GpioIrq");
    if (wake && (e.boot.flags & power::PowerBootGpioIrqValid) != 0)
      j.u(e.boot.pm1GpioIrq);
    else
      j.null_();
    j.key("pm1BootI2cConfig");
    if (wake && (e.boot.flags & power::PowerBootI2cConfigRead) != 0)
      j.u(e.boot.pm1I2cConfig);
    else
      j.null_();
    j.key("pm1I2cConfig");
    if (!wake && (e.sleep.flags & power::PowerSleepPm1I2cConfigValid) != 0)
      j.u(e.sleep.pm1I2cConfig);
    else
      j.null_();
    j.close();
  }
  j.arrayClose().close();
}

// Heap is shaped differently from the rings: a "live" object (current allocator
// state + since-boot low-water mark and alloc-fail counters) alongside the
// history `entries` (per-bucket worst case). The live block is what you read
// for "how close to the edge right now"; the entries are the trend.
void writeHeap(JsonStream& j, const diagnostics::HeapLive& live,
               const diagnostics::HeapSample* rows, size_t n, uint32_t writes) {
  j.open()
      .key("cap")
      .u(diagnostics::HeapMonitor::CAP)
      .key("bucketMs")
      .u(diagnostics::HeapMonitor::BUCKET_MS)
      .key("writes")
      .u(writes)
      .key("live")
      .open()
      .key("internalFree")
      .u(live.internalFree)
      .key("internalLargest")
      .u(live.internalLargest)
      .key("internalMinEver")
      .u(live.internalMinEver)
      .key("dmaLargest")
      .u(live.dmaLargest)
      .key("psramFree")
      .u(live.psramFree)
      .key("psramLargest")
      .u(live.psramLargest)
      .key("allocFailCount")
      .u(live.allocFailCount)
      .key("lastFailSize")
      .u(live.lastFailSize)
      .key("lastFailCaps")
      .u(live.lastFailCaps)
      .key("lastFailTask")
      .str(live.lastFailTask)
      .key("lastFailFn")
      .str(live.lastFailFn)
      .close()
      .key("entries")
      .arrayOpen();
  for (size_t k = 0; k < n; ++k) {
    const diagnostics::HeapSample& e = rows[k];
    if (k) j.comma();
    j.open()
        .key("ms")
        .u(e.ms)
        .key("utcSec")
        .u(e.utcSec)
        .key("intFree")
        .u(e.minInternalFree)
        .key("intLargest")
        .u(e.minInternalLargest)
        .key("dmaLargest")
        .u(e.minDmaLargest)
        .key("psramFree")
        .u(e.minPsramFree)
        .close();
  }
  j.arrayClose().close();
}

// BLE scan: the DiagScan device table plus scan status. `busy` means the
// application holds the radio (e.g. an extraction is connected), so the scan
// is suppressed; `active` means a scan is actually running. Each device's
// interval estimate is null until enough adverts have been seen.
void writeBleScan(JsonStream& j, const BleScaleService::ScanResults& r) {
  j.open()
      .key("active")
      .boolean(r.active)
      .key("busy")
      .boolean(r.busy)
      .key("devices")
      .arrayOpen();
  for (uint8_t k = 0; k < r.count; ++k) {
    const BleScaleService::ScanEntry& e = r.entries[k];
    if (k) j.comma();
    char addr[18];
    std::snprintf(addr, sizeof(addr), "%02X:%02X:%02X:%02X:%02X:%02X",
                  e.addr[0], e.addr[1], e.addr[2], e.addr[3], e.addr[4],
                  e.addr[5]);
    j.open()
        .key("addr")
        .str(addr)
        .key("name")
        .str(e.name[0] ? e.name : nullptr)
        .key("rssi")
        .i(e.rssi)
        .key("recognized")
        .boolean(e.recognized)
        .key("intervalMs");
    // Only trustworthy once a few plausible interval samples have accumulated.
    if (e.samples >= 4 && e.minIntervalMs > 0)
      j.u(e.minIntervalMs);
    else
      j.null_();
    j.close();
  }
  j.arrayClose().close();
}

// Scale message log: the diagnostic capture tap's snapshot. `armed` is whether
// the on-device firmware is capturing (diagnostics capture screen is
// foreground); `counts` is indexed by MsgTag; `records` is newest-first, each
// with a numeric `tag` (labels live in the web client), space-separated `hex`
// of the first rawLen bytes, and `truncated` when the on-air packet was longer
// than what we kept.
void writeScaleMsg(JsonStream& j, const BleScaleService::MsgLogSnapshot& s) {
  j.open().key("armed").boolean(s.armed).key("counts").arrayOpen();
  for (int i = 0; i < BleScaleService::MSG_TAG_COUNT; ++i) {
    if (i) j.comma();
    j.u(s.byTag[i]);
  }
  j.arrayClose().key("records").arrayOpen();
  for (uint8_t k = 0; k < s.count; ++k) {
    const BleScaleService::MsgRecord& r = s.records[k];
    if (k) j.comma();
    char hex[3 * sizeof(r.raw) + 1];
    size_t pos = 0;
    for (uint8_t b = 0; b < r.rawLen && pos + 4 < sizeof(hex); ++b) {
      pos += std::snprintf(hex + pos, sizeof(hex) - pos, b ? " %02X" : "%02X",
                           r.raw[b]);
    }
    hex[pos] = '\0';
    j.open()
        .key("ms")
        .u(r.ms)
        .key("dir")
        .u(r.dir)
        .key("tag")
        .u(static_cast<uint32_t>(r.tag))
        .key("hex")
        .str(hex)
        .key("truncated")
        .boolean(r.wireLen > r.rawLen)
        .close();
  }
  j.arrayClose().close();
}

// --- Handlers ---------------------------------------------------------------

// The boot nonce prevents a browser that remains open across a reboot from
// reusing a validator for new in-memory contents. `changeToken` must identify
// the corresponding snapshot and change.
bool conditional(WebServer& s, const char* name, uint32_t changeToken) {
  char etag[48];
  std::snprintf(etag, sizeof(etag), "W/\"%s-%08lx-%lu\"", name,
                static_cast<unsigned long>(gLogBootNonce),
                static_cast<unsigned long>(changeToken));
  if (httpEtagOr304(s, etag)) return true;
  s.sendHeader("ETag", etag);
  return false;
}

// Each serve*() takes one atomic snapshot of the entries and counters,
// preventing a concurrent log-clearing request from pairing the same ETag with
// different contents.
void serveExtraction(WebServer& s) {
  diagnostics::ExtractionStat
      rows[diagnostics::RuntimeEventLog::EXTRACTION_CAP];
  diagnostics::RuntimeEventLog::SnapshotState state{};
  const size_t n = runtimeEventLog.snapshotExtraction(
      rows, diagnostics::RuntimeEventLog::EXTRACTION_CAP, &state);
  if (conditional(s, "extraction", state.revision)) return;
  JsonStream j(s);
  writeExtraction(j, rows, n, state.writes);
  j.finish();
}

void servePumpDetection(WebServer& s) {
  diagnostics::PumpDetectionEvent
      rows[diagnostics::RuntimeEventLog::PUMP_DETECTION_CAP];
  diagnostics::RuntimeEventLog::SnapshotState state{};
  const size_t n = runtimeEventLog.snapshotPumpDetection(
      rows, diagnostics::RuntimeEventLog::PUMP_DETECTION_CAP, &state);
  if (conditional(s, "pump", state.revision)) return;
  JsonStream j(s);
  writePumpDetection(j, rows, n, state.writes);
  j.finish();
}

void serveNet(WebServer& s) {
  diagnostics::NetFailure rows[diagnostics::RuntimeEventLog::NET_CAP];
  diagnostics::RuntimeEventLog::SnapshotState state{};
  const size_t n = runtimeEventLog.snapshotNet(
      rows, diagnostics::RuntimeEventLog::NET_CAP, &state);
  if (conditional(s, "net", state.revision)) return;
  JsonStream j(s);
  writeNet(j, rows, n, state.writes);
  j.finish();
}

bool parsePowerOffset(WebServer& s, size_t& out) {
  out = 0;
  if (!s.hasArg("offset")) return true;
  const String raw = s.arg("offset");
  if (raw.isEmpty()) return false;
  char* end = nullptr;
  errno = 0;
  const unsigned long value = std::strtoul(raw.c_str(), &end, 10);
  if (errno != 0 || end == raw.c_str() || *end != '\0' ||
      value > power::PowerEventLog::CAP) {
    return false;
  }
  out = static_cast<size_t>(value);
  return true;
}

// Skips the ETag/304 path like serveHeap: the live block (current charge,
// plugged-in state) changes between power events, so a writes-based validator
// would keep serving a stale battery until the next wake/sleep. History is
// paged so each response remains within JsonStream's lwIP heap budget.
void servePower(WebServer& s, power::PowerEventLog& powerEventLog) {
  size_t offset = 0;
  if (!parsePowerOffset(s, offset)) {
    s.send(400, "application/json", "{\"error\":\"bad offset\"}");
    return;
  }
  power::BatteryStatus live{};
  const power::BatteryStatus* livePage = nullptr;
  if (offset == 0) {
    // Live values belong to the first page so later pages do not repeat shared
    // I2C bus reads while assembling one history response.
    live = power::getBatteryStatus();
    livePage = &live;
  }
  power::PowerEvent rows[power::PowerEventLog::CAP];
  uint32_t writes = 0;
  const size_t total =
      powerEventLog.snapshot(rows, power::PowerEventLog::CAP, &writes);
  if (offset > total) offset = total;
  const size_t n = std::min(kPowerPageCap, total - offset);
  JsonStream j(s);
  writePower(j, livePage, rows + offset, n, writes, total, offset);
  j.finish();
}

// Heap deliberately skips the ETag/304 path. Its ETag would key off the bucket
// `writes` counter, which only advances once per BUCKET_MS — but the live block
// (current free, alloc-fail count) moves continuously, and a dashboard wants
// that fresh on every poll. The payload is small, so always send 200.
void serveHeap(WebServer& s) {
  diagnostics::HeapLive live{};
  heapMonitor.liveStats(live);
  diagnostics::HeapSample rows[diagnostics::HeapMonitor::CAP];
  uint32_t writes = 0;
  const size_t n =
      heapMonitor.snapshot(rows, diagnostics::HeapMonitor::CAP, &writes);
  JsonStream j(s);
  writeHeap(j, live, rows, n, writes);
  j.finish();
}

// Returns a read-only mirror of the BLE scan table and status. The on-device
// BLE-scan diagnostic screen controls the radio; the web client tells the user
// to open that screen when scanning is inactive. The table changes
// continuously, so this response has no ETag.
void serveBleScan(WebServer& s) {
  const BleScaleService::ScanResults r = bleScale.scanSnapshot();
  JsonStream j(s);
  writeBleScan(j, r);
  j.finish();
}

// Scale message log: read-only mirror, driven by the scale message diagnostic
// screen on the device. Its snapshot write count changes for each record and
// when capture starts or stops.
void serveScaleMsg(WebServer& s) {
  const BleScaleService::MsgLogSnapshot snap = bleScale.messageLogSnapshot();
  if (conditional(s, "scalemsg", snap.writes)) return;
  JsonStream j(s);
  writeScaleMsg(j, snap);
  j.finish();
}

// GET /sys/diagnostics?log=<name> returns one runtime log. Power accepts an
// optional history offset; panic has its own unauthenticated route below.
//
// The frontend fetches only the visible tab. Pump, extraction, network, and
// scale-message responses carry ETags, so polling an unchanged log returns a
// small 304 response.
void handleLog(WebServer& s, power::PowerEventLog& powerEventLog) {
  const String log = s.hasArg("log") ? s.arg("log") : String();
  if (log == "pump") {
    servePumpDetection(s);
  } else if (log == "extraction") {
    serveExtraction(s);
  } else if (log == "net") {
    serveNet(s);
  } else if (log == "power") {
    servePower(s, powerEventLog);
  } else if (log == "heap") {
    serveHeap(s);
  } else if (log == "blescan") {
    serveBleScan(s);
  } else if (log == "scalemsg") {
    serveScaleMsg(s);
  } else {
    s.send(400, "application/json", "{\"error\":\"missing or unknown log\"}");
  }
}

// POST /sys/diagnostics/clear?log=pump|extraction|net|power|all — mirrors
// the device's per-page long-press clear. Power is NVS-backed and its clear()
// can fail (NVS open/erase), so we surface that as a 500.
void handleClear(WebServer& s, power::PowerEventLog& powerEventLog) {
  if (!s.hasArg("log")) {
    s.send(400, "application/json", "{\"error\":\"missing log\"}");
    return;
  }
  const String log = s.arg("log");
  bool ok = true;
  if (log == "pump") {
    runtimeEventLog.clearPumpDetection();
  } else if (log == "extraction") {
    runtimeEventLog.clearExtraction();
  } else if (log == "net") {
    runtimeEventLog.clearNet();
  } else if (log == "power") {
    ok = powerEventLog.clear();
  } else if (log == "heap") {
    heapMonitor.clearHistory();
  } else if (log == "panic") {
    ok = diagnostics::clearLastPanicAndResync();
  } else if (log == "all") {
    runtimeEventLog.clearPumpDetection();
    runtimeEventLog.clearExtraction();
    runtimeEventLog.clearNet();
    ok = powerEventLog.clear();
    heapMonitor.clearHistory();
    ok = diagnostics::clearLastPanicAndResync() && ok;
  } else {
    s.send(400, "application/json", "{\"error\":\"unknown log\"}");
    return;
  }
  if (!ok) {
    s.send(500, "application/json", "{\"error\":\"clear failed\"}");
    return;
  }
  std::string body = "{\"cleared\":\"";
  json::escapeAppend(body, log.c_str());
  body += "\"}";
  s.send(200, "application/json", body.c_str());
}

}  // namespace

void registerDiagnosticsRoutes(HttpServer& server,
                               power::PowerEventLog& powerEventLog) {
  gLogBootNonce = esp_random();
  server.registerRoutes("/sys/diagnostics",
                        {
                            HttpRoute{"", HTTP_GET,
                                      [&powerEventLog](WebServer& s) {
                                        handleLog(s, powerEventLog);
                                      }},
                            HttpRoute{"/clear", HTTP_POST,
                                      [&powerEventLog](WebServer& s) {
                                        handleClear(s, powerEventLog);
                                      }},
                        });
}
