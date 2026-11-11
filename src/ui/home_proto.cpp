// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#include "k2/ui/home_proto.hpp"

namespace k2::home {
namespace {

using fastcharge::Protocol;

constexpr uint32_t kCableMaxMa = 5000;   // a standard cable carries 5 A: the cap of an AVS current figure

uint32_t nominal_mv(int32_t vbus_mv) {
    return vbus_mv <= 0 ? 0 : (static_cast<uint32_t>(vbus_mv) + 500) / 1000 * 1000;
}

ProtoInfo from_pd(const ProtoSource &s) {
    ProtoInfo p;
    p.kind = ProtoKind::PdFixed;
    const pd::SourceCaps *c = s.caps;
    const bool have_caps = c && c->valid && c->num_pdos > 0;
    uint32_t top = 0;
    if (have_caps)
        for (int i = 0; i < c->num_pdos && i < static_cast<int>(pd::kMaxPdos); i++)
            top = c->pdos[i].max_mv > top ? c->pdos[i].max_mv : top;
    if (top) p.max_mv = top;
    // No PDO at the Request's position (caps missed at boot, sniffed later, or the position beyond
    // the list): the RDO layout is a guess - a PPS voltage would read as a max current ("PD
    // 0V5.82A"). Show the measured level and no current, never a guessed value. A Request repeated
    // every <= kPpsKeepaliveMs with no caps is the PPS keep-alive: label it PPS with the level.
    if (!have_caps || s.ctr.pos < 1 || s.ctr.pos > c->num_pdos) {
        const uint32_t v = nominal_mv(s.vbus_mv);
        p.mv = v;
        p.ma = 0;
        if (!have_caps) p.max_mv = 0;
        if (!have_caps && s.req_period_ms != 0 && s.req_period_ms <= kPpsKeepaliveMs) {
            p.kind = ProtoKind::Pps;
            p.lo_mv = v;
            p.hi_mv = v;
        }
        return p;
    }
    p.mv = s.ctr.mv;
    p.ma = s.ctr.max_ma;
    if (!p.max_mv) p.max_mv = s.ctr.mv;
    const pd::Pdo &o = c->pdos[s.ctr.pos - 1];
    switch (o.type) {
    case pd::PdoType::Pps:
        p.kind = ProtoKind::Pps;
        p.lo_mv = o.min_mv; p.hi_mv = o.max_mv;
        if (o.max_ma) p.ma = o.max_ma;
        break;
    case pd::PdoType::Avs: {
        p.kind = ProtoKind::Avs;
        p.lo_mv = o.min_mv; p.hi_mv = o.max_mv; p.mw = o.max_mw;
        const uint32_t ref = s.ctr.mv ? s.ctr.mv : o.max_mv;
        if (o.max_ma) p.ma = o.max_ma;
        else if (o.max_mw && ref) { const uint32_t a = static_cast<uint32_t>(static_cast<uint64_t>(o.max_mw) * 1000 / ref); p.ma = a > kCableMaxMa ? kCableMaxMa : a; }
        break;
    }
    default:
        p.kind = (s.ctr.pos >= 8 || s.ctr.mv > 20000) ? ProtoKind::PdEpr : ProtoKind::PdFixed;
        if (!p.mv) p.mv = o.max_mv;
        if (o.max_ma) p.ma = o.max_ma;
        break;
    }
    return p;
}

ProtoInfo fixed_info(ProtoKind k, uint32_t mv, uint32_t ma, uint32_t max_mv) {
    ProtoInfo p;
    p.kind = k; p.mv = mv; p.ma = ma; p.max_mv = max_mv;
    return p;
}

}  // namespace

ProtoInfo derive_proto(const ProtoSource &s) {
    if (s.vooc_live) {
        // A live, proven VOOC exchange on D+/D- wins over any PD contract: a phone holds a 5 V contract on CC and runs
        // SuperVOOC on the data lines at the same time, and the lines carry what really charges it. The charger
        // announces no level we can read, so the row shows the measured VBUS and |IBUS| at 0.1 resolution. No rating:
        // ma stays 0 (no limit bar, no alarm). The PD row returns when the session ends.
        ProtoInfo p;
        p.kind = ProtoKind::Svooc;
        p.measured = true;
        p.mv = s.vbus_mv > 0 ? (static_cast<uint32_t>(s.vbus_mv) + 50) / 100 * 100 : 0;
        const uint32_t a = static_cast<uint32_t>(s.ibus_ma < 0 ? -s.ibus_ma : s.ibus_ma);
        p.shown_ma = (a + 50) / 100 * 100;
        return p;
    }
    // The K2's OWN sink holds the contract (SinkStatus ready/held, pos and PDO known via the caps):
    // the footer uses that contract - the K2 never hears its own Requests on CC, so the sniffed
    // `ctr` belongs to a far sink and is used only when ours holds none (or its PDO is unknown).
    // Own AVS -> "AVS 12.6V", PPS -> "PPS 9V", fixed -> "PD 9V 3A" (ROADMAP "PD 0V5.82A" row).
    if (s.sink && (s.sink->state == pd::SinkState::Ready || s.sink->held) && s.sink->pos >= 1 &&
        s.caps && s.caps->valid && s.sink->pos <= s.caps->num_pdos) {
        ProtoSource own = s;
        own.ctr = {};
        own.ctr.valid = true; own.ctr.ready = true;
        own.ctr.pos = s.sink->pos;
        own.ctr.mv = s.sink->mv;
        own.ctr.op_ma = s.sink->ma; own.ctr.max_ma = s.sink->ma;
        own.req_period_ms = 0;   // own Requests are never sniffed: no keep-alive guessing
        ProtoInfo p = from_pd(own);
        if (p.kind == ProtoKind::Pps || p.kind == ProtoKind::Avs) {
            // the contract's own level, not the APDO's advertised range: "PPS 9V" / "AVS 12.6V"
            p.lo_mv = p.mv; p.hi_mv = p.mv; p.ma = 0; p.mw = 0;
        }
        return p;
    }
    if (s.ctr.valid && (s.pd_link || s.caps)) return from_pd(s);
    const uint32_t v = nominal_mv(s.vbus_mv);
    switch (s.fc) {
    case Protocol::Pending: return {ProtoKind::Detecting};
    case Protocol::NoInput: return {};
    case Protocol::NoQuick:
    case Protocol::Usb2Hi: case Protocol::Usb11Low: case Protocol::UsbData:
        return fixed_info(ProtoKind::TypeC, v, s.typec_ma, 5000);
    case Protocol::Dcp5V: case Protocol::Sam20: return fixed_info(ProtoKind::Dcp, 5000, 0, 5000);
    case Protocol::Apple24: return fixed_info(ProtoKind::Dcp, 5000, 2400, 5000);
    case Protocol::Apple21: return fixed_info(ProtoKind::Dcp, 5000, 2100, 5000);
    case Protocol::Apple10: return fixed_info(ProtoKind::Dcp, 5000, 1000, 5000);
    case Protocol::Qc2_5V: return fixed_info(ProtoKind::Qc2, 5000, 0, 12000);
    case Protocol::Qc2_9V: return fixed_info(ProtoKind::Qc2, 9000, 0, 12000);
    case Protocol::Qc2_12V: return fixed_info(ProtoKind::Qc2, 12000, 0, 12000);
    case Protocol::Qc2_20V: return fixed_info(ProtoKind::Qc2, 20000, 0, 20000);
    case Protocol::Qc3: {
        ProtoInfo p;
        p.kind = ProtoKind::Qc3; p.lo_mv = 3600; p.hi_mv = 12000; p.max_mv = 12000;
        return p;
    }
    case Protocol::AfcFcp: return fixed_info(ProtoKind::Fcp, v, 0, 0);
    case Protocol::Scp: return fixed_info(ProtoKind::Scp, v, 0, 0);
    case Protocol::VoocWarp: case Protocol::Svooc: return fixed_info(ProtoKind::Svooc, v, 0, 0);
    case Protocol::PdPe: case Protocol::PdMtk: case Protocol::Pd: case Protocol::PdLikely:
        // A "PD" label needs a decoded PD message: here that is the Source_Capabilities (a decoded
        // contract is handled by the first branch). Otherwise only the level and the Rp current are
        // known, so show those, like a plain Type-C source.
        if (s.caps && s.caps->valid) return fixed_info(ProtoKind::PdFixed, v, 0, 0);
        return fixed_info(ProtoKind::TypeC, v, s.typec_ma, 0);
    }
    return {ProtoKind::Unknown};
}

}  // namespace k2::home
