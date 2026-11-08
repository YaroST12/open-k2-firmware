// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// How to read a cable's e-marker, chosen from the link state (pure, host-tested in
// tests/test_fusb302_scan.cpp).
//
//  Vconn - nothing supplies VBUS: the K2 powers the cable (VCONN on the unused CC, PC13 high), drops
//          Rd and sends SOP' Discover Identity itself.
//  Sniff - a source is attached (VBUS present): it is the VCONN source and talks SOP' to the cable
//          at attach (it must Discover Identity SOP' before offering more than 3 A or 240 W EPR).
//          The K2 only listens: Rd stays on, CC switches and BMC routing stay untouched, SOP'
//          receive stays enabled. Injecting would remove Rd, the source would see a detach, drop
//          VBUS and VCONN, and the cable would never answer.
#pragma once
#include "k2/pd/pd_link.hpp"

namespace k2::pd {

enum class EmarkerMode : unsigned char { Vconn, Sniff };

constexpr EmarkerMode emarker_mode(bool attached, bool vbus_known, std::int32_t vbus_mv) {
    if (!attached) return EmarkerMode::Vconn;
    if (vbus_known && vbus_mv < kVSafe0Mv) return EmarkerMode::Vconn;   // below vSafe0V: no source
    return EmarkerMode::Sniff;   // VBUS present, or unknown while attached: never fight a source
}

}  // namespace k2::pd
