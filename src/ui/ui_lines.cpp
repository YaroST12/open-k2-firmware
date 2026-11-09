// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#include "k2/ui/ui_lines.hpp"

namespace k2 {

void rp_text(StrBuf<24> &s, bool have, std::uint8_t active_cc, int32_t cc1, int32_t cc2) {
    if (!have) { s.put("--"); return; }
    // The live Rp-over-Rd wire is the one the PD engine listens on; name that wire even when it is
    // the lower-voltage one. Without an active line, fall back to the higher-voltage wire.
    const bool second = active_cc == 2 || (active_cc != 1 && cc2 > cc1);
    const int32_t mv = second ? cc2 : cc1;
    if (mv < 200) { s.put("none on CC"); return; }
    if (mv < 660) s.put("USB");
    else if (mv < 1230) s.put("1.5A");
    else if (mv < 2200) s.put("3.0A");
    else s.put("?");
    s.put(second ? " on CC2" : " on CC1");
}

void mcu_text(StrBuf<24> &s, bool valid, int32_t d) {
    if (!valid) { s.put("--"); return; }
    const int32_t whole = d >= 0 ? (d + 5) / 10 : -((-d + 5) / 10);
    s.put_fixed(whole * 1000, 0).put("\xC2\xB0" "C");
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
        set("PD?", "", Tone::Caution, "No caps seen");
        return v;
    }
    switch (p) {
    case Protocol::Pending:  set("...", "", Tone::Dim, "Reading the data lines"); break;
    case Protocol::NoInput:  set("No VBUS", "", Tone::Caution, "Plug in a source"); break;
    case Protocol::NoQuick:  set("5V only", "", Tone::Dim, "No handshake on D+ / D-"); break;
    case Protocol::Dcp5V:    set("DCP short", "", Tone::Ok, "D+ = D-: dedicated charger"); break;
    case Protocol::Qc3:      set("QC3.0", "", Tone::Info, "D+ 0.6 V, D- steps: QC3.0"); break;
    case Protocol::Qc2_5V:   set("QC2.0", "5V", Tone::Info, "D+ 0.6 V, D- 0 V: QC 5 V"); break;
    case Protocol::Qc2_9V:   set("QC2.0", "9V", Tone::Info, "D+ 3.3 V, D- 0.6 V: 9 V"); break;
    case Protocol::Qc2_12V:  set("QC2.0", "12V", Tone::Info, "D+ 0.6 V, D- 0.6 V: 12 V"); break;
    case Protocol::Qc2_20V:  set("QC2.0", "20V", Tone::Info, "D+ 3.3 V, D- 3.3 V: 20 V"); break;
    case Protocol::AfcFcp:   set("AFC/FCP", "", Tone::Info, "Samsung/Huawei handshake"); break;
    case Protocol::Scp:      set("SCP", "", Tone::Info, "Huawei super charge"); break;
    case Protocol::Apple24:  set("Divider", "Apple 2.4A", Tone::Info, "D+/D- dividers: Apple 2.4A"); break;
    case Protocol::Apple21:  set("Divider", "Apple 2.1A", Tone::Info, "D+/D- dividers: Apple 2.1A"); break;
    case Protocol::Apple10:  set("Divider", "Apple 1.0A", Tone::Info, "D+/D- dividers: Apple 1.0A"); break;
    case Protocol::Usb2Hi:   set("USB2.0", "", Tone::Dim, "USB 2.0 high-speed data"); break;
    case Protocol::Usb11Low: set("USB1.1", "", Tone::Dim, "USB 1.1 low-speed data"); break;
    case Protocol::Sam20:    set("Divider", "Samsung 2A", Tone::Info, "D+/D- dividers: Samsung"); break;
    case Protocol::VoocWarp: set("VOOC", "", Tone::Info, "VOOC or Warp handshake"); break;
    case Protocol::Svooc:    set("SVOOC", "", Tone::Info, "Super VOOC handshake"); break;
    case Protocol::PdPe:     set("PD PE", "", Tone::Ok, "PD on CC, PE on D+/D-"); break;
    case Protocol::PdMtk:    set("PD MTK", "", Tone::Ok, "PD on CC, MTK on D+/D-"); break;
    case Protocol::Pd:       set("PD", "", Tone::Ok, "PD: see Who's attached"); break;
    case Protocol::UsbData:  set("USB data", "", Tone::Dim, "A data device idles here"); break;
    case Protocol::PdLikely: set("PD?", "", Tone::Caution, "Load above 5.5 V, no caps"); break;
    }
    return v;
}

}  // namespace k2
