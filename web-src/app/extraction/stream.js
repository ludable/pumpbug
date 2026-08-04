// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

// SSE connection state.
//
// Lifecycle: ticketing -> opening -> streaming -> closed/replaced/sleeping.
// The current stream is tracked explicitly so idle-timeout liveness only
// applies once the EventSource has actually opened. A slow server handoff
// (e.g. an old SSE session still winding down) must not be mistaken for a
// dead connection.
let currentStream = null;
let reconnectTimer = null;
let backoffMs = INITIAL_BACKOFF_MS;
let connecting = false;
let connectEpoch = 0;
let wakePollTimer = null;

function base64Decode(b64) {
  const bin = atob(b64);
  const out = new Uint8Array(bin.length);
  for (let i = 0; i < bin.length; i++) out[i] = bin.charCodeAt(i);
  return out.buffer;
}

function readUvarintAt(dv, ref) {
  let v = 0, s = 0, b;
  do { b = dv.getUint8(ref.p++); v |= (b & 0x7f) << s; s += 7; }
  while (b & 0x80);
  return v >>> 0;
}
function readSvarintAt(dv, ref) {
  const u = readUvarintAt(dv, ref);
  return (u >>> 1) ^ -(u & 1);
}

function parseStatePayload(dv, off) {
  const flags = dv.getUint8(off);
  const active = (flags & 0x01) !== 0;
  const hasWeight = (flags & 0x02) !== 0;
  const hasYield = (flags & 0x04) !== 0;
  const pouring = (flags & 0x08) !== 0;
  const hasDisplayShot = (flags & 0x10) !== 0;
  return {
    active,
    pouring,
    hasDisplayShot,
    phase: PHASE[dv.getUint8(off + 1)] || 'IDLE',
    scale: SCALE_STATE[dv.getUint8(off + 2)] || 'idle',
    storage: SHOT_STORAGE_STATUS[dv.getUint8(off + 3)] || 'unavailable',
    elapsedMs: dv.getUint32(off + 4, true),
    savedSeq: dv.getUint32(off + 8, true),
    currentWeightCg: hasWeight ? dv.getInt16(off + 12, true) : null,
    currentYieldCg: hasYield ? dv.getInt16(off + 14, true) : null,
  };
}

function parseSampleBatchPayload(dv, off, beginMs) {
  const firstIdx = dv.getUint16(off, true);
  const count = dv.getUint16(off + 2, true);
  const anchorTMs = dv.getUint32(off + 4, true);
  const anchorCg = dv.getInt16(off + 8, true);
  const flags = dv.getUint8(off + 10);
  const hasScaleTimerFields = (flags & SAMPLE_BATCH_FLAG_SCALE_TIMERS) !== 0;
  const ref = { p: off + 11 };
  let tMs = anchorTMs;
  let cg = anchorCg;
  const samples = [];
  for (let i = 0; i < count; i++) {
    tMs = (tMs + readUvarintAt(dv, ref)) >>> 0;
    cg += readSvarintAt(dv, ref);
    const scaleTimerMs = hasScaleTimerFields
      ? readUvarintAt(dv, ref)
      : SCALE_TIMER_UNKNOWN_MS;
    samples.push({ tMs: unwrapTimestamp(tMs, beginMs), cg, scaleTimerMs });
  }
  return { firstIdx, count, samples };
}

function handleStatePacket(dv, payloadOff, shotSeq) {
  const s = parseStatePayload(dv, payloadOff);
  s.acceptedSeq = shotSeq;
  // Receipt time anchors the pouring timer between rate-limited STATEs.
  s.rxAtMs = performance.now();

  const wasPumping = inPumpWindow(state);

  // Abandoned-in-flight detection: a screen exit or a rejected finalize
  // (flush, grinder dose) clears the recorder without a FINAL_RECORD, so
  // the stream sends a STATE going IDLE with the shot seq unchanged. Drop
  // our stale in-flight model on that transition; otherwise renderLive
  // would keep the abandoned samples around.
  if (inflight && wasPumping && s.phase === 'IDLE' &&
      s.acceptedSeq === state.acceptedSeq) {
    inflight = null;
  }

  // A user-expanded Last-shot card only stays open for the pump window that
  // expanded it; the next idle render collapses back to the default.
  if (wasPumping && !inPumpWindow(s)) lastShotExpanded = false;

  // The device says it has no display shot (e.g. a reflashed/reset device
  // behind a reconnect, or a failed shot load with nothing to fall back to):
  // drop ours, or the card would show a shot the device no longer has.
  if (!s.hasDisplayShot) lastRecord = null;

  state = s;
  maybeRefreshShots();
}

function handleCurrentRecordPacket(dv, payloadOff, shotSeq) {
  try {
    const ext = parseExtractionFromDv(dv, payloadOff);
    ext.shotSeq = shotSeq;
    inflight = ext;
  } catch (err) {
    console.warn('parseExtraction failed', err);
  }
}

function handleFinalRecordPacket(dv, payloadOff, shotSeq) {
  try {
    const ext = parseExtractionFromDv(dv, payloadOff);
    // Our watched in-flight graduating: the header's shotSeq advances past
    // the seq the in-flight was bound to, so retire it into the record slot
    // and badge it "new" for a while. An unchanged shotSeq means a stored
    // shot was loaded onto the device — it updates the record slot but
    // leaves a live pour alone. The badge is bound to the graduated shot's
    // beginMs so a different record arriving inside the badge window (a
    // shot load) doesn't wear it.
    if (inflight && shotSeq !== inflight.shotSeq) {
      inflight = null;
      freshFinalizedAt = Date.now();
      freshBeginMs = ext.beginMs;
      setTimeout(() => scheduleRender(), FRESH_SHOT_BADGE_MS + 250);
    }
    ext.shotSeq = shotSeq;
    lastRecord = ext;
  } catch (err) {
    console.warn('parseExtraction failed', err);
  }
}

function handleSampleBatchPacket(dv, payloadOff, shotSeq) {
  // Append samples to the in-flight extraction. If our local model
  // doesn't match the packet's context (no in-flight record, wrong
  // shotSeq, gap in the sample index), force a fresh reconnect to
  // resync from a CURRENT_RECORD.
  if (!inflight || inflight.shotSeq !== shotSeq) {
    forceReconnect('sample batch without matching current record');
    return;
  }
  const { firstIdx, samples } =
    parseSampleBatchPayload(dv, payloadOff, inflight.beginMs);
  if (firstIdx !== inflight.samples.length) {
    forceReconnect(
      'sample gap: firstIdx=' + firstIdx +
      ' local=' + inflight.samples.length);
    return;
  }
  for (const s of samples) inflight.samples.push(s);
  inflight.sampleCount = inflight.samples.length;
}

function onPacketEvent(event) {
  let buf;
  try { buf = base64Decode(event.data); }
  catch (e) { console.warn('bad base64 in SSE packet', e); return; }
  if (buf.byteLength < PACKET_HEADER_BYTES) return;
  const dv = new DataView(buf);
  if (dv.getUint8(0) !== 0x53 || dv.getUint8(1) !== 0x54 ||
      dv.getUint8(2) !== 0x4B || dv.getUint8(3) !== 0x50) {
    console.warn('bad packet magic');
    return;
  }
  if (dv.getUint8(4) !== PACKET_VERSION) {
    handleProtocolMismatch(dv.getUint8(4));
    return;
  }
  const type = dv.getUint8(5);
  // streamSeq at offset 6 — unused for now (would matter for replay/
  // gap detection if the server ever supports Last-Event-ID).
  const shotSeq = dv.getUint32(10, true);
  let shouldRender = true;
  switch (type) {
    case PACKET_STATE:
      handleStatePacket(dv, PACKET_HEADER_BYTES, shotSeq);
      shouldRender = false;
      scheduleLiveRender();
      break;
    case PACKET_CURRENT_RECORD:
      handleCurrentRecordPacket(dv, PACKET_HEADER_BYTES, shotSeq);
      shouldRender = view === 'live';
      break;
    case PACKET_SAMPLE_BATCH:
      handleSampleBatchPacket(dv, PACKET_HEADER_BYTES, shotSeq);
      shouldRender = view === 'live';
      break;
    case PACKET_FINAL_RECORD:
      handleFinalRecordPacket(dv, PACKET_HEADER_BYTES, shotSeq);
      shouldRender = view === 'live';
      break;
    case PACKET_HEARTBEAT:
      shouldRender = false;
      break;
    default:
      console.warn('unknown packet type', type);
  }
  if (shouldRender) scheduleRender();
}

function handleProtocolMismatch(actualVersion) {
  protocolMismatch =
    'firmware UI protocol changed — reload this page';
  console.warn('unsupported packet version', actualVersion,
               'expected', PACKET_VERSION);
  closeCurrentStream();
  scheduleRender();
}

// Server-driven takeover:
// The server enforces single-client by force-closing the prior SSE
// connection on a new ticket. To stop the displaced client from
// immediately re-minting and kicking the new one (a fight that
// would loop forever — same browser, two browsers, two devices,
// doesn't matter), the server sends a final `event: replaced` SSE
// frame before TCP close. We register a listener for it that
// suppresses auto-reconnect; user reloads to opt back in.
//
// When the device intentionally goes to deep sleep, it sends a final
// `event: sleeping` frame. The client closes the stream and enters
// wake polling so it reconnects within seconds of the device waking,
// instead of waiting for heartbeat liveness to time out.
//
// No localStorage / BroadcastChannel: the displaced-client / sleeping
// signal rides the same SSE socket the data does, so it's universal by
// construction — there's nothing for the client to coordinate on its
// own. A persisted local lock would also wrongly strand the only tab
// after a refresh by treating yesterday's claim as live.

function armIdleTimer(stream) {
  clearTimeout(stream.idleTimer);
  const timeoutMs = (stream.opened && stream.firstPacketSeen)
    ? SSE_IDLE_TIMEOUT_MS
    : SSE_FIRST_PACKET_TIMEOUT_MS;
  stream.idleTimer = setTimeout(() => {
    if (currentStream !== stream) return;
    if (!stream.firstPacketSeen) {
      startWakePolling('SSE first-packet timeout');
      return;
    }
    startWakePolling('SSE idle timeout');
  }, timeoutMs);
}

function closeStream(stream) {
  if (!stream) return;
  clearTimeout(stream.idleTimer);
  stream.es.close();
  if (currentStream === stream) currentStream = null;
}

function closeCurrentStream() {
  closeStream(currentStream);
}

function openStream(url) {
  // Drop any prior stream before adopting this one.
  closeCurrentStream();

  const stream = {
    es: new EventSource(url),
    opened: false,
    firstPacketSeen: false,
    replaced: false,
    sleeping: false,
    idleTimer: null,
  };

  currentStream = stream;

  stream.es.addEventListener('packet', (event) => {
    if (currentStream !== stream) return;
    stream.firstPacketSeen = true;
    armIdleTimer(stream);
    onPacketEvent(event);
  });

  stream.es.addEventListener('replaced', () => {
    if (currentStream !== stream) return;
    stream.replaced = true;
    closeStream(stream);
    setBanner('another client took over — reload to retake the stream',
              'idle');
  });

  stream.es.addEventListener('sleeping', () => {
    if (currentStream !== stream) return;
    stream.sleeping = true;
    closeStream(stream);
    startWakePolling('device sleeping');
  });

  stream.es.onopen = () => {
    if (currentStream !== stream) return;
    stream.opened = true;
    armIdleTimer(stream);
    backoffMs = INITIAL_BACKOFF_MS;
    // Banner gets rewritten by handleStatePacket as soon as the
    // server's initial STATE arrives; this is just a transient
    // pre-data signal.
    if (!state) setBanner('connected, waiting for state…', 'idle');
  };

  stream.es.onerror = () => {
    if (currentStream !== stream) return;
    const wasReplaced = stream.replaced;
    const wasSleeping = stream.sleeping;
    closeStream(stream);
    if (!wasReplaced && !wasSleeping) {
      setBanner('disconnected; reconnecting…', 'idle');
      scheduleReconnect();
    }
  };
}

async function connect() {
  if (protocolMismatch) return;
  if (connecting) return;
  stopWakePolling();
  if (reconnectTimer) { clearTimeout(reconnectTimer); reconnectTimer = null; }
  const myEpoch = ++connectEpoch;
  connecting = true;
  const ac = new AbortController();
  const ticketTimeout = setTimeout(() => ac.abort(), TICKET_FETCH_TIMEOUT_MS);
  try {
    const r = await fetch('/app/extraction/stream-ticket',
                          { method: 'POST', cache: 'no-store',
                            signal: ac.signal });
    if (myEpoch !== connectEpoch) return;
    if (!r.ok) {
      if (r.status === 401) {
        setBanner('not paired — open /config to pair this device', 'bad');
      } else {
        setBanner('ticket request failed (' + r.status + ')', 'bad');
      }
      scheduleReconnect();
      return;
    }
    const j = await r.json();
    if (myEpoch !== connectEpoch) return;
    openStream(j.url);
  } catch (err) {
    if (myEpoch !== connectEpoch) return;
    if (err.name === 'AbortError') {
      setBanner('ticket request timed out', 'bad');
    } else {
      setBanner('error: ' + err.message, 'bad');
    }
    scheduleReconnect();
  } finally {
    clearTimeout(ticketTimeout);
    if (myEpoch === connectEpoch) connecting = false;
  }
}

function scheduleReconnect() {
  if (protocolMismatch) return;
  if (reconnectTimer) return;
  reconnectTimer = setTimeout(() => {
    reconnectTimer = null;
    connect();
  }, backoffMs);
  backoffMs = Math.min(backoffMs * 2, MAX_BACKOFF_MS);
}

function forceReconnect(reason) {
  if (protocolMismatch) return;
  if (reason) console.warn('forcing extraction stream reconnect:', reason);
  ++connectEpoch;
  connecting = false;
  closeCurrentStream();
  // Drop only the in-flight model; the record card stays up through the
  // reconnect (the new session re-sends the display shot anyway).
  inflight = null;
  backoffMs = INITIAL_BACKOFF_MS;
  setBanner('stream out of sync; reconnecting…', 'idle');
  scheduleReconnect();
}

// Wake polling: when we know the device is sleeping (or the SSE connection
// died unexpectedly), probe a cheap HTTP endpoint periodically and reconnect
// immediately as soon as the device responds. This avoids the long exponential
// backoff window that would otherwise delay reconnect after wake.
function startWakePolling(reason) {
  if (protocolMismatch) return;
  if (wakePollTimer) return;
  ++connectEpoch;
  connecting = false;
  closeCurrentStream();
  if (reconnectTimer) { clearTimeout(reconnectTimer); reconnectTimer = null; }
  backoffMs = INITIAL_BACKOFF_MS;
  if (reason) {
    console.log('start wake polling:', reason);
    setBanner(reason + ' — waiting for device…', 'idle');
  }

  async function poll() {
    wakePollTimer = null;
    if (currentStream || protocolMismatch) return;
    try {
      // Any HTTP response means the device is reachable; we let the
      // normal connect() path handle 401/503/etc. by fetching a fresh
      // ticket. Only network-level failure keeps us polling.
      await fetchWithTimeout('/app/extraction/state',
                             { cache: 'no-store' },
                             WAKE_POLL_TIMEOUT_MS);
      // Device is back. Reconnect via the normal SSE path.
      connect();
    } catch (_) {
      wakePollTimer = setTimeout(poll, document.hidden ? WAKE_POLL_HIDDEN_INTERVAL_MS : WAKE_POLL_INTERVAL_MS);
    }
  }

  wakePollTimer = setTimeout(poll, 0);
}

function stopWakePolling() {
  if (wakePollTimer) {
    clearTimeout(wakePollTimer);
    wakePollTimer = null;
  }
}
