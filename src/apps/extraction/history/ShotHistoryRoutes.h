// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include "ShotStore.h"

class WebServer;

namespace pump_scale {

// HTTP adapter for GET /app/extraction/shots. ShotStore owns persistence,
// paging, and validation; this layer owns query parsing, ETags, and response
// streaming.
void handleShotHistory(WebServer& server, ShotStore& store);

}  // namespace pump_scale
