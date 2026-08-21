// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#include "ShotHistoryRoutes.h"

#include <WebServer.h>

#include <cstdio>
#include <cstdlib>
#include <vector>

#include "net/HttpServer.h"
#include "net/JsonStream.h"
#include "util/storage.h"

namespace pump_scale {

namespace {

bool parseShotId(const String& s, uint32_t& out) {
  char* endp = nullptr;
  const unsigned long parsed = std::strtoul(s.c_str(), &endp, 10);
  if (endp == s.c_str() || *endp != '\0' || parsed == 0) return false;
  out = static_cast<uint32_t>(parsed);
  return true;
}

bool requireShotStorage(WebServer& server) {
  if (storage::mountState() == storage::MountState::Ready) return true;
  // An unavailable volume is neither empty history nor a missing record, so
  // report the recovery condition before emitting an ETag or reading a file.
  server.send(503, "application/json",
              "{\"error\":\"shot history unavailable\"}");
  return false;
}

void handleShotDetail(WebServer& server, ShotStore& store) {
  uint32_t id = 0;
  if (!parseShotId(server.arg("id"), id)) {
    server.send(400, "application/json", "{\"error\":\"bad id\"}");
    return;
  }
  if (!requireShotStorage(server)) return;

  // IDs are stable within one storage lifetime. The boot nonce prevents a
  // browser from reusing shot #N after the on-device erase resets the NVS id
  // counter and a later shot receives the same number.
  char detailEtag[40];
  std::snprintf(detailEtag, sizeof(detailEtag), "\"shot-%08lx-%lu\"",
                static_cast<unsigned long>(store.bootNonce()),
                static_cast<unsigned long>(id));
  if (httpEtagOr304(server, detailEtag)) return;

  ShotBinary record;
  if (!store.readBinary(id, record)) {
    server.send(404, "application/json", "{\"error\":\"not found\"}");
    return;
  }

  server.sendHeader("ETag", detailEtag);
  server.sendHeader("Cache-Control", "private, no-cache");
  server.setContentLength(record.size);
  server.send(200, "application/octet-stream", "");
  server.sendContent(reinterpret_cast<const char*>(record.data.get()),
                     record.size);
}

void writeShotRow(JsonStream& j, const ShotMeta& m) {
  j.open()
      .key("id")
      .u(m.id)
      .key("startUtcSec")
      .u(m.startUtcSec)
      .key("beginMs")
      .u(m.beginMs)
      .key("durationMs")
      .u(m.durationMs)
      .key("yieldCg")
      .i(m.yieldCg)
      .key("totalPumpOnMs")
      .u(m.totalPumpOnMs)
      .close();
}

void handleShotList(WebServer& server, ShotStore& store) {
  if (!requireShotStorage(server)) return;

  // List: a newest-first page, re-derived from the directory each request (no
  // cached model). The ETag (per-boot nonce + change counter) lets an unchanged
  // in-app revalidation return 304 with no directory walk; a full page reload
  // simply loses the client's in-memory validator and pays one bounded page().
  const uint32_t nonce = store.bootNonce();
  char etag[48];
  std::snprintf(etag, sizeof(etag), "W/\"shots-%08lx-%lu\"",
                static_cast<unsigned long>(nonce),
                static_cast<unsigned long>(store.revision()));
  if (httpEtagOr304(server, etag)) return;

  // Cursor + page size. before=0 -> newest page; pass the lowest id returned to
  // fetch the next (older) page. Absent `limit` -> all, so a pre-pagination
  // client on a stale cached bundle still gets its whole history.
  uint32_t before = 0;
  if (server.hasArg("before")) {
    before = static_cast<uint32_t>(
        std::strtoul(server.arg("before").c_str(), nullptr, 10));
  }
  size_t limit = static_cast<size_t>(-1);
  if (server.hasArg("limit")) {
    const unsigned long l =
        std::strtoul(server.arg("limit").c_str(), nullptr, 10);
    limit = l < 1 ? 1 : (l > 100 ? 100 : static_cast<size_t>(l));
  }

  bool hasMore = false;
  size_t total = 0;
  uint32_t rev = 0;
  const std::vector<uint32_t> ids =
      store.page(before, limit, &hasMore, &total, &rev);

  // Stamp the 200's ETag from the page snapshot's revision, so validator and
  // body describe the same moment.
  std::snprintf(etag, sizeof(etag), "W/\"shots-%08lx-%lu\"",
                static_cast<unsigned long>(nonce),
                static_cast<unsigned long>(rev));
  server.sendHeader("ETag", etag);

  // Stream the page with JsonStream (chunked, auto-flushing), reading each
  // header on demand so the body never buffers all rows at once. The ETag
  // header above is set before constructing JsonStream (its ctor emits 200).
  JsonStream j(server);
  j.open().key("shots").arrayOpen();
  bool first = true;
  ShotMeta meta;
  for (const uint32_t id : ids) {
    if (!store.readMeta(id, meta)) continue;  // evicted between snapshot & read
    if (!first) j.comma();
    first = false;
    writeShotRow(j, meta);
  }
  j.arrayClose()
      .key("hasMore")
      .boolean(hasMore)
      .key("total")
      .u(static_cast<uint32_t>(total))
      .close();
  j.finish();
}

}  // namespace

void handleShotHistory(WebServer& server, ShotStore& store) {
  if (server.hasArg("id")) {
    handleShotDetail(server, store);
  } else {
    handleShotList(server, store);
  }
}

}  // namespace pump_scale
