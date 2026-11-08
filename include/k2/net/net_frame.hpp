// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Ethernet frame classifier for the [NET] debug counters. Pure and header-only so the host
// tests can build it without lwIP.
#pragma once
#include <cstddef>
#include <cstdint>
#include <span>

namespace k2::net {

enum class FrameKind : std::uint8_t { Other, Arp, Icmp, Dhcp };

// Classify an Ethernet II frame; only the first 38 bytes are needed. `from_k2` selects the
// DHCP direction: replies leave the K2 from UDP port 67, requests reach it on port 67.
constexpr FrameKind classify(std::span<const std::uint8_t> f, bool from_k2) {
    constexpr std::size_t kEthHdr = 14;
    if (f.size() < kEthHdr) return FrameKind::Other;

    const unsigned type = (f[12] << 8) | f[13];
    if (type == 0x0806) return FrameKind::Arp;
    if (type != 0x0800 || f.size() < 38) return FrameKind::Other;

    const std::uint8_t proto = f[23];
    if (proto == 1) return FrameKind::Icmp;
    if (proto != 17) return FrameKind::Other;

    const std::size_t udp = kEthHdr + (f[14] & 0x0F) * 4u;  // IPv4 header length varies with options
    if (f.size() < udp + 4) return FrameKind::Other;
    const unsigned sport = (f[udp] << 8) | f[udp + 1];
    const unsigned dport = (f[udp + 2] << 8) | f[udp + 3];
    return (from_k2 ? sport : dport) == 67 ? FrameKind::Dhcp : FrameKind::Other;
}

}  // namespace k2::net
