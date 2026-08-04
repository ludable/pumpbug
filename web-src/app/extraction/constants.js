// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

const PHASE = ['IDLE', 'RUNNING', 'POST_PUMP', 'DONE'];
const END_CAUSE = ['NONE', 'STABLE', 'TIMEOUT'];
// User-facing wording for abnormal shot ends. STABLE (the scale settled) is
// how a shot ends normally, so it has no entry and the row is omitted.
const END_CAUSE_LABEL = { 2: 'timed out' };
const EVENT_KIND = ['BEGIN','TARE','PUMP_ON','PUMP_OFF',
  'SCALE_CONNECTED','SCALE_DISCONNECTED','STABLE_DETECTED','END',
  'ALARM_TRIGGERED'];
const SCALE_STATE = ['off', 'scanning', 'connecting',
                     'connected', 'reconnecting'];
const SHOT_STORAGE_STATUS = ['ready', 'unavailable'];

// SSE packet types (must match ExtractionStream.h).
const PACKET_STATE = 1;
const PACKET_CURRENT_RECORD = 2;
const PACKET_SAMPLE_BATCH = 3;
const PACKET_FINAL_RECORD = 4;
const PACKET_HEARTBEAT = 5;
const PACKET_HEADER_BYTES = 14;
const PACKET_VERSION = 4;
const SAMPLE_BATCH_FLAG_SCALE_TIMERS = 0x01;
const RECORD_FLAG_SCALE_TIMERS = 0x02;
const SCALE_TIMER_UNKNOWN_MS = 0xffffffff;
const NO_WEIGHT_CG = -32768;

// How long the Last-shot card wears its "new" badge after a shot we watched
// finish live. Purely cosmetic; the card itself stays.
const FRESH_SHOT_BADGE_MS = 120000;

// The pouring timer extrapolates wall-clock time past the last STATE packet
// only while packets are this fresh. During a healthy pour STATEs arrive
// every ~200 ms, so this never binds; when the device's clock stops advancing
// (a paused replay) or the stream dies, packets stop and the timer freezes at
// the last reported value instead of running away.
const TIMER_EXTRAPOLATE_MAX_MS = 1000;

// Reconnect backoff bounds — multiplicative, capped.
const INITIAL_BACKOFF_MS = 1000;
const MAX_BACKOFF_MS = 30000;

// SSE liveness. The server emits a HEARTBEAT every 10 s while a stream
// is active; if the client sees no packet at all for longer than this,
// the connection is likely dead (e.g. the device was reflashed / deep-
// slept and the browser hasn't noticed). Close it and request a fresh
// ticket so a shot that starts right after wake-up is not missed.
// Note: this timeout is only applied after the EventSource has opened,
// so a slow server handoff is not mistaken for a dead connection.
// The first-packet timeout catches a half-open stream (TCP connected but
// the server isn't sending) quickly, without interfering with the server's
// 30 s ticket-handoff window, because it only runs after onopen.
const SSE_FIRST_PACKET_TIMEOUT_MS = 5000;
// 20 s gives a 2x margin over the 10 s heartbeat, so one delayed
// heartbeat (e.g. Wi-Fi contention) does not spuriously tear down a
// live stream while still detecting a dead connection much faster than
// the original 35 s timeout.
const SSE_IDLE_TIMEOUT_MS = 20000;
const TICKET_FETCH_TIMEOUT_MS = 10000;

// Wake polling. After the device intentionally goes to sleep (or after an
// unexpected SSE idle timeout), the client probes /app/extraction/state
// periodically. Foreground polls are frequent so the web UI reconnects
// within ~1 s of the device waking; background polls are throttled.
const WAKE_POLL_TIMEOUT_MS = 1500;
const WAKE_POLL_INTERVAL_MS = 1000;
const WAKE_POLL_HIDDEN_INTERVAL_MS = 5000;
