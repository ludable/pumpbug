// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#include "AcaiaV2Codec.h"

#include <cstring>

namespace AcaiaV2 {

const char* SERVICE_UUID = "49535343-fe7d-4ae5-8fa9-9fafd205e455";
const char* CHAR_WRITE_UUID = "49535343-8841-43f4-a8d4-ecbe34729bb3";
const char* CHAR_NOTIFY_UUID = "49535343-1e4d-4bd9-ba61-23c647249616";
const char* LEGACY_SERVICE_UUID = "00001820-0000-1000-8000-00805f9b34fb";
const char* LEGACY_CHAR_UUID = "00002a80-0000-1000-8000-00805f9b34fb";

namespace {

constexpr uint8_t HDR1 = 0xEF;
constexpr uint8_t HDR2 = 0xDD;

namespace Msg {
constexpr uint8_t SYSTEM = 0x00;  // heartbeat lives here
constexpr uint8_t TARE = 0x04;
constexpr uint8_t TIMER = 0x07;
constexpr uint8_t SETTINGS = 0x08;
constexpr uint8_t IDENTIFY = 0x0B;
constexpr uint8_t EVENT = 0x0C;  // both subscribe (TX) and broadcast (RX)
}  // namespace Msg

void checksum(const uint8_t* data, size_t len, uint8_t* even, uint8_t* odd) {
  uint8_t ce = 0, co = 0;
  for (size_t i = 0; i < len; ++i) {
    if ((i & 1) == 0)
      ce = static_cast<uint8_t>(ce + data[i]);
    else
      co = static_cast<uint8_t>(co + data[i]);
  }
  if (even) *even = ce;
  if (odd) *odd = co;
}

// Header + msg type + body + even/odd body-byte checksum. The first body byte
// is the Acaia block length and counts itself.
size_t pack(uint8_t* out, uint8_t msgType, const uint8_t* payload,
            size_t plen) {
  out[0] = HDR1;
  out[1] = HDR2;
  out[2] = msgType;
  for (size_t i = 0; i < plen; ++i) {
    out[3 + i] = payload[i];
  }
  uint8_t ce = 0, co = 0;
  checksum(payload, plen, &ce, &co);
  out[3 + plen] = ce;
  out[3 + plen + 1] = co;
  return 3 + plen + 2;
}

bool parseWireFrame(const uint8_t* data, size_t len, Frame* frame) {
  if (len < 6) return false;
  if (data[0] != HDR1 || data[1] != HDR2) return false;

  uint8_t ce = 0, co = 0;
  const size_t bodyLen = data[3];
  if (bodyLen == 0 || bodyLen > MAX_FRAME - 5) return false;
  const size_t wireLen = bodyLen + 5;
  if (len != wireLen) return false;

  const uint8_t* body = data + 3;
  checksum(body, bodyLen, &ce, &co);
  if (ce != data[wireLen - 2] || co != data[wireLen - 1]) return false;

  if (frame) {
    frame->type = data[2];
    frame->body = body;
    frame->bodyLen = bodyLen;
    frame->payload = body + 1;
    frame->payloadLen = bodyLen - 1;
    frame->wire = data;
    frame->wireLen = wireLen;
  }
  return true;
}

InboundKind eventKind(uint8_t eventTypeId) {
  switch (eventTypeId) {
    case 0x05:
      return InboundKind::Weight;
    case 0x06:
      return InboundKind::Battery;
    case 0x07:
      return InboundKind::Timer;
    case 0x08:
      return InboundKind::Key;
    default:
      return InboundKind::Unknown;
  }
}

InboundKind heartbeatResponseKind(const uint8_t* payload, size_t len) {
  // Legacy scales can surface weight/timer samples inside an event type 0x0B
  // heartbeat response. The full EVENT payload includes the outer 0x0B first,
  // so payload[3] is the nested sample kind. Only classify as a sample once
  // the nested body is long enough for the decoder to emit that sample.
  if (len < 4) return InboundKind::Unknown;
  switch (payload[3]) {
    case 0x05:
      return len >= 10 ? InboundKind::Weight : InboundKind::Unknown;
    case 0x07:
      return len >= 7 ? InboundKind::Timer : InboundKind::Unknown;
    default:
      return InboundKind::Unknown;
  }
}

bool decodeTimerMs(const uint8_t* d, size_t dn, uint32_t* out) {
  if (dn < 3) return false;
  // Acaia timer event payload: [minutes, seconds, deciseconds]. Store as a
  // millisecond value for easier use; precision is still 100 ms.
  if (out) {
    *out = static_cast<uint32_t>(d[0]) * 60000u +
           static_cast<uint32_t>(d[1]) * 1000u +
           static_cast<uint32_t>(d[2]) * 100u;
  }
  return true;
}

}  // namespace

size_t buildHeartbeat(uint8_t* out) {
  static const uint8_t p[] = {0x02, 0x00};
  return pack(out, Msg::SYSTEM, p, sizeof(p));
}

size_t buildIdentify(uint8_t* out) {
  // 15-byte handshake. The scale validates length, not content; pyacaia sends
  // 0x2D ('-') repeated.
  static const uint8_t p[15] = {0x2d, 0x2d, 0x2d, 0x2d, 0x2d, 0x2d, 0x2d, 0x2d,
                                0x2d, 0x2d, 0x2d, 0x2d, 0x2d, 0x2d, 0x2d};
  return pack(out, Msg::IDENTIFY, p, sizeof(p));
}

size_t buildEventSubscribe(uint8_t* out) {
  // Payload = length byte + (subscribe-event-id, period) pairs, period 0 =
  // "send on change". Subscribe to weight, battery, timer, key. Note that the
  // SUBSCRIBE event-id space (0..3) differs from the BROADCAST event-id space
  // (5..8) — that's pyacaia's observed protocol, not a bug here.
  static const uint8_t p[] = {
      0x09,        // length (counts this byte)
      0x00, 0x01,  // weight, on-change
      0x01, 0x02,  // battery, on-change
      0x02, 0x03,  // timer, on-change
      0x03, 0x04,  // key, on-change
  };
  return pack(out, Msg::EVENT, p, sizeof(p));
}

size_t buildTare(uint8_t* out) {
  static const uint8_t p[] = {0x00};
  return pack(out, Msg::TARE, p, sizeof(p));
}

size_t buildTimer(uint8_t* out, TimerCmd cmd) {
  const uint8_t p[] = {0x00, static_cast<uint8_t>(cmd)};
  return pack(out, Msg::TIMER, p, sizeof(p));
}

// ---- Frame assembly ---------------------------------------------------------

void Receiver::FrameFramer::push(const uint8_t* data, size_t len,
                                 Receiver* owner, FrameCallback frameCb,
                                 void* user) {
  for (size_t i = 0; i < len; ++i) {
    appendByte(data[i]);
    drain(owner, frameCb, user);
  }
}

void Receiver::FrameFramer::appendByte(uint8_t b) {
  if (_len == 0) {
    if (b == HDR1) _buf[_len++] = b;
    return;
  }

  if (_len == 1) {
    if (b == HDR2) {
      _buf[_len++] = b;
    } else if (b != HDR1) {
      _len = 0;
    }
    return;
  }

  if (_len >= MAX_FRAME) {
    reset();
    if (b == HDR1) _buf[_len++] = b;
    return;
  }
  _buf[_len++] = b;
}

void Receiver::FrameFramer::drain(Receiver* owner, FrameCallback frameCb,
                                  void* user) {
  while (true) {
    syncToHeader();
    if (_len < 4) return;

    const size_t bodyLen = _buf[3];
    if (bodyLen == 0 || bodyLen > MAX_FRAME - 5) {
      reject(_len >= 4 ? 4 : _len, frameCb, user);
      resyncAfterReject();
      continue;
    }

    _need = bodyLen + 5;
    if (_len < _need) return;

    Frame frame;
    if (parseWireFrame(_buf, _need, &frame)) {
      if (owner) owner->handleFrame(frame, frameCb, user);
      discard(_need);
    } else {
      reject(_need, frameCb, user);
      resyncAfterReject();
    }
  }
}

void Receiver::FrameFramer::discard(size_t n) {
  if (n >= _len) {
    reset();
    return;
  }
  std::memmove(_buf, _buf + n, _len - n);
  _len -= n;
  _need = 0;
}

void Receiver::FrameFramer::syncToHeader() {
  if (_len == 0) return;
  for (size_t i = 0; i + 1 < _len; ++i) {
    if (_buf[i] == HDR1 && _buf[i + 1] == HDR2) {
      if (i) discard(i);
      return;
    }
  }
  if (_buf[_len - 1] == HDR1) {
    _buf[0] = HDR1;
    _len = 1;
  } else {
    reset();
  }
}

void Receiver::FrameFramer::reject(size_t len, FrameCallback frameCb,
                                   void* user) {
  if (frameCb && len > 0) {
    frameCb(_buf, len, InboundKind::Rejected, 0, user);
  }
}

void Receiver::FrameFramer::resyncAfterReject() {
  for (size_t i = 1; i + 1 < _len; ++i) {
    if (_buf[i] == HDR1 && _buf[i + 1] == HDR2) {
      discard(i);
      return;
    }
  }
  if (_len > 0 && _buf[_len - 1] == HDR1) {
    _buf[0] = HDR1;
    _len = 1;
    _need = 0;
  } else {
    reset();
  }
}

// ---- Classification (side-effect-free) -------------------------------------

OutboundKind classifyOutbound(const uint8_t* data, size_t len) {
  if (len < 3 || data[0] != HDR1 || data[1] != HDR2) return OutboundKind::Other;
  switch (data[2]) {
    case Msg::SYSTEM:
      return OutboundKind::Heartbeat;  // the only SYSTEM packet we send
    case Msg::TARE:
      return OutboundKind::Tare;
    case Msg::TIMER:
      return OutboundKind::Timer;
    case Msg::IDENTIFY:
      return OutboundKind::Identify;
    case Msg::EVENT:
      return OutboundKind::Subscribe;
    default:
      return OutboundKind::Other;
  }
}

InboundKind classifyInbound(const Frame& frame) {
  switch (frame.type) {
    case Msg::SETTINGS:
      return InboundKind::Settings;
    case Msg::TIMER:
      return InboundKind::Timer;
    case Msg::EVENT:
      break;
    default:
      return InboundKind::Unknown;
  }

  // An EVENT frame is keyed by its primary sub-event:
  // [eventType][event payload...]. Current Lunar weight frames may append a
  // timer marker inside the weight payload; classification still follows the
  // primary sub-event so the diagnostic counts it as one weight frame.
  if (frame.payloadLen < 1) return InboundKind::Rejected;
  if (frame.payload[0] == 0x0B) {
    return heartbeatResponseKind(frame.payload, frame.payloadLen);
  }
  return eventKind(frame.payload[0]);
}

InboundKind classifyInbound(const uint8_t* data, size_t len) {
  Frame frame;
  if (!parseWireFrame(data, len, &frame)) return InboundKind::Rejected;
  return classifyInbound(frame);
}

// ---- Event decode ----------------------------------------------------------

size_t Receiver::decodeFrame(const Frame& frame) {
  if (frame.type != Msg::EVENT || frame.payloadLen < 1) return 0;
  // Primary sub-event first, then payload to the end of the block. Weight
  // decode also looks for the current Lunar's embedded timer marker after the
  // 6-byte weight payload.
  return decodeAndEmit(frame.payload[0], frame.payload + 1,
                       frame.payloadLen - 1)
             ? 1
             : 0;
}

bool Receiver::decodeAndEmit(uint8_t evType, const uint8_t* d, size_t dn) {
  Event ev = {};
  switch (evType) {
    case 0x05: {  // weight: [b0, b1, b2, b3, exponent, flags]
      if (dn < 6) return false;
      const uint32_t raw = static_cast<uint32_t>(d[0]) |
                           (static_cast<uint32_t>(d[1]) << 8) |
                           (static_cast<uint32_t>(d[2]) << 16) |
                           (static_cast<uint32_t>(d[3]) << 24);
      // d[4] = 10^N divisor (1 → 0.1g, 2 → 0.01g, …)
      float divisor = 1.0f;
      for (uint8_t i = 0; i < d[4]; ++i) divisor *= 10.0f;
      float grams = static_cast<float>(raw) / divisor;
      // d[5] bit 1 = negative, bit 0 = "weight changing" (so stable = !bit0)
      if (d[5] & 0x02) grams = -grams;
      ev.type = EventType::WEIGHT;
      ev.weight.grams = grams;
      ev.weight.stable = (d[5] & 0x01) == 0;
      if (dn >= 10 && d[6] == 0x07 &&
          decodeTimerMs(d + 7, dn - 7, &ev.weight.scaleTimerMs)) {
        ev.weight.hasScaleTimer = true;
      }
      break;
    }
    case 0x06:  // battery: [percent]
      if (dn < 1) return false;
      ev.type = EventType::BATTERY;
      ev.battery.percent = d[0];
      break;
    case 0x07:  // timer: [minutes, seconds, deciseconds]
      if (!decodeTimerMs(d, dn, &ev.timer.ms)) return false;
      ev.type = EventType::TIMER;
      break;
    case 0x08:  // key
      if (dn < 1) return false;
      ev.type = EventType::KEY;
      ev.key.code = d[0];
      break;
    case 0x0B:  // heartbeat response: nested kind at d[2]
      if (dn >= 4 && d[2] == 0x05) {
        return decodeAndEmit(0x05, d + 3, dn - 3);
      }
      if (dn >= 4 && d[2] == 0x07) {
        return decodeAndEmit(0x07, d + 3, dn - 3);
      }
      ev.type = EventType::UNKNOWN;
      break;
    default:
      ev.type = EventType::UNKNOWN;
      break;
  }
  if (_cb) _cb(ev, _user);
  return true;
}

void Receiver::pushNotify(const uint8_t* data, size_t len,
                          FrameCallback frameCb, void* user) {
  _framer.push(data, len, this, frameCb, user);
}

void Receiver::handleFrame(const Frame& frame, FrameCallback frameCb,
                           void* user) {
  const InboundKind kind = classifyInbound(frame);
  const size_t emitted = kind == InboundKind::Rejected ? 0 : decodeFrame(frame);
  if (frameCb) frameCb(frame.wire, frame.wireLen, kind, emitted, user);
}

}  // namespace AcaiaV2
