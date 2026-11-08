// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// USB network link: lwIP (NO_SYS) over TinyUSB's CDC-NCM class driver, see k2/net/net.hpp.
//
// Everything runs in one context: tud_task() (main loop) invokes tud_network_recv_cb(), which
// feeds lwIP; lwIP answers through linkoutput(), which hands the frame back to the driver.
// Nothing here is called from an ISR, so the counters need no atomics.
#include "k2/core/log.hpp"
#include "k2/net/net.hpp"

#include <algorithm>
#include <array>
#include <cstdint>

#include "k2/drivers/mode_ram.hpp"
#include "k2/net/net_frame.hpp"
#include "k2/drivers/systick.hpp"
#include "k2/drivers/usb.hpp"
#include "tusb.h"

#include "lwip/etharp.h"
#include "lwip/init.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/timeouts.h"
#include "lwip/udp.h"
#include "netif/ethernet.h"

#include "dhserver.h"

// The host-side MAC is defined with the descriptors (src/drivers/usb_descriptors.cpp); the device side of
// the link uses the same address with the low bit flipped.
extern "C" std::uint8_t tud_network_mac_address[6];

namespace k2::net {
namespace {

using std::uint32_t;

// One counter per hop, so a single [NET] log shows where frames stop:
// USB OUT -> recv_cb -> lwIP netif input -> dhserver -> linkoutput -> USB IN
struct Counters {
    uint32_t rx_cb;                       // tud_network_recv_cb() calls, including empty ones
    uint32_t rx_zero;                     // ... with size 0 (RNDIS parse failure)
    uint32_t rx_alloc;                    // pbuf_alloc failures
    uint32_t rx_input_err;                // netif->input() != ERR_OK
    uint32_t rx_frames;                   // frames handed to lwIP
    uint32_t rx_arp, rx_icmp, rx_dhcp;    // classified host frames
    uint32_t tx_frames;                   // frames handed to the USB driver
    uint32_t tx_busy;                     // dropped: driver still sending the previous frame
    uint32_t tx_notready;                 // linkoutput while !tud_ready()
    uint32_t tx_arp, tx_icmp, tx_dhcp;    // classified K2 frames
    uint32_t dh_sendto;                   // udp_sendto() calls made by dhserver
    int32_t dh_sendto_err;                // last non-OK err_t from udp_sendto()
    int32_t dh_init = -99;                // dhserv_init() result
};
constinit Counters c{};

constexpr ip4_addr_t ip4(std::uint8_t a, std::uint8_t b, std::uint8_t cc, std::uint8_t d) {
    return {PP_HTONL(LWIP_MAKEU32(a, b, cc, d))};
}

constexpr ip4_addr_t kIp = ip4(192, 168, 7, 1);
constexpr ip4_addr_t kNetmask = ip4(255, 255, 255, 0);
constexpr ip4_addr_t kGateway = ip4(0, 0, 0, 0);
constexpr uint32_t kLease = 24 * 60 * 60;  // seconds

constinit dhcp_entry_t dhcp_entries[] = {
    {{0}, ip4(192, 168, 7, 2), kLease},
    {{0}, ip4(192, 168, 7, 3), kLease},
};

// router and dns left at 0.0.0.0: dhserver then omits both options, so the host gets an
// on-link /24 and no default route or resolver from the K2.
const dhcp_config_t dhcp_config = {
    .router = ip4(0, 0, 0, 0),
    .port = 67,
    .dns = ip4(0, 0, 0, 0),
    .domain = "",
    .num_entry = static_cast<int>(std::size(dhcp_entries)),
    .entries = dhcp_entries,
};

constinit netif k_netif{};

void count(std::span<const std::uint8_t> frame, bool from_k2) {
    using K = FrameKind;
    switch (classify(frame, from_k2)) {
        case K::Arp:  (from_k2 ? c.tx_arp : c.rx_arp)++; break;
        case K::Icmp: (from_k2 ? c.tx_icmp : c.rx_icmp)++; break;
        case K::Dhcp: (from_k2 ? c.tx_dhcp : c.rx_dhcp)++; break;
        case K::Other: break;
    }
}

err_t linkoutput(netif*, pbuf* p) {
    if (!tud_ready()) {
        c.tx_notready++;
        return ERR_USE;
    }
    // No tud_task() here: this runs inside a tud_task() callback. If the driver is still
    // sending the previous frame the frame is dropped; ARP and DHCP peers retry.
    if (!tud_network_can_xmit(p->tot_len)) {
        c.tx_busy++;
        return ERR_USE;
    }
    std::array<std::uint8_t, 40> head;
    const auto n = pbuf_copy_partial(p, head.data(), head.size(), 0);
    count({head.data(), n}, true);
    tud_network_xmit(p, 0);
    c.tx_frames++;
    return ERR_OK;
}

err_t netif_setup(netif* nif) {
    nif->mtu = CFG_TUD_NET_MTU - 14;  // driver buffer holds the Ethernet header too
    nif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_LINK_UP | NETIF_FLAG_UP;
    nif->state = nullptr;
    nif->name[0] = 'K';
    nif->name[1] = '2';
    nif->linkoutput = linkoutput;
    nif->output = etharp_output;
    return ERR_OK;
}

}  // namespace

void init() {
    if (!mode_ram::claim(mode_ram::Tenant::Net)) {   // lwIP's pools share RAM with the drive (k2/drivers/mode_ram.hpp)
        K2LOG("[NET] shared RAM held by another USB function, network off\n");
        return;
    }
    lwip_init();

    k_netif.hwaddr_len = sizeof(tud_network_mac_address);
    std::copy(std::begin(tud_network_mac_address), std::end(tud_network_mac_address), k_netif.hwaddr);
    k_netif.hwaddr[5] ^= 0x01;

    netif_add(&k_netif, &kIp, &kNetmask, &kGateway, nullptr, netif_setup, ethernet_input);
    netif_set_default(&k_netif);  // lwIP-internal only: no gateway is offered to the host
    netif_set_up(&k_netif);

    c.dh_init = dhserv_init(&dhcp_config);
    K2LOG("[NET] lwIP up 192.168.7.1/24, dhserv=%d mtu=%d\n", int(c.dh_init), int(k_netif.mtu));
}

void poll() { sys_check_timeouts(); }

// Four short lines, one 64-byte log slot each, per heartbeat.
void log_stats() {
    K2LOG("[NET] rx=%d zero=%d afail=%d inerr=%d\n", int(c.rx_cb), int(c.rx_zero), int(c.rx_alloc),
                 int(c.rx_input_err));
    K2LOG("[NET] rx arp=%d icmp=%d dhcp=%d\n", int(c.rx_arp), int(c.rx_icmp), int(c.rx_dhcp));
    K2LOG("[NET] tx=%d busy=%d nrdy=%d arp=%d icmp=%d dhcp=%d\n", int(c.tx_frames), int(c.tx_busy),
                 int(c.tx_notready), int(c.tx_arp), int(c.tx_icmp), int(c.tx_dhcp));
    K2LOG("[NET] dhinit=%d send=%d err=%d if=%x cfg=%d\n", int(c.dh_init), int(c.dh_sendto),
                 int(c.dh_sendto_err), unsigned(k_netif.flags), int(tud_mounted()));
}

}  // namespace k2::net

// ---- Callbacks looked up by name (C linkage) ----
extern "C" {

// Linked with -Wl,--wrap=udp_sendto: sees every reply dhserver tries to send.
err_t __real_udp_sendto(udp_pcb* pcb, pbuf* p, const ip_addr_t* dst, u16_t port);
err_t __wrap_udp_sendto(udp_pcb* pcb, pbuf* p, const ip_addr_t* dst, u16_t port) {
    using namespace k2::net;
    c.dh_sendto++;
    const err_t e = __real_udp_sendto(pcb, p, dst, port);
    if (e != ERR_OK) c.dh_sendto_err = e;
    return e;
}

// lwIP random source and time base (the SysTick millisecond counter).
std::uint32_t net_rand(void) {
    static std::uint32_t state;
    state = state * 1664525u + 1013904223u + k2::systick::millis();
    return state;
}

std::uint32_t sys_now(void) { return k2::systick::millis(); }

// lwIP's netif_find() parses the interface number with atoi(). The newlib one drags strtol, the
// stdio file table and malloc into the image (the gate forbids a heap), so a decimal-only
// replacement is linked instead; netif_find() itself is never called and is garbage collected.
int atoi(const char* s) {
    int v = 0;
    while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
    return v;
}

void tud_network_init_cb(void) {}

bool tud_network_recv_cb(const std::uint8_t* src, std::uint16_t size) {
    using namespace k2::net;
    c.rx_cb++;
    if (size == 0) {
        // Empty datagram from the driver: the OUT endpoint must be re-armed or the data
        // pipe stalls for good.
        c.rx_zero++;
        tud_network_recv_renew();
        return true;
    }
    count({src, size}, false);
    pbuf* p = pbuf_alloc(PBUF_RAW, size, PBUF_POOL);
    if (!p) {
        c.rx_alloc++;
        return false;  // the driver re-arms the endpoint itself
    }
    pbuf_take(p, src, size);
    c.rx_frames++;
    if (k_netif.input(p, &k_netif) != ERR_OK) {
        c.rx_input_err++;
        pbuf_free(p);
    }
    tud_network_recv_renew();
    return true;
}

std::uint16_t tud_network_xmit_cb(std::uint8_t* dst, void* ref, std::uint16_t) {
    const auto* p = static_cast<pbuf*>(ref);
    return pbuf_copy_partial(p, dst, p->tot_len, 0);
}

}  // extern "C"
