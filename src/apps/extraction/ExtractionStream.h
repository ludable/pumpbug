// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <WebServer.h>
#include <WiFiClient.h>

#include <cstdint>

#include "Extraction.h"
#include "ExtractionStatusSnapshot.h"

class HttpServer;
class SseServer;

namespace pump_scale {

class ExtractionController;

// Pushes live extraction data over a Server-Sent Events stream.
//
// Wire format: each SSE `data` field carries one base64-encoded binary
// packet. Packet layout (little-endian):
//
//   offset  size  field
//      0      4   magic "STKP"
//      4      1   version (= 4)
//      5      1   type (see kPacket* below)
//      6      4   streamSeq (u32 — monotonic per SSE connection)
//     10      4   shotSeq (u32 — latest accepted-shot sequence)
//     14      …   per-type payload
//
// Types:
//   STATE          (1)  — every-tick status. 16-byte payload:
//                          u8 flags (bit0 active, bit1 hasCurrentWeight,
//                                    bit2 hasCurrentYield, bit3 pouring,
//                                    bit4 hasDisplayShot,
//                                    bit5 pumpDecayCandidate)
//                          u8 phase
//                          u8 scaleState
//                          u8 storageState (see util/storage.h)
//                          u32 elapsedMs
//                          u32 savedSeq
//                          i16 currentWeightCg (INT16_MIN when !hasWeight)
//                          i16 currentYieldCg (INT16_MIN when !hasYield)
//   CURRENT_RECORD (2)  — full in-flight Extraction encoded with the current
//                          EXTR schema.
//   SAMPLE_BATCH   (3)  — appended samples for the in-flight shot:
//                          u16 firstSampleIndex (within current.samples)
//                          u16 sampleCount
//                          u32 anchorTMs (absolute tMs the first delta is
//                                          from — beginMs for the first
//                                          batch, last sample's tMs after)
//                          i16 anchorCg
//                          u8 flags (bit0 sample scale-timer fields present)
//                          LEB128 stream: uvarint tMsDelta, svarint cgDelta,
//                                          optional uvarint scaleTimerMs
//                                          (raw value or
//                                          scale_time::UNKNOWN_MS)
//   FINAL_RECORD   (4)  — display shot encoded with the current EXTR schema.
//   HEARTBEAT      (5)  — empty payload, keeps proxies/clients from idling.
//
// ExtractionStream keeps one browser's SSE session synchronized with the
// controller.
//
// The controller owns extraction state. A Session stores only what this
// connection has successfully received: packet sequence numbers, the latest
// controller update observed, the accepted shot reported to the client, the
// in-flight shot base, sample-delta positions, and packet timestamps. Comparing
// a new controller snapshot with those values determines which packet to send.
//
// ExtractionController::liveSeq() is the inexpensive change signal. Equality
// lets the session skip controller snapshots. A changed value causes one status
// snapshot and the following work, in order:
//
//   1. If the display-shot counter (_displayShotSeq) changed (a shot graduated,
//      or a stored shot was loaded — boot restore, web "set as last shot"),
//      send FINAL_RECORD for the display shot and clear the in-flight cursors.
//   2. If an extraction is active, send CURRENT_RECORD when its begin time or
//      phase changed. Otherwise, send SAMPLE_BATCH for appended samples.
//   3. Send STATE immediately for significant status changes, or after
//      kStateMinIntervalMs for continuously changing measurements.
//
// Ordering the accepted shot before the in-flight extraction matters because
// one controller update can finish a shot and start another before the SSE task
// runs.
//
// ## Session lifecycle
//
// The ticket route stops any active session and issues a short-lived ticket.
// SseServer validates that ticket and calls _runSession() with the connected
// client. _runSession() then:
//
//   1. Creates a fresh Session and records liveSeq before taking snapshots, so
//      an update during initial encoding remains visible to the main loop.
//   2. Sends STATE and enough record data to render immediately: FINAL_RECORD
//      for the display shot when one exists, plus CURRENT_RECORD for an active
//      extraction.
//   3. Polls liveSeq and processes changed controller state as described above.
//   4. Sends HEARTBEAT when no other packet has been sent for
//      kHeartbeatIntervalMs.
//   5. Returns when the client disconnects, SseServer requests replacement or
//      shutdown, or a packet write fails.
//
// SseServer owns connection admission and socket cleanup. Session cursors are
// never reused by a later connection.
//
// ## Controller inputs
//
//   ExtractionController::liveSeq()            Reports whether stream-visible
//                                              state may have changed.
//   ExtractionController::snapshotStatus()     Provides the small status copy
//                                              used for decisions and STATE.
//   ExtractionController::snapshotExtraction() Provides the full record used
//                                              by CURRENT_RECORD, FINAL_RECORD,
//                                              and SAMPLE_BATCH, plus the
//                                              controller's accepted-shot
//                                              sequence at copy time.
//
// ## Invariants
//
//   * streamSeq starts at zero for each connection and advances once per
//     successfully written packet.
//   * SseServer allows only one active connection, so only one Session exists
//     at a time.
//   * FINAL_RECORD precedes data for a new in-flight extraction discovered in
//     the same controller update.
//   * Status metadata is only a prefilter for copying the full record. Packet
//     boundaries and sample cursors are derived from that record itself.
//   * A record copied across an accepted-shot sequence change is deferred to
//     the next liveSeq tickle, preserving FINAL_RECORD ordering.
//   * A changed record beginMs identifies a new in-flight extraction, which
//     receives CURRENT_RECORD before any SAMPLE_BATCH.
//   * SAMPLE_BATCH is append-only from lastSentSampleIdx and uses lastSentTMs
//     and lastSentCg as its delta base.
//   * Significant STATE changes bypass the rate limit. Continuously changing
//     measurements are rate-limited.
//   * HEARTBEAT changes only streamSeq and lastPacketMs.
class ExtractionStream {
 public:
  // Packet constants are public so the wire-format tests use these definitions.
  static constexpr uint8_t kPacketState = 1;
  static constexpr uint8_t kPacketCurrentRecord = 2;
  static constexpr uint8_t kPacketSampleBatch = 3;
  static constexpr uint8_t kPacketFinalRecord = 4;
  static constexpr uint8_t kPacketHeartbeat = 5;
  static constexpr size_t kPacketHeaderBytes = 14;

  // Registers the SSE session route and the authenticated ticket route.
  // `sse`, `http`, and `controller` must outlive this object. Requesting a
  // ticket stops the active session so a new browser tab replaces a stale one.
  void registerWith(SseServer& sse, HttpServer& http,
                    ExtractionController& controller);

  // Flush a final `event: sleeping` control frame to any connected web
  // client and close the SSE session. Called by the power manager just before
  // deep sleep so the client can stop waiting on heartbeats and enter
  // wake polling. Safe to call when no client is connected.
  void notifySleeping() const;

 private:
  static constexpr uint32_t kHeartbeatIntervalMs = 10000;
  static constexpr uint32_t kStateMinIntervalMs = 200;
  static constexpr uint32_t kPollDelayMs = 20;

  ExtractionController* _controller = nullptr;
  SseServer* _sse = nullptr;

  // Per-connection state — the SSE handler runs synchronously inside
  // SseServer's task; only one of these is alive at a time.
  struct Session {
    uint32_t streamSeq = 0;
    uint32_t lastSeenLiveSeq = 0;
    // The displayShotSeq value most recently reported to this client; a
    // snapshot with a different value means the display shot changed and a
    // FINAL_RECORD is due.
    uint32_t lastDisplaySeq = 0;
    uint16_t lastSentSampleIdx = 0;
    uint32_t lastSentTMs = 0;
    int32_t lastSentCg = 0;
    // beginMs of the in-flight shot we've sent a CURRENT_RECORD for; 0
    // when no in-flight is currently being tracked. A change here means
    // a new shot started mid-stream and the client needs a fresh base.
    uint32_t lastInFlightBeginMs = 0;
    Phase lastSentRecordPhase = Phase::IDLE;
    bool lastSentRecordInit = false;
    ExtractionStatusSnapshot lastSentState{};
    bool lastSentStateInit = false;
    uint32_t lastStateMs = 0;
    uint32_t lastPacketMs = 0;
  };

  void _runSession(WiFiClient& client);
  void _handleTicketRoute(WebServer& server);

  // Each packet emitter writes one SSE frame, increments streamSeq after a
  // successful write, and returns whether the write succeeded.
  bool _sendState(WiFiClient& client, Session& s,
                  const ExtractionStatusSnapshot& snap);
  bool _sendSampleBatch(WiFiClient& client, Session& s, const Extraction& ext,
                        uint32_t shotSeq);
  bool _sendCurrentRecord(WiFiClient& client, Session& s, const Extraction& ext,
                          uint32_t shotSeq);
  bool _sendFullRecord(WiFiClient& client, Session& s, uint8_t type,
                       const Extraction& ext, uint32_t shotSeq);
  bool _sendHeartbeat(WiFiClient& client, Session& s, uint32_t shotSeq);

  // Frames `bin` as `id: N\nevent: packet\ndata: <base64>\n\n` and writes
  // to client. Returns false on write failure / client disconnect.
  bool _writeSseFrame(WiFiClient& client, uint32_t streamSeq,
                      const uint8_t* bin, size_t binLen);

  // Reports whether the latest status contains information the client should
  // receive now. Continuously changing values are rate-limited.
  bool _stateShouldSend(const ExtractionStatusSnapshot& cur, const Session& s,
                        uint32_t nowMs) const;

  static void _writePacketHeader(uint8_t* hdr, uint8_t type, uint32_t streamSeq,
                                 uint32_t shotSeq);
};

}  // namespace pump_scale
