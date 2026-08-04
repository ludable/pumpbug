// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#include "SseServer.h"

#include <M5Unified.h>
#include <esp_random.h>

#include <cstdio>
#include <cstring>

#include "util/hex.h"
#include "util/scoped_lock.h"

SseServer sseServer;

namespace {
using Lock = ScopedLock;

}  // namespace

SseServer::SseServer() : _mutex(xSemaphoreCreateMutex()) {}

SseServer::~SseServer() {
  stop();
  if (_mutex) {
    vSemaphoreDelete(_mutex);
    _mutex = nullptr;
  }
}

void SseServer::registerRoute(const char* path, ClientHandler handler) {
  // Routes go into the WebServer's handler table once, here — not in
  // _taskLoop, where each begin()/stop() cycle would re-on() and leak
  // handler closures. WebServer::close() preserves the route table
  // across listener restarts, so this is correct as long as
  // registerRoute is called at most once.
  _routePath = path ? path : "";
  _handler = std::move(handler);
  if (_routePath.empty()) return;
  _server.on(_routePath.c_str(), HTTP_GET, [this]() { _handleRoute(); });
  _server.onNotFound(
      [this]() { _server.send(404, "text/plain", "Not found"); });
}

bool SseServer::begin() {
  if (_shouldRun.load(std::memory_order_acquire)) return true;
  if (_routePath.empty() || !_handler) {
    M5_LOGE("SseServer: begin() before registerRoute()");
    return false;
  }
  // Wait for any prior task to finish unwinding (mirrors HttpServer's
  // pattern — between _taskLoop returning and vTaskDelete firing there's
  // a small window).
  while (!_taskExited.load(std::memory_order_acquire)) {
    vTaskDelay(pdMS_TO_TICKS(5));
  }
  _taskExited.store(false, std::memory_order_release);
  _stopReason.store(StopReason::None, std::memory_order_release);
  _shouldRun.store(true, std::memory_order_release);

  // ESP-IDF's xTaskCreatePinnedToCore takes usStackDepth in BYTES on the
  // ESP32 port (a documented deviation from vanilla FreeRTOS). Dividing
  // by sizeof(StackType_t) is either a no-op (if uint8_t) or shrinks the
  // stack to a quarter (if uint32_t) — both unintended.
  const BaseType_t ok = xTaskCreatePinnedToCore(
      &SseServer::_taskTrampoline, "sse", kTaskStackBytes, this, kTaskPriority,
      &_taskHandle, kTaskCore);
  if (ok != pdPASS) {
    M5_LOGE("SseServer: failed to spawn task");
    _shouldRun.store(false, std::memory_order_release);
    _taskExited.store(true, std::memory_order_release);
    return false;
  }
  return true;
}

void SseServer::stop() {
  if (!_shouldRun.load(std::memory_order_acquire)) return;
  _shouldRun.store(false, std::memory_order_release);
  // Signal Shutdown (not Replaced) so _handleRoute's post-handler path
  // skips the `event: replaced` frame — the right UX during shutdown is
  // to let the client's onerror/backoff path resume once the device
  // comes back, not strand the page on a "reload to retake" banner.
  _stopReason.store(StopReason::Shutdown, std::memory_order_release);
  while (!_taskExited.load(std::memory_order_acquire)) {
    vTaskDelay(pdMS_TO_TICKS(5));
  }
  _taskHandle = nullptr;
}

std::string SseServer::mintTicket() {
  uint8_t bytes[kTicketBytes];
  esp_fill_random(bytes, sizeof(bytes));
  std::string token = hexOf(bytes, sizeof(bytes));

  Lock l(_mutex);
  _ticketToken = token;
  _ticketMintedMs = millis();
  _ticketConsumed = false;
  return token;
}

bool SseServer::_consumeTicket(const char* token) {
  if (!token || !*token) return false;
  Lock l(_mutex);
  if (_ticketConsumed) return false;
  if (_ticketToken.empty()) return false;
  if (millis() - _ticketMintedMs > kTicketTtlMs) {
    _ticketConsumed = true;
    return false;
  }
  if (_ticketToken != token) return false;
  _ticketConsumed = true;
  // Wipe the token so a memory dump after consumption doesn't leak it.
  _ticketToken.clear();
  return true;
}

void SseServer::forceStopActiveSession() {
  // Cooperative: hand the running handler the "wind down" signal. The
  // handler polls isSessionStopRequested() in its loop, exits, and the
  // post-handler code in _handleRoute writes the `event: replaced` SSE
  // frame on the still-live WiFiClient before closing it. No socket
  // fiddling here — there's no SseServer-owned WiFiClient copy to call
  // .stop() on, and even if there were it wouldn't actually close the
  // FD while the handler still holds its own copy.
  _stopReason.store(StopReason::Replaced, std::memory_order_release);
}

void SseServer::signalSleeping(uint32_t waitMs) {
  // If no browser tab is connected, there is no SSE session to notify.
  // isRunning() only means the listener task is up (which it is whenever
  // Wi-Fi is configured), so we need a real active-session flag.
  if (!_sessionActive.load(std::memory_order_acquire)) return;

  _stopReason.store(StopReason::Sleeping, std::memory_order_release);

  // Wait for the handler to notice and return. The task loop polls
  // _stopReason every kLoopDelayTicks (10 ms) via isSessionStopRequested().
  const uint32_t startedMs = millis();
  while (_stopReason.load(std::memory_order_acquire) == StopReason::Sleeping &&
         millis() - startedMs < waitMs) {
    vTaskDelay(pdMS_TO_TICKS(5));
  }

  // Whether the handler flushed the frame or we hit the deadline, make
  // sure Sleeping is not left stuck. If the device does not actually
  // power off after this callback, a stale Sleeping reason would cause
  // the next connecting browser to immediately emit a spurious sleeping
  // frame and enter wake polling.
  StopReason expected = StopReason::Sleeping;
  _stopReason.compare_exchange_strong(expected, StopReason::None,
                                      std::memory_order_acq_rel);

  // If the deadline passed without the handler returning, abandon the
  // sleeping frame and cut power anyway. The client will fall back to
  // its idle-timeout / wake-polling path.
  if (millis() - startedMs >= waitMs) {
    M5_LOGW("SseServer: sleeping frame not flushed in %lums", waitMs);
  }
}

void SseServer::_taskTrampoline(void* arg) {
  SseServer* self = static_cast<SseServer*>(arg);
  self->_taskLoop();
  self->_taskExited.store(true, std::memory_order_release);
  vTaskDelete(nullptr);
}

void SseServer::_taskLoop() {
  if (!_shouldRun.load(std::memory_order_acquire)) return;

  _server.begin();

  M5_LOGI("SseServer: listening on :%u%s", kPort, _routePath.c_str());

  while (_shouldRun.load(std::memory_order_acquire)) {
    _server.handleClient();
    vTaskDelay(kLoopDelayTicks);
  }

  _server.close();
  M5_LOGI("SseServer: stopped");
}

void SseServer::_handleRoute() {
  // Closes the race where a connection arrives between stop() setting
  // _shouldRun=false and the task loop noticing. Without this, the new
  // session would clear _stopReason on entry and run with no exit
  // signal until the socket dies on its own, blocking _taskExited.
  if (!_shouldRun.load(std::memory_order_acquire)) {
    _server.send(503, "text/plain", "shutting down\n");
    return;
  }

  const String ticket = _server.arg("ticket");
  if (!_consumeTicket(ticket.c_str())) {
    _server.send(401, "text/plain", "invalid or expired ticket\n");
    return;
  }

  // Take over the underlying socket. server.send() would helpfully add
  // Connection: close, which is exactly what we don't want.
  WiFiClient client = _server.client();
  client.print(
      "HTTP/1.1 200 OK\r\n"
      "Content-Type: text/event-stream\r\n"
      "Cache-Control: no-cache\r\n"
      "Connection: keep-alive\r\n"
      "Access-Control-Allow-Origin: *\r\n"
      "X-Accel-Buffering: no\r\n"
      "\r\n");
  client.flush();

  // Fresh session — downgrade a leftover Replaced to None so a prior
  // takeover doesn't immediately kill this session, but leave Shutdown
  // sticky. If stop() fired between the _shouldRun check above and
  // this line (it can — the two are separate atomic loads), Shutdown
  // must survive; isSessionStopRequested() also checks _shouldRun so
  // the handler still observes the stop signal even if the CAS slips.
  StopReason expected = StopReason::Replaced;
  _stopReason.compare_exchange_strong(expected, StopReason::None,
                                      std::memory_order_acq_rel);

  if (_handler) {
    _sessionActive.store(true, std::memory_order_release);
    _handler(client);
    _sessionActive.store(false, std::memory_order_release);
  }

  // After the handler returns, decide whether to emit a final SSE event
  // based on why it exited. Takeover sends `event: replaced` so the
  // browser's EventSource fires its `replaced` listener and suppresses
  // auto-reconnect. Shutdown skips the frame on purpose — the client's
  // normal reconnect-backoff path is the right UX once the device
  // comes back. Natural disconnects (StopReason::None) also skip.
  const StopReason reason = _stopReason.load(std::memory_order_acquire);
  if (!client.connected()) {
    // Peer already gone; no final frame.
  } else if (reason == StopReason::Replaced) {
    static constexpr const char kReplacedFrame[] =
        "event: replaced\ndata: \n\n";
    client.write(reinterpret_cast<const uint8_t*>(kReplacedFrame),
                 sizeof(kReplacedFrame) - 1);
    client.flush();
  } else if (reason == StopReason::Sleeping) {
    static constexpr const char kSleepingFrame[] =
        "event: sleeping\ndata: \n\n";
    client.write(reinterpret_cast<const uint8_t*>(kSleepingFrame),
                 sizeof(kSleepingFrame) - 1);
    client.flush();
    // Clear the Sleeping reason so signalSleeping() can observe that the
    // frame was emitted and stop waiting before its deadline.
    StopReason expected = StopReason::Sleeping;
    _stopReason.compare_exchange_strong(expected, StopReason::None,
                                        std::memory_order_acq_rel);
  }

  // Single live copy of the WiFiClient — refcount drops to zero, FD
  // actually closes, browser sees the TCP end. This is the change that
  // makes the previous patch's contortions unnecessary.
  client.stop();
}
