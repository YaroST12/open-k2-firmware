// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// One-time cleanup of the old raw-store pages (k2/core/legacy_proof.hpp): erases exactly the sectors that carry the
// signature of one of this project's old stores and nothing else. Other content in the same address range is
// left alone, byte for byte, and the kept top page (flash::kKeptPages) is passed over even with a signature on it.
//
// Descending address order. The log snapshot's second sector is proven by the first one, so it must be erased
// first: a power cut then leaves the first sector's header in place and the next run finishes the job.
// Stops at the first failed erase; the next boot tries again (the pages are still proven, so nothing is lost).
#pragma once
#include <cstdint>

#include "k2/drivers/flash.hpp"
#include "k2/core/legacy_proof.hpp"

namespace k2::legacy {

struct WipeResult {
    flash::Status err = flash::Status::Ok;
    std::uint32_t erased = 0;      // sectors erased
    std::uint32_t failed_at = 0;   // address of the sector that failed, 0 = none
};

// Env: `std::uint16_t read16(std::uint32_t addr) const` and `flash::Status erase(std::uint32_t sector)`.
// The erase is expected to apply the same guard (flash::store_erase_guarded), so even a wrong proof here
// cannot reach a page that is not ours.
template <class Env>
WipeResult wipe(Env& env) {
    WipeResult r;
    auto rd = [&env](std::uint32_t a) -> std::uint16_t { return env.read16(a); };
    for (std::uint32_t a = kEnd; a > kBase;) {
        a -= kSector;
        if (flash::kept_page(a) || !ours(a, rd)) continue;   // the kept top page is never erased, whatever it holds
        const flash::Status st = env.erase(a);
        if (st != flash::Status::Ok) {
            r.err = st;
            r.failed_at = a;
            return r;
        }
        r.erased++;
    }
    return r;
}

}  // namespace k2::legacy
