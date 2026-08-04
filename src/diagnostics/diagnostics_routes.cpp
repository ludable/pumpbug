// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#include "diagnostics/diagnostics_routes.h"

#include <WebServer.h>

#include <cstdint>
#include <cstdio>
#include <string>

#include "apps/extraction/Extraction.h"  // EndCause values, NO_WEIGHT sentinel
#include "ble/BleScaleService.h"         // bleScale.scanSnapshot() for BLE scan
#include "diagnostics/HeapMonitor.h"
#include "diagnostics/PanicDump.h"
#include "diagnostics/PowerEventLog.h"
#include "diagnostics/RuntimeEventLog.h"
#include "net/HttpServer.h"
#include "net/JsonStream.h"
#include "net/WifiManager.h"
#include "util/power.h"  // power::getBatteryStatus() for the live block

namespace {

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
// auto-flushes, so peak memory stays bounded no matter how many entries.

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
        .key("lastPumpOffMs")
        .u(e.lastPumpOffMs)
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

void writePower(JsonStream& j, const power::BatteryStatus& live,
                const diagnostics::PowerEvent* rows, size_t n,
                uint32_t writes) {
  j.open()
      .key("cap")
      .u(diagnostics::PowerEventLog::CAP)
      .key("writes")
      .u(writes)
      .key("live")
      .open()
      .key("batteryPct");
  // Current charge, or null when the reading failed.
  if (live.percent < 0)
    j.null_();
  else
    j.i(live.percent);
  j.key("batteryMv");
  if (live.voltageMv < 0)
    j.null_();
  else
    j.i(live.voltageMv);
  j.key("hasExternalPower")
      .boolean(live.hasExternalPower)
      .key("bluetoothConditioned")
      .boolean(wifiManager.isBluetoothConditioned())
      .close()
      .key("entries")
      .arrayOpen();
  for (size_t k = 0; k < n; ++k) {
    const diagnostics::PowerEvent& e = rows[k];
    const bool wake =
        e.kind == static_cast<uint8_t>(diagnostics::PowerEventKind::Wake);
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
        .close();
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

// Sets the ring's ETag (derived from `writes`) on the response — for both the
// 304 and the upcoming 200 — and, if it matches the client's If-None-Match,
// sends 304 and returns true. `writes` must be the value captured *with* the
// snapshot, so the ETag and body describe the same atomic moment.
bool conditional(WebServer& s, const char* name, uint32_t writes) {
  char etag[40];
  std::snprintf(etag, sizeof(etag), "W/\"%s-%lu\"", name,
                static_cast<unsigned long>(writes));
  if (httpEtagOr304(s, etag)) return true;
  // Miss: httpEtagOr304 sets nothing, so emit the validator for the upcoming
  // 200 here. `writes` is from the snapshot, so ETag and body still match.
  s.sendHeader("ETag", etag);
  return false;
}

// Each serve*() takes one atomic snapshot — entries *and* the write counter
// under the log's own lock — then uses that single `writes` for both the ETag
// and the body. So a clear() (on-device Logs/Erase or a web clear) racing the
// request can never produce a writes/entries mismatch or an ETag that describes
// a different moment than the body.
void serveExtraction(WebServer& s) {
  diagnostics::ExtractionStat
      rows[diagnostics::RuntimeEventLog::EXTRACTION_CAP];
  uint32_t writes = 0;
  const size_t n = runtimeEventLog.snapshotExtraction(
      rows, diagnostics::RuntimeEventLog::EXTRACTION_CAP, &writes);
  if (conditional(s, "extraction", writes)) return;
  JsonStream j(s);
  writeExtraction(j, rows, n, writes);
  j.finish();
}

void serveNet(WebServer& s) {
  diagnostics::NetFailure rows[diagnostics::RuntimeEventLog::NET_CAP];
  uint32_t writes = 0;
  const size_t n = runtimeEventLog.snapshotNet(
      rows, diagnostics::RuntimeEventLog::NET_CAP, &writes);
  if (conditional(s, "net", writes)) return;
  JsonStream j(s);
  writeNet(j, rows, n, writes);
  j.finish();
}

// Skips the ETag/304 path like serveHeap: the live block (current charge,
// plugged-in state) changes between power events, so a writes-based validator
// would keep serving a stale battery until the next wake/sleep. Payload is
// small (<=32 events), so always send 200. The live battery read serializes its
// PMIC transactions on the shared I2C bus, so it is safe on this task.
void servePower(WebServer& s) {
  const power::BatteryStatus live = power::getBatteryStatus();
  diagnostics::PowerEvent rows[diagnostics::PowerEventLog::CAP];
  uint32_t writes = 0;
  const size_t n =
      powerEventLog.snapshot(rows, diagnostics::PowerEventLog::CAP, &writes);
  JsonStream j(s);
  writePower(j, live, rows, n, writes);
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
// screen on the device. ETag'd off the snapshot's monotonic `writes` (bumps per
// record and on arm/disarm), so an idle/disarmed tab is cheap 304s but an
// armed-state flip always transfers.
void serveScaleMsg(WebServer& s) {
  const BleScaleService::MsgLogSnapshot snap = bleScale.messageLogSnapshot();
  if (conditional(s, "scalemsg", snap.writes)) return;
  JsonStream j(s);
  writeScaleMsg(j, snap);
  j.finish();
}

// GET /sys/diagnostics?log=extraction|net|power|heap — returns just
// that ring. (Panic has its own dedicated, unauthenticated route; see below.)
//
// A single ring caps the in-flight body at ~3KB, leaving headroom; the frontend
// fetches only the visible tab. Each log carries its own ETag — its monotonic
// write counter — so tailing one tab is a cheap 304 stream until that log
// advances or is cleared.
void handleLog(WebServer& s) {
  const String log = s.hasArg("log") ? s.arg("log") : String();
  if (log == "extraction") {
    serveExtraction(s);
  } else if (log == "net") {
    serveNet(s);
  } else if (log == "power") {
    servePower(s);
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

// POST /sys/diagnostics/clear?log=extraction|net|power|all — mirrors
// the device's per-page long-press clear. Power is NVS-backed and its clear()
// can fail (NVS open/erase), so we surface that as a 500.
void handleClear(WebServer& s) {
  if (!s.hasArg("log")) {
    s.send(400, "application/json", "{\"error\":\"missing log\"}");
    return;
  }
  const String log = s.arg("log");
  bool ok = true;
  if (log == "extraction") {
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

void registerDiagnosticsRoutes(HttpServer& server) {
  server.registerRoutes(
      "/sys/diagnostics",
      {
          HttpRoute{"", HTTP_GET, [](WebServer& s) { handleLog(s); }},
          HttpRoute{"/clear", HTTP_POST, [](WebServer& s) { handleClear(s); }},
      });
}
