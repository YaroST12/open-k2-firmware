// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#include "k2/ui/ui_emarker.hpp"
#include <cstring>

namespace k2 {

namespace {
enum Kind : uint8_t { Vendor, Vid, Pid, HwFw, Length, Latency, Term, Xid, Sop, Temp, Usb4, Tbt, Vdo };

// Row kinds in display order; raw VDOs (one row each) come last.
int layout(const pd::Emarker &e, bool tbt, uint8_t *kinds) {
    int n = 0;
    kinds[n++] = Vendor; kinds[n++] = Vid; kinds[n++] = Pid; kinds[n++] = HwFw;
    kinds[n++] = Length; kinds[n++] = Latency; kinds[n++] = Term; kinds[n++] = Xid; kinds[n++] = Sop;
    if (e.vdo2_valid) { kinds[n++] = Temp; kinds[n++] = Usb4; }
    if (tbt) kinds[n++] = Tbt;
    const int vdos = e.num_vdos > 6 ? 6 : e.num_vdos;
    for (int i = 0; i < vdos && n < kEmKvMax; i++) kinds[n++] = Vdo;
    return n;
}

// "<10 ns (~1 m)" -> "<10 ns": the text before the bracket.
void latency_short(const char *s, StrBuf<16> &out) {
    if (!s) { out.put("n/a"); return; }
    for (; *s && *s != '('; s++)
        if (!(*s == ' ' && s[1] == '(')) out.put_char(*s);
}
}  // namespace

void emarker_amps_text(const pd::Emarker &e, StrBuf<8> &out) { out.put_u32(e.max_current_a).put_char('A'); }
void emarker_volts_text(const pd::Emarker &e, StrBuf<8> &out) { out.put_u32(e.max_voltage_v).put_char('V'); }
void emarker_watts_text(const pd::Emarker &e, StrBuf<8> &out) { out.put_u32(e.max_power_w).put_char('W'); }

void emarker_speed_pill(const pd::Emarker &e, StrBuf<12> &out) {
    const char *s = e.speed_str ? e.speed_str : "USB 2.0";
    if (!std::strncmp(s, "Thunderbolt", 11)) out.put("TBT");
    else if (!std::strncmp(s, "USB 3.2 Gen1", 12)) out.put("USB3.2 G1");
    else if (!std::strncmp(s, "USB 3.2 Gen2", 12)) out.put("USB3.2 G2");
    else if (!std::strncmp(s, "USB4 Gen3", 9)) out.put("USB4 G3");
    else if (!std::strncmp(s, "USB4 Gen4", 9)) out.put("USB4 G4");
    else out.put("USB2.0");
}

int emarker_kv_count(const pd::Emarker &e, bool tbt) {
    uint8_t k[kEmKvMax];
    return layout(e, tbt, k);
}

void emarker_kv(const pd::Emarker &e, bool tbt, int i, StrBuf<12> &key, StrBuf<16> &val) {
    uint8_t k[kEmKvMax];
    const int n = layout(e, tbt, k);
    if (i < 0 || i >= n) return;
    switch (k[i]) {
    case Vendor: key.put("Vendor"); val.put(e.vendor_str ? e.vendor_str : "Unknown"); break;
    case Vid: key.put("VID"); val.put_hex(e.vid, 4); break;
    case Pid: key.put("PID"); val.put_hex(e.pid, 4); break;
    case HwFw: key.put("HW/FW"); val.put_u32(e.hw_rev).put(" / ").put_u32(e.fw_rev); break;
    case Length:
        key.put("Length");
        if (e.latency_code >= 1 && e.latency_code <= 7) val.put("~").put_u32(e.latency_code).put(" m");
        else if (e.latency_code == 8) val.put(">7 m");
        else val.put("n/a");
        break;
    case Latency: key.put("Latency"); latency_short(e.latency_str, val); break;
    case Term:
        key.put("Term");
        val.put(e.termination == 1 ? "VCONN req" : e.termination == 2 ? "active 1" : e.termination == 3 ? "active 2" : "passive");
        break;
    case Xid: key.put("XID"); val.put_hex(e.xid, 8); break;
    case Sop:
        key.put("SOP");
        val.put(e.sop == 2 ? "''" : "'").put(e.spec_rev == 1 ? " PD2.0" : " PD3.x");
        break;
    case Temp: key.put("Temp"); val.put_u32(e.max_temp_c).put_char('/').put_u32(e.shutdown_temp_c).put_char('C'); break;
    case Usb4:
        key.put("USB4");
        val.put(e.usb4_supported ? "yes " : "no ").put_u32(e.usb_lanes).put("ln");
        break;
    case Tbt: key.put("TBT"); val.put("SVID 8087"); break;
    default: {
        int idx = 0;
        for (int j = 0; j < i; j++) if (k[j] == Vdo) idx++;
        key.put("VDO").put_u32(static_cast<uint32_t>(idx));
        val.put_hex(e.raw_vdos[idx], 8);
    }
    }
}

}  // namespace k2
