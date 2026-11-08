// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// USB link classifier, see k2/pd/usb_link.hpp for what is and is not observable.
// Speed ranks used internally: 0 USB 2.0 480M, 1 USB 3.2 Gen1 5G, 2 Gen2 10G, 3 USB4 Gen2 20G, 4 USB4 Gen3 40G,
// 5 USB4 Gen4 80G. PD speed code (UFP VDO 2:0, cable VDO 2:0, Enter_USB cable speed): 0 USB2, 1 Gen1,
// 2 Gen2 (USB4 Gen2 when USB4 is in play), 3 USB4 Gen3, 4 USB4 Gen4.
#include "k2/pd/usb_link.hpp"

#include <algorithm>
#include <array>
#include <cstring>

namespace k2::usblink {

namespace {

constexpr std::uint8_t kMsgEnterUsb = 0x08, kMsgVdm = 0x0F;
constexpr std::uint8_t kCtlAccept = 3, kCtlReject = 4, kCtlDataReset = 14, kCtlNotSupported = 16;
constexpr std::uint16_t kSvidPd = 0xFF00, kSvidTbt = 0x8087, kSvidDp = 0xFF01;
constexpr unsigned kCmdIdentity = 1, kCmdSvids = 2, kCmdModes = 3, kCmdEnter = 4, kCmdExit = 5, kCmdDpConfigure = 17;

std::uint32_t le32(const std::uint8_t* p) {
    return std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8) | (std::uint32_t(p[2]) << 16) | (std::uint32_t(p[3]) << 24);
}

int code_rank(unsigned code, bool usb4) {
    switch (code) {
        case 0: return 0;
        case 1: return 1;
        case 2: return usb4 ? 3 : 2;
        case 3: return 4;
        default: return 5;
    }
}

struct Speed { Family family; std::uint32_t mbps; };
Speed rank_speed(int r) {
    switch (r) {
        case 0: return {Family::Usb2, 480};
        case 1: return {Family::Usb32, 5000};
        case 2: return {Family::Usb32, 10000};
        case 3: return {Family::Usb4, 20000};
        case 4: return {Family::Usb4, 40000};
        default: return {Family::Usb4v2, 80000};
    }
}

class Out {
public:
    explicit Out(std::span<char> b) : b_(b) {}
    void put(const char* s) {
        for (; *s && n_ + 1 < b_.size(); s++) b_[n_++] = *s;
    }
    std::size_t finish() {
        if (!b_.empty()) b_[n_] = 0;
        return n_;
    }
private:
    std::span<char> b_;
    std::size_t n_ = 0;
};

}  // namespace

const char* family_name(Family f) {
    switch (f) {
        case Family::LowSpeed: return "USB LS";
        case Family::FullSpeed: return "USB FS";
        case Family::Usb2: return "USB 2.0";
        case Family::Usb32: return "USB 3.2";
        case Family::Usb4: return "USB4";
        case Family::Usb4v2: return "USB4v2";
        default: return "Unknown";
    }
}
const char* tbt_name(Tbt t) {
    switch (t) {
        case Tbt::Tbt3: return "TB3/4";   // legacy TBT mode: a TB4 port in TB3 compat looks the same
        case Tbt::Tbt4: return "TBT4";
        case Tbt::Tbt5: return "TBT5";
        default: return "";
    }
}
const char* source_name(Source s) {
    switch (s) {
        case Source::DpDm: return "D+/D-";
        case Source::Identity: return "Identity";
        case Source::Cable: return "Cable";
        case Source::EnterUsb: return "Enter_USB";
        case Source::EnterTbt: return "Enter_Mode TBT";
        case Source::DpAlt: return "DP alt";
        case Source::DataReset: return "Data_Reset";
        default: return "none";
    }
}
const char* confidence_name(Confidence c) {
    switch (c) {
        case Confidence::Hint: return "hint";
        case Confidence::Advertised: return "adv";
        case Confidence::Negotiated: return "neg";
        default: return "none";
    }
}

std::size_t format(const Link& l, std::span<char> out) {
    Out o(out);
    if (l.family == Family::Unknown && l.tbt == Tbt::None) {
        o.put(l.dp ? "DP alt" : "--");
        if (l.dp) {
            o.put(" (");
            o.put(confidence_name(l.conf));
            o.put(")");
        }
        return o.finish();
    }
    switch (l.family) {
        case Family::LowSpeed: o.put("USB 1.x LS"); break;
        case Family::FullSpeed: o.put("USB 1.1/2.0 FS"); break;
        case Family::Usb2: o.put("USB 2.0 480M"); break;
        case Family::Usb32: o.put(l.mbps >= 10000 ? "USB 3.2 Gen2 10G" : "USB 3.2 Gen1 5G"); break;
        case Family::Usb4: o.put(l.mbps >= 40000 ? "USB4 40G" : "USB4 20G"); break;
        case Family::Usb4v2: o.put("USB4v2 80G"); break;
        default: break;
    }
    if (l.tbt != Tbt::None) {
        if (l.family != Family::Unknown) o.put(" ");
        o.put(tbt_name(l.tbt));
        if (l.family == Family::Unknown && l.mbps) o.put(l.mbps >= 40000 ? " 40G" : l.mbps >= 20000 ? " 20G" : " 10G");
    }
    if (l.dp) o.put(" +DP");
    o.put(" (");
    o.put(confidence_name(l.conf));
    o.put(")");
    return o.finish();
}

namespace {
void put_speed(Out& o, const Link& l) {
    switch (l.family) {
        case Family::LowSpeed: o.put("1.x LS"); break;
        case Family::FullSpeed: o.put("FS"); break;
        case Family::Usb2: o.put("2.0 480M"); break;
        case Family::Usb32: o.put(l.mbps >= 10000 ? "3.2 Gen2 10G" : "3.2 Gen1 5G"); break;
        case Family::Usb4: o.put(l.mbps >= 40000 ? "USB4 40G" : "USB4 20G"); break;
        case Family::Usb4v2: o.put("USB4v2 80G"); break;
        default: break;
    }
}
}  // namespace

std::size_t format_speed(const Link& l, std::span<char> out) {
    Out o(out);
    const bool hint_only = l.conf <= Confidence::Hint;
    if (l.family == Family::Unknown || (hint_only && l.tbt == Tbt::None)) {
        // A TBT link entered without a USB speed still has a rate; otherwise no guess.
        o.put(l.tbt != Tbt::None && l.mbps ? (l.mbps >= 40000 ? "40G" : l.mbps >= 20000 ? "20G" : "10G") : "Unkn");
        return o.finish();
    }
    put_speed(o, l);
    return o.finish();
}

const char* svid_name(std::uint16_t svid) {
    switch (svid) {
        case 0x05AC: return "Apple";
        case 0x2717: return "Xiaomi";
        case 0x2E87: return "Cuktech";
        case 0x17EF: return "Lenovo";
        case 0x04E8: return "Samsung";
        case 0x18D1: return "Google";
        case 0x03F0: return "HP";
        case 0x413C: return "Dell";
        default: return nullptr;
    }
}

AltState format_alt(const Link& l, std::span<char> out, bool tag) {
    Out o(out);
    bool first = true;
    const auto sep = [&] { if (!first) o.put(", "); first = false; };
    if (l.dp || l.tbt != Tbt::None) {
        if (l.dp) { sep(); o.put(l.dp_4lane ? "DP 4-lane" : l.dp_2lane ? "DP 2-lane" : "DP"); }
        if (l.tbt != Tbt::None) { sep(); o.put(tbt_name(l.tbt)); }
        o.finish();
        return AltState::Entered;
    }
    if (l.dp_adv) { sep(); o.put("DP"); }
    if (l.tbt_adv) { sep(); o.put("TB"); }
    for (unsigned i = 0; i < l.n_other && i < l.other_svids.size(); i++) {
        sep();
        const char* nm = svid_name(l.other_svids[i]);
        if (nm) o.put(nm);
        else {
            char hx[5];
            for (int k = 0; k < 4; k++) hx[k] = "0123456789ABCDEF"[(l.other_svids[i] >> (12 - 4 * k)) & 15];
            hx[4] = 0;
            o.put(hx);
        }
    }
    if (first) { o.put("-"); o.finish(); return AltState::None; }
    if (tag) o.put(" (adv)");
    o.finish();
    return AltState::Advertised;
}

std::size_t format_ui(const Link& l, std::span<char> out) {
    Out o(out);
    // Nothing PD-backed (no identity/mode data, only D+/D- hints): no guess.
    const bool hint_only = l.conf <= Confidence::Hint;
    if (l.tbt == Tbt::None && (l.family == Family::Unknown || hint_only)) {
        o.put(l.dp ? "DP alt" : "Unkn");
        return o.finish();
    }
    put_speed(o, l);
    if (l.tbt != Tbt::None) {
        if (l.family != Family::Unknown) o.put(" ");
        o.put(l.tbt == Tbt::Tbt3 ? "TB3/4" : l.tbt == Tbt::Tbt4 ? "TB4" : "TB5");
        if (l.family == Family::Unknown && l.mbps) o.put(l.mbps >= 40000 ? " 40G" : l.mbps >= 20000 ? " 20G" : " 10G");
    }
    if (l.dp) o.put(" +DP");
    return o.finish();
}

void Classifier::on_lines(std::int32_t dp_mv, std::int32_t dm_mv) {
    dp_mv_ = dp_mv;
    dm_mv_ = dm_mv;
}

void Classifier::on_identity(Sop sop, std::span<const std::uint32_t> v, std::size_t n) {
    if (n < 2) return;
    const std::uint32_t id = v[1];
    const unsigned ufp_type = (id >> 27) & 7, dfp_type = (id >> 23) & 7;
    if (sop == Sop::Sop) {
        id_ = true;
        id_usb_host_ = id >> 31;
        id_usb_dev_ = (id >> 30) & 1;
        const bool ufp_vdo = (ufp_type == 1 || ufp_type == 2) && n > 4;  // PDUSB hub / peripheral
        if (ufp_vdo && (v[4] & 7) <= 4) {
            ufp_ = true;
            ufp_cap_ = std::uint8_t((v[4] >> 24) & 0xF);
            ufp_alt_ = std::uint8_t((v[4] >> 3) & 7);
            ufp_speed_ = std::uint8_t(v[4] & 7);
        }
        if (ufp_type == 5 && n > 4) {  // Alternate Mode Adapter VDO, b2:0 USB SS signalling
            static constexpr std::uint8_t kAma[8] = {1, 2, 3, 1, 0, 0, 0, 0};  // rank + 1, 0 none; 3 = billboard only
            ama_rank1_ = kAma[v[4] & 7];
        }
        const std::size_t di = (ufp_type == 1 || ufp_type == 2) ? 5 : 4;  // DFP VDO follows the UFP VDO
        if ((dfp_type == 1 || dfp_type == 2) && n > di) {  // PDUSB hub / host
            dfp_ = true;
            dfp_cap_ = std::uint8_t((v[di] >> 24) & 7);
        }
    } else if ((ufp_type == 3 || ufp_type == 4) && n > 4 && (v[4] & 7) <= 4) {  // passive / active cable
        cable_ = true;
        cable_active_ = ufp_type == 4;
        cable_intel_ = (id & 0xFFFF) == kSvidTbt;
        cable_speed_ = std::uint8_t(v[4] & 7);
        if (cable_active_ && n > 5) {
            cable_no_usb4_ = (v[5] >> 8) & 1;
            cable_no_usb3_ = (v[5] >> 4) & 1;
        }
    }
}

void Classifier::on_vdm(Sop sop, std::span<const std::uint8_t> payload) {
    if (payload.size() < 4) return;
    std::array<std::uint32_t, 7> v{};
    const std::size_t n = std::min(payload.size() / 4, v.size());
    for (std::size_t i = 0; i < n; i++) v[i] = le32(&payload[4 * i]);
    const std::uint32_t h = v[0];
    if (!((h >> 15) & 1)) return;  // unstructured
    const std::uint16_t svid = std::uint16_t(h >> 16);
    const unsigned type = (h >> 6) & 3, cmd = h & 0x1F;  // type 0 REQ, 1 ACK, 2 NAK, 3 BUSY
    if (svid == kSvidPd && type == 1) {
        if (cmd == kCmdIdentity) on_identity(sop, v, n);
        else if (cmd == kCmdSvids && sop == Sop::Sop) {
            for (std::size_t i = 1; i < n; i++)
                for (const std::uint16_t s : {std::uint16_t(v[i] >> 16), std::uint16_t(v[i] & 0xFFFF)}) {
                    if (s == kSvidTbt) tbt_svid_ = true;
                    if (s == kSvidDp) dp_svid_ = true;
                    if (s != 0 && s != kSvidPd && s != kSvidTbt && s != kSvidDp && n_other_ < other_.size() &&
                        std::find(other_.begin(), other_.begin() + n_other_, s) == other_.begin() + n_other_)
                        other_[n_other_++] = s;
                }
        }
        return;
    }
    if (sop != Sop::Sop) return;  // alt modes are entered with the partner
    if (svid != kSvidTbt && svid != kSvidDp) return;
    bool& pend = svid == kSvidTbt ? tbt_pend_ : dp_pend_;
    bool& active = svid == kSvidTbt ? tbt_active_ : dp_active_;
    if (cmd == kCmdModes && type == 1 && svid == kSvidTbt && n >= 2) tbt_mode_ = true;
    else if (cmd == kCmdEnter) {
        if (type == 0) {
            pend = true;
            if (svid == kSvidTbt) tbt_enter_vdo_ = n > 1 ? v[1] : 0;
        } else if (pend && type == 1) {
            active = true;
            pend = false;
        } else if (pend) pend = false;
    } else if (cmd == kCmdExit && type == 1) {
        active = false;
        if (svid == kSvidDp) dp_pin_ = 0;
    } else if (cmd == kCmdDpConfigure && svid == kSvidDp) {
        if (type == 0 && n > 1) dp_cfg_pin_ = std::uint8_t((v[1] >> 8) & 0x3F);
        else if (type == 1) dp_pin_ = dp_cfg_pin_;
    }
}

void Classifier::on_message(Sop sop, std::uint16_t header, std::span<const std::uint8_t> payload) {
    if ((header >> 15) & 1) return;  // extended messages carry nothing we use
    const unsigned type = header & 0x1F, ndo = (header >> 12) & 7;
    if (ndo == 0) {
        if (sop != Sop::Sop) return;
        if (type == kCtlAccept && eu_pend_) {
            eu_pend_ = false;
            eu_ok_ = true;
            reset_ = false;
        } else if ((type == kCtlReject || type == kCtlNotSupported) && eu_pend_) {
            eu_pend_ = false;
        } else if (type == kCtlDataReset) {  // USB goes back to USB 2.0 until Enter_USB runs again
            eu_pend_ = eu_ok_ = false;
            tbt_active_ = tbt_pend_ = dp_active_ = dp_pend_ = false;
            dp_pin_ = 0;
            reset_ = true;
        }
        return;
    }
    if (type == kMsgEnterUsb && sop == Sop::Sop && payload.size() >= 4) {
        eu_vdo_ = le32(payload.data());
        eu_pend_ = true;
        eu_ok_ = false;
    } else if (type == kMsgVdm) {
        on_vdm(sop, payload);
    }
}

int Classifier::partner_rank() const {
    int r = -1;
    if (ufp_) r = code_rank(ufp_speed_, ufp_cap_ & 8);
    else if (ama_rank1_) r = ama_rank1_ - 1;
    if (dfp_) {
        const int d = (dfp_cap_ & 4) ? 4 : (dfp_cap_ & 2) ? 1 : 0;
        r = r < 0 ? d : std::min(r, d);
    }
    return r;
}

int Classifier::cable_rank(int partner) const {
    if (!cable_) return -1;
    int r = code_rank(cable_speed_, partner >= 3 || partner < 0);
    if (cable_no_usb4_ && r >= 3) r = 2;
    return r;
}

Link Classifier::link() const {
    Link l = link_core();
    l.dp_adv = dp_svid_;
    l.tbt_adv = tbt_svid_ || (ufp_alt_ & 1);
    l.dp_2lane = dp_active_ && dp_pin_ != 0 && (dp_pin_ & 0x14) == 0;
    l.other_svids = other_;
    l.n_other = n_other_;
    return l;
}

Link Classifier::link_core() const {
    Link l;
    l.billboard = ufp_ && (ufp_cap_ & 2);
    const auto set_rank = [&](int r) {
        const Speed s = rank_speed(r);
        l.family = s.family;
        l.mbps = s.mbps;
    };
    const auto tbt_level = [&](int r) { return r >= 5 ? Tbt::Tbt5 : r >= 3 ? Tbt::Tbt4 : Tbt::Tbt3; };

    // 1. Negotiated state: what the host and dock actually entered.
    int neg = -1;
    if (eu_ok_) {
        const unsigned mode = (eu_vdo_ >> 28) & 7, cs = (eu_vdo_ >> 21) & 7;
        neg = mode == 2 ? std::max(3, code_rank(cs, true)) : mode == 1 ? (cs >= 2 ? 2 : cs == 1 ? 1 : 0) : 0;
    }
    const bool dp4 = dp_active_ && (dp_pin_ & 0x14);  // pin assignments C (b2) and E (b4) use all four lanes
    if (dp4) {
        set_rank(0);
        l.dp = true;
        l.dp_4lane = true;
        l.conf = Confidence::Negotiated;
        l.source = Source::DpAlt;
        return l;
    }
    if (eu_ok_ || tbt_active_) {
        l.conf = Confidence::Negotiated;
        l.dp = dp_active_;
        if (eu_ok_) {
            set_rank(neg);
            l.source = Source::EnterUsb;
        }
        if (tbt_active_) {
            l.tbt = eu_ok_ ? tbt_level(neg) : Tbt::Tbt3;
            if (!eu_ok_) {
                // Enter_Mode VDO 18:16 cable speed (per lane): 1 USB3 Gen1, 2 USB3 passive (10 Gbps),
                // 3 "10 and 20 Gbps". TBT3 bonds two lanes: 20 Gbps per lane = 40G, 10 per lane = 20G.
                const std::uint32_t spd = (tbt_enter_vdo_ >> 16) & 7;
                l.mbps = spd >= 3 ? 40000 : spd == 2 ? 20000 : 10000;
                l.source = Source::EnterTbt;
            }
        }
        return l;
    }
    if (reset_) {
        set_rank(0);
        l.conf = Confidence::Hint;
        l.source = Source::DataReset;
        return l;
    }

    // 2. Advertised capability, clamped by the cable.
    const int p = partner_rank(), c = cable_rank(p);
    // A cable only limits: alone it says nothing about the ends (a 240 W USB4 cable on a 10G dock).
    const int r = p < 0 ? -1 : c < 0 ? p : std::min(p, c);
    if (r >= 0) {
        set_rank(r);
        l.cable_limited = p >= 0 && c >= 0 && c < p;
        l.source = l.cable_limited || p < 0 ? Source::Cable : Source::Identity;
        l.conf = p < 0 ? Confidence::Hint : Confidence::Advertised;
    }
    if (dp_active_) {  // DP entered: the alt mode is negotiated, TBT stays unclaimed (advertised only)
        l.dp = true;
        l.conf = Confidence::Negotiated;
        l.source = Source::DpAlt;
        return l;
    }
    // Thunderbolt is claimed only from a TBT Enter_Mode ACK or Enter_USB (section 1): advertised
    // SVID / alt-mode flags are capability, never the link, so no TBT tag here.
    if (l.conf != Confidence::None) return l;

    // 3. D+/D- pull-up: the only thing left when no PD data was seen.
    const bool dp_hi = dp_mv_ >= 2000, dm_hi = dm_mv_ >= 2000;
    if (dp_hi != dm_hi) {  // both high is a charger shorting the lines, not a device
        l.family = dp_hi ? Family::FullSpeed : Family::LowSpeed;
        l.mbps = dp_hi ? 12 : 2;
        l.conf = Confidence::Hint;
        l.source = Source::DpDm;
    }
    return l;
}

}  // namespace k2::usblink
