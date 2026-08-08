// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

class HttpServer;

// Exposes each runtime log at a time under /sys/diagnostics?log=<name>, plus a
// per-log clear.
//
// Register once at boot; the routes require auth like the rest of /sys via
// the server's middleware.
void registerDiagnosticsRoutes(HttpServer& server);
