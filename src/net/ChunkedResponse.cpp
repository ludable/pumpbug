// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#include "net/ChunkedResponse.h"

#include <cassert>
#include <cstring>

ChunkedResponse::ChunkedResponse(WebServer& server, int code,
                                 const char* contentType)
    : _server(server) {
  _server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  _server.send(code, contentType, "");
}

ChunkedResponse::~ChunkedResponse() { finish(); }

void ChunkedResponse::write(const char* data) {
  if (!data) return;
  write(data, std::strlen(data));
}

void ChunkedResponse::write(const char* data, size_t len) {
  assert(!_finished && "ChunkedResponse: write after finish()");
  if (!data || len == 0) return;
  _server.sendContent(data, len);
}

void ChunkedResponse::write(const uint8_t* data, size_t len) {
  write(reinterpret_cast<const char*>(data), len);
}

void ChunkedResponse::finish() {
  if (_finished) return;
  // CONTENT_LENGTH_UNKNOWN uses HTTP chunked transfer encoding. The Arduino
  // WebServer does not reliably emit the terminating zero-length chunk for
  // streamed responses on handler return, and browsers may keep fetch/download
  // requests pending without it.
  _server.sendContent("");
  _finished = true;
}
