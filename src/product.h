// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

// Product strings shared by the device interface and network services. Keeping
// them here prevents general-purpose UI and networking code from hard-coding
// the product name.
//
// The browser interface defines its strings separately under web-src/.
namespace product {

// Human-facing product title used in headings and browser titles.
constexpr const char* PRODUCT_NAME = "Pump Bug";

// Prefix for the MAC-derived access point name and default mDNS hostname. It
// must contain only lowercase letters, digits, and hyphens. For example, this
// prefix can produce `pumpbug-a1b2c3`.
constexpr const char* NET_PREFIX = "pumpbug";

}  // namespace product
