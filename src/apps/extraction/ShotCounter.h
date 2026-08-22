// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <cstdint>

namespace pump_scale {

// Stores the cumulative shot count displayed in the status bar. It is
// independent of the shot-history archive and can be reset separately.
// All operations run on the main task.
class ShotCounter {
 public:
  bool load();

  uint64_t value() const { return _value; }

  // The visible count advances for the current session even when the NVS write
  // fails. A later accepted shot retries with the accumulated value.
  bool increment();

  // The visible count changes only after NVS confirms the reset.
  bool reset();

 private:
  uint64_t _value = 0;
  bool _loaded = false;
};

}  // namespace pump_scale
