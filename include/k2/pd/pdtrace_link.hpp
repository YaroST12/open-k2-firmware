// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The link that was actually negotiated, read from sniffed PD messages only (passive: nothing is
// sent). The K2 sees the CC wire, not the data pairs, so these are what the two ends agreed to,
// not a measurement of the data link:
//   Enter_USB (data message 8)            USB mode, cable speed / type / current
//   Enter Mode ACK, SVID 0x8087           Thunderbolt 3, with the speed / active flag of the request
//   Enter Mode ACK, SVID 0xFF01           DisplayPort alt mode; Configure ACK gives the pin assignment
//   Discover Identity ACK on SOP'         e-marker cable: speed and VBUS current
//   Discover Identity ACK on SOP          the partner's USB host / device capability (ID header)
// Pure code over record buffers, host-tested in tests/test_pdtrace_link.cpp.
#pragma once
#include <cstddef>
#include <cstdint>
#include <span>

#include "k2/pd/pdtrace_view.hpp"

namespace k2::pdtrace::view {

struct Link {
    bool eusb = false;                    // an Enter_USB was seen
    std::uint8_t usb_mode = 0;            // EUDO bits 30:28: 0 USB2, 1 USB3.2, 2 USB4
    std::uint8_t usb_speed = 0;           // EUDO bits 23:21 (cable speed): 0 USB2, 1 Gen1, 2 Gen2, 3 Gen3, 4 Gen4
    std::uint8_t usb_ctype = 0;           // EUDO bits 20:19: 0 passive, 1 retimer, 2 redriver, 3 optical
    std::uint8_t usb_cur = 0;             // EUDO bits 18:17: 2 = 3 A, 3 = 5 A
    bool tbt = false, tbt_active = false; // Enter Mode for 0x8087 acknowledged; request VDO bit 24
    std::uint8_t tbt_speed = 0;           // request VDO bits 18:16 (3 = 10 and 20 Gbit/s lanes, 40 Gbit/s link)
    bool dp = false;                      // Enter Mode for 0xFF01 acknowledged
    char dp_pin = 0;                      // 'A'..'F' once Configure was acknowledged
    bool cable = false;                   // cable data known (from the e-marker, else from Enter_USB)
    bool cable_emarker = false;           // ... and it came from the e-marker
    bool cable_active = false;
    std::uint8_t cable_speed = 0;         // as usb_speed
    std::uint8_t cable_amps = 0;          // 0 unknown, 3, 5
    std::uint8_t cable_ctype = 0;         // as usb_ctype (Enter_USB only)
    bool peer = false, peer_host = false, peer_dev = false;   // Discover Identity USB capability
    bool peer_sink = false;               // ... answered by a sink-role sender (preferred over the source's)

    bool any() const { return eusb || tbt || dp || cable || peer; }
};

// Feeds records (or single messages) in time order.
class LinkAcc {
public:
    void feed(std::span<const std::uint8_t> records);
    // One Rx payload (token, header, objects, CRC) or Tx payload (header, objects).
    void feed_message(bool rx, std::span<const std::uint8_t> payload);
    void reset() { *this = LinkAcc(); }
    const Link& link() const { return l_; }

private:
    Link l_;
    bool tbt_req_ = false, tbt_req_active_ = false, dp_req_ = false;
    std::uint8_t tbt_req_speed_ = 0;
    char dp_req_pin_ = 0;
};

Link link_of(std::span<const std::uint8_t> records);

// "USB4 Gen3 40G | TBT3 40G act | DP alt C | cable 40G 5A | partner USB host+dev"; "not seen" when
// nothing was. Returns the length.
std::size_t format_link(const Link& l, std::span<char> out);
// The parts as separate strings (empty = not seen): usb, tbt, dp ("C"), cable, peer ("host+dev").
struct LinkText {
    char usb[20] = {}, tbt[16] = {}, dp[8] = {}, cable[24] = {}, peer[12] = {};
};
LinkText link_text(const Link& l);
// One fixed-width line for the drive file: {"link":{"usb":"..","tbt":"..","dp":"C","cable":"..","peer":".."}}
// with the parts that were seen, space padded to kJsonLine with a newline last.
void json_link(const Link& l, std::span<char, kJsonLine> out);

}  // namespace k2::pdtrace::view
