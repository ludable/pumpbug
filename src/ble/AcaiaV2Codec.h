// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <cstddef>
#include <cstdint>

// Acaia EF/DD packet codec. Current Lunar/Pyxis/Pearl firmware and older
// legacy Lunar/Pearl firmware use the same packet framing; they differ at the
// GATT profile layer. Standalone: takes / produces raw bytes, no NimBLE
// coupling, so the BLE transport layer can be swapped or tested in isolation.
//
// Protocol constants and event mappings were informed by pyacaia. See
// THIRD_PARTY_NOTICES.md for provenance and license information.
namespace AcaiaV2 {

// GATT identifiers. Newer firmware uses the Microchip "Transparent UART"
// service. Older Pearl / Lunar 2019 ("legacy") use 00001820-... with a single
// read/write/notify characteristic.
extern const char* SERVICE_UUID;
extern const char* CHAR_WRITE_UUID;   // we write to this
extern const char* CHAR_NOTIFY_UUID;  // we subscribe to this
extern const char* LEGACY_SERVICE_UUID;
extern const char* LEGACY_CHAR_UUID;  // legacy write + notify characteristic

// Outbound commands. Each writes a complete packet into `out` and returns the
// byte count. MAX_PACKET covers the largest command we send (event-subscribe at
// 14 bytes); 32 leaves comfortable headroom.
inline constexpr size_t MAX_PACKET = 32;

// Largest inbound protocol frame we'll reassemble from BLE notifications.
// Message-log storage may keep only the first bytes; `wireLen` preserves the
// true frame length there.
inline constexpr size_t MAX_FRAME = 64;

size_t buildHeartbeat(uint8_t* out);
size_t buildIdentify(uint8_t* out);
size_t buildEventSubscribe(uint8_t* out);
size_t buildTare(uint8_t* out);

enum class TimerCmd : uint8_t { START = 0, STOP = 1, RESET = 2 };
size_t buildTimer(uint8_t* out, TimerCmd cmd);

// Inbound events emitted by the decoder.
enum class EventType : uint8_t {
  NONE = 0,
  WEIGHT,
  BATTERY,
  TIMER,
  KEY,
  UNKNOWN,
};

struct Event {
  EventType type;
  union {
    struct {
      float grams;
      bool stable;
      bool hasScaleTimer;
      uint32_t scaleTimerMs;
    } weight;
    struct {
      uint8_t percent;
    } battery;
    struct {
      uint32_t ms;
    } timer;
    struct {
      uint8_t code;
    } key;
  };
};

// Checksum-verified inbound frame view. Pointers are borrowed from the caller
// or receiver buffer and are valid only for the duration of the call/callback
// that supplied the frame.
struct Frame {
  uint8_t type = 0;
  const uint8_t* body = nullptr;  // length byte + payload bytes
  size_t bodyLen = 0;
  const uint8_t* payload = nullptr;  // body after the length byte
  size_t payloadLen = 0;
  const uint8_t* wire = nullptr;  // EF DD ... checksum
  size_t wireLen = 0;
};

// --- Packet classification (side-effect-free) -------------------------------
// Lightweight tagging for the scale message-log diagnostic: map a raw packet to
// its kind without decoding values or invoking any callback. Kept here so the
// protocol knowledge (header, msg-type byte, sub-event ids) lives in one place.

// Outbound (commands we send). Mirrors the Msg:: type byte at data[2].
enum class OutboundKind : uint8_t {
  Heartbeat,  // SYSTEM
  Tare,       // TARE
  Timer,      // TIMER
  Identify,   // IDENTIFY
  Subscribe,  // EVENT (outbound EVENT is always subscribe; we never broadcast)
  Other,      // valid-ish header, unrecognized type
};
OutboundKind classifyOutbound(const uint8_t* data, size_t len);

// Inbound (notifications from the scale). Tags a frame by its single event
// type — no value decode. An EVENT frame carries one sub-event, so it's tagged
// by that event's type; SETTINGS/TIMER message frames map straight across.
enum class InboundKind : uint8_t {
  Weight,
  Battery,
  Timer,
  Key,
  Unknown,   // valid frame, unrecognized command or sub-event id
  Mixed,     // not currently produced (one event per frame); kept for the
             // index-aligned MsgTag/web label table
  Rejected,  // bad header / checksum / framing
  Settings,  // valid settings/status frame (command 0x08)
};
InboundKind classifyInbound(const Frame& frame);
InboundKind classifyInbound(const uint8_t* data, size_t len);

// Owns the Acaia inbound path: BLE notification bytes -> complete protocol
// frames -> decoded live events + per-frame diagnostic metadata.
class Receiver {
 public:
  using EventCallback = void (*)(const Event& ev, void* user);
  using FrameCallback = void (*)(const uint8_t* frame, size_t frameLen,
                                 InboundKind kind, size_t emittedEvents,
                                 void* user);

  void setEventCallback(EventCallback cb, void* user) {
    _cb = cb;
    _user = user;
  }
  void reset() { _framer.reset(); }
  void pushNotify(const uint8_t* data, size_t len, FrameCallback frameCb,
                  void* user);

 private:
  // Reassembles Acaia protocol frames from the BLE notify byte stream. The
  // frame length is deterministic: EF DD <type> <bodyLen> <body...> <ck...>,
  // where bodyLen counts the length byte itself.
  class FrameFramer {
   public:
    void reset() {
      _len = 0;
      _need = 0;
    }
    void push(const uint8_t* data, size_t len, Receiver* owner,
              FrameCallback frameCb, void* user);

   private:
    uint8_t _buf[MAX_FRAME] = {};
    size_t _len = 0;
    size_t _need = 0;

    void appendByte(uint8_t b);
    void drain(Receiver* owner, FrameCallback frameCb, void* user);
    void discard(size_t n);
    void syncToHeader();
    void reject(size_t len, FrameCallback frameCb, void* user);
    void resyncAfterReject();
  };

  FrameFramer _framer;
  EventCallback _cb = nullptr;
  void* _user = nullptr;

  void handleFrame(const Frame& frame, FrameCallback frameCb, void* user);
  size_t decodeFrame(const Frame& frame);
  bool decodeAndEmit(uint8_t eventTypeId, const uint8_t* d, size_t dn);
};

}  // namespace AcaiaV2
