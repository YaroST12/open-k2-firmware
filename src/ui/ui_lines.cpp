// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#include "k2/ui/ui_lines.hpp"

#include <iterator>

#include "k2/core/strpack.hpp"
#include "k2/ui/trunc.hpp"

namespace k2 {

void rp_text(StrBuf<24> &s, bool have, std::uint8_t active_cc, int32_t cc1, int32_t cc2) {
    if (!have) { s.put("--"); return; }
    // The live Rp-over-Rd wire is the one the PD engine listens on; name that wire even when it is
    // the lower-voltage one. Without an active line, fall back to the higher-voltage wire.
    const bool second = active_cc == 2 || (active_cc != 1 && cc2 > cc1);
    const int32_t mv = second ? cc2 : cc1;
    if (mv < 200) { s.put(K2_P("none on CC")); return; }
    if (mv < 660) s.put("USB");
    else if (mv < 1230) s.put("1.5A");
    else if (mv < 2200) s.put("3.0A");
    else s.put("?");
    s.put(second ? K2_P(" on CC2") : K2_P(" on CC1"));
}

void mcu_text(StrBuf<24> &s, bool valid, int32_t d) {
    if (!valid) { s.put("--"); return; }
    const int32_t whole = d >= 0 ? (d + 5) / 10 : -((-d + 5) / 10);
    s.put_fixed(whole * 1000, 0).put("\xC2\xB0" "C");
}

namespace {
// Indexed by fastcharge::Protocol.
constexpr const char *kDpDmWords[] = {
    "--", "--", "none", K2_P("DCP short"), K2_P("QC3.0"), K2_P("QC2.0 5V"), K2_P("QC2.0 9V"), K2_P("QC2.0 12V"), K2_P("QC2.0 20V"), K2_P("AFC/FCP"), "SCP",
    K2_P("Apple 2.4A"), K2_P("Apple 2.1A"), K2_P("Apple 1.0A"), K2_P("USB2.0"), K2_P("USB1.1"), K2_P("Samsung 2A"), "VOOC", K2_P("SVOOC"),
    "none", "none", "none", K2_P("USB data"), "none"};
static_assert(std::size(kDpDmWords) == static_cast<std::size_t>(fastcharge::Protocol::PdLikely) + 1);
static_assert(all_fit_p(kDpDmWords, 8, 12 * 8), "the Lines value cell holds 12 cells of the 16 px font");
}  // namespace

DpDmWord dpdm_word(fastcharge::Protocol p) {
    using fastcharge::Protocol;
    const auto i = static_cast<std::size_t>(p);
    if (i >= std::size(kDpDmWords)) return {"--", true};
    const bool dim = p == Protocol::Pending || p == Protocol::NoInput || p == Protocol::NoQuick || p == Protocol::Usb2Hi ||
                     p == Protocol::Usb11Low || p == Protocol::UsbData || kDpDmWords[i][0] == 'n';
    return {kDpDmWords[i], dim};
}

LinesVerdict lines_verdict(fastcharge::Protocol p, bool pd_decoded) {
    using fastcharge::Protocol;
    LinesVerdict v;
    auto set = [&](const char *main, const char *extra, Tone t, const char *foot) {
        v.main.put(main); v.extra.put(extra); v.tone = t; v.footer.put(foot);
    };
    // A PD-family verdict is only proven by a decoded PD message (a contract or source caps); without
    // one the chip stays a neutral "PD?" caution, whatever the CC and D+/D- levels suggest.
    if (!pd_decoded && (p == Protocol::Pd || p == Protocol::PdPe || p == Protocol::PdMtk)) {
        set("PD?", "", Tone::Caution, K2_P("No caps seen"));
        return v;
    }
    switch (p) {
    case Protocol::Pending:  set("...", "", Tone::Dim, K2_P("Reading the data lines")); break;
    case Protocol::NoInput:  set(K2_P("No VBUS"), "", Tone::Caution, K2_P("Plug in a source")); break;
    case Protocol::NoQuick:  set(K2_P("5V only"), "", Tone::Dim, K2_P("No handshake on D+ / D-")); break;
    case Protocol::Dcp5V:    set(K2_P("DCP short"), "", Tone::Ok, K2_P("D+ = D-: dedicated charger")); break;
    case Protocol::Qc3:      set(K2_P("QC3.0"), "", Tone::Info, K2_P("D+ 0.6 V, D- steps: QC3.0")); break;
    case Protocol::Qc2_5V:   set(K2_P("QC2.0"), "5V", Tone::Info, K2_P("D+ 0.6 V, D- 0 V: QC 5 V")); break;
    case Protocol::Qc2_9V:   set(K2_P("QC2.0"), "9V", Tone::Info, K2_P("D+ 3.3 V, D- 0.6 V: 9 V")); break;
    case Protocol::Qc2_12V:  set(K2_P("QC2.0"), "12V", Tone::Info, K2_P("D+ 0.6 V, D- 0.6 V: 12 V")); break;
    case Protocol::Qc2_20V:  set(K2_P("QC2.0"), "20V", Tone::Info, K2_P("D+ 3.3 V, D- 3.3 V: 20 V")); break;
    case Protocol::AfcFcp:   set(K2_P("AFC/FCP"), "", Tone::Info, K2_P("Samsung/Huawei handshake")); break;
    case Protocol::Scp:      set("SCP", "", Tone::Info, K2_P("Huawei super charge")); break;
    case Protocol::Apple24:  set(K2_P("Divider"), K2_P("Apple 2.4A"), Tone::Info, K2_P("D+/D- dividers: Apple 2.4A")); break;
    case Protocol::Apple21:  set(K2_P("Divider"), K2_P("Apple 2.1A"), Tone::Info, K2_P("D+/D- dividers: Apple 2.1A")); break;
    case Protocol::Apple10:  set(K2_P("Divider"), K2_P("Apple 1.0A"), Tone::Info, K2_P("D+/D- dividers: Apple 1.0A")); break;
    case Protocol::Usb2Hi:   set(K2_P("USB2.0"), "", Tone::Dim, K2_P("USB 2.0 high-speed data")); break;
    case Protocol::Usb11Low: set(K2_P("USB1.1"), "", Tone::Dim, K2_P("USB 1.1 low-speed data")); break;
    case Protocol::Sam20:    set(K2_P("Divider"), K2_P("Samsung 2A"), Tone::Info, K2_P("D+/D- dividers: Samsung")); break;
    case Protocol::VoocWarp: set("VOOC", "", Tone::Info, K2_P("VOOC or Warp handshake")); break;
    case Protocol::Svooc:    set(K2_P("SVOOC"), "", Tone::Info, K2_P("Super VOOC handshake")); break;
    case Protocol::PdPe:     set(K2_P("PD PE"), "", Tone::Ok, K2_P("PD on CC, PE on D+/D-")); break;
    case Protocol::PdMtk:    set(K2_P("PD MTK"), "", Tone::Ok, K2_P("PD on CC, MTK on D+/D-")); break;
    case Protocol::Pd:       set("PD", "", Tone::Ok, K2_P("PD: see Bench > Charger")); break;
    case Protocol::UsbData:  set(K2_P("USB data"), "", Tone::Dim, K2_P("A data device idles here")); break;
    case Protocol::PdLikely: set("PD?", "", Tone::Caution, K2_P("Load above 5.5 V, no caps")); break;
    }
    return v;
}

}  // namespace k2
