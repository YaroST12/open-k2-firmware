// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Host tests for k2::net::classify, the Ethernet frame classifier behind the [NET] counters.
// Frames are built by hand (Ethernet II + IPv4 + UDP) so the offsets are checked independently.
#include <array>
#include <cstring>

#include "k2/net/net_frame.hpp"
#include "test_util.h"

using k2::net::FrameKind;
using k2::net::classify;

// Ethernet header with the given EtherType, zero-padded to `len` bytes.
static std::array<uint8_t, 64> frame(uint16_t type) {
    std::array<uint8_t, 64> f{};
    f[12] = type >> 8;
    f[13] = type & 0xFF;
    return f;
}

static std::array<uint8_t, 64> udp(uint8_t ihl_words, uint16_t sport, uint16_t dport) {
    auto f = frame(0x0800);
    f[14] = 0x40 | ihl_words;
    f[23] = 17;
    const std::size_t u = 14 + ihl_words * 4u;
    f[u] = sport >> 8, f[u + 1] = sport & 0xFF, f[u + 2] = dport >> 8, f[u + 3] = dport & 0xFF;
    return f;
}

int main() {
    // Too short for an Ethernet header.
    CHECK_EQ(classify(std::span<const uint8_t>(frame(0x0806).data(), 13), false), FrameKind::Other);

    // ARP needs only the 14-byte header.
    CHECK_EQ(classify(std::span<const uint8_t>(frame(0x0806).data(), 14), false), FrameKind::Arp);
    CHECK_EQ(classify(frame(0x0806), true), FrameKind::Arp);

    // ICMP: IPv4 protocol 1, needs 38 bytes.
    auto icmp = frame(0x0800);
    icmp[14] = 0x45;
    icmp[23] = 1;
    CHECK_EQ(classify(icmp, false), FrameKind::Icmp);
    CHECK_EQ(classify(std::span<const uint8_t>(icmp.data(), 37), false), FrameKind::Other);

    // DHCP: UDP; host frames go to port 67, K2 replies come from port 67.
    CHECK_EQ(classify(udp(5, 68, 67), false), FrameKind::Dhcp);
    CHECK_EQ(classify(udp(5, 67, 68), true), FrameKind::Dhcp);
    CHECK_EQ(classify(udp(5, 68, 67), true), FrameKind::Other);   // wrong direction
    CHECK_EQ(classify(udp(5, 67, 68), false), FrameKind::Other);
    CHECK_EQ(classify(udp(5, 1234, 5678), false), FrameKind::Other);

    // IPv4 options move the UDP header (IHL 6 = 24 bytes).
    CHECK_EQ(classify(udp(6, 68, 67), false), FrameKind::Dhcp);
    // UDP header cut off by the frame end (len < 14 + ihl + 4).
    CHECK_EQ(classify(std::span<const uint8_t>(udp(5, 68, 67).data(), 38), false), FrameKind::Dhcp);
    CHECK_EQ(classify(std::span<const uint8_t>(udp(6, 68, 67).data(), 41), false), FrameKind::Other);
    CHECK_EQ(classify(std::span<const uint8_t>(udp(6, 68, 67).data(), 42), false), FrameKind::Dhcp);

    // Other EtherTypes and IP protocols.
    CHECK_EQ(classify(frame(0x86DD), false), FrameKind::Other);
    auto tcp = frame(0x0800);
    tcp[14] = 0x45;
    tcp[23] = 6;
    CHECK_EQ(classify(tcp, false), FrameKind::Other);

    return test_report("test_net_frame");
}
