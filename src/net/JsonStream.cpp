// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#include "net/JsonStream.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace json {

void escapeAppend(std::string& out, const char* s) {
  for (; s && *s; ++s) {
    const unsigned char c = static_cast<unsigned char>(*s);
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\b':
        out += "\\b";
        break;
      case '\f':
        out += "\\f";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", c);
          out += buf;
        } else {
          out += static_cast<char>(c);
        }
    }
  }
}

}  // namespace json

JsonStream::JsonStream(WebServer& server, size_t flushAt)
    : _response(server, 200, "application/json"), _flushAt(flushAt) {
  // One entry can push the buffer a little past the high-water mark before the
  // flush check; the headroom keeps that from forcing a reallocation.
  _buf.reserve(flushAt + 256);
}

JsonStream::~JsonStream() { finish(); }

void JsonStream::finish() {
  if (_finished) return;
  flush();
  _response.finish();
  _finished = true;
}

void JsonStream::flush() {
  if (!_buf.empty()) {
    _response.write(_buf.c_str(), _buf.size());
    _buf.clear();  // keeps capacity
  }
}

void JsonStream::flushIfFull() {
  if (_buf.size() >= _flushAt) flush();
}

JsonStream& JsonStream::raw(const char* s) { return raw(s, std::strlen(s)); }

JsonStream& JsonStream::raw(const char* s, size_t n) {
  assert(!_finished && "JsonStream: write after finish()");
  _buf.append(s, n);
  flushIfFull();
  return *this;
}

JsonStream& JsonStream::open() {
  raw("{", 1);
  assert(_objectDepth < sizeof(_objectNeedComma));
  if (_objectDepth < sizeof(_objectNeedComma)) {
    _objectNeedComma[_objectDepth++] = false;
  }
  return *this;
}

JsonStream& JsonStream::close() {
  assert(_objectDepth > 0 && "JsonStream: object close without open");
  if (_objectDepth > 0) --_objectDepth;
  return raw("}", 1);
}

JsonStream& JsonStream::key(const char* k) {
  assert(!_finished && "JsonStream: write after finish()");
  if (_objectDepth > 0) {
    bool& needComma = _objectNeedComma[_objectDepth - 1];
    if (needComma) _buf += ',';
    needComma = true;
  }
  _buf += '"';
  json::escapeAppend(_buf, k);
  _buf += "\":";
  flushIfFull();
  return *this;
}

JsonStream& JsonStream::str(const char* s) {
  assert(!_finished && "JsonStream: write after finish()");
  if (!s) {
    _buf += "null";
  } else {
    _buf += '"';
    json::escapeAppend(_buf, s);
    _buf += '"';
  }
  flushIfFull();
  return *this;
}

JsonStream& JsonStream::u(uint32_t v) {
  char b[12];
  const int n =
      std::snprintf(b, sizeof(b), "%lu", static_cast<unsigned long>(v));
  return raw(b, static_cast<size_t>(n));
}

JsonStream& JsonStream::i(int v) {
  char b[12];
  const int n = std::snprintf(b, sizeof(b), "%d", v);
  return raw(b, static_cast<size_t>(n));
}

JsonStream& JsonStream::f(float v) {
  if (!std::isfinite(v)) return raw("null", 4);
  char b[24];
  const int n = std::snprintf(b, sizeof(b), "%.2f", static_cast<double>(v));
  return raw(b, static_cast<size_t>(n));
}
