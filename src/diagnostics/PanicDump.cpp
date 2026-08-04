// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#include "diagnostics/PanicDump.h"

#include <WebServer.h>
#include <esp_core_dump.h>  // brings in sdkconfig.h + the summary API (if enabled)

#include <algorithm>
#include <atomic>
#include <cstring>

#include "net/HttpServer.h"
#include "net/JsonStream.h"

namespace diagnostics {

namespace {
// Set by a clear (any task), consumed by the main loop to resync the UI alert.
std::atomic<bool> gAlertResync{false};

void servePanic(WebServer& server) {
  PanicSummary panic{};
  const bool present = readLastPanic(panic);

  JsonStream json(server);
  json.open().key("entries").arrayOpen();
  if (present) {
    json.open()
        .key("task")
        .str(panic.task)
        .key("excCause")
        .u(panic.excCause)
        .key("excPc")
        .u(panic.excPc)
        .key("excVaddr")
        .u(panic.excVaddr)
        .key("btCorrupted")
        .boolean(panic.btCorrupted != 0)
        .key("bt")
        .arrayOpen();
    const uint8_t depth = panic.btDepth > 16 ? 16 : panic.btDepth;
    for (uint8_t index = 0; index < depth; ++index) {
      if (index) json.comma();
      json.u(panic.bt[index]);
    }
    json.arrayClose().close();
  }
  json.arrayClose().close();
  json.finish();
}
}  // namespace

bool readLastPanic(PanicSummary& out) {
  // image_check() is always available regardless of menuconfig and returns
  // ESP_OK only for a valid dump — and only the panic path ever writes one — so
  // it doubles as the "did we crash" check. ESP_ERR_NOT_FOUND on a clean boot;
  // a CRC/size error on a torn write.
  if (esp_core_dump_image_check() != ESP_OK) return false;

#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH && CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF
  esp_core_dump_summary_t summary{};  // ~250B; fine on the web task stack (8KB)
  if (esp_core_dump_get_summary(&summary) != ESP_OK) return false;

  out = PanicSummary{};
  out.excPc = summary.exc_pc;
  out.excCause = summary.ex_info.exc_cause;
  out.excVaddr = summary.ex_info.exc_vaddr;
  const uint32_t depth = std::min<uint32_t>(summary.exc_bt_info.depth, 16);
  out.btDepth = static_cast<uint8_t>(depth);
  out.btCorrupted = summary.exc_bt_info.corrupted ? 1 : 0;
  std::memcpy(out.bt, summary.exc_bt_info.bt, sizeof(out.bt));
  std::strncpy(out.task, summary.exc_task, sizeof(out.task) - 1);
  return true;
#else
  return false;  // image present but this build can't decode the summary
#endif
}

bool hasPanicDump() { return esp_core_dump_image_check() == ESP_OK; }

bool consumePanicAlertResync() {
  return gAlertResync.exchange(false, std::memory_order_acquire);
}

// Erase the dump and request a status-bar resync as one operation, so every
// clear attempt refreshes the marker from the stored dump.
bool clearLastPanicAndResync() {
  const bool ok = esp_core_dump_image_erase() == ESP_OK;
  gAlertResync.store(true, std::memory_order_release);
  return ok;
}

void registerPanicRoute(HttpServer& server) {
  server.registerRoutes(
      "/sys/diagnostics",
      {
          // Diagnosing the previous crash must not depend on authentication,
          // which may be the subsystem that failed. This read-only summary
          // exposes task and exception metadata, not memory contents.
          HttpRoute{"/panic", HTTP_GET, servePanic, /*requiresAuth=*/false},
      });
}

}  // namespace diagnostics
