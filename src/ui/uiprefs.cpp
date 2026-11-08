// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Remembered user preferences on the littlefs volume. See include/k2/ui/uiprefs.hpp.
#include "k2/core/log.hpp"
#include "k2/ui/uiprefs.hpp"

#include <cstring>

#include "k2/drivers/flash.hpp"
#include "k2/drivers/fs_flash.hpp"
#include "k2/drivers/usb.hpp"

namespace k2::uiprefs {
namespace {

bool g_imported = false;

// One-time import of the pre-littlefs raw page: a valid record there becomes /prefs.cfg.
void import_legacy(fs::Fs& f) {
    if (g_imported) return;
    g_imported = true;
    if (f.exists(kPath)) return;
    std::array<std::uint8_t, 8> r{};
    std::memcpy(r.data(), flash::store_view(kLegacyAddr), r.size());
    if (std::memcmp(r.data(), kMagic.data(), kMagic.size()) != 0) return;
    if (decode(r).qc_after_pd) f.write_file(kPath, r);
}

}  // namespace

Prefs stored() {
    fs::Fs& f = fs::instance();
    import_legacy(f);
    return load(f);
}

bool set(Prefs p) {
    const bool ok = save(fs::instance(), p);
    K2LOG("[PREFS] qc_after_pd=%d screenshot=%d smoothing=%s (%s)\n", p.qc_after_pd ? 1 : 0, p.screenshot ? 1 : 0,
          smoothing::name(p.smoothing), ok ? "saved" : "FLASH ERROR");
    return ok;
}

}  // namespace k2::uiprefs
