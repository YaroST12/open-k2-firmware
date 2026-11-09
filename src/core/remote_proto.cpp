// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Remote-control framing and payload encoders. Pure, see remote_proto.hpp for the layouts.
#include "k2/core/remote_proto.hpp"
#include <algorithm>

namespace k2::remote {

namespace {

// Bounded little-endian writer: bytes past the end are dropped, size() reports what fit.
class Writer {
public:
    explicit Writer(std::span<uint8_t> out) : out_(out) {}
    void u8(uint8_t v) { if (n_ < out_.size()) out_[n_++] = v; }
    void u16(uint16_t v) { u8(static_cast<uint8_t>(v)); u8(static_cast<uint8_t>(v >> 8)); }
    void u32(uint32_t v) { u16(static_cast<uint16_t>(v)); u16(static_cast<uint16_t>(v >> 16)); }
    void i32(int32_t v) { u32(static_cast<uint32_t>(v)); }
    void text(std::string_view s) { for (char c : s) u8(static_cast<uint8_t>(c)); }
    void pad_text(std::string_view s, size_t width) {
        for (size_t i = 0; i < width; i++) u8(i < s.size() ? static_cast<uint8_t>(s[i]) : 0);
    }
    size_t size() const { return n_; }

private:
    std::span<uint8_t> out_;
    size_t n_ = 0;
};

}  // namespace

ParseResult parse_request(std::span<const uint8_t> raw) {
    ParseResult r;
    if (raw.size() < kReqHeader) {
        r.error = ParseError::TooShort;
        return r;
    }
    if (raw[0] != kReqMagic0 || raw[1] != kReqMagic1) {
        r.error = ParseError::BadMagic;
        return r;
    }
    r.header_valid = true;
    r.cmd = raw[2];
    r.seq = raw[3];
    const size_t len = raw[4];
    if (len > kMaxReqPayload) {
        r.error = ParseError::Oversize;
        return r;
    }
    if (raw.size() < kReqHeader + len) {
        r.error = ParseError::Truncated;
        return r;
    }
    r.payload = raw.subspan(kReqHeader, len);
    return r;
}

size_t build_response(std::span<uint8_t, kFrameSize> out, uint8_t cmd, uint8_t seq, Status status,
                      std::span<const uint8_t> payload) {
    std::fill(out.begin(), out.end(), uint8_t{0});
    const size_t len = std::min(payload.size(), kMaxRspPayload);
    out[0] = kRspMagic0;
    out[1] = kRspMagic1;
    out[2] = cmd;
    out[3] = seq;
    out[4] = static_cast<uint8_t>(status);
    out[5] = static_cast<uint8_t>(len);
    std::copy_n(payload.begin(), len, out.begin() + kRspHeader);
    return kRspHeader + len;
}

size_t encode_ping(std::span<uint8_t> out, const PingInfo &p) {
    Writer w(out);
    w.u8(kProtoVersion);
    w.u32(p.uptime_ms);
    w.pad_text(p.git, kPingGitLen);
    w.text(p.version);
    return w.size();
}

namespace {
bool legacy_step(const charger_test::Step& s) {
    return s.group <= charger_test::Group::SuperVooc65 && s.algorithm < charger_test::Algorithm::Pd30Revision;
}
uint8_t legacy_reason(charger_test::Reason r, const charger_test::Step* step = nullptr,
                      const charger_test::Evidence* e = nullptr) {
    using R = charger_test::Reason;
    switch (r) {
    case R::RevisionUntestable: case R::UnsupportedOffer: return static_cast<uint8_t>(R::NotImplemented);
    case R::EmptySelection: case R::NoQc3Plus: return static_cast<uint8_t>(R::NoOffer);   // schema 1 has no "no QC3+ response": "not advertised"
    case R::ClassAOnly: return static_cast<uint8_t>(R::NoOffer);   // schema 1 has no "Class A only": the 20 V level is "not advertised"
    case R::NeedsExternalPower: return static_cast<uint8_t>(R::Disabled);   // schema 1 has no "needs external power": not run
    case R::PdGaveUp: return static_cast<uint8_t>(R::NoOffer);              // schema 1 has no "PD gave up": "not advertised"
    case R::NotUnlocked: return static_cast<uint8_t>(R::NoOffer);           // schema 1 has no "not unlocked": the 6 A level is "not advertised"
    case R::Qc3Only20V: return static_cast<uint8_t>(R::Voltage);           // schema 1 has no "20 V via QC3 only": the level was not reached
    case R::PdOnly: return static_cast<uint8_t>(R::PdActive);              // schema 1 has no "PD only": the same family as "PD active"
    case R::Capped: return static_cast<uint8_t>(R::Timeout);                // schema 1 has no "capped": the level never came (what it was before)
    case R::NoContract: return static_cast<uint8_t>(R::NoSource);
    case R::EprRefused: return static_cast<uint8_t>(R::Request);
    case R::StayedPd: case R::BlockedByPd: return static_cast<uint8_t>(R::PdActive);
    case R::RebootRequest: case R::RebootHandover: case R::RebootLimit: case R::ShutOff: case R::NoReattach: case R::CutRecovered: return static_cast<uint8_t>(R::Lost);
    case R::NeedsLoad: return static_cast<uint8_t>(R::Lost);               // schema 1 has no "needs load": the source dropped out (what it was before)
    case R::CableLimit:
        return static_cast<uint8_t>(step && e && step->mv > e->cable_mv ? R::OverCap : R::NoOffer);
    default: return static_cast<uint8_t>(r);
    }
}
}
size_t charger_test_count(const ChargerTestSnapshot& s, uint8_t schema) {
    if (schema != kChargerTestSchema && schema != kChargerTestSchema2) return 0;
    return schema == kChargerTestSchema2 ? s.steps.size() : std::count_if(s.steps.begin(), s.steps.end(), legacy_step);
}
size_t encode_charger_test(std::span<uint8_t> out, const ChargerTestSnapshot& s, uint8_t start, bool rows, uint8_t schema) {
    if (schema != kChargerTestSchema && schema != kChargerTestSchema2) return 0;
    const bool modern = schema == kChargerTestSchema2;
    const auto total = charger_test_count(s, schema);
    const size_t header = modern ? kChargerTestHeader2 : kChargerTestHeader;
    const size_t page = modern ? kChargerTestPage2 : kChargerTestPage;
    const size_t available = start <= total ? total - start : 0;
    const size_t fit = out.size() >= header ? (out.size() - header) / kChargerTestRow : 0;
    const size_t n = rows ? std::min({available, fit, page}) : 0;
    size_t current = std::min<size_t>(s.current, s.steps.size());
    if (!modern) {
        current = std::count_if(s.steps.begin(), s.steps.begin() + current, legacy_step);
    }
    Writer w(out);
    w.u8(schema); w.u32(s.run_id); w.u32(s.revision);
    w.u8(s.state); w.u8(modern ? s.reason : legacy_reason(static_cast<charger_test::Reason>(s.reason)));
    w.u8(static_cast<uint8_t>(total)); w.u8(static_cast<uint8_t>(current)); w.u8(start); w.u8(static_cast<uint8_t>(n));
    if (modern) {
        const auto& e = s.evidence;
        w.u32(e.selected); w.u32(s.configured);
        w.u8((e.wire_rev & 3) | ((s.stage & 15) << 2) | (std::min<unsigned>(s.resumed, 3) << 6));
        w.u8((e.reported.rev_major << 4) | e.reported.rev_minor); w.u8(e.flags);
        w.u16(e.monitor_mv); w.u16(e.cable_mv); w.u16(e.cable_ma);
        w.u8((e.reported.ver_major << 4) | e.reported.ver_minor);
        w.u32(e.session); w.u16(e.raw_header); w.u8((e.num_pdos & 15) | (std::min<unsigned>(s.cursor, 15) << 4));
    }
    size_t row = 0, written = 0;
    for (const auto& step : s.steps) {
        if (!modern && !legacy_step(step)) continue;
        if (row++ < start) continue;
        if (written++ == n) break;
        w.u8(modern ? static_cast<uint8_t>(step.algorithm) : static_cast<uint8_t>(step.group));
        w.u8(step.pos);
        w.u8((step.continuous ? 1 : 0) | (step.avs ? 2 : 0) | (std::min<unsigned>(step.tries, 7) << 2));
        w.u16(step.mv); w.u16(step.ma); w.u16(step.measured_mv);
        w.u8(static_cast<uint8_t>(step.verdict));
        w.u8(modern ? static_cast<uint8_t>(step.reason) : legacy_reason(step.reason, &step, &s.evidence));
        w.u8(step.detail); w.u32(step.pdo);
    }
    return w.size();
}

size_t encode_state(std::span<uint8_t> out, const StateInfo &s) {
    Writer w(out);
    w.u8(s.page); w.u8(s.active_cc); w.u8(s.spec_rev); w.u8(s.num_pdos);
    w.u8(s.pd_valid); w.u8(s.chip); w.u8(s.pm_online); w.u8(s.log_on);
    w.i32(s.vbus_mv); w.i32(s.current_ma); w.i32(s.power_mw); w.i32(s.energy_mwh);
    w.u16(s.dp_mv); w.u16(s.dm_mv); w.u16(s.cc1_mv); w.u16(s.cc2_mv); w.u16(s.aux_mv);
    w.u16(static_cast<uint16_t>(s.temp_deci_c)); w.u8(s.fc); w.u8(s.adc_flags);
    w.u16(static_cast<uint16_t>(s.mcu_temp_deci_c));
    w.u8(s.link_flags); w.u32(s.age_s); w.u32(s.cable_age_s);
    w.u32(s.reset_flags);
    return w.size();
}

size_t encode_pdo(std::span<uint8_t> out, const PdoSnapshot &s, uint8_t index) {
    Writer w(out);
    w.u8(s.valid); w.u8(s.spec_rev); w.u8(s.num_pdos); w.u8(index);
    if (index < s.num_pdos && index < s.pdo.size()) {
        const PdoRec &p = s.pdo[index];
        w.u8(p.type); w.u32(p.raw);
        w.u16(p.min_mv); w.u16(p.max_mv); w.u16(p.max_ma); w.u32(p.max_mw);
    }
    return w.size();
}

size_t build_stream_frame(std::span<uint8_t, kFrameSize> out, uint8_t seq, uint8_t strip, uint8_t flags,
                          std::span<const uint8_t> rle) {
    std::fill(out.begin(), out.end(), uint8_t{0});
    const size_t len = std::min(rle.size(), kShotChunk);
    out[0] = kRspMagic0;
    out[1] = kRspMagic1;
    out[2] = kCmdScreenData;
    out[3] = seq;
    out[4] = 0;
    out[5] = static_cast<uint8_t>(2 + len);
    out[6] = strip;
    out[7] = flags;
    std::copy_n(rle.begin(), len, out.begin() + kRspHeader + 2);
    return kRspHeader + 2 + len;
}

size_t build_ripple_frame(std::span<uint8_t, kFrameSize> out, uint8_t seq, uint16_t offset,
                          std::span<const uint16_t> samples) {
    std::fill(out.begin(), out.end(), uint8_t{0});
    const size_t n = std::min(samples.size(), kRippleChunk);
    out[0] = kRspMagic0;
    out[1] = kRspMagic1;
    out[2] = kCmdRippleData;
    out[3] = seq;
    out[4] = 0;
    out[5] = static_cast<uint8_t>(2 + 2 * n);
    out[6] = static_cast<uint8_t>(offset);
    out[7] = static_cast<uint8_t>(offset >> 8);
    for (size_t i = 0; i < n; i++) {
        out[kRspHeader + 2 + 2 * i] = static_cast<uint8_t>(samples[i]);
        out[kRspHeader + 3 + 2 * i] = static_cast<uint8_t>(samples[i] >> 8);
    }
    return kRspHeader + 2 + 2 * n;
}

size_t encode_adc_raw(std::span<uint8_t> out, const AdcRawSnapshot &a) {
    Writer w(out);
    w.u8(a.ok);
    for (uint16_t v : a.all) w.u16(v);
    for (uint16_t v : a.scan) w.u16(v);
    return w.size();
}

size_t encode_vconn(std::span<uint8_t> out, const VconnSnapshot &v) {
    Writer w(out);
    w.u8(v.ok); w.u8(v.state); w.u8(v.why); w.u8(v.applied); w.u8(v.swaps_rx); w.u8(v.active_cc);
    w.u16(v.cc1_mv); w.u16(v.cc2_mv);
    return w.size();
}

size_t encode_epr_info(std::span<uint8_t> out, const EprInfoSnapshot &s) {
    Writer w(out);
    const size_t n = std::min<size_t>(s.num_pdos, s.pdos.size());
    w.u8(s.ok); w.u8(s.err); w.u8(s.state); w.u8(static_cast<uint8_t>(n));
    for (size_t k = 0; k < n; k++) w.u32(s.pdos[k]);
    return w.size();
}

size_t encode_lcd(std::span<uint8_t> out, const LcdSnapshot &s) {
    Writer w(out);
    w.u8(s.flags); w.u32(s.transfers); w.u32(s.stalls);
    return w.size();
}

size_t encode_cc_probe(std::span<uint8_t> out, const ccprobe::Result &r) {
    Writer w(out);
    const size_t n = std::min<size_t>(r.n, r.s.size());
    w.u8(r.flags); w.u8(static_cast<uint8_t>(n)); w.u8(r.sw0); w.u8(r.sw1); w.u8(r.pc); w.u16(r.modes);
    for (size_t k = 0; k < n; k++) {
        const ccprobe::Sample &s = r.s[k];
        w.u32(s.t_us); w.u16(s.cc1_raw); w.u16(s.cc2_raw); w.u16(s.vbus_mv); w.u8(s.st0_cc1); w.u8(s.st0_cc2);
    }
    return w.size();
}

size_t encode_fc(std::span<uint8_t> out, const FcInfo &i) {
    Writer w(out);
    w.u8(i.state); w.u8(i.proto); w.u8(i.fail); w.u8(i.scan_flags); w.u8(i.scan_step);
    w.u16(i.target_mv); w.u16(i.vbus_mv);
    const size_t n = std::min<size_t>(i.num_results, i.results.size());
    w.u8(static_cast<uint8_t>(n));
    for (size_t k = 0; k < n; k++) { w.u8(i.results[k].proto); w.u8(i.results[k].outcome); w.u16(i.results[k].mv); }
    return w.size();
}

size_t encode_store(std::span<uint8_t> out, const StoreInfo &i) {
    Writer w(out);
    w.u8(i.err); w.u8(i.count); w.u8(i.flags); w.u8(i.slot); w.u16(i.log_len); w.u8(i.bank); w.u8(i.native);
    return w.size();
}

size_t encode_factory_wipe(std::span<uint8_t> out, const FactoryWipeInfo &i) {
    Writer w(out);
    w.u8(i.err); w.u16(i.sectors); w.u16(i.erased); w.u16(i.verified); w.u16(i.failed); w.u32(i.first_fail); w.u8(i.status);
    return w.size();
}

size_t encode_cable_r(std::span<uint8_t> out, const CableRInfo &i) {
    Writer w(out);
    w.u8(i.flags); w.u8(i.grade); w.u16(i.r_mohm);
    w.u16(i.idle_mv); w.u16(i.idle_ma); w.u16(i.load_mv); w.u16(i.load_ma);
    w.u16(i.pos_mohm); w.u16(i.gnd_mohm);
    return w.size();
}

size_t encode_ufcs(std::span<uint8_t> out, const UfcsInfo &i) {
    Writer w(out);
    w.u8(i.state); w.u8(i.verdict); w.u8(i.hs_class); w.u8(i.start); w.u16(i.blob_len);
    w.u8(static_cast<uint8_t>(ufcs::kFieldCount));
    return w.size();
}

size_t encode_ufcs_field(std::span<uint8_t> out, uint8_t field, uint32_t value) {
    if (field >= ufcs::kFieldCount) return 0;
    const ufcs::FieldDef &d = ufcs::kFields[field];
    Writer w(out);
    w.u8(field); w.u32(value); w.u32(d.lo); w.u32(d.hi); w.u32(d.def);
    return w.size();
}

size_t encode_lab(std::span<uint8_t> out, const LabResult &r) {
    Writer w(out);
    w.u8(r.reason); w.u8(r.duty); w.u16(r.ms);
    w.u16(static_cast<uint16_t>(r.idle_ma)); w.u16(static_cast<uint16_t>(r.peak_ma));
    w.u16(r.min_vbus_mv); w.u16(r.vbus_before_mv);
    w.u16(static_cast<uint16_t>(r.temp_before_dc)); w.u16(static_cast<uint16_t>(r.temp_after_dc)); w.u8(r.pin_high_pct);
    return w.size();
}

size_t encode_sniff(std::span<uint8_t> out, const SniffInfo &r) {
    Writer w(out);
    w.u8(r.state); w.u8(r.reason); w.u8(r.flags); w.u16(r.edges);
    return w.size();
}

size_t encode_ripple(std::span<uint8_t> out, uint8_t code, const RippleInfo &r) {
    Writer w(out);
    w.u8(code); w.u32(r.period_ns);
    w.u16(r.count); w.u16(r.min); w.u16(r.max); w.u16(r.mean);
    return w.size();
}

size_t encode_charger_cable(std::span<uint8_t> out, const ctcable::Info &i) {
    Writer w(out);
    w.u8(static_cast<uint8_t>(i.choice.kind)); w.u16(i.choice.seq);
    w.u8(static_cast<uint8_t>(i.run.kind)); w.u16(i.run.seq);
    w.pad_text(std::string_view(i.run_name.data(), ctcable::kNameLen), ctcable::kNameLen);   // the name stops at its first NUL on the wire reader
    return w.size();
}

size_t encode_charger_hunt(std::span<uint8_t> out, const ChargerHuntReply &r, bool with_slot) {
    Writer w(out);
    w.u32(r.run_id); w.u8(r.count);
    if (with_slot) {
        const auto &e = r.slot;
        w.u8(e.step); w.u8(e.cause); w.u8(e.answer); w.u8(e.last_step); w.u8(e.caps); w.u8(e.pos); w.u8(e.type);
        w.u16(e.flags); w.u16(e.apdo_ma); w.u16(e.req_mv); w.u16(e.req_ma); w.u16(e.cable_ma); w.u16(e.authed_ms); w.u16(e.answer_ms);
        w.u32(e.apdo); w.u32(e.rdo);
    }
    return w.size();
}

size_t encode_screen_info(std::span<uint8_t> out, const ScreenInfo &i) {
    Writer w(out);
    w.u16(i.width); w.u16(i.height); w.u8(i.strip_rows); w.u8(i.num_strips); w.u8(kShotFormatRle565);
    return w.size();
}

size_t encode_emarker(std::span<uint8_t> out, const EmarkerSnapshot &e, uint8_t part) {
    Writer w(out);
    if (part == 1) {
        const size_t n = std::min<size_t>(e.num_vdos, e.vdo.size());
        for (size_t i = 0; i < n; i++) w.u32(e.vdo[i]);
        return w.size();
    }
    if (part == 2) {
        w.u8(e.sop); w.u8(e.spec_rev); w.u8(e.vdo_version); w.u8(e.latency_code);
        w.u8(e.termination); w.u8(e.flags);
        w.u8(e.max_temp_c); w.u8(e.shutdown_temp_c); w.u8(e.u3_power); w.u8(e.usb_lanes);
        return w.size();
    }
    w.u8(e.valid); w.u8(e.is_active);
    w.u16(e.vid); w.u16(e.pid); w.u32(e.xid);
    w.u8(e.hw_rev); w.u8(e.fw_rev); w.u8(e.max_a); w.u8(e.max_v);
    w.u16(e.max_w);
    w.u8(e.num_vdos);
    w.text(e.vendor); w.u8(0); w.text(e.speed);
    return w.size();
}

size_t encode_identity(std::span<uint8_t> out, const IdentitySnapshot &s) {
    Writer w(out);
    const size_t n = std::min<size_t>(s.num_svids, s.svids.size());
    w.u8(s.flags); w.u8(s.valid);
    w.u16(s.vid); w.u16(s.pid); w.u16(s.bcd);
    w.u8(static_cast<uint8_t>(n));
    for (size_t i = 0; i < n; i++) w.u16(s.svids[i]);
    w.text(s.vendor); w.u8(0); w.text(s.model);
    return w.size();
}

size_t encode_pd_trigger(std::span<uint8_t> out, const PdTriggerSnapshot &s) {
    Writer w(out);
    w.u8(s.state); w.u8(s.result); w.u8(s.held); w.u8(s.epr); w.u8(s.pos);
    w.u16(s.mv); w.u16(s.ma); w.u32(s.hard_resets); w.u32(s.source_hard_resets);
    return w.size();
}

size_t encode_match(std::span<uint8_t> out, const MatchSnapshot &s) {
    Writer w(out);
    const size_t n = std::min<size_t>(s.rows, s.row.size());
    w.u8(s.caps_seen); w.u8(s.request_seen); w.u8(s.sink_ok); w.u8(s.flags); w.u8(s.verdict);
    w.u8(static_cast<uint8_t>(n)); w.u8(s.want_pos);
    w.u16(s.want_mv); w.u16(s.want_ma); w.u16(s.need_ma); w.u16(s.have_ma);
    w.u16(s.src_max_mv); w.u16(s.src_max_w);
    for (size_t i = 0; i < n; i++) w.u8(s.row[i]);
    return w.size();
}

size_t encode_usb_link(std::span<uint8_t> out, const UsbLinkSnapshot &s) {
    Writer w(out);
    w.u8(s.family); w.u8(s.tbt); w.u8(s.conf); w.u8(s.source); w.u8(s.flags);
    w.u32(s.mbps);
    w.text(std::string_view(s.text.data(), std::min<size_t>(s.text_len, s.text.size())));
    return w.size();
}

size_t encode_identify_progress(std::span<uint8_t> out, const IdentifyProgress &p) {
    Writer w(out);
    const size_t n = std::min<size_t>(p.num_sink_pdos, p.sink_pdos.size());
    w.u8(p.state); w.u8(p.step); w.u8(p.ok_mask); w.u8(p.skipped_mask); w.u8(p.retries);
    w.u8(static_cast<uint8_t>(n));
    w.u32(p.revision);
    for (size_t i = 0; i < n; i++) w.u32(p.sink_pdos[i]);
    return w.size();
}

size_t encode_reread(std::span<uint8_t> out, const RereadInfo &i) {
    Writer w(out);
    w.u8(i.state); w.u8(i.step); w.u8(i.mask); w.u8(i.ok_mask); w.u8(i.skipped_mask);
    w.u8(i.pdos_before); w.u8(i.pdos_last); w.u8(i.caps_rx); w.u8(i.flags);
    return w.size();
}

}  // namespace k2::remote
