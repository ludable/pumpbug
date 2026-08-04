// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <WebServer.h>
#include <WiFiClient.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

// A small Server-Sent Events server on a dedicated port. The sync WebServer
// model doesn't support streaming handlers cleanly when they share the
// listening socket with normal request/response endpoints — a long-lived
// SSE handler blocks the same handleClient() loop that serves /sys, /auth,
// etc. Run this on a second port so port 80 stays responsive while a stream
// is in flight.
//
// V1 invariants:
//   - One registered route (set via registerRoute()).
//   - One active client at a time. Takeover is cooperative: the handler
//     must poll isSessionStopRequested() in any loop it runs.
//   - Tickets are RAM-only, single-use, expire ~kTicketTtlMs after mint.
//     Port 80 mints them (auth-gated); this server validates on connect
//     and never touches cookies.
//
// Ownership: there is exactly one WiFiClient copy per session — the one
// _handleRoute hands to the handler. SseServer does NOT keep its own
// copy. WiFiClient's socket is shared_ptr-refcounted: extra copies
// prevent .stop() from actually closing the FD, so we keep the live
// reference count at one to keep .stop() honest.
//
// Replacement policy: implicit, latest-authenticated-client wins. A new
// ticket from an already-paired user transparently takes over any
// active stream via forceStopActiveSession + the `replaced` SSE frame.
// The alternative shape — explicit takeover that returns 409 when a
// stream is in use — is architecturally tidier (no replaced frame, no
// stop-reason enum, no race window) but it surfaces a fake conflict on
// the most common interaction: a normal page reload. The new page
// races the old EventSource's TCP close and would frequently see "stream
// in use" against the tab it just replaced. Implicit takeover keeps
// reload-as-reconnect smooth and pushes the friction onto the rare
// multi-tab/multi-device case, which matches this device's threat
// model (single user, trusted LAN). Switch to explicit takeover if
// that model ever changes (shared device, multi-user, untrusted clients).
//
// =============================================================================
//   State diagram
// =============================================================================
//
//   +-----------+   stop() — _shouldRun=false; _stopReason=Shutdown
//   |  STOPPED  | <-------------- task exits, _taskExited=true ------+
//   | no task   |                                                    |
//   +-----+-----+                                                    |
//         |                                                          |
//         | begin() — _taskExited:=false; _stopReason:=None;         |
//         | _shouldRun:=true; spawn task on core 0                   |
//         v                                                          |
//   +----------------+                                               |
//   |   LISTENING    |                                               |
//   | task running   | <-- handler returned; _handleRoute cleanup --+|
//   | _taskLoop in   |                                              ||
//   | handleClient() |                                              ||
//   | no session     |                                              ||
//   +--------+-------+                                              ||
//            |                                                      ||
//            | _handleRoute invoked (GET /<route>?ticket=…)         ||
//            |   — _shouldRun guard: 503 if shutdown in flight      ||
//            |   — _consumeTicket: 401 if missing/expired/used      ||
//            |   — write SSE headers on the WiFiClient              ||
//            |   — CAS _stopReason: Replaced -> None                ||
//            |     (Shutdown stays sticky so a stop() during        ||
//            |      session-start is not lost)                      ||
//            |   — invoke _handler(client)                          ||
//            v                                                      ||
//   +-------------------+                                           ||
//   |  SESSION_ACTIVE   |                                           ||
//   | handler running   |                                           ||
//   | _stopReason=None  |                                           ||
//   | (Replaced or      |                                           ||
//   |  Shutdown set     |                                           ||
//   |  asynchronously)  |                                           ||
//   +---------+---------+                                           ||
//             |                                                     ||
//             | handler returns                                     ||
//             |  (client.connected() false, OR                      ||
//             |   isSessionStopRequested() true, OR                 ||
//             |   send-side write failure)                          ||
//             v                                                     ||
//   +-----------------------+   _stopReason==Replaced:              ||
//   |  POST_HANDLER         |      write `event: replaced` SSE      ||
//   |  read _stopReason     |      frame on the still-live client.  ||
//   |  emit frame if needed |   _stopReason==Shutdown or None:      ||
//   |  client.stop() — FD   |      no frame.                        ||
//   |  closes (refcount=1)  |                                       ||
//   +-----------+-----------+                                       ||
//               |                                                   ||
//               +---------------------------------------------------+|
//                                                                    |
//   Task-loop tail: when _shouldRun goes false, the while loop in    |
//   _taskLoop exits, _server.close() runs, trampoline sets           |
//   _taskExited=true, vTaskDelete(nullptr). -------------------------+
//
//   Asynchronous from any task:
//     forceStopActiveSession() : _stopReason := Replaced
//     stop()                   : _shouldRun := false;
//                                _stopReason := Shutdown
//   Both rely on the handler polling isSessionStopRequested() to
//   actually exit. WiFiClient's shared_ptr socket handle means there is
//   no reliable way to force-close the FD from outside the handler.
//
// =============================================================================
//   Cooperative handover (takeover by a new ticket)
// =============================================================================
//
//   Port-80 task (ExtractionStream's /stream-ticket handler):
//     1. Auth middleware verifies the auth_token cookie.
//     2. sseServer.forceStopActiveSession() — _stopReason := Replaced.
//     3. sseServer.mintTicket() — new RAM-only token, kTicketTtlMs TTL.
//     4. Return JSON: {url: "http://host:81/<route>?ticket=…"}.
//
//   Port-81 task (this server's _taskLoop):
//     a. Handler's loop polls isSessionStopRequested(), sees true,
//        returns.
//     b. POST_HANDLER reads _stopReason == Replaced, writes
//        `event: replaced\ndata: \n\n` on the still-live WiFiClient.
//     c. client.stop() — refcount drops to zero, FD closes, browser
//        sees both the SSE event AND the TCP end.
//     d. _handleRoute returns, handleClient returns, _taskLoop loops.
//     e. Next handleClient accepts the new ticket holder's TCP
//        connection. _handleRoute validates the fresh ticket, CAS
//        downgrades Replaced -> None, invokes the handler.
//
//   Old browser: EventSource fires `replaced` listener -> sets a flag
//     -> closes -> onerror sees the flag, does not reconnect.
//   New browser: receives initial STATE / CURRENT_RECORD / FINAL_RECORD
//     packets and renders without polling.
//
//   The TTL is generous (kTicketTtlMs = 30 s) to absorb the worst-case
//   handoff: a slow / blocked old handler can stall the accept queue
//   until it returns, and we'd rather the new ticket still be valid
//   when port 81 finally gets around to it.
//
// =============================================================================
//   Shutdown (Wi-Fi loss, app-driven teardown)
// =============================================================================
//
//   Caller (main task on Wi-Fi state change in main.cpp):
//     sseServer.stop()
//       1. _shouldRun := false
//       2. _stopReason := Shutdown
//       3. Spin on _taskExited until the task winds down.
//
//   Port-81 task:
//     a. Handler's poll observes isSessionStopRequested() via either
//        _stopReason==Shutdown OR _shouldRun==false. Both paths are
//        honored so a session-start race where _handleRoute clears
//        _stopReason after stop() has already set it can't strand the
//        handler.
//     b. Handler returns. POST_HANDLER sees _stopReason==Shutdown,
//        deliberately does NOT emit `replaced` — the right UX is for
//        the client's onerror/backoff path to resume when the device
//        comes back, not strand the page on "reload to retake."
//     c. client.stop(), _handleRoute returns. handleClient returns.
//     d. _taskLoop's while loop sees _shouldRun==false, exits.
//     e. _server.close(); trampoline sets _taskExited=true; task self-
//        deletes.
//
//   Caller observes _taskExited and returns from stop().
class SseServer {
 public:
  // Called by _handleRoute with a WiFiClient that already has SSE
  // response headers written. Returns when the session ends. The
  // handler MUST poll isSessionStopRequested() inside any loop it
  // runs — that's the only way SseServer can ask it to wind down for
  // a takeover or shutdown. The handler runs on the SSE server's task;
  // while it executes, no new clients can be accepted on this port.
  using ClientHandler = std::function<void(WiFiClient&)>;

  SseServer();
  ~SseServer();

  // Idempotent. Returns true if the task is up (or just started).
  bool begin();
  void stop();
  bool isRunning() const { return _shouldRun.load(std::memory_order_acquire); }

  // Install the one route this server exposes. Must be called once before
  // begin(); changing it after begin() is a no-op (the WebServer's route
  // table is wired during task startup).
  void registerRoute(const char* path, ClientHandler handler);

  // RAM-only single-ticket store. Mint replaces any prior unconsumed
  // token. consume() is called by the route handler — clients submit the
  // token as ?ticket=<token>.
  std::string mintTicket();

  // Ticket lifetime. Exposed so callers (e.g. the ticket route) can
  // advertise an accurate expiresMs to clients.
  static constexpr uint32_t ticketTtlMs() { return kTicketTtlMs; }

  // Asks the running handler to wind down so a new ticket holder can
  // take its place. Cooperative — the handler polls
  // isSessionStopRequested(). After the handler returns, _handleRoute
  // writes a final `event: replaced` SSE frame on the still-live client
  // before closing the socket, so the displaced browser knows not to
  // auto-reconnect. Safe to call from any task; non-blocking.
  void forceStopActiveSession();

  // Signal an intentional device sleep. The running handler will exit,
  // then _handleRoute emits an `event: sleeping` frame so the browser
  // can stop waiting on heartbeats and enter wake polling. Blocks up to
  // `waitMs` for the handler to return and the frame to be flushed.
  // Safe to call from any task. Returns immediately if no session is
  // active (i.e. no browser tab is connected). Call this just before
  // deep sleep.
  void signalSleeping(uint32_t waitMs = 250);

  // Polled by the ClientHandler from inside its session loop. WiFiClient
  // copies share the underlying socket via shared_ptr, so SseServer
  // can't reliably close the FD from outside — the handler holds the
  // single live copy, and only its own client.stop() (on return) drops
  // the refcount to zero. This flag is therefore the only signal that can
  // tell the handler to exit its loop; the handler must check it.
  //
  // Two terminal signals are honored: any non-None _stopReason, AND
  // _shouldRun being false. The latter covers a shutdown race where
  // stop() runs after _handleRoute checked _shouldRun but before it
  // reset _stopReason — without it the new session would clear
  // Shutdown to None and the handler would hang past stop()'s wait.
  bool isSessionStopRequested() const {
    return _stopReason.load(std::memory_order_acquire) != StopReason::None ||
           !_shouldRun.load(std::memory_order_acquire);
  }

 private:
  // Distinguishes a cooperative takeover from a server shutdown. After
  // the handler returns, _handleRoute emits a `replaced` SSE frame for
  // Replaced (so the browser suppresses auto-reconnect) but not for
  // Shutdown — the right UX there is to let the client's normal
  // reconnect-backoff path resume once the device comes back.
  enum class StopReason : uint8_t {
    None,
    Replaced,
    Shutdown,
    Sleeping,
  };

  static constexpr uint16_t kPort = 81;
  static constexpr uint32_t kTaskStackBytes = 8192;
  static constexpr UBaseType_t kTaskPriority = 1;
  static constexpr BaseType_t kTaskCore = 0;
  static constexpr TickType_t kLoopDelayTicks = pdMS_TO_TICKS(10);
  // 30 s is generous slack for the takeover handoff window: the new
  // ticket holder's TCP connection can't be accepted until the old
  // handler returns, and a slow client / blocking write can stretch
  // that out. 10 s was tight enough that a wedged old client could
  // expire the new ticket before port 81 got around to validating it.
  static constexpr uint32_t kTicketTtlMs = 30000;
  static constexpr size_t kTicketBytes = 16;  // hex-encoded → 32 chars

  static void _taskTrampoline(void* arg);
  void _taskLoop();
  void _handleRoute();
  bool _consumeTicket(const char* token);

  std::atomic<bool> _shouldRun{false};
  std::atomic<bool> _taskExited{true};
  // Drives the handler's exit path. Reset to None at the start of each
  // session in _handleRoute; set to Replaced by forceStopActiveSession
  // or Shutdown by stop(). Reads by isSessionStopRequested() use
  // acquire; writes use release.
  std::atomic<StopReason> _stopReason{StopReason::None};
  TaskHandle_t _taskHandle = nullptr;

  WebServer _server{kPort};
  std::string _routePath;
  ClientHandler _handler;

  // True while a handler is running inside _handleRoute. Set before
  // invoking _handler and cleared after post-handler cleanup. Used by
  // signalSleeping() to return immediately when no browser is connected.
  std::atomic<bool> _sessionActive{false};

  // Protects the ticket store from cross-task access (mint from the
  // port-80 task; consume from this server's task).
  mutable SemaphoreHandle_t _mutex;

  std::string _ticketToken;
  uint32_t _ticketMintedMs = 0;
  bool _ticketConsumed = true;
};

extern SseServer sseServer;
