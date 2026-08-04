// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <WebServer.h>

#include <cstddef>
#include <cstdint>
#include <string>

#include "net/ChunkedResponse.h"

namespace json {
// Append `s` to `out` as the contents of a JSON string (no surrounding quotes),
// escaping ", \, the C0 control characters, and the short escapes. For callers
// that build a small body in a std::string and send it in one shot; JsonStream
// uses this internally for its quoted forms.
void escapeAppend(std::string& out, const char* s);
}  // namespace json

// Streams a JSON body to a WebServer over chunked transfer-encoding, through a
// bounded buffer that auto-flushes once it fills.  It avoids building the whole
// body in a std::string and then having send() copy it again — that
// application-side double-buffer peaks near 2x the body size. The trade-off is
// no Content-Length header so only chunked transfer encoding.
//
// What it does NOT buy: it does not make a large response safe. The bytes still
// pile up in the lwIP TCP send buffer (also internal heap) until the client
// ACKs, so peak heap tracks the *total response size* regardless of how small
// the flush chunks are. Keep each response to ~2-3KB; a ~8KB body still drops
// free heap to near zero mid-send and wedges under concurrent requests.
//
// Usage — construct, write, let it fall out of scope:
//
//   server.sendHeader("ETag", etag);   // any extra headers BEFORE constructing
//   JsonStream j(server);              // emits the chunked 200
//   j.open().key("n").u(count).close();
//   // dtor flushes the tail; ChunkedResponse emits the terminating chunk.
//
// Object comma placement is handled by key(). Arrays still use explicit
// comma() between values.
class JsonStream {
 public:
  // Begins a chunked 200 application/json response on `server`. Set any extra
  // response headers before constructing (the ctor sends the head). `flushAt`
  // is the buffer high-water mark in bytes.
  explicit JsonStream(WebServer& server, size_t flushAt = 1024);
  ~JsonStream();  // calls finish() if not already done

  JsonStream(const JsonStream&) = delete;
  JsonStream& operator=(const JsonStream&) = delete;

  // Complete the response by flushing the buffered tail. Idempotent, and the
  // destructor calls it automatically, so it's only needed to mark completion
  // explicitly or to finish before doing more work in the same scope. Writing
  // after finish() is a usage error.
  void finish();

  // Verbatim — structural punctuation and pre-formatted literals.
  JsonStream& raw(const char* s);
  JsonStream& raw(const char* s, size_t n);

  // Structural sugar.
  JsonStream& open();
  JsonStream& close();
  JsonStream& arrayOpen() { return raw("[", 1); }
  JsonStream& arrayClose() { return raw("]", 1); }
  JsonStream& comma() { return raw(",", 1); }

  // `"k":` — escaped key plus colon.
  JsonStream& key(const char* k);

  // Values.
  JsonStream& str(const char* s);  // escaped, quoted; `null` when s == nullptr
  JsonStream& u(uint32_t v);
  JsonStream& i(int v);
  JsonStream& f(float v);  // two decimals, or `null` when not finite
  JsonStream& boolean(bool v) { return raw(v ? "true" : "false"); }
  JsonStream& null_() { return raw("null", 4); }

 private:
  void flushIfFull();
  void flush();

  ChunkedResponse _response;
  std::string _buf;
  size_t _flushAt;
  bool _finished = false;
  bool _objectNeedComma[8] = {};
  uint8_t _objectDepth = 0;
};
