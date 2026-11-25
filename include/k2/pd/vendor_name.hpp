// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// A decoded vendor name held by value: the table stores names packed (k2/pd/vendor_pack.hpp), so a lookup fills a
// buffer of its own and two names alive at once never share storage. Table names are capitals.
#pragma once
#include <cstddef>
#include <string_view>

#include "k2/core/strpack.hpp"

namespace k2::pd {

inline constexpr std::size_t kVendorMax = 16;   // characters of a table name (tools/gen_vendors.py NAME_MAX)

struct VendorName {
    char s[kVendorMax + 1] = {};

    constexpr VendorName() = default;
    // From a literal or any text; cut at kVendorMax, null reads as empty.
    constexpr VendorName(const char* p) {   // NOLINT: implicit, so a fixture can say id.vendor = "Apple"
        if !consteval {
            if (strpack::is_packed(p)) { strpack::unpack_rt(p, s, sizeof s); return; }
        }
        for (std::size_t i = 0; p && p[i] && i < kVendorMax; i++) s[i] = p[i];
    }
    constexpr const char* c_str() const { return s; }
    constexpr bool empty() const { return s[0] == 0; }
    constexpr explicit operator bool() const { return s[0] != 0; }
    std::string_view view() const { return s; }
};

}  // namespace k2::pd
