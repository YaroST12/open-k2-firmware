// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The K2 screen's compact diagnosis, see k2/pd/diag_screen.hpp. The finding fields (a, b, c, d) mean what diag.cpp's
// evaluate() puts in them; the full-line texts in diag.cpp title() / note() are the reference for each code.
#include "k2/pd/diag_screen.hpp"

#include <cstring>

#include "k2/pd/diag_out.hpp"
#include "k2/pd/pd_spec_timing.hpp"

namespace k2::pd::diag::screen {
namespace {

using detail::Out;

constexpr unsigned kReject = 4, kWait = 12, kNotSupported = 16;   // control message types (a swap's answer)
constexpr std::uint16_t kSvidDp = 0xFF01;

std::uint32_t un(std::int32_t v) { return v < 0 ? 0 : std::uint32_t(v); }

// The fixed label of a finding (<= kLabelChars): the same words every time, the numbers go to the note.
const char* label(const Finding& f) {
    switch (f.code) {
    case Code::PowerShort: return "Power short";
    case Code::MismatchFlag: return "Mismatch flagged";
    case Code::NoMismatchFlag: return "No mismatch flag";
    case Code::OverDraw: return (f.d & 3) == 3 ? "Over: drop+reset" : f.d & 1 ? "Over, VBUS fell" : f.d & 2 ? "Over, hard reset"
                                                                                                         : "Over contract";
    case Code::Pinned: return "Pinned at limit";
    case Code::Draw: return nullptr;   // "Draw 26W": built in row()
    case Code::DrawReduced: return "Draw reduced";
    case Code::FiveVoltOnly: return "5V contract only";
    case Code::ContractUnseen: return "Contract unseen";
    case Code::NoContract: return "No contract";
    case Code::ActiveSink: return "K2 is the sink";
    case Code::HardReset: return "Hard reset";
    case Code::SoftReset: return "Soft reset";
    case Code::Reattach: return "Re-attached";
    case Code::Rejected: return "Request rejected";
    case Code::WaitLoop: return "Answered Wait";
    case Code::ManyRequests: return "Many Requests";
    case Code::NoAccept: return "No Accept";
    case Code::LateAccept: return "Accept late";
    case Code::NoPsRdy: return "No PS_RDY";
    case Code::LatePsRdy: return "PS_RDY late";
    case Code::CapsUnanswered: return "Offer unanswered";
    case Code::VbusLow: return "VBUS low";
    case Code::VbusHigh: return "VBUS high";
    case Code::VbusLost: return "VBUS lost";
    case Code::InputCuts: return "Input cuts out";
    case Code::OverSustained: return "Sustained over";
    case Code::NoGoodCrc: return "No GoodCRC";
    case Code::Retries: return "Retries";
    case Code::TxFail: return "K2 send failed";
    case Code::RxBad: return "Garbled frames";
    case Code::SwapRefused:
        if (f.a == 1) return f.b ? "Won't take power" : "Won't give power";
        return f.a == 2 ? "DR_Swap refused" : f.a == 4 ? "VCONN refused" : "FR_Swap refused";
    case Code::ModeNak: return f.a & 1 ? "DP mode refused" : f.a & 2 ? "TBT mode refused" : "Mode refused";
    case Code::EnterUsbRefused: return "Enter_USB NAK";
    case Code::EprFailed: return "EPR entry failed";
    case Code::CableCap: return "Cable caps 3A";
    case Code::VdmLoop: return "VDM repeats";
    case Code::CableSilent: return "Cable silent";
    }
    return "?";
}

// Findings whose t_ms is the moment the event happened (the tail shows it). The others carry the contract's or the
// Request's time, or none: a tail there would read as "it happened at 0:00".
bool timed(Code c) {
    switch (c) {
    case Code::MismatchFlag: case Code::OverDraw: case Code::DrawReduced: case Code::HardReset: case Code::SoftReset:
    case Code::Reattach: case Code::Rejected: case Code::NoAccept: case Code::NoPsRdy: case Code::VbusLow:
    case Code::VbusHigh: case Code::VbusLost: case Code::InputCuts: case Code::OverSustained: case Code::SwapRefused:
    case Code::ModeNak:
        return true;
    default: return false;
    }
}

// The tail time, <= kTailChars: m:ss below an hour, then "1h05m" / "12h05m", from 100 h whole hours.
void tail_clock(Out& o, std::uint32_t ms) {
    const std::uint32_t t = ms / 1000;
    if (t < 3600) { o.clock(ms); return; }
    o.u(t / 3600); o.c('h');
    if (t / 3600 < 100) { o.two(t / 60 % 60); o.c('m'); }
}

const char* vdm_cmd(std::uint32_t key) {   // <= 17 characters
    const unsigned cmd = key & 0x1F, svid = key >> 8;
    switch (cmd) {
    case 1: return "Discover_Identity";
    case 2: return "Discover_SVIDs";
    case 3: return "Discover_Modes";
    case 4: return "Enter_Mode";
    case 5: return "Exit_Mode";
    case 6: return "Attention";
    case 16: return svid == kSvidDp ? "DP Status" : "VDM 16";
    case 17: return svid == kSvidDp ? "DP Configure" : "VDM 17";
    default: return "VDM request";
    }
}
const char* vdm_answer(std::uint32_t a) {
    switch (a) {
    case 1: return "ACK";
    case 2: return "NAK";
    case 3: return "BUSY";
    case 4: return "Not_Supported";
    default: return "no answer";
    }
}

// The count: "3 times" (the tail shows the first time, so the note does not repeat it).
void times(Out& o, const Finding& f) { o.u(un(f.a)); o.s(f.a == 1 ? " time" : " times"); }

void note(const Report& r, const Finding& f, Out& o) {
    switch (f.code) {
    case Code::PowerShort:   // "needs 65W (twin), gets 60W" (the offer's top)
        o.s("needs "); o.watts(f.a);
        o.s(r.need_from == NeedFrom::SinkCaps ? " (caps)" : r.need_from == NeedFrom::Skedb ? " (PDP)" : " (twin)");
        o.s(", gets "); o.watts(f.b);
        break;
    case Code::MismatchFlag: o.s("set in its Request"); break;
    case Code::NoMismatchFlag: o.s("took "); o.volts(f.a); o.amps(f.b); o.s(", bit not set"); break;
    case Code::OverDraw: {   // "6.05A on 3.25A, 48 bursts" / "for 7.9s" / ", 40% of time"
        o.amps(f.a); o.s(" on "); o.amps(f.b);
        const unsigned bursts = un(f.c) >> 8 & 0xFF, hi = un(f.d) >> 12 & 0x3FF;
        if (bursts > 1) { o.s(", "); o.u(bursts); o.s(" bursts"); }
        else if (bursts) { o.s(" for "); o.u(hi / 10); o.c('.'); o.u(hi % 10); o.c('s'); }
        else { o.s(", "); o.u(un(f.c) & 0xFF); o.s("% of time"); }
        break;
    }
    case Code::Pinned: o.s(">=95% of "); o.amps(f.a); o.s(", "); o.u(un(f.b)); o.s("% of "); o.dur(un(f.c)); break;
    case Code::Draw:
        if (f.a) { o.s("on "); o.volts(f.a); o.amps(f.b); }
        else { o.s("at "); o.volts(r.vbus_mv); }
        o.s(", peak "); o.amps(r.peak_ma);
        break;
    case Code::DrawReduced:
        o.amps(f.a); o.s(" to "); o.amps(f.b);
        if (f.c > 1) { o.s(", "); o.u(un(f.c)); o.c('x'); }
        o.s(f.d ? ", no PD" : ", with PD");
        break;
    case Code::FiveVoltOnly: o.s("offer to "); o.volts(f.a); o.s(", device to "); o.volts(f.b); break;
    case Code::ContractUnseen:
        if (f.b) { o.s("VBUS "); o.volts(f.a); o.s(" when the K2 saw it"); }
        else o.s("no VBUS reading");
        break;
    case Code::NoContract: o.s("VBUS "); o.volts(f.a); o.s(", no Accept/PS_RDY"); break;
    case Code::ActiveSink:
        if (f.a) { o.volts(f.a); o.amps(f.b); }
        else { o.s("PDO "); o.u(un(f.c)); }
        o.s(" contract, no load");
        break;
    case Code::HardReset: case Code::Rejected: case Code::NoPsRdy: times(o, f); break;
    case Code::SoftReset:
        times(o, f);
        if (f.b) { o.s(", "); o.u(un(f.b)); o.s(" by K2"); }
        break;
    case Code::Reattach: times(o, f); o.s(", CC back"); break;
    case Code::NoAccept: times(o, f); o.s(", no answer"); break;
    case Code::WaitLoop: o.u(un(f.a)); o.s("x, the source said retry"); break;
    case Code::ManyRequests: o.u(un(f.a)); o.s(" fixed, by the device"); break;
    case Code::LateAccept: o.u(un(f.a)); o.s("ms, limit "); o.u(spec::kAcceptLimitMs); o.s("ms"); break;
    case Code::LatePsRdy:
        o.u(un(f.a)); o.s("ms, limit "); o.u(f.b ? spec::kPsTransitionEprMaxMs : spec::kPsTransitionMaxMs); o.s("ms");
        break;
    case Code::CapsUnanswered: o.u(un(f.a)); o.s("x, no GoodCRC or Request"); break;
    case Code::VbusLow:   // "18.45V on 20V -8% at 0.89A"
        o.volts2(f.a); o.s(" on "); o.volts(f.b);
        if (f.b > 0) { o.s(" -"); o.u((un(f.b - f.a) * 100 + un(f.b) / 2) / un(f.b)); o.c('%'); }
        o.s(" at "); o.amps(f.c);
        break;
    case Code::VbusHigh: o.volts2(f.a); o.s(" on "); o.volts(f.b); o.s(" contract"); break;
    case Code::VbusLost:   // the source cut out: CC stayed attached
        if (f.c && f.b * 1000 > f.c * 1050) { o.s("CC on; drew "); o.amps(f.b); o.s(" on "); o.amps(f.c); }
        else o.s("CC still attached");
        break;
    case Code::InputCuts: o.s("0mA in "); o.u(un(f.a)); o.s(" of "); o.u(un(f.b)); o.s(" windows"); break;
    case Code::OverSustained:   // "3.89A on 3.25A, 83% of 20s"
        o.amps(f.a); o.s(" on "); o.amps(f.b); o.s(", "); o.u(un(f.c) >> 16); o.s("% of "); o.dur(un(f.d));
        break;
    case Code::NoGoodCrc: o.u(un(f.a)); o.s(" SOP frames unacknowledged"); break;
    case Code::Retries: o.u(un(f.a)); o.s(" frames sent again"); break;
    case Code::TxFail: o.u(un(f.a)); o.s(" sends, no GoodCRC on SOP"); break;
    case Code::RxBad: o.u(un(f.a)); o.s(" frames, unknown token"); break;
    case Code::SwapRefused:
        if (f.a == 1) {   // "PR_Swap rejected; 20V source" / "PR_Swap unanswered; stays sink"
            const unsigned t = un(f.c);
            o.s("PR_Swap ");
            o.s(t == kReject ? "rejected" : t == kWait ? "Wait" : t == kNotSupported ? "unsupported" : "unanswered");
            if (f.b) { o.s("; "); o.milli(f.d ? f.d : 5000, 0); o.s("V source"); }
            else o.s("; stays sink");
        } else {
            o.s(f.a == 2 ? "DR_Swap" : f.a == 4 ? "VCONN_Swap" : "FR_Swap"); o.s(" not accepted");
        }
        break;
    case Code::ModeNak: o.s(f.b ? "the cable plug NAKed Enter_Mode" : "Enter_Mode answered NAK"); break;
    case Code::EnterUsbRefused: o.s("not accepted by the other end"); break;
    case Code::EprFailed: o.s("Enter_Failed, reason "); o.u(un(f.a)); break;
    case Code::CableCap: o.s("e-marker 3A, device asks "); o.amps(f.a); break;
    case Code::VdmLoop: {   // "Discover_SVIDs 5x, ACK" (the answer only when it fits)
        o.s(vdm_cmd(un(f.a))); o.c(' '); o.u(un(f.b)); o.c('x');
        const char* a = vdm_answer(un(f.c));
        if (o.n + 2 + std::strlen(a) <= kNoteChars) { o.s(", "); o.s(a); }
        break;
    }
    case Code::CableSilent: o.s("Discover_Identity "); o.u(un(f.a)); o.s("x, no chip"); break;
    }
}

// The healthy lines, in the full text's order (diag.cpp line()).
unsigned healthy_count(const Report& r) {
    return unsigned(r.quiet_contract && r.quiet_link) + unsigned(r.quiet_quality && r.band_checked) + unsigned(r.quiet_roles);
}

void lanes_note(const Report& r, Out& o) {   // "pass through; PD: USB4 TBT DP D"
    const auto& l = r.lanes;
    o.s("pass through; PD: ");
    bool any = false;
    auto sep = [&] { if (any) o.c(' '); any = true; };
    if (l.eusb) { sep(); o.s(l.usb_mode == 2 ? "USB4" : l.usb_mode == 1 ? "USB3" : "USB2"); }
    if (l.tbt) { sep(); o.s("TBT"); }
    if (l.dp) {
        sep();
        if (!l.eusb && !l.tbt && !l.dp_pin) o.s("DP alt mode");
        else { o.s("DP"); if (l.dp_pin) { o.c(' '); o.c(l.dp_pin); } }
    }
    if (!any) o.s("no alt mode");
}

const char* power_word(std::uint8_t p) {   // <= 12 characters
    switch (p) {
    case kPowerShort: return "power short";
    case kPowerOk: return "power OK";
    case kPowerUnknown: return "need unknown";
    case kPowerUnseen: return "not observed";
    case kPowerNone: return "no contract";
    case kPowerActive: return "K2 is sink";
    case kPowerOver: return "over draw";
    default: return "";
    }
}

}  // namespace

unsigned row_count(const Report& r) { return r.empty ? 0 : r.n + healthy_count(r) + 1; }

bool row(const Report& r, unsigned idx, Row& out) {
    out = Row();
    if (idx >= row_count(r)) return false;
    Out lb{out.label}, nt{out.note};
    lb.tight = nt.tight = true;
    if (idx < r.n) {
        const Finding& f = r.f[idx];
        out.sev = f.sev;
        if (f.code == Code::Draw) { lb.s("Draw "); lb.watts(f.c); }
        else lb.s(label(f));
        note(r, f, nt);
        if (timed(f.code) && f.t_ms) { Out t{out.tail}; tail_clock(t, f.t_ms); }
        return true;
    }
    idx -= r.n;
    out.sev = Sev::Ok;
    if (r.quiet_contract && r.quiet_link && idx-- == 0) { lb.s("No resets"); nt.s("no resets, retries or rejects"); return true; }
    if (r.quiet_quality && r.band_checked && idx-- == 0) { lb.s("VBUS in band"); nt.s("within 5% of the contract"); return true; }
    if (r.quiet_roles && idx-- == 0) { lb.s("No refusals"); nt.s("no refused swaps or modes"); return true; }
    out.lanes = true;
    lb.s("Data lanes");
    lanes_note(r, nt);
    return true;
}

void verdict(const Report& r, std::span<char> out) {
    Out o{out};
    o.tight = true;
    if (!out.empty()) out[0] = 0;
    if (r.empty) { o.s("No PD session yet"); return; }
    if (r.no_pd) { o.s("No PD seen, VBUS "); o.volts(r.vbus_mv); return; }
    o.s(r.link == 2 ? "Link unstable, " : r.link == 1 ? "Link degraded, " : "Link OK, ");
    o.s(power_word(r.power));
}

Sev worst(const Report& r) {
    Sev w = Sev::Ok;
    for (unsigned i = 0; i < r.n; i++) if (r.f[i].sev < w) w = r.f[i].sev;
    return w;
}

void word(const Report& r, std::span<char> out, Sev& sev) {
    Out o{out};
    if (!out.empty()) out[0] = 0;
    sev = Sev::Ok;
    if (r.empty) { o.s("No PD session"); return; }
    if (r.no_pd) { o.s("No PD seen"); return; }
    if (r.n && r.f[0].sev <= Sev::Warn) {   // findings are ranked: the first is the worst
        sev = r.f[0].sev;
        Row w;
        row(r, 0, w);
        o.s(w.label.data());
        return;
    }
    switch (r.power) {
    case kPowerUnseen: o.s("Contract unseen"); break;
    case kPowerNone: o.s("No contract"); break;
    case kPowerActive: o.s("K2 is the sink"); break;
    case kPowerUnknown: o.s("Link healthy"); break;
    default: o.s("Healthy"); break;
    }
}

}  // namespace k2::pd::diag::screen
