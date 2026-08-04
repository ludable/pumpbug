// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <WebServer.h>

#include <cstddef>
#include <cstdint>

// Owns one HTTP response that uses chunked transfer-encoding.
//
// Set any extra headers on `server` before constructing; the constructor emits
// the status line and content type. finish(), also called by the destructor,
// sends the terminating zero-length chunk. This is intentionally narrow: use
// fixed Content-Length responses when the length is known.
class ChunkedResponse {
 public:
  ChunkedResponse(WebServer& server, int code, const char* contentType);
  ~ChunkedResponse();

  ChunkedResponse(const ChunkedResponse&) = delete;
  ChunkedResponse& operator=(const ChunkedResponse&) = delete;

  void write(const char* data);
  void write(const char* data, size_t len);
  void write(const uint8_t* data, size_t len);

  void finish();

 private:
  WebServer& _server;
  bool _finished = false;
};
