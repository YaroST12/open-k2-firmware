// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The K2 screen's compact diagnosis, see k2/pd/diag_screen.hpp. The finding fields (a, b, c, d) mean what diag.cpp's
// evaluate() puts in them; the full-line texts in diag.cpp title() / note() are the reference for each code.
#include "k2/pd/diag_screen.hpp"

#include <cstring>

#include "k2/core/strpack.hpp"
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
    case Code::PowerShort: return K2_P("Power short");
    case Code::MismatchFlag: return K2_P("Mismatch flagged");
    case Code::NoMismatchFlag: return K2_P("No mismatch flag");
    case Code::OverDraw: return (f.d & 3) == 3 ? K2_P("Over: drop+reset") : f.d & 1 ? K2_P("Over, VBUS fell") : f.d & 2 ? K2_P("Over, hard reset")
                                                                                                         : K2_P("Over contract");
    case Code::Pinned: return K2_P("Pinned at limit");
    case Code::Draw: return nullptr;   // "Draw 26W": built in row()
    case Code::DrawReduced: return K2_P("Draw reduced");
    case Code::FiveVoltOnly: return K2_P("5V contract only");
    case Code::ContractUnseen: return K2_P("Contract unseen");
    case Code::NoContract: return K2_P("No contract");
    case Code::ActiveSink: return K2_P("K2 is the sink");
    case Code::HardReset: return K2_P("Hard reset");
    case Code::SoftReset: return K2_P("Soft reset");
    case Code::Reattach: return K2_P("Re-attached");
    case Code::Rejected: return K2_P("Request rejected");
    case Code::WaitLoop: return K2_P("Answered Wait");
    case Code::ManyRequests: return K2_P("Many Requests");
    case Code::NoAccept: return K2_P("No Accept");
    case Code::LateAccept: return K2_P("Accept late");
    case Code::NoPsRdy: return K2_P("No PS_RDY");
    case Code::LatePsRdy: return K2_P("PS_RDY late");
    case Code::CapsUnanswered: return K2_P("Offer unanswered");
    case Code::VbusLow: return K2_P("VBUS low");
    case Code::VbusHigh: return K2_P("VBUS high");
    case Code::VbusLost: return K2_P("VBUS lost");
    case Code::InputCuts: return K2_P("Input cuts out");
    case Code::OverSustained: return K2_P("Sustained over");
    case Code::NoGoodCrc: return K2_P("No GoodCRC");
    case Code::Retries: return K2_P("Retries");
    case Code::TxFail: return K2_P("K2 send failed");
    case Code::RxBad: return K2_P("Garbled frames");
    case Code::SwapRefused:
        if (f.a == 1) return f.b ? K2_P("Won't take power") : K2_P("Won't give power");
        return f.a == 2 ? K2_P("DR_Swap refused") : f.a == 4 ? K2_P("VCONN refused") : K2_P("FR_Swap refused");
    case Code::ModeNak: return f.a & 1 ? K2_P("DP mode refused") : f.a & 2 ? K2_P("TBT mode refused") : K2_P("Mode refused");
    case Code::EnterUsbRefused: return K2_P("Enter_USB NAK");
    case Code::EprFailed: return K2_P("EPR entry failed");
    case Code::CableCap: return K2_P("Cable caps 3A");
    case Code::VdmLoop: return K2_P("VDM repeats");
    case Code::CableSilent: return K2_P("Cable silent");
    case Code::CaptureGap: return K2_P("Capture gap");
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
    case 1: return K2_P("Discover_Identity");
    case 2: return K2_P("Discover_SVIDs");
    case 3: return K2_P("Discover_Modes");
    case 4: return K2_P("Enter_Mode");
    case 5: return K2_P("Exit_Mode");
    case 6: return K2_P("Attention");
    case 16: return svid == kSvidDp ? K2_P("DP Status") : K2_P("VDM 16");
    case 17: return svid == kSvidDp ? K2_P("DP Configure") : K2_P("VDM 17");
    default: return K2_P("VDM request");
    }
}
const char* vdm_answer(std::uint32_t a) {
    switch (a) {
    case 1: return "ACK";
    case 2: return "NAK";
    case 3: return "BUSY";
    case 4: return "Not_Supported";   // k2-plain: strlen measured (VdmLoop note)
    default: return "no answer";   // k2-plain: strlen measured
    }
}

// The count: "3 times" (the tail shows the first time, so the note does not repeat it).
void times(Out& o, const Finding& f) { o.u(un(f.a)); o.s(f.a == 1 ? K2_P(" time") : K2_P(" times")); }

void note(const Report& r, const Finding& f, Out& o) {
    switch (f.code) {
    case Code::PowerShort:   // "needs 65W (twin), gets 60W" (the offer's top)
        o.s(K2_P("needs ")); o.watts(f.a);
        o.s(r.need_from == NeedFrom::SinkCaps ? K2_P(" (caps)") : r.need_from == NeedFrom::Skedb ? K2_P(" (PDP)") : K2_P(" (twin)"));
        o.s(K2_P(", gets ")); o.watts(f.b);
        break;
    case Code::MismatchFlag: o.s(K2_P("set in its Request")); break;
    case Code::NoMismatchFlag: o.s(K2_P("took ")); o.volts(f.a); o.amps(f.b); o.s(K2_P(", bit not set")); break;
    case Code::OverDraw: {   // "6.05A on 3.25A, 48 bursts" / "for 7.9s" / ", 40% of time"
        o.amps(f.a); o.s(" on "); o.amps(f.b);
        const unsigned bursts = un(f.c) >> 8 & 0xFF, hi = un(f.d) >> 12 & 0x3FF;
        if (bursts > 1) { o.s(", "); o.u(bursts); o.s(K2_P(" bursts")); }
        else if (bursts) { o.s(K2_P(" for ")); o.u(hi / 10); o.c('.'); o.u(hi % 10); o.c('s'); }
        else { o.s(", "); o.u(un(f.c) & 0xFF); o.s(K2_P("% of time")); }
        break;
    }
    case Code::Pinned: o.amps(f.a); o.s(K2_P(" pinned, ")); o.u(un(f.b)); o.s(K2_P("% of ")); o.dur(un(f.c)); break;
    case Code::Draw:
        if (f.a) { o.s("on "); o.volts(f.a); o.amps(f.b); }
        else { o.s("at "); o.volts(r.vbus_mv); }
        o.s(K2_P(", peak ")); o.amps(r.peak_ma);
        break;
    case Code::DrawReduced:
        o.amps(f.a); o.s(" to "); o.amps(f.b);
        if (f.c > 1) { o.s(", "); o.u(un(f.c)); o.c('x'); }
        o.s(f.d ? K2_P(", no PD") : K2_P(", with PD"));
        break;
    case Code::FiveVoltOnly: o.s(K2_P("offer ")); o.volts(f.a); o.s(K2_P(", device ")); o.volts(f.b); break;
    case Code::ContractUnseen:
        if (f.b) { o.s(K2_P("VBUS ")); o.volts(f.a); o.s(K2_P(" when the K2 saw it")); }
        else o.s(K2_P("no VBUS reading"));
        break;
    case Code::NoContract: o.s(K2_P("VBUS ")); o.volts(f.a); o.s(K2_P(", no Accept/PS_RDY")); break;
    case Code::ActiveSink:
        if (f.a) { o.volts(f.a); o.amps(f.b); }
        else { o.s("PDO "); o.u(un(f.c)); }
        o.s(K2_P(" contract, no load"));
        break;
    case Code::HardReset: case Code::Rejected: case Code::NoPsRdy: times(o, f); break;
    case Code::SoftReset:
        times(o, f);
        if (f.b) { o.s(", "); o.u(un(f.b)); o.s(K2_P(" by K2")); }
        break;
    case Code::Reattach: times(o, f); o.s(K2_P(", CC back")); break;
    case Code::NoAccept: times(o, f); o.s(K2_P(", no answer")); break;
    case Code::WaitLoop: o.u(un(f.a)); o.s(K2_P("x, the source said retry")); break;
    case Code::ManyRequests: o.u(un(f.a)); o.s(K2_P(" fixed, by the device")); break;
    case Code::LateAccept: o.u(un(f.a)); o.s(K2_P("ms, limit ")); o.u(spec::kAcceptLimitMs); o.s("ms"); break;
    case Code::LatePsRdy:
        o.u(un(f.a)); o.s(K2_P("ms, limit ")); o.u(f.b ? spec::kPsTransitionEprMaxMs : spec::kPsTransitionMaxMs); o.s("ms");
        break;
    case Code::CapsUnanswered: o.u(un(f.a)); o.s(K2_P("x, no GoodCRC or Request")); break;
    case Code::VbusLow:   // "18.45V on 20V -8% at 0.89A"
        o.volts2(f.a); o.s(" on "); o.volts(f.b);
        if (f.b > 0) { o.s(" -"); o.u((un(f.b - f.a) * 100 + un(f.b) / 2) / un(f.b)); o.c('%'); }
        o.s(o.n + 10 <= kNoteChars ? " at " : " "); o.amps(f.c);   // "at" only while the widest current still fits
        break;
    case Code::VbusHigh: o.volts2(f.a); o.s(" on "); o.volts(f.b); o.s(K2_P(" contract")); break;
    case Code::VbusLost:   // the source cut out: CC stayed attached
        if (f.c && f.b * 1000 > f.c * 1050) { o.s(K2_P("CC on; drew ")); o.amps(f.b); o.s(" on "); o.amps(f.c); }
        else o.s(K2_P("CC still attached"));
        break;
    case Code::InputCuts: o.s(K2_P("0mA in ")); o.u(un(f.a)); o.s(" of "); o.u(un(f.b)); o.s(K2_P(" windows")); break;
    case Code::OverSustained:   // "3.89A on 3.25A, 83% of 20s"
        o.amps(f.a); o.s(" on "); o.amps(f.b); o.s(", "); o.u(un(f.c) >> 16); o.s(o.n + 10 <= kNoteChars ? K2_P("% of ") : "% "); o.dur(un(f.d));
        break;
    case Code::NoGoodCrc: o.u(un(f.a)); o.s(K2_P(" SOP frames unacknowledged")); break;
    case Code::Retries: o.u(un(f.a)); o.s(K2_P(" frames sent again")); break;
    case Code::TxFail: o.u(un(f.a)); o.s(K2_P(" sends, no GoodCRC on SOP")); break;
    case Code::RxBad: o.u(un(f.a)); o.s(K2_P(" frames, unknown token")); break;
    case Code::SwapRefused:
        if (f.a == 1) {   // "PR_Swap rejected; 20V source" / "PR_Swap unanswered; stays sink"
            const unsigned t = un(f.c);
            o.s(K2_P("PR_Swap "));
            o.s(t == kReject ? K2_P("rejected") : t == kWait ? "Wait" : t == kNotSupported ? K2_P("unsupported") : K2_P("no reply"));
            if (f.b) { o.s(K2_P("; src ")); o.milli(f.d ? f.d : 5000, 0); o.s("V"); }
            else o.s(K2_P("; stays sink"));
        } else {
            o.s(f.a == 2 ? K2_P("DR_Swap") : f.a == 4 ? K2_P("VCONN_Swap") : K2_P("FR_Swap")); o.s(K2_P(" not accepted"));
        }
        break;
    case Code::ModeNak: o.s(f.b ? K2_P("cable plug NAKed Enter_Mode") : K2_P("Enter_Mode answered NAK")); break;
    case Code::EnterUsbRefused: o.s(K2_P("not accepted by the other end")); break;
    case Code::EprFailed: o.s(K2_P("Enter_Failed, reason ")); o.u(un(f.a)); break;
    case Code::CableCap: o.s(K2_P("e-marker 3A, wants ")); o.amps(f.a); break;
    case Code::VdmLoop: {   // "Discover_SVIDs 5x, ACK" (the answer only when it fits)
        o.s(vdm_cmd(un(f.a))); o.c(' '); o.u(un(f.b)); o.c('x');
        const char* a = vdm_answer(un(f.c));
        if (o.n + 2 + std::strlen(a) <= kNoteChars) { o.s(", "); o.s(a); }
        break;
    }
    case Code::CableSilent: o.s(K2_P("Discover_Identity ")); o.u(un(f.a)); o.s(K2_P("x, no ID")); break;
    case Code::CaptureGap: o.u(un(f.a)); o.s(f.a == 1 ? K2_P(" frame not decoded") : K2_P(" frames not decoded")); break;
    }
}

// The healthy lines, in the full text's order (diag.cpp line()).
unsigned healthy_count(const Report& r) {
    return unsigned(r.quiet_contract && r.quiet_link) + unsigned(r.quiet_quality && r.band_checked) + unsigned(r.quiet_roles);
}

void lanes_note(const Report& r, Out& o) {   // "unobserved; PD: USB4 TBT DP D"
    const auto& l = r.lanes;
    o.s(K2_P("unobserved; PD: "));
    bool any = false;
    auto sep = [&] { if (any) o.c(' '); any = true; };
    if (l.eusb) { sep(); o.s(l.usb_mode == 2 ? "USB4" : l.usb_mode == 1 ? "USB3" : "USB2"); }
    if (l.tbt) { sep(); o.s("TBT"); }
    if (l.dp) {
        sep();
        if (!l.eusb && !l.tbt && !l.dp_pin) o.s(K2_P("DP alt mode"));
        else { o.s("DP"); if (l.dp_pin) { o.c(' '); o.c(l.dp_pin); } }
    }
    if (!any) o.s(K2_P("no alt mode"));
}

const char* power_word(std::uint8_t p) {   // <= 12 characters
    switch (p) {
    case kPowerShort: return K2_P("power short");
    case kPowerOk: return K2_P("power OK");
    case kPowerUnknown: return K2_P("need unknown");
    case kPowerUnseen: return K2_P("not observed");
    case kPowerNone: return K2_P("no contract");
    case kPowerActive: return K2_P("K2 is sink");
    case kPowerOver: return K2_P("over draw");
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
        if (f.code == Code::Draw) { lb.s(K2_P("Draw ")); lb.watts(f.c); }
        else lb.s(label(f));
        note(r, f, nt);
        if (timed(f.code) && f.t_ms) { Out t{out.tail}; tail_clock(t, f.t_ms); }
        return true;
    }
    idx -= r.n;
    out.sev = Sev::Ok;
    if (r.quiet_contract && r.quiet_link && idx-- == 0) { lb.s(K2_P("No resets")); nt.s(K2_P("no resets, retries or rejects")); return true; }
    if (r.quiet_quality && r.band_checked && idx-- == 0) { lb.s(K2_P("VBUS in band")); nt.s(K2_P("within 5% of the contract")); return true; }
    if (r.quiet_roles && idx-- == 0) { lb.s(K2_P("No refusals")); nt.s(K2_P("no refused swaps or modes")); return true; }
    out.lanes = true;
    lb.s(K2_P("Data lanes"));
    lanes_note(r, nt);
    return true;
}

void verdict(const Report& r, std::span<char> out) {
    Out o{out};
    o.tight = true;
    if (!out.empty()) out[0] = 0;
    if (r.empty) { o.s(K2_P("No PD session yet")); return; }
    if (r.no_pd) { o.s(K2_P("No PD seen, VBUS ")); o.volts(r.vbus_mv); return; }
    o.s(r.link == 2 ? K2_P("Link unstable, ") : r.link == 1 ? K2_P("Link degraded, ") : K2_P("Link OK, "));
    o.s(power_word(r.power));
}

Sev worst(const Report& r) {
    Sev w = Sev::Ok;
    for (unsigned i = 0; i < r.n; i++) if (r.f[i].sev < w) w = r.f[i].sev;
    return w;
}

bool names_top_finding(const Report& r) { return !r.empty && !r.no_pd && r.n && r.f[0].sev <= Sev::Warn; }

bool is_event(Code c) {
    switch (c) {   // no default: a new code must be classed here (-Wswitch)
    case Code::OverDraw: case Code::OverSustained: case Code::InputCuts: case Code::HardReset: case Code::SoftReset:
    case Code::Reattach: case Code::Rejected: case Code::WaitLoop: case Code::ManyRequests: case Code::NoAccept:
    case Code::LateAccept: case Code::NoPsRdy: case Code::LatePsRdy: case Code::VbusLow: case Code::VbusHigh:
    case Code::VbusLost: case Code::NoGoodCrc: case Code::Retries: case Code::TxFail: case Code::RxBad: case Code::ModeNak:
    case Code::EnterUsbRefused: case Code::EprFailed:
        return true;
    case Code::PowerShort: case Code::MismatchFlag: case Code::NoMismatchFlag: case Code::Pinned: case Code::Draw:
    case Code::DrawReduced: case Code::FiveVoltOnly: case Code::ContractUnseen: case Code::NoContract: case Code::ActiveSink:
    case Code::CapsUnanswered: case Code::SwapRefused: case Code::CableCap: case Code::VdmLoop: case Code::CableSilent:
    case Code::CaptureGap:
        return false;
    }
    return false;
}

void word(const Report& r, std::span<char> out, Sev& sev, std::uint16_t hide) {
    Out o{out};
    if (!out.empty()) out[0] = 0;
    sev = Sev::Ok;
    if (r.empty) { o.s(K2_P("No PD session")); return; }
    if (r.no_pd) { o.s(K2_P("No PD seen")); return; }
    for (unsigned i = 0; i < r.n && r.f[i].sev <= Sev::Warn; i++) {   // findings are ranked: the first shown is the worst
        if (i < 16 && (hide >> i & 1u)) continue;
        sev = r.f[i].sev;
        Row w;
        row(r, i, w);
        o.s(w.label.data());
        return;
    }
    switch (r.power) {
    case kPowerUnseen: o.s(K2_P("Contract unseen")); break;
    case kPowerNone: o.s(K2_P("No contract")); break;
    case kPowerActive: o.s(K2_P("K2 is the sink")); break;
    case kPowerUnknown: o.s(K2_P("Link healthy")); break;
    default: o.s(K2_P("Healthy")); break;
    }
}

}  // namespace k2::pd::diag::screen
