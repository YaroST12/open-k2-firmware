// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#include "k2/pd/pdtrace_link.hpp"

#include "k2/pd/pdtrace.hpp"

namespace k2::pdtrace::view {
namespace {

constexpr std::uint32_t rd32(const std::uint8_t* p) {
    return p[0] | p[1] << 8 | p[2] << 16 | std::uint32_t(p[3]) << 24;
}

constexpr std::uint16_t kSvidTbt = 0x8087, kSvidDp = 0xFF01, kSvidPd = 0xFF00;
constexpr unsigned kCmdIdentity = 1, kCmdEnter = 4, kCmdDpConfigure = 17;

struct Cursor {
    std::span<char> out;
    std::size_t n = 0;
    void put(char c) { if (n < out.size()) out[n++] = c; }
    void put(const char* s) { while (*s) put(*s++); }
    void num(unsigned v) {
        char t[10];
        int k = 0;
        do { t[k++] = char('0' + v % 10); v /= 10; } while (v);
        while (k) put(t[--k]);
    }
};

const char* speed_g(unsigned code) {   // cable speed code to the link rate of a full-speed cable
    switch (code) {
    case 0: return "USB2";
    case 1: return "5G";
    case 2: return "10G";
    case 3: return "40G";
    case 4: return "80G";
    default: return nullptr;
    }
}

void copy(char* dst, std::size_t cap, Cursor& c) {
    std::size_t i = 0;
    for (; i < c.n && i + 1 < cap; i++) dst[i] = c.out[i];
    dst[i] = '\0';
}
}  // namespace

void LinkAcc::feed_message(bool rx, std::span<const std::uint8_t> payload) {
    const std::size_t skip = rx ? 1 : 0;
    if (payload.size() < skip + 2) return;
    const std::uint8_t token = rx ? payload[0] : 0xE0;
    const unsigned hdr = payload[skip] | payload[skip + 1] << 8;
    const unsigned n = hdr >> 12 & 7;
    if (!n || (hdr >> 15 & 1) || payload.size() < skip + 2 + 4 * n) return;
    const std::uint8_t* o = &payload[skip + 2];
    const unsigned type = hdr & 0x1F;
    const bool sop = (token & 0xE0) == 0xE0, sop1 = (token & 0xE0) == 0xC0;

    if (type == 8 && sop && !l_.eusb) {   // Enter_USB: the data object is the EUDO
        const std::uint32_t d = rd32(o);
        l_.eusb = true;
        l_.usb_mode = d >> 28 & 7;
        l_.usb_speed = d >> 21 & 7;
        l_.usb_ctype = d >> 19 & 3;
        l_.usb_cur = d >> 17 & 3;
        if (!l_.cable_emarker) {
            l_.cable = true;
            l_.cable_speed = l_.usb_speed;
            l_.cable_ctype = l_.usb_ctype;
            l_.cable_active = l_.usb_ctype != 0;
            l_.cable_amps = l_.usb_cur == 3 ? 5 : l_.usb_cur == 2 ? 3 : 0;
        }
        return;
    }
    if (type != 15) return;
    const std::uint32_t vdm = rd32(o);
    if (!(vdm >> 15 & 1)) return;   // unstructured
    const unsigned svid = vdm >> 16, ctype = vdm >> 6 & 3, cmd = vdm & 0x1F;

    if (svid == kSvidPd && cmd == kCmdIdentity && ctype == 1 && rx) {
        if (sop1 && n >= 5) {                    // e-marker: ID header product type, cable VDO 1
            const std::uint32_t idh = rd32(o + 4), c = rd32(o + 16);
            l_.cable = l_.cable_emarker = true;
            l_.cable_active = (idh >> 27 & 7) == 4;
            l_.cable_speed = c & 7;
            const unsigned amps = c >> 5 & 3;
            l_.cable_amps = amps == 2 ? 5 : amps == 1 ? 3 : 0;
            l_.cable_ctype = 0;
        } else if (sop && n >= 2) {              // partner: USB communications capable as host / device
            const std::uint32_t idh = rd32(o + 4);
            const bool sink = !(hdr >> 8 & 1);
            if (!l_.peer || (sink && !l_.peer_sink)) {
                l_.peer = true;
                l_.peer_sink = sink;
                l_.peer_host = idh >> 31 & 1;
                l_.peer_dev = idh >> 30 & 1;
            }
        }
        return;
    }
    if (!sop) return;
    if (cmd == kCmdEnter && svid == kSvidTbt) {
        if (ctype == 0) {
            tbt_req_ = true;
            const std::uint32_t v = n >= 2 ? rd32(o + 4) : 0;
            tbt_req_speed_ = v >> 16 & 7;
            tbt_req_active_ = v >> 24 & 1;
        } else if (ctype == 1 && tbt_req_) {
            l_.tbt = true;
            l_.tbt_speed = tbt_req_speed_;
            l_.tbt_active = tbt_req_active_;
        }
    } else if (svid == kSvidDp && cmd == kCmdEnter) {
        if (ctype == 1) l_.dp = true;
    } else if (svid == kSvidDp && cmd == kCmdDpConfigure) {
        if (ctype == 0 && n >= 2) {
            const unsigned pins = rd32(o + 4) >> 8 & 0xFF;
            dp_req_ = false;
            for (unsigned b = 0; b < 6; b++) {
                if (pins >> b & 1) { dp_req_pin_ = char('A' + b); dp_req_ = true; break; }
            }
        } else if (ctype == 1 && dp_req_) {
            l_.dp = true;
            l_.dp_pin = dp_req_pin_;
        }
    }
}

void LinkAcc::feed(std::span<const std::uint8_t> rec) {
    std::size_t i = 0;
    while (i < rec.size() && rec[i] >= kHeader && i + rec[i] <= rec.size()) {
        const Type t = Type(rec[i + 1]);
        if (t == Type::Rx || t == Type::Tx)
            feed_message(t == Type::Rx, rec.subspan(i + kHeader, rec[i] - kHeader));
        i += rec[i];
    }
}

Link link_of(std::span<const std::uint8_t> records) {
    LinkAcc a;
    a.feed(records);
    return a.link();
}

LinkText link_text(const Link& l) {
    LinkText t;
    char b[24];
    if (l.eusb) {
        Cursor c{b};
        switch (l.usb_mode) {
        case 0: c.put("USB2"); break;
        case 1:
            c.put("USB3.2");
            if (l.usb_speed == 1) c.put(" Gen1 5G");
            else if (l.usb_speed == 2) c.put(" Gen2 10G");
            break;
        case 2:
            c.put("USB4");
            if (l.usb_speed == 2) c.put(" Gen2 20G");
            else if (l.usb_speed == 3) c.put(" Gen3 40G");
            else if (l.usb_speed == 4) c.put(" Gen4 80G");
            break;
        default: c.put("USB?"); break;
        }
        copy(t.usb, sizeof t.usb, c);
    }
    if (l.tbt) {
        Cursor c{b};
        c.put("TBT3");
        if (l.tbt_speed == 3) c.put(" 40G");
        if (l.tbt_active) c.put(" act");
        copy(t.tbt, sizeof t.tbt, c);
    }
    if (l.dp_pin) { t.dp[0] = l.dp_pin; t.dp[1] = '\0'; }
    if (l.cable) {
        Cursor c{b};
        if (const char* s = speed_g(l.cable_speed)) c.put(s); else c.put('?');
        if (l.cable_amps) { c.put(' '); c.num(l.cable_amps); c.put('A'); }
        if (l.cable_emarker) { if (l.cable_active) c.put(" act"); }
        else if (l.cable_ctype == 1) c.put(" retimer");
        else if (l.cable_ctype == 2) c.put(" redriver");
        else if (l.cable_ctype == 3) c.put(" optical");
        copy(t.cable, sizeof t.cable, c);
    }
    if (l.peer) {
        Cursor c{b};
        if (l.peer_host && l.peer_dev) c.put("host+dev");
        else if (l.peer_host) c.put("host");
        else if (l.peer_dev) c.put("dev");
        else c.put("none");
        copy(t.peer, sizeof t.peer, c);
    }
    return t;
}

std::size_t format_link(const Link& l, std::span<char> out) {
    Cursor c{out};
    if (!l.any()) { c.put("not seen"); return c.n; }
    const LinkText t = link_text(l);
    bool first = true;
    auto sep = [&] { if (!first) c.put(" | "); first = false; };
    if (t.usb[0]) { sep(); c.put(t.usb); }
    if (t.tbt[0]) { sep(); c.put(t.tbt); }
    if (t.dp[0]) { sep(); c.put("DP alt "); c.put(t.dp); }
    else if (l.dp) { sep(); c.put("DP"); }
    if (t.cable[0]) { sep(); c.put("cable "); c.put(t.cable); }
    if (t.peer[0]) {
        sep();
        c.put(l.peer_host || l.peer_dev ? "partner USB " : "partner no USB");
        if (l.peer_host || l.peer_dev) c.put(t.peer);
    }
    return c.n;
}

void json_link(const Link& l, std::span<char, kJsonLine> out) {
    for (char& c : out) c = ' ';
    char buf[kJsonLine];
    Cursor c{buf};
    const LinkText t = link_text(l);
    c.put("{\"link\":{");
    bool first = true;
    auto field = [&](const char* name, const char* v) {
        if (!v[0]) return;
        if (!first) c.put(',');
        first = false;
        c.put('"'); c.put(name); c.put("\":\""); c.put(v); c.put('"');
    };
    field("usb", t.usb);
    field("tbt", t.tbt);
    field("dp", t.dp);
    field("cable", t.cable);
    field("peer", t.peer);
    c.put("}}");
    for (std::size_t i = 0; i < c.n && i < kJsonLine - 1; i++) out[i] = buf[i];
    out[kJsonLine - 1] = '\n';
}

}  // namespace k2::pdtrace::view
