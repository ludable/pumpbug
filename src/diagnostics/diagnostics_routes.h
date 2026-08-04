// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

class HttpServer;

// Exposes the runtime event logs over HTTP under /sys/diagnostics: a single
// combined, newest-first JSON snapshot of all four logs (the in-RAM
// pump/extraction/net rings plus the persistent power log), and a per-log clear
// that mirrors the device's long-press clear.
//
// The combined GET carries a weak ETag derived from the logs' monotonic write
// counters, so a polling "tail" client revalidates with If-None-Match and gets
// a 304 on every poll where nothing changed — the body (a few KB) only goes out
// on the rare poll that actually advanced a log.
//
// Register once at boot; the routes require auth like the rest of /sys via
// the server's middleware.
void registerDiagnosticsRoutes(HttpServer& server);
