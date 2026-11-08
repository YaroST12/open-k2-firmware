// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Host test of the whole network path: the real src/net/net.cpp, lwIP and TinyUSB's dhserver run on the
// host; only the USB driver is faked. A DHCP DISCOVER fed through tud_network_recv_cb() must come
// out of the driver as an OFFER for 192.168.7.2 from 192.168.7.1, and an ARP request for the K2
// must be answered. This is the check that caught the missing netif up / DHCP port bind on hardware.
#include <array>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <vector>

#include "k2/net/net.hpp"
#include "k2/drivers/systick.hpp"
#include "k2/drivers/usb.hpp"
#include "test_util.h"

// ---- fakes for the USB side ----
extern "C" {
std::uint8_t tud_network_mac_address[6] = {0x02, 0x4B, 0x32, 0x00, 0x00, 0x00};
bool tud_ready(void) { return true; }
bool tud_mounted(void) { return true; }
bool tud_network_can_xmit(std::uint16_t) { return true; }
void tud_network_recv_renew(void) {}
}
static std::vector<std::vector<std::uint8_t>> g_sent;
extern "C" std::uint16_t tud_network_xmit_cb(std::uint8_t* dst, void* ref, std::uint16_t);
extern "C" void tud_network_xmit(void* ref, std::uint16_t arg) {
    std::uint8_t buf[1600];
    const std::uint16_t n = tud_network_xmit_cb(buf, ref, arg);
    g_sent.emplace_back(buf, buf + n);
}
namespace k2::systick { std::uint32_t millis() { static std::uint32_t t; return t += 10; } }
namespace k2::usb {
void log_fmt(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap);
}
void log(std::string_view s) { fwrite(s.data(), 1, s.size(), stdout); }
}
extern "C" bool tud_network_recv_cb(const std::uint8_t* src, std::uint16_t size);

static std::uint16_t ip_sum(const std::uint8_t* p, int n) {
    std::uint32_t s = 0;
    for (int i = 0; i < n; i += 2) s += (p[i] << 8) | p[i + 1];
    while (s >> 16) s = (s & 0xFFFF) + (s >> 16);
    return static_cast<std::uint16_t>(~s);
}

constexpr std::uint8_t kHostMac[6] = {0x3C, 0x97, 0x0E, 0x11, 0x22, 0x33};

static std::vector<std::uint8_t> dhcp_discover() {
    std::vector<std::uint8_t> f(14 + 20 + 8 + 240 + 8, 0);
    std::memset(f.data(), 0xFF, 6);
    std::memcpy(&f[6], kHostMac, 6);
    f[12] = 0x08; f[13] = 0x00;
    std::uint8_t* ip = &f[14];
    const std::uint16_t ip_len = static_cast<std::uint16_t>(f.size() - 14);
    ip[0] = 0x45; ip[2] = ip_len >> 8; ip[3] = ip_len & 0xFF; ip[8] = 64; ip[9] = 17;
    std::memset(ip + 16, 0xFF, 4);                                   // 0.0.0.0 -> 255.255.255.255
    const std::uint16_t cs = ip_sum(ip, 20);
    ip[10] = cs >> 8; ip[11] = cs & 0xFF;
    std::uint8_t* udp = ip + 20;
    const std::uint16_t udp_len = static_cast<std::uint16_t>(ip_len - 20);
    udp[0] = 0; udp[1] = 68; udp[2] = 0; udp[3] = 67; udp[4] = udp_len >> 8; udp[5] = udp_len & 0xFF;
    std::uint8_t* d = udp + 8;
    d[0] = 1; d[1] = 1; d[2] = 6; d[4] = 0xDE; d[5] = 0xAD; d[6] = 0xBE; d[7] = 0xEF;
    d[10] = 0x80;                                                    // broadcast flag
    std::memcpy(d + 28, kHostMac, 6);
    d[236] = 99; d[237] = 130; d[238] = 83; d[239] = 99;
    d[240] = 53; d[241] = 1; d[242] = 1; d[243] = 255;
    return f;
}

static std::vector<std::uint8_t> arp_request() {
    std::vector<std::uint8_t> f(42, 0);
    std::memset(f.data(), 0xFF, 6);
    std::memcpy(&f[6], kHostMac, 6);
    f[12] = 0x08; f[13] = 0x06;
    std::uint8_t* a = &f[14];
    a[1] = 1; a[2] = 0x08; a[4] = 6; a[5] = 4; a[7] = 1;
    std::memcpy(a + 8, kHostMac, 6);
    a[14] = 192; a[15] = 168; a[16] = 7; a[17] = 2;                  // sender 192.168.7.2
    a[24] = 192; a[25] = 168; a[26] = 7; a[27] = 1;                  // target 192.168.7.1
    return f;
}

int main() {
    k2::net::init();
    const auto disc = dhcp_discover();
    g_sent.clear();
    tud_network_recv_cb(disc.data(), static_cast<std::uint16_t>(disc.size()));
    k2::net::poll();
    CHECK_EQ(static_cast<int>(g_sent.size()), 1);
    if (!g_sent.empty()) {
        const auto& o = g_sent[0];
        CHECK_EQ(o.size() >= 14 + 20 + 8 + 240 ? 1 : 0, 1);
        if (o.size() >= 14 + 20 + 8 + 240) {
            CHECK_EQ(o[12], 0x08); CHECK_EQ(o[13], 0x00);
            CHECK_EQ(o[14 + 12], 192); CHECK_EQ(o[14 + 15], 1);               // source 192.168.7.1
            const std::uint8_t* d = &o[14 + 20 + 8];
            CHECK_EQ(d[0], 2);                                                // BOOTREPLY
            CHECK_EQ(d[16], 192); CHECK_EQ(d[17], 168); CHECK_EQ(d[18], 7); CHECK_EQ(d[19], 2);   // yiaddr
            CHECK_EQ(d[240], 53); CHECK_EQ(d[242], 2);                        // DHCPOFFER
        }
    }
    g_sent.clear();
    const auto arp = arp_request();
    tud_network_recv_cb(arp.data(), static_cast<std::uint16_t>(arp.size()));
    CHECK_EQ(static_cast<int>(g_sent.size()), 1);
    if (!g_sent.empty()) CHECK_EQ(g_sent[0][12] == 0x08 && g_sent[0][13] == 0x06 && g_sent[0][21] == 2 ? 1 : 0, 1);
    return test_report("test_net_dhcp");
}
extern "C" bool tud_suspended(void) { return false; }
