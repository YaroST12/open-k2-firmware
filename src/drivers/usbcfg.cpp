// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Remembered USB link setting on the littlefs volume. See include/k2/drivers/usbcfg.hpp.
#include "k2/core/log.hpp"
#include "k2/drivers/usbcfg.hpp"

#include <cstring>

#include "k2/drivers/flash.hpp"
#include "k2/drivers/fs_flash.hpp"
#include "k2/drivers/usb.hpp"

namespace k2::usbcfg {

// One-time import of the pre-littlefs raw page: a valid record there becomes /usb.cfg, so the USB link
// setting survives the move (without it USB would default to off and the remote would be unreachable).
static void import_legacy(fs::Fs& f) {
    if (f.exists(kPath)) return;
    std::array<std::uint8_t, 8> r{};
    std::memcpy(r.data(), flash::store_view(kLegacyAddr), r.size());
    if (std::memcmp(r.data(), kMagic.data(), kMagic.size()) != 0) return;
    if (any(decode(r))) f.write_file(kPath, r);
}

#if !K2_NET
// This build has no network function (K2_NET=0): a stored HID+Net record runs as the rest of it. The record
// stays on flash untouched, so a K2_NET=1 build reads it again.
static void note_net_dropped(fs::Fs& f) {
    std::array<std::uint8_t, 8> r{};
    std::size_t n = 0;
    if (f.read_file(kPath, r, n) != fs::Err::Ok || n != r.size() || !requests_net(r)) return;
    K2LOG("[USBCFG] net not built (K2_NET=0): stored %s runs as %s\n", describe(decode(r, true)), describe(decode(r)));
}
#endif

void init() {
    fs::Fs& f = fs::instance();
    import_legacy(f);
#if !K2_NET
    note_net_dropped(f);
#endif
    current = with_shell(load(f));
}

Setting stored() { return load(fs::instance()); }

bool set(Setting s) {
    const bool ok = save(fs::instance(), s);
    K2LOG("[USBCFG] next boot: %s (%s)\n", describe(s), ok ? "saved" : "FLASH ERROR");
    return ok;
}

}  // namespace k2::usbcfg
