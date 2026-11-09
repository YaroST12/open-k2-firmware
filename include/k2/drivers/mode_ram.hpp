// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// RAM shared by the USB functions that never run in the same session. The USB mode is fixed at
// boot (k2/drivers/usbcfg.hpp) and normalize() makes Net and Drive exclusive, so the linker places their
// buffers at the same address (OVERLAY .mode_net / .mode_drive in link.ld):
//
//   .mode_net    lwIP heap and pools, the NCM endpoint buffers, the DHCP server, the HTTP server
//                (selected by object file in link.ld; no source attribute needed). Empty when the
//                firmware is built without K2_NET (the default), so the overlay is as big as .mode_sniff
//   .mode_drive  objects marked K2_MODE_RAM_DRIVE (the MSC drive image)
//   .mode_sniff  objects marked K2_MODE_RAM_SNIFF (the D+/D- edge buffer, claimed when a capture arms:
//                refused while Net or Drive owns the region, i.e. it works in HID-only mode)
//   .mode_trans  objects marked K2_MODE_RAM_TRANS (the transition-capture ring, k2/pd/transcap.hpp). Unlike the
//                others this tenant gives the region back (release()), so a later sniff can take it; the
//                region is zeroed on release. Net and Drive claim at boot, before it can ask.
//
// The region sits inside the range Reset_Handler zeroes, so the owner starts from zeroed memory.
// Objects in .mode_drive carry no initializer: the owner constructs them after claim().
// Every tenant calls claim() before it touches its buffers; a second tenant is refused, which
// turns a configuration bug into a log line instead of silently corrupted pools.
#pragma once
#include <cstdint>

#define K2_MODE_RAM_DRIVE __attribute__((section(".bss.mode_drive")))
#define K2_MODE_RAM_SNIFF __attribute__((section(".bss.mode_sniff")))
#define K2_MODE_RAM_TRANS __attribute__((section(".bss.mode_trans")))

namespace k2::mode_ram {

enum class Tenant : std::uint8_t { None, Net, Drive, Sniff, Trans };

class Lease {
public:
    // True when the region was free or already held by `t`; it then belongs to `t` for the session.
    constexpr bool claim(Tenant t) {
        if (t == Tenant::None) return false;
        if (owner_ == Tenant::None) owner_ = t;
        return owner_ == t;
    }
    constexpr Tenant owner() const { return owner_; }
    // Gives the region back. Only the owner can, and only a tenant that cleans up after itself should.
    constexpr bool release(Tenant t) {
        if (t == Tenant::None || owner_ != t) return false;
        owner_ = Tenant::None;
        return true;
    }

private:
    Tenant owner_ = Tenant::None;
};

// The session's lease (firmware only).
bool claim(Tenant t);
// Releases `t`'s claim (the region is zeroed first); false when `t` does not own it.
bool release(Tenant t);
// Who owns the region now (Tenant::None when free), so a buffer placed in one tenant's section is only
// touched while that tenant owns the region.
Tenant owner();

}  // namespace k2::mode_ram
