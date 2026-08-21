// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <cstdint>

class HttpServer;

// Read-only access to the most recent crash, decoded on demand from the
// ESP-IDF core-dump partition.
//
// On a panic the IDF panic handler writes a full ELF core dump to the dedicated
// `coredump` flash partition (this build has core-dump-to-flash enabled). That
// partition is already durable across reboots and is overwritten by the next
// panic, so it *is* the store — there is no second copy to keep in sync,
// nothing to persist, and no boot-time capture/erase dance. We just decode the
// summary when something asks for it (the /sys/diagnostics/panic route); the
// full raw dump stays in flash for offline analysis until the user clears the
// crash record or performs a full-chip erase with esptool.
//
// The crash time and reset reason live elsewhere on purpose: PowerEventLog
// records every boot, so a panic shows up there as a Wake with
// esp_reset_reason()==ESP_RST_PANIC and a wall-clock stamp. This helper carries
// only what the core dump uniquely knows — the fault location and backtrace.
namespace diagnostics {

constexpr uint32_t kXtensaExcCauseMask = 0x3f;

inline uint32_t xtensaExcCause(uint32_t rawCause) {
  return rawCause & kXtensaExcCauseMask;
}

// Decoded summary of the crash currently stored in the coredump partition.
// Addresses are raw; decode offline against the matching firmware.elf, e.g.
//   xtensa-esp32s3-elf-addr2line -pfiaC -e firmware.elf <pc> <bt...>
struct PanicSummary {
  uint32_t excPc;     // program counter at the exception
  uint32_t excCause;  // raw coredump cause; mask with 0x3f for Xtensa EXCCAUSE
  uint32_t excVaddr;  // faulting data address (for load/store-prohibited)
  uint32_t bt[16];    // backtrace: array of PCs, innermost first
  uint8_t btDepth;    // valid entries in bt[] (<= 16)
  uint8_t btCorrupted;  // bool: the unwinder flagged the backtrace suspect
  char task[16];        // name of the task that crashed
};

// Decode the stored core dump's summary into `out`. Returns true iff a valid
// dump is present and this build can decode it; false on a clean boot (no
// dump), a corrupt/absent image, or a build without the ELF summary API. Reads
// flash + parses ELF, so call from a task context (the web handler), not an
// ISR.
bool readLastPanic(PanicSummary& out);

// Whether a valid core dump is currently stored — i.e. the device has an
// unacknowledged crash on record. Cheap (a flash CRC check, no ELF decode); the
// UI uses it at boot to raise a persistent "go look" marker that survives clean
// reboots and clears when the dump is cleared. Always available.
bool hasPanicDump();

// Erase the stored core dump and resync the crash marker, in one call.
// Callable from any task. Returns true on success, including when nothing was
// stored.
bool clearLastPanicAndResync();

// Main-loop side of the resync raised by clearLastPanicAndResync(). The status
// bar is single-owner (only main.cpp's loop writes Chrome's alert), so a clear
// on any task raises a cross-task request that the loop drains here, re-reading
// hasPanicDump() as the source of truth. Returns true once per pending request;
// main loop only.
bool consumePanicAlertResync();

// Registers the read-only crash-summary route. The route does not require
// authentication because a panic may have damaged or erased the pairing state
// needed to inspect it.
void registerPanicRoute(HttpServer& server);

}  // namespace diagnostics
