// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#include "ExtractionStream.h"

#include <M5Unified.h>
#include <mbedtls/base64.h>

#include <climits>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>
#include <vector>

#include "ExtractionController.h"
#include "ScaleReadingTimingObserver.h"
#include "extraction_encoding.h"
#include "net/HttpServer.h"
#include "net/SseServer.h"

namespace pump_scale {

namespace {
constexpr uint8_t SAMPLE_BATCH_FLAG_SCALE_TIMERS = 0x01;

// Base64 encoded length (no newlines, with padding) for `n` input bytes.
constexpr size_t base64EncodedLen(size_t n) { return ((n + 2) / 3) * 4; }
}  // namespace

void ExtractionStream::registerWith(SseServer& sse, HttpServer& http,
                                    ExtractionController& controller) {
  _controller = &controller;
  _sse = &sse;
  sse.registerRoute("/app/extraction/events",
                    [this](WiFiClient& c) { _runSession(c); });
  http.registerRoutes(
      "/app/extraction",
      {
          HttpRoute{"/stream-ticket", HTTP_POST,
                    [this](WebServer& s) { _handleTicketRoute(s); }},
      });
}

void ExtractionStream::notifySleeping() const {
  if (!_sse) return;
  _sse->signalSleeping(/*waitMs=*/250);
}

void ExtractionStream::_handleTicketRoute(WebServer& server) {
  if (!_sse) {
    server.send(503, "application/json",
                "{\"error\":\"sse server not initialized\"}");
    return;
  }
  // Hard-stop any active stream so the new ticket holder is the only one
  // when it connects. Browser sees connection drop and reconnects to the
  // new URL (which carries the fresh ticket).
  _sse->forceStopActiveSession();
  const std::string token = _sse->mintTicket();

  // Build an absolute URL pointing at port 81. The host the client used
  // to reach us is preserved (mDNS vs IP); only the port is rewritten.
  String host = server.hostHeader();
  int colon = host.indexOf(':');
  if (colon >= 0) host = host.substring(0, colon);
  char body[256];
  std::snprintf(body, sizeof(body),
                "{\"url\":\"http://%s:81/app/extraction/events?ticket=%s\","
                "\"expiresMs\":%u}",
                host.c_str(), token.c_str(),
                static_cast<unsigned>(SseServer::ticketTtlMs()));
  server.send(200, "application/json", body);
}

void ExtractionStream::_writePacketHeader(uint8_t* hdr, uint8_t type,
                                          uint32_t streamSeq,
                                          uint32_t shotSeq) {
  hdr[0] = 'S';
  hdr[1] = 'T';
  hdr[2] = 'K';
  hdr[3] = 'P';
  hdr[4] = 4;  // version
  hdr[5] = type;
  writeU32LE(hdr + 6, streamSeq);
  writeU32LE(hdr + 10, shotSeq);
}

bool ExtractionStream::_writeSseFrame(WiFiClient& client, uint32_t streamSeq,
                                      const uint8_t* bin, size_t binLen) {
  // Base64 in heap so big CURRENT_RECORD / FINAL_RECORD packets don't
  // blow the 8 KB task stack. STATE / HEARTBEAT / SAMPLE_BATCH are
  // small but the same path keeps things uniform.
  const size_t b64Cap = base64EncodedLen(binLen) + 1;
  std::unique_ptr<char[]> b64(new (std::nothrow) char[b64Cap]);
  if (!b64) {
    M5_LOGE("ExtractionStream: no heap for %u-byte base64 frame",
            static_cast<unsigned>(b64Cap));
    return false;
  }
  size_t written = 0;
  const int rc =
      mbedtls_base64_encode(reinterpret_cast<unsigned char*>(b64.get()), b64Cap,
                            &written, bin, binLen);
  if (rc != 0) {
    M5_LOGE("ExtractionStream: base64 encode failed (%d)", rc);
    return false;
  }

  char idLine[40];
  const int idLen = std::snprintf(
      idLine, sizeof(idLine),
      "id: %u\nevent: packet\ndata: ", static_cast<unsigned>(streamSeq));
  if (idLen <= 0) return false;
  if (client.write(reinterpret_cast<const uint8_t*>(idLine),
                   static_cast<size_t>(idLen)) != static_cast<size_t>(idLen)) {
    return false;
  }
  if (client.write(reinterpret_cast<const uint8_t*>(b64.get()), written) !=
      written) {
    return false;
  }
  // Two newlines: terminate the data line and the event.
  if (client.write(reinterpret_cast<const uint8_t*>("\n\n"), 2) != 2) {
    return false;
  }
  client.flush();
  return true;
}

bool ExtractionStream::_sendState(WiFiClient& client, Session& s,
                                  const ExtractionStatusSnapshot& snap) {
  uint8_t buf[kPacketHeaderBytes + 16];
  _writePacketHeader(buf, kPacketState, s.streamSeq, snap.acceptedSeq);
  uint8_t flags = 0;
  if (snap.active) flags |= 0x01;
  if (snap.hasCurrentWeight) flags |= 0x02;
  if (snap.hasCurrentYield) flags |= 0x04;
  if (snap.currentPouring) flags |= 0x08;
  if (snap.hasDisplayShot) flags |= 0x10;
  if (snap.pumpSignalState == PumpSignalState::DecayCandidate) flags |= 0x20;
  buf[kPacketHeaderBytes + 0] = flags;
  buf[kPacketHeaderBytes + 1] = static_cast<uint8_t>(snap.currentPhase);
  buf[kPacketHeaderBytes + 2] = static_cast<uint8_t>(snap.scaleState);
  buf[kPacketHeaderBytes + 3] = static_cast<uint8_t>(snap.storageState);
  writeU32LE(buf + kPacketHeaderBytes + 4, snap.currentElapsedMs);
  // acceptedSeq identifies the latest real shot. savedSeq advances after its
  // file is verified, so the client can both refresh history and report an
  // unsuccessful latest save.
  writeU32LE(buf + kPacketHeaderBytes + 8, snap.savedSeq);
  writeI16LE(buf + kPacketHeaderBytes + 12,
             snap.hasCurrentWeight ? snap.currentWeightCg : INT16_MIN);
  writeI16LE(buf + kPacketHeaderBytes + 14,
             snap.hasCurrentYield ? snap.currentYieldCg : INT16_MIN);

  if (!_writeSseFrame(client, s.streamSeq, buf, sizeof(buf))) return false;
  if (snap.hasCurrentWeight) {
    observeScaleReadingWebStateSent(snap.currentWeightSequence, millis());
  }
  ++s.streamSeq;
  s.lastSentState = snap;
  s.lastSentStateInit = true;
  s.lastStateMs = millis();
  s.lastPacketMs = s.lastStateMs;
  return true;
}

bool ExtractionStream::_sendHeartbeat(WiFiClient& client, Session& s,
                                      uint32_t shotSeq) {
  uint8_t buf[kPacketHeaderBytes];
  _writePacketHeader(buf, kPacketHeartbeat, s.streamSeq, shotSeq);
  if (!_writeSseFrame(client, s.streamSeq, buf, sizeof(buf))) return false;
  ++s.streamSeq;
  s.lastPacketMs = millis();
  return true;
}

bool ExtractionStream::_sendFullRecord(WiFiClient& client, Session& s,
                                       uint8_t type, const Extraction& ext,
                                       uint32_t shotSeq) {
  size_t bodyLen = 0;
  if (!encodeCompactSize(ext, bodyLen)) {
    M5_LOGE("ExtractionStream: record version cannot be encoded");
    return false;
  }
  const size_t totalLen = kPacketHeaderBytes + bodyLen;
  std::unique_ptr<uint8_t[]> buf(new (std::nothrow) uint8_t[totalLen]);
  if (!buf) {
    M5_LOGE("ExtractionStream: no heap for %u-byte record packet",
            static_cast<unsigned>(totalLen));
    return false;
  }
  _writePacketHeader(buf.get(), type, s.streamSeq, shotSeq);

  // append the encoded EXTR body after the packet header.
  size_t pos = kPacketHeaderBytes;
  const WireSink sink = [&](const uint8_t* data, size_t len) {
    if (pos + len <= totalLen) {
      std::memcpy(buf.get() + pos, data, len);
      pos += len;
    }
  };
  const bool encoded = encodeCompact(ext, sink);
  if (!encoded || pos != totalLen) {
    M5_LOGE("ExtractionStream: encoder size mismatch (pos=%u total=%u)",
            static_cast<unsigned>(pos), static_cast<unsigned>(totalLen));
    return false;
  }

  if (!_writeSseFrame(client, s.streamSeq, buf.get(), totalLen)) return false;
  ++s.streamSeq;
  s.lastPacketMs = millis();
  return true;
}

bool ExtractionStream::_sendCurrentRecord(WiFiClient& client, Session& s,
                                          const Extraction& ext,
                                          uint32_t shotSeq) {
  if (!_sendFullRecord(client, s, kPacketCurrentRecord, ext, shotSeq)) {
    return false;
  }
  // CURRENT_RECORD ships all samples to date. Reset the sample tracker exactly
  // when the snapshot was successfully written, so subsequent SAMPLE_BATCH
  // deltas anchor against the state the client now has.
  s.lastInFlightBeginMs = ext.beginMs;
  s.lastSentRecordPhase = ext.phase;
  s.lastSentRecordInit = true;
  s.lastSentSampleIdx = ext.sampleCount;
  if (ext.sampleCount > 0) {
    const Sample& last = ext.samples[ext.sampleCount - 1];
    s.lastSentTMs = last.tMs;
    s.lastSentCg = last.cg;
  } else {
    s.lastSentTMs = ext.beginMs;
    s.lastSentCg = 0;
  }
  return true;
}

bool ExtractionStream::_sendSampleBatch(WiFiClient& client, Session& s,
                                        const Extraction& ext,
                                        uint32_t shotSeq) {
  const uint16_t startIdx = s.lastSentSampleIdx;
  if (ext.sampleCount <= startIdx) return true;  // nothing to send
  const uint16_t count = ext.sampleCount - startIdx;

  // Anchor: the (tMs, cg) the first delta in this batch is computed from.
  // For the very first batch of a shot this is (beginMs, 0); afterward it's
  // the last sample we sent.
  const uint32_t anchorTMs = startIdx == 0 ? ext.beginMs : s.lastSentTMs;
  const int32_t anchorCg = startIdx == 0 ? 0 : s.lastSentCg;
  const bool includeScaleTimers =
      sampleRangeHasScaleTimer(ext.samples + startIdx, count);
  const uint8_t flags =
      includeScaleTimers ? SAMPLE_BATCH_FLAG_SCALE_TIMERS : 0x00;

  // Pre-pass to size the buffer.
  size_t bodyLen = 2 + 2 + 4 + 2 + 1;  // fixed batch header
  {
    SampleDeltaCursor sampleCursor{anchorTMs, anchorCg, includeScaleTimers};
    for (uint16_t i = 0; i < count; ++i) {
      bodyLen +=
          sampleDeltaEncodedSize(sampleCursor, ext.samples[startIdx + i]);
    }
  }

  const size_t totalLen = kPacketHeaderBytes + bodyLen;
  std::unique_ptr<uint8_t[]> buf(new (std::nothrow) uint8_t[totalLen]);
  if (!buf) {
    M5_LOGE("ExtractionStream: no heap for %u-byte sample packet",
            static_cast<unsigned>(totalLen));
    return false;
  }
  _writePacketHeader(buf.get(), kPacketSampleBatch, s.streamSeq, shotSeq);

  size_t pos = kPacketHeaderBytes;
  writeU16LE(buf.get() + pos, startIdx);
  pos += 2;
  writeU16LE(buf.get() + pos, count);
  pos += 2;
  writeU32LE(buf.get() + pos, anchorTMs);
  pos += 4;
  writeI16LE(buf.get() + pos, static_cast<int16_t>(anchorCg));
  pos += 2;
  buf.get()[pos++] = flags;

  {
    SampleDeltaCursor sampleCursor{anchorTMs, anchorCg, includeScaleTimers};
    for (uint16_t i = 0; i < count; ++i) {
      pos += writeSampleDelta(buf.get() + pos, sampleCursor,
                              ext.samples[startIdx + i]);
    }
  }

  if (pos != totalLen) {
    M5_LOGE("ExtractionStream: sample-batch size mismatch (pos=%u total=%u)",
            static_cast<unsigned>(pos), static_cast<unsigned>(totalLen));
    return false;
  }

  if (!_writeSseFrame(client, s.streamSeq, buf.get(), totalLen)) return false;
  ++s.streamSeq;
  s.lastPacketMs = millis();

  // Advance trackers.
  s.lastSentSampleIdx = ext.sampleCount;
  if (ext.sampleCount > 0) {
    const Sample& last = ext.samples[ext.sampleCount - 1];
    s.lastSentTMs = last.tMs;
    s.lastSentCg = last.cg;
  }
  return true;
}

bool ExtractionStream::_stateShouldSend(const ExtractionStatusSnapshot& cur,
                                        const Session& s,
                                        uint32_t nowMs) const {
  if (!s.lastSentStateInit) return true;
  const ExtractionStatusSnapshot& last = s.lastSentState;
  // Significant transitions always go out promptly. savedSeq counts as
  // significant because the client's History tab keys its cache invalidation
  // off it, and the verified save happens after the accepted-shot transition.
  if (cur.active != last.active) return true;
  if (cur.currentPhase != last.currentPhase) return true;
  if (cur.scaleState != last.scaleState) return true;
  if (cur.acceptedSeq != last.acceptedSeq) return true;
  if (cur.savedSeq != last.savedSeq) return true;
  if (cur.storageState != last.storageState) return true;
  if (cur.hasCurrentWeight != last.hasCurrentWeight) return true;
  if (cur.hasCurrentYield != last.hasCurrentYield) return true;
  // The pouring bit flips the client's whole live treatment (yield headline,
  // running timer), so it goes out promptly like the phase transitions.
  if (cur.currentPouring != last.currentPouring) return true;
  // Send candidate onset and cancellation immediately so the live cue does not
  // wait for the next rate-limited measurement update.
  const bool decayCandidate =
      cur.pumpSignalState == PumpSignalState::DecayCandidate;
  const bool lastDecayCandidate =
      last.pumpSignalState == PumpSignalState::DecayCandidate;
  if (decayCandidate != lastDecayCandidate) return true;
  // hasDisplayShot going false is the only signal that removes the client's
  // Last-shot card (no FINAL_RECORD follows), and it could change with nothing
  // else moving, e.g. a failed shot load with no accepted shot to fall back to.
  if (cur.hasDisplayShot != last.hasDisplayShot) return true;
  // Continuously evolving fields obey the rate limit.
  const bool elapsedChanged = cur.currentElapsedMs != last.currentElapsedMs;
  const bool weightChanged = cur.currentWeightCg != last.currentWeightCg;
  const bool yieldChanged = cur.currentYieldCg != last.currentYieldCg;
  if ((elapsedChanged || weightChanged || yieldChanged) &&
      (nowMs - s.lastStateMs) >= kStateMinIntervalMs) {
    return true;
  }
  return false;
}

void ExtractionStream::_runSession(WiFiClient& client) {
  if (!_controller) return;

  Session s;
  // Extraction is ~10 KB; keep it on the heap, not the SSE task's 8 KB
  // stack. One allocation, reused for every snapshot in this session.
  auto ext = std::unique_ptr<Extraction>(new (std::nothrow) Extraction());
  if (!ext) {
    M5_LOGE("ExtractionStream: no heap for session record");
    return;
  }

  auto sendDisplayRecord = [&](const ExtractionStatusSnapshot& status) {
    if (status.hasDisplayShot) {
      const uint32_t recordSeq =
          _controller->snapshotExtraction(*ext, /*wantDisplayShot=*/true);
      if (!_sendFullRecord(client, s, kPacketFinalRecord, *ext, recordSeq)) {
        return false;
      }
    }
    s.lastDisplaySeq = status.displayShotSeq;
    // Force the next in-flight update to send CURRENT_RECORD before resuming
    // sample deltas; a display-shot change can coincide with a new in-flight
    // shot.
    s.lastInFlightBeginMs = 0;
    s.lastSentRecordPhase = Phase::IDLE;
    s.lastSentRecordInit = false;
    s.lastSentSampleIdx = 0;
    s.lastSentTMs = 0;
    s.lastSentCg = 0;
    return true;
  };

  // Capture liveSeq BEFORE the initial snapshot/send. Any tickle that
  // lands while we're encoding the first packets needs to be re-processed
  // in the loop below — recording the post-snapshot value here would
  // silently consume that transition.
  s.lastSeenLiveSeq = _controller->liveSeq();

  // Initial: STATE plus a snapshot of whatever's "current view" for the
  // client. New connections always get enough to render immediately
  // without polling: the display shot (Last-shot card, including a
  // boot-restored or web-loaded shot) and, when a shot is in flight, the
  // live record on top.
  ExtractionStatusSnapshot snap;
  _controller->snapshotStatus(snap);
  // Bail immediately on any write failure during the initial dispatch:
  // the client is gone, and continuing to encode/send packets to a dead
  // socket just delays release of the SSE session slot for the next
  // ticket holder.
  if (!_sendState(client, s, snap)) return;
  if (!sendDisplayRecord(snap)) return;
  if (snap.active && snap.currentPhase != Phase::IDLE) {
    const uint32_t recordSeq =
        _controller->snapshotExtraction(*ext, /*wantDisplayShot=*/false);
    // Graduation may land between the independent status and record
    // snapshots. Defer the new in-flight record so the next liveSeq tickle
    // sends its predecessor's FINAL_RECORD first.
    const bool recordActive =
        ext->phase == Phase::RUNNING || ext->phase == Phase::POST_PUMP;
    if (recordActive && recordSeq == snap.acceptedSeq &&
        !_sendCurrentRecord(client, s, *ext, recordSeq)) {
      return;
    }
  }

  s.lastPacketMs = millis();

  // client.connected() isn't the sole exit condition: WiFiClient's
  // socket handle is shared_ptr-backed, so a session whose handle has
  // been .stop()'d from another copy might still report connected. The
  // cooperative isSessionStopRequested() flag is the authoritative
  // signal for takeover / shutdown. Write failures also break out
  // immediately — a dead client shouldn't keep the session slot warm.
  while (client.connected() && !_sse->isSessionStopRequested()) {
    const uint32_t now = millis();
    const uint32_t curLiveSeq = _controller->liveSeq();
    const bool tickle = curLiveSeq != s.lastSeenLiveSeq;
    if (tickle) s.lastSeenLiveSeq = curLiveSeq;

    if (tickle) {
      _controller->snapshotStatus(snap);

      // Display shot first: catches "the in-flight shot just graduated"
      // (or web/boot load into the display slot) so the client gets the
      // authoritative FINAL_RECORD before the next SAMPLE_BATCH (which
      // would be for a new shot).
      if (snap.displayShotSeq != s.lastDisplaySeq) {
        if (!sendDisplayRecord(snap)) break;
      }

      // In-flight shot: refresh the full record when we need to update
      // metadata fields STATE and SAMPLE_BATCH don't carry (events[],
      // totalPumpOnMs, peak, endCause, etc.) — that's a new beginMs
      // (new shot) or a phase transition within the same shot
      // (RUNNING→POST_PUMP appends a PUMP_OFF_CONFIRMED event, etc.).
      // Otherwise just send SAMPLE_BATCH for appended samples.
      if (snap.active && snap.currentPhase != Phase::IDLE) {
        const bool maybeNewInFlight =
            snap.currentBeginMs != s.lastInFlightBeginMs;
        // phaseChanged stays false until we've sent a CURRENT_RECORD, so the
        // initial record already covers this case.
        const bool maybePhaseChanged =
            s.lastSentRecordInit && snap.currentPhase != s.lastSentRecordPhase;
        const bool maybeNewSamples =
            snap.currentSampleCount > s.lastSentSampleIdx;
        if (maybeNewInFlight || maybePhaseChanged || maybeNewSamples) {
          const uint32_t recordSeq =
              _controller->snapshotExtraction(*ext,
                                              /*wantDisplayShot=*/false);
          // The status copy is only a prefilter. A controller update can land
          // before this full-record copy, so packet selection must use the
          // record that will actually be sent.
          const bool recordActive =
              ext->phase == Phase::RUNNING || ext->phase == Phase::POST_PUMP;
          const bool newInFlight = ext->beginMs != s.lastInFlightBeginMs;
          const bool phaseChanged =
              s.lastSentRecordInit && ext->phase != s.lastSentRecordPhase;
          const bool needFullRecord = newInFlight || phaseChanged;
          const bool newSamples = ext->sampleCount > s.lastSentSampleIdx;

          // If a shot graduated between snapshots, wait for the next liveSeq
          // tickle so FINAL_RECORD remains ahead of the next CURRENT_RECORD.
          if (recordActive && recordSeq == snap.acceptedSeq) {
            if (needFullRecord) {
              if (!_sendCurrentRecord(client, s, *ext, recordSeq)) {
                break;
              }
            } else if (newSamples) {
              if (!_sendSampleBatch(client, s, *ext, recordSeq)) {
                break;
              }
            }
          }
        }
      }

      // STATE if anything that the client renders has changed.
      if (_stateShouldSend(snap, s, now)) {
        if (!_sendState(client, s, snap)) break;
      }
    }

    if (millis() - s.lastPacketMs > kHeartbeatIntervalMs) {
      if (!_sendHeartbeat(client, s, snap.acceptedSeq)) break;
    }

    vTaskDelay(pdMS_TO_TICKS(kPollDelayMs));
  }
}

}  // namespace pump_scale
