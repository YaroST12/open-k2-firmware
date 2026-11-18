// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#include "k2/chargertest/report_card.hpp"

namespace k2::report_card {

[[gnu::noinline]] Card::Card() = default;

namespace {

struct Out {
    char* out;
    size_t cap;
    size_t n = 0;

    void ch(char c) {
        if (n + 1 < cap) out[n++] = c;
        if (cap) out[n] = 0;
    }
    void str(const char* s) { while (*s) ch(*s++); }
    void milli(unsigned v, unsigned dec) {
        char whole[8];
        unsigned w = v / 1000, k = 0;
        do { whole[k++] = char('0' + w % 10); w /= 10; } while (w && k < sizeof whole);
        while (k) ch(whole[--k]);
        unsigned frac = v % 1000, scale = 100;
        char digits[3];
        unsigned nd = 0;
        for (unsigned i = 0; i < dec && i < 3; ++i, scale /= 10) digits[nd++] = char('0' + frac / scale % 10);
        while (nd && digits[nd - 1] == '0') --nd;
        if (nd) {
            ch('.');
            for (unsigned i = 0; i < nd; ++i) ch(digits[i]);
        }
    }
    void dv(unsigned d) { milli(d * 100u, 1); }
    void amps(unsigned ma) {
        const unsigned whole_a = ma / 1000, frac_ma = ma % 1000;
        if (whole_a >= 10) ch(char('0' + whole_a / 10));
        ch(char('0' + whole_a % 10));
        if (frac_ma > 0) {
            ch('.');
            ch(char('0' + frac_ma / 100));
            ch(char('0' + (frac_ma % 100) / 10));
        }
        ch('A');
    }
};

bool pps_span(const Card& c, unsigned& lo, unsigned& hi, uint16_t& max_ma) {
    lo = 0xFFFF;
    hi = 0;
    max_ma = 0;
    bool any = false;
    for (unsigned i = 0; i < c.n_ranges && i < kMaxRanges; ++i) {
        const Range& r = c.range[i];
        if (r.alg != uint8_t(ct::Algorithm::Pps)) continue;
        any = true;
        if (r.lo_dv < lo) lo = r.lo_dv;
        if (r.hi_dv > hi) hi = r.hi_dv;
        if (r.ma > max_ma) max_ma = r.ma;
    }
    return any;
}

void pack_group(uint8_t* q, const GroupResult& r) {
    const uint32_t v = detail::group_dv(r.max_mv) | detail::group_dv(r.fail_mv) << 9 | detail::pack_detail(r.detail) << 18;
    q[0] = uint8_t(unsigned(r.verdict) << 6 | (r.reason & 0x3F));
    q[1] = uint8_t(v);
    q[2] = uint8_t(v >> 8);
    q[3] = uint8_t(v >> 16);
}

bool unpack_group(const uint8_t* q, GroupResult& r) {
    if ((q[0] & 0x3F) >= kReasonCount) return false;
    const uint32_t v = q[1] | uint32_t(q[2]) << 8 | uint32_t(q[3]) << 16;
    r = {GV(q[0] >> 6), uint16_t((v & 0x1FF) * 100), detail::unpack_detail(v >> 18),
         uint8_t(q[0] & 0x3F), uint16_t((v >> 9 & 0x1FF) * 100)};
    return true;
}

uint32_t fnv_step(uint32_t h, uint8_t b) { return (h ^ b) * 16777619u; }

}  // namespace

const char* group_why(unsigned group, const GroupResult& r) {
    if (r.reason == uint8_t(ct::Reason::NoQc3Plus)) return "no QC3+";
    if (r.detail) {
        if (group == unsigned(ct::Group::Pd)) {
            if (const char* epr = ct::epr_detail_name(r.detail)) return epr;
            if (r.detail >= 128 && unsigned(r.detail) - 128u <= unsigned(pd::Err::BadChoice))
                return pd::err_name(pd::Err(r.detail - 128));
            if (r.detail <= unsigned(pd::SinkResult::BadChoice))
                return pd::result_name(pd::SinkResult(r.detail));
        } else if (r.detail <= unsigned(fc::Fail::Detected)) {
            return fc::name(fc::Fail(r.detail));
        }
    }
    return ct::name(ct::Reason(r.reason));
}

bool scp_note(const Card& c, char* out, size_t cap) {
    if (cap == 0) return false;
    out[0] = 0;
    const unsigned ma = scp_cap_ma(c);
    if (ma == 0) return false;
    Out o{out, cap};
    o.str("SCP cap ");
    const GroupResult& g = c.group[unsigned(ct::Group::Scp)];
    if (g.verdict == GV::Pass && g.max_mv >= 1000) {
        o.milli(g.max_mv, 1);
        o.str("V ");
    }
    o.milli(ma, 2);
    o.ch('A');
    return true;
}

QcLabel qc_label(const Card& c) {
    bool pps = false, epr = false, over5 = false, sfc3 = false, sfc2_15 = false, sfc2_11 = false;
    for (unsigned i = 0; i < c.n_ranges && i < kMaxRanges; ++i) {
        const Range& r = c.range[i];
        epr = epr || r.alg == uint8_t(ct::Algorithm::EprAvs);
        if (r.alg != uint8_t(ct::Algorithm::Pps)) continue;
        pps = true;
        over5 = over5 || r.ma > kMotoMinMa;
        sfc3 = sfc3 || (r.hi_dv >= 200 && r.ma >= 3000);
        sfc2_15 = sfc2_15 || (r.hi_dv >= 150 && r.ma >= 3000);
        sfc2_11 = sfc2_11 || (r.lo_dv && r.lo_dv <= 33 && r.hi_dv >= 110 && r.ma >= kSfc2MaxCapMa);
    }
    if (!pps) return QcLabel::None;
    if (c.id.vid == kQualcommVid) return c.id.pdp_w >= kQc5MinWatts || epr ? QcLabel::Qc5 : QcLabel::Qc4Plus;
    if ((c.id.vid == kMotorolaVid || c.id.vid == kLenovoVid) && over5) return QcLabel::MotoTurbo;
    if (c.id.vid != kSamsungVid) return QcLabel::None;
    if (sfc3) return QcLabel::Sfc3;
    if (sfc2_15 || (sfc2_11 && c.cable.max_ma >= 5000)) return QcLabel::Sfc2;
    return QcLabel::None;
}

const char* qc_label_name(QcLabel l) {
    static constexpr const char* kNames[] = {"", "QC4+", "QC5", "Moto TurboPower", "Samsung SFC 2.0", "Samsung SFC 3.0"};
    const unsigned i = unsigned(l);
    return i < std::size(kNames) ? kNames[i] : "";
}

size_t qc_pps_span_text(const Card& c, char* out, size_t cap) {
    if (cap == 0) return 0;
    out[0] = 0;
    unsigned lo = 0, hi = 0;
    uint16_t max_ma = 0;
    if (!pps_span(c, lo, hi, max_ma)) return 0;
    Out o{out, cap};
    o.dv(lo);
    o.ch('-');
    o.dv(hi);
    o.ch('V');
    return o.n;
}

size_t qc_pps_va_text(const Card& c, char* out, size_t cap) {
    if (cap == 0) return 0;
    out[0] = 0;
    unsigned lo = 0, hi = 0;
    uint16_t max_ma = 0;
    if (!pps_span(c, lo, hi, max_ma)) return 0;
    Out o{out, cap};
    o.dv(lo);
    o.ch('-');
    o.dv(hi);
    o.ch('V');
    if (max_ma > 0) {
        o.ch(' ');
        o.amps(max_ma);
    }
    return o.n;
}

size_t qc_pd_va_text(const Card& c, char* out, size_t cap) {
    if (cap == 0) return 0;
    out[0] = 0;
    if (c.pd_ma == 0) return 0;
    const uint16_t volts_mv = c.group[unsigned(ct::Group::Pd)].max_mv;
    if (volts_mv == 0) return 0;
    Out o{out, cap};
    const unsigned volts = volts_mv / 1000;
    if (volts >= 10) o.ch(char('0' + volts / 10));
    o.ch(char('0' + volts % 10));
    o.str("V ");
    o.amps(c.pd_ma);
    return o.n;
}

size_t qc_label_justification(const Card& c, char* out, size_t cap) {
    if (cap == 0) return 0;
    out[0] = 0;
    const QcLabel l = qc_label(c);
    if (l == QcLabel::None) return 0;
    Out o{out, cap};
    char sub[20];
    if (l == QcLabel::Qc5) {
        if (qc_pd_va_text(c, sub, sizeof sub)) {
            o.str("via PD ");
            o.str(sub);
        } else if (qc_pps_span_text(c, sub, sizeof sub)) {
            o.str("Qualcomm VID, PPS ");
            o.str(sub);
        }
    } else if (qc_pps_va_text(c, sub, sizeof sub)) {
        o.str("via PPS ");
        o.str(sub);
    } else if (qc_pps_span_text(c, sub, sizeof sub)) {
        o.str(l == QcLabel::Qc4Plus ? "Qualcomm VID, PPS " : "PPS ");
        o.str(sub);
    }
    return o.n;
}

size_t qc_label_text(const Card& c, char* out, size_t cap) {
    if (cap == 0) return 0;
    out[0] = 0;
    const QcLabel l = qc_label(c);
    if (l == QcLabel::None) return 0;
    Out o{out, cap};
    o.str(qc_label_name(l));
    char justification[32];
    if (qc_label_justification(c, justification, sizeof justification)) {
        o.str(" (");
        o.str(justification);
        o.ch(')');
    }
    return o.n;
}

uint32_t fingerprint(std::span<const uint32_t> pdos, uint8_t spec_rev) {
    uint32_t h = 2166136261u;
    for (uint32_t p : pdos)
        for (unsigned i = 0; i < 4; ++i) h = fnv_step(h, uint8_t(p >> (8 * i)));
    h = fnv_step(h, spec_rev);
    return h ? h : 1;
}

uint32_t legacy_fingerprint(std::span<const ct::Step> steps) {
    uint32_t h = 2166136261u;
    for (const uint8_t b : {'L', 'G', 'C', 'Y'}) h = fnv_step(h, b);
    for (const auto& s : steps)
        if (s.verdict == ct::Verdict::Pass) {
            h = fnv_step(h, uint8_t(s.group));
            h = fnv_step(h, uint8_t(s.mv / 100));
            h = fnv_step(h, uint8_t(s.mv / 25600));
        }
    return h ? h : 1;
}

uint32_t fw_hash(std::string_view text) {
    auto hex = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; };
    size_t end = text.size();
    while (end > 0) {
        size_t begin = end;
        while (begin > 0 && text[begin - 1] != '-') --begin;
        std::string_view tok(text.data() + begin, end - begin);
        if (tok.size() >= 8 && tok.front() == 'g' && hex(tok[1]) >= 0) tok.remove_prefix(1);
        bool ok = tok.size() >= 7;
        for (char c : tok) ok = ok && hex(c) >= 0;
        if (ok) {
            uint32_t v = 0;
            for (size_t i = 0; i < tok.size() && i < 8; ++i) v = (v << 4) | uint32_t(hex(tok[i]));
            return v;
        }
        end = begin ? begin - 1 : 0;
    }
    return 0;
}

void set_name(Card& c, std::string_view n) {
    c.name[0] = 0;
    const size_t k = n.size() < kNameMax ? n.size() : kNameMax;
    std::memcpy(c.name, n.data(), k);
    c.name[k] = 0;
}

const char* brand(uint16_t vid) {
    struct Ent { uint16_t vid; const char* name; };
    static constexpr Ent kBrands[] = {
        {0x05AC, "Apple"}, {kSamsungVid, "Samsung"}, {0x18D1, "Google"}, {0x2717, "Xiaomi"}, {0x12D1, "Huawei"},
        {0x17EF, "Lenovo"}, {0x413C, "Dell"}, {0x03F0, "HP"}, {0x2A70, "OnePlus"}, {0x22D9, "OPPO"},
        {0x0B05, "ASUS"}, {0x050D, "Belkin"}, {0x22B8, "Motorola"}, {0x19D2, "ZTE"}, {0x291A, "Anker"},
    };
    for (const auto& e : kBrands) if (e.vid == vid) return e.name;
    return nullptr;
}

size_t auto_name(char* out, uint16_t vid, std::string_view mfr, uint8_t pdp_w) {
    if (vid == 0xFFFF) vid = 0;
    char base[kNameMax + 1] = {};
    size_t n = 0;
    if (const char* b = brand(vid)) {
        n = std::strlen(b);
        std::memcpy(base, b, n);
    } else {
        for (char c : mfr) {
            if (n >= kNameMax) break;
            const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
            if (ok || (c == ' ' && n > 0)) base[n++] = c;
        }
        while (n && base[n - 1] == ' ') base[--n] = 0;
        if (n == 0 && vid) {
            static constexpr char hx[] = "0123456789ABCDEF";
            std::memcpy(base, "VID ", 4);
            for (unsigned i = 0; i < 4; ++i) base[4 + i] = hx[(vid >> (12 - 4 * i)) & 15];
            n = 8;
        }
    }
    char suffix[6] = {};
    size_t sn = 0;
    if (n && pdp_w) {
        suffix[sn++] = ' ';
        if (pdp_w >= 100) suffix[sn++] = char('0' + pdp_w / 100);
        if (pdp_w >= 10) suffix[sn++] = char('0' + pdp_w / 10 % 10);
        suffix[sn++] = char('0' + pdp_w % 10);
        suffix[sn++] = 'W';
    }
    if (n + sn > kNameMax) n = kNameMax - sn;
    while (n && base[n - 1] == ' ') --n;
    std::memcpy(out, base, n);
    std::memcpy(out + n, suffix, sn);
    out[n + sn] = 0;
    return n ? n + sn : 0;
}

void set_auto_name(Card& c, std::string_view mfr) {
    char buf[kNameMax + 1] = {};
    if (auto_name(buf, c.id.vid, mfr, c.id.pdp_w) == 0) return;
    set_name(c, buf);
    c.name_auto = true;
}

void inherit(Card& c, const Card& prev) {
    if (prev.name[0] && !prev.name_auto) { std::memcpy(c.name, prev.name, sizeof c.name); c.name_auto = false; }
    else if (c.name[0] == 0) { std::memcpy(c.name, prev.name, sizeof c.name); c.name_auto = prev.name_auto; }
    if (c.id.vid == 0 && c.id.pid == 0) { c.id.vid = prev.id.vid; c.id.pid = prev.id.pid; }
    if (c.id.pdp_w == 0) c.id.pdp_w = prev.id.pdp_w;
}

bool same_results(const Card& prev, const Card& run) {
    if (prev.id.fp != run.id.fp || prev.num_pdos != run.num_pdos || prev.spec_rev != run.spec_rev || prev.ev_flags != run.ev_flags ||
        prev.pd_ma != run.pd_ma || prev.src_flags != run.src_flags || prev.fw != run.fw || prev.handover != run.handover ||
        prev.state != run.state || prev.reason != run.reason || prev.n_ranges != run.n_ranges || prev.ufcs != run.ufcs ||
        prev.level != run.level)
        return false;
    const Cable &a = prev.cable, &b = run.cable;
    if (a.vid != b.vid || a.pid != b.pid || a.max_mv != b.max_mv || a.max_ma != b.max_ma || a.kind != b.kind) return false;
    for (unsigned g = 0; g < kGroups; ++g) if (!detail::same_result(prev.group[g], run.group[g])) return false;
    for (unsigned i = 0; i < kMaxRanges; ++i) {
        const Range &x = prev.range[i], &y = run.range[i];
        if (x.alg != y.alg || x.pos != y.pos || x.lo_dv != y.lo_dv || x.hi_dv != y.hi_dv || x.ma != y.ma || x.fail_dv != y.fail_dv || !detail::within(x.max_dv, y.max_dv, 1))
            return false;
    }
    for (unsigned i = 0; i < kEprLevels; ++i)
        if (prev.epr[i].dv != run.epr[i].dv || !detail::same_result(prev.epr[i].r, run.epr[i].r)) return false;
    return true;
}

void fold_repeat(const Card& prev, Card& c) {
    char name[sizeof c.name];
    std::memcpy(name, c.name, sizeof name);
    const bool name_auto = c.name_auto;
    const uint32_t boots = c.boots, uptime_s = c.uptime_s;
    const uint16_t vid = c.id.vid, pid = c.id.pid, pdp_w = c.id.pdp_w;
    c = prev;
    c.format = kFormat;
    c.runs = prev.runs == 0xFFFF ? prev.runs : uint16_t(prev.runs + 1);
    c.times = prev.times == 0xFF ? prev.times : uint8_t(prev.times + 1);
    c.last_boots = boots > 0xFFFF ? uint16_t(0xFFFF) : uint16_t(boots);
    c.last_uptime_s = uptime_s > 0xFFFFFF ? 0xFFFFFFu : uptime_s;
    if (!(c.name[0] && !c.name_auto) && name[0]) { std::memcpy(c.name, name, sizeof name); c.name_auto = name_auto; }
    if (c.id.vid == 0 && c.id.pid == 0) { c.id.vid = vid; c.id.pid = pid; }
    if (c.id.pdp_w == 0) c.id.pdp_w = pdp_w;
}

[[gnu::noinline]] void build_into(Card& c, std::span<const ct::Step> steps, const ct::Evidence& ev, ct::HandoverInfo h, Identity id, Context cx,
                                  ct::State state, ct::Reason reason, const Cable& cable, uint8_t resumed, ctsweep::View sweep) {
    c.id = id; c.num_pdos = ev.num_pdos; c.spec_rev = ev.wire_rev; c.ev_flags = ev.flags; c.cable = cable; c.resumed = resumed;
    c.rebalanced = ev.rebalanced;
    c.src_flags = uint8_t(((ev.seen & ct::SeenOppoSvid) ? kSrcOppoSvid : 0) | (std::min<unsigned>(ev.scp_ma / 50u, 127u) << 1));
    c.handover = h.outcome; c.handover_ds = h.ms / 100 > 65535 ? uint16_t(65535) : uint16_t(h.ms / 100);
    c.state = state; c.reason = reason; c.fw = cx.fw; c.boots = cx.boots; c.uptime_s = cx.uptime_s;

    struct GroupAcc {
        uint8_t passes = 0, fails = 0, skips = 0;
        bool have_fail = false, have_skip = false, no_response = false, proven = false;
        uint8_t pass_detail = 0;
        GroupResult first_fail{}, first_skip{};
    };
    std::array<GroupAcc, kGroups> ga{};
    struct SpreadEnt { uint16_t mv = 0; int8_t err = 0; };
    std::array<std::array<SpreadEnt, kSpreadLevels>, kMaxRanges> lvl{};
    std::array<uint8_t, kMaxRanges> n_lvl{};

    for (auto& a : c.spread) a.fill(kSpreadNone);
    for (const auto& s : steps) {
        const unsigned g = unsigned(s.group);
        // The Xiaomi unit (its chain row and the modes it unlocked) is the extension block's section (report_card_deep.hpp), never
        // part of the PD group, its ranges, spread or EPR levels: a card reads the same with or without it.
        if (g >= kGroups || s.hunt) continue;
        GroupAcc& acc = ga[g];
        const bool range_step = (ct::kPpsAlgorithms & ct::bit(s.algorithm)) != 0;
        // A caution's detail is a measurement or flag bits, not a fast-charge code (flag 15 would read as "detected"): the card keeps none of it.
        const uint8_t det = s.reason == ct::Reason::StepResolution || s.reason == ct::Reason::ProgCheck ? uint8_t(0) : s.detail;
        const uint16_t target_dv_mv = uint16_t(detail::dv_round(s.mv) * 100u);
        const GroupResult why{GV::None, 0, det, uint8_t(s.reason), range_step ? uint16_t(0) : target_dv_mv};

        if (s.verdict == ct::Verdict::Pass) {
            if (acc.passes < 255) ++acc.passes;
            if (det != uint8_t(fc::Fail::Detected) && det != uint8_t(fc::Fail::NoVoltage)) acc.proven = true;
            else if (det == uint8_t(fc::Fail::NoVoltage)) acc.pass_detail = det;
            else if (acc.pass_detail == 0) acc.pass_detail = det;
            const uint16_t meas_dv_mv = uint16_t(detail::dv_round(s.measured_mv) * 100u);
            if (!range_step && meas_dv_mv > c.group[g].max_mv) c.group[g].max_mv = meas_dv_mv;
        } else if (s.verdict == ct::Verdict::Fail) {
            if (acc.fails < 255) ++acc.fails;
            if (!acc.have_fail || (!range_step && acc.first_fail.fail_mv == 0)) {
                acc.have_fail = true;
                acc.first_fail = why;
            }
        } else if (s.verdict == ct::Verdict::Skip) {
            if (acc.skips < 255) ++acc.skips;
            if (!acc.have_skip) { acc.have_skip = true; acc.first_skip = why; }
            if (s.reason == ct::Reason::NoQc3Plus) acc.no_response = true;
        }

        if (ct::UfcsMode m; ct::ufcs_mode_of(s, m) && m.max_mv != 0) {
            const UfcsRange r{uint16_t(std::min<unsigned>(m.min_mv / 100u, 255u)), uint16_t(std::min<unsigned>(m.max_mv / 100u, 511u)),
                              uint16_t(std::min<unsigned>(m.max_ma / 50u, 127u) * 50u)};
            unsigned n = 0;
            while (n < kMaxUfcs && c.ufcs[n].hi_dv != 0 && !(c.ufcs[n] == r)) ++n;
            if (n < kMaxUfcs && c.ufcs[n].hi_dv == 0) c.ufcs[n] = r;
        }

        if (g == unsigned(ct::Group::Pd) && !range_step && s.pdo && (s.pdo >> 30) == 0) {
            const unsigned ma = std::min<unsigned>((s.pdo & 0x3FFu) * 10u / 50u * 50u, 6350u);
            if (ma > c.pd_ma) c.pd_ma = uint16_t(ma);
        }

        if (s.continuous && s.pdo && range_step) {
            const bool spr_avs = s.algorithm == ct::Algorithm::SprAvs;
            const uint16_t lo = spr_avs ? uint16_t(90) : uint16_t((s.pdo >> 8) & 0xFF);
            const uint16_t hi = spr_avs ? uint16_t((s.pdo & 0x3FFu) ? 200 : 150)
                                        : uint16_t((s.pdo >> 17) & (s.algorithm == ct::Algorithm::EprAvs ? 0x1FFu : 0xFFu));
            int ri = -1;
            for (unsigned i = 0; i < c.n_ranges; ++i)
                if (c.range[i].alg == uint8_t(s.algorithm) && c.range[i].pos == s.pos) { ri = int(i); break; }
            if (ri < 0 && c.n_ranges < kMaxRanges) {
                ri = int(c.n_ranges++);
                c.range[ri].alg = uint8_t(s.algorithm);
                c.range[ri].pos = s.pos;
                c.range[ri].lo_dv = lo;
                c.range[ri].hi_dv = hi;
            }
            if (ri >= 0) {
                Range& rg = c.range[ri];
                if (!rg.ma) rg.ma = detail::apdo_ma(s.algorithm, s.pdo, rg.hi_dv);
                if (s.verdict == ct::Verdict::Pass || s.reason == ct::Reason::Capped) {
                    const uint16_t m = detail::dv_round(s.measured_mv);
                    if (m > rg.max_dv) rg.max_dv = m;
                }
                if (s.verdict == ct::Verdict::Fail && rg.fail_dv == 0) rg.fail_dv = detail::dv_round(s.mv);
                if (s.verdict == ct::Verdict::Pass && n_lvl[ri] < kSpreadLevels) {
                    auto& a = lvl[ri];
                    const unsigned n = n_lvl[ri];
                    unsigned at = n;
                    while (at > 0 && a[at - 1].mv > s.mv) { a[at] = a[at - 1]; --at; }
                    if (!(at < n && a[at].mv == s.mv)) {
                        a[at] = {s.mv, int8_t(std::clamp(int(s.measured_mv) - int(s.mv), -127, 127))};
                        ++n_lvl[ri];
                    }
                }
            }
        }

        if (s.group == ct::Group::Pd && s.algorithm == ct::Algorithm::Epr && !s.continuous) {
            const uint16_t target_dv = detail::dv_round(s.mv);
            for (unsigned i = 0; i < kEprLevels; ++i) {
                EprLevel& el = c.epr[i];
                if (el.dv == 0) el.dv = target_dv;
                if (el.dv != target_dv) continue;
                if (s.verdict == ct::Verdict::Fail && el.r.verdict != GV::Fail) {
                    el.r = {GV::Fail, 0, s.detail, uint8_t(s.reason), target_dv_mv};
                } else if (s.verdict == ct::Verdict::Pass) {
                    const uint16_t mv100 = uint16_t(detail::dv_round(s.measured_mv) * 100u);
                    if (mv100 > el.r.max_mv) el.r.max_mv = mv100;
                    if (el.r.verdict == GV::None) el.r.verdict = GV::Pass;
                } else if (s.verdict == ct::Verdict::Skip && el.r.verdict == GV::None) {
                    el.r = {GV::Skip, 0, s.detail, uint8_t(s.reason), 0};
                }
                break;
            }
        }
    }

    for (unsigned i = 0; i < kMaxRanges; ++i) {
        for (unsigned k = 0; k < kSpreadLevels; ++k)
            c.spread[i][k] = k < n_lvl[i] ? lvl[i][k].err : kSpreadNone;
        if (i < c.n_ranges) {
            const auto rw = sweep.apdo(c.range[i].alg, c.range[i].pos);
            if (!rw) continue;
            const auto& sm = rw->sum;
            c.sweep[i] = {
                int8_t(std::clamp(int(sm.min_err), -127, 127)),
                int8_t(std::clamp(int(sm.max_err), -127, 127)),
                int8_t(std::clamp(int(sm.worst_err), -127, 127)),
                uint16_t(std::min<unsigned>(sm.worst_mv / 100u, 511u)),
                uint16_t(std::min<unsigned>(sm.peak_mv / 100u, 511u)),
                uint16_t(std::min<unsigned>(sm.peak_at_mv / 100u, 511u)),
                uint16_t(std::min<unsigned>(detail::dv_round(sm.fold_mv), 511u)),
                uint16_t(std::min<unsigned>(detail::dv_round(sm.fold_from_mv), 511u))};
        }
    }

    for (unsigned g = 0; g < kGroups; ++g) {
        const GroupAcc& acc = ga[g];
        GroupResult& r = c.group[g];
        r.verdict = acc.fails ? GV::Fail : acc.no_response ? GV::Skip : acc.passes ? GV::Pass : acc.skips ? GV::Skip : GV::None;
        if (r.verdict == GV::Fail) { r.detail = acc.first_fail.detail; r.reason = acc.first_fail.reason; r.fail_mv = acc.first_fail.fail_mv; }
        else if (r.verdict == GV::Pass) r.detail = acc.proven ? uint8_t(0) : acc.pass_detail;
        else if (r.verdict == GV::Skip) { r.detail = acc.first_skip.detail; r.reason = acc.first_skip.reason; }
    }
}

Card build(std::span<const ct::Step> steps, const ct::Evidence& ev, ct::HandoverInfo h, Identity id, Context cx,
           ct::State state, ct::Reason reason, const Cable& cable, uint8_t resumed, ctsweep::View sweep) {
    Card c;
    build_into(c, steps, ev, h, id, cx, state, reason, cable, resumed, sweep);
    return c;
}

size_t encode_card(std::span<uint8_t> out, const Card& c) {
    using namespace detail;
    uint8_t* b = out.data();
    std::memset(b, 0, kMaxRecord);
    b[0] = kMagic; b[1] = kTypeCard; b[2] = kPayload; b[3] = 0;
    uint8_t* p = b + 4;
    put32(p + 0, c.id.fp); put16(p + 4, c.id.vid); put16(p + 6, c.id.pid);
    put16(p + 8, c.card_no); put16(p + 10, c.runs);
    put32(p + 12, c.boots); put32(p + 16, c.uptime_s); put32(p + 20, c.fw);
    put16(p + 24, c.handover_ds);
    p[26] = c.spec_rev; p[27] = c.num_pdos; p[28] = uint8_t(c.handover); p[29] = uint8_t(c.state); p[30] = uint8_t(c.reason);
    p[31] = c.n_ranges; p[32] = c.ev_flags; p[33] = kFormat;
    std::memcpy(p + off::kName, c.name, kNameMax);
    p[off::kPdp] = c.id.pdp_w;
    p[off::kFlags] = uint8_t((c.name_auto && c.name[0] ? kFlagNameAuto : 0) | (c.pd_ma / 50 > 127 ? 127 : c.pd_ma / 50) << 1);
    for (unsigned g = 0; g < kGroups; ++g) pack_group(p + off::kGroups + 4 * g, c.group[g]);
    for (unsigned i = 0; i < kMaxRanges; ++i) {
        uint8_t* q = p + off::kRanges + 10 * i;
        q[0] = c.range[i].alg; q[1] = c.range[i].pos;
        put16(q + 2, uint16_t((c.range[i].lo_dv & 0x1FF) | (c.range[i].ma / 50 > 127 ? 127 : c.range[i].ma / 50) << 9));
        put16(q + 4, c.range[i].hi_dv); put16(q + 6, c.range[i].max_dv); put16(q + 8, c.range[i].fail_dv);
    }
    for (unsigned i = 0; i < kEprLevels; ++i) {
        uint8_t* q = p + off::kEpr + 6 * i;
        put16(q, c.epr[i].dv);
        pack_group(q + 2, c.epr[i].r);
    }
    {
        uint8_t* q = p + off::kCable;
        put32(q, c.cable.seq); put16(q + 4, c.cable.vid); put16(q + 6, c.cable.pid);
        put16(q + 8, c.cable.max_mv); put16(q + 10, c.cable.max_ma);
        q[12] = c.cable.kind; q[13] = c.cable.flags; q[14] = c.resumed; q[15] = c.src_flags;
    }
    for (unsigned i = 0; i < kMaxUfcs; ++i) {
        uint8_t* q = p + off::kUfcs + 3 * i;
        const UfcsRange& r = c.ufcs[i];
        const uint32_t v = r.hi_dv == 0 ? 0u : std::min<unsigned>(r.lo_dv, 255u) | std::min<unsigned>(r.hi_dv, 511u) << 8 | std::min<unsigned>(r.ma / 50u, 127u) << 17;
        q[0] = uint8_t(v); q[1] = uint8_t(v >> 8); q[2] = uint8_t(v >> 16);
    }
    {
        uint8_t* q = p + off::kRepeat;
        const uint32_t up = c.last_uptime_s > 0xFFFFFFu ? 0xFFFFFFu : c.last_uptime_s;
        q[0] = c.times; put16(q + 1, c.last_boots);
        q[3] = uint8_t(up); q[4] = uint8_t(up >> 8); q[5] = uint8_t(up >> 16);
    }
    p[off::kV8] = c.rebalanced;
    p[off::kV8 + 1] = uint8_t(c.level);
    for (unsigned i = 0; i < kMaxRanges; ++i)
        for (unsigned k = 0; k < kSpreadLevels; ++k)
            p[off::kV9 + kSpreadLevels * i + k] = uint8_t(c.spread[i][k]);
    for (unsigned i = 0; i < kMaxRanges; ++i) {
        uint8_t* q = p + off::kV10 + kSweepBytes * i;
        const auto& w = c.sweep[i];
        q[0] = uint8_t(w.min_err); q[1] = uint8_t(w.max_err); q[2] = uint8_t(w.worst_err);
        const uint32_t w0 = std::min<unsigned>(w.worst_dv, 511u);
        const uint32_t w1 = std::min<unsigned>(w.peak_dv, 511u);
        const uint32_t w2 = std::min<unsigned>(w.peak_at_dv, 511u);
        const uint32_t w3 = std::min<unsigned>(w.fold_dv, 511u);
        const uint32_t w4 = std::min<unsigned>(w.fold_from_dv, 511u);
        put32(q + 3, w0 | (w1 << 9) | (w2 << 18) | (w3 << 27));
        put16(q + 7, uint16_t((w3 >> 5) | (w4 << 4)));
    }
    put16(b + 4 + kPayload, bbstore::crc16(0xFFFF, {b, 4 + kPayload}));
    return kMaxRecord;
}

bool decode_card(std::span<const uint8_t> payload, Card& c) {
    using namespace detail;
    if (payload.size() < 144 || payload.size() > kPayload || payload.size() % 2) return false;
    const uint8_t* p = payload.data();
    Card d;
    for (auto& a : d.spread) a.fill(kSpreadNone);
    d.id.fp = get32(p); d.id.vid = get16(p + 4); d.id.pid = get16(p + 6);
    d.card_no = get16(p + 8); d.runs = get16(p + 10);
    d.boots = get32(p + 12); d.uptime_s = get32(p + 16); d.fw = get32(p + 20);
    d.handover_ds = get16(p + 24);
    d.spec_rev = p[26]; d.num_pdos = p[27];
    if (p[28] > uint8_t(ct::HandoverOutcome::InContract) || p[29] > uint8_t(ct::State::Attach) || p[30] >= kReasonCount || p[31] > kMaxRanges) return false;
    d.handover = ct::HandoverOutcome(p[28]); d.state = ct::State(p[29]); d.reason = ct::Reason(p[30]);
    if (p[33] > kFormat) return false;
    d.format = p[33];
    d.n_ranges = p[31]; d.ev_flags = p[32];
    std::memcpy(d.name, p + off::kName, kNameMax); d.name[kNameMax] = 0;
    if (p[33] >= 2) { d.id.pdp_w = p[off::kPdp]; d.name_auto = (p[off::kFlags] & kFlagNameAuto) && d.name[0]; }
    if (p[33] >= 3) d.pd_ma = uint16_t((p[off::kFlags] >> 1) * 50);
    for (unsigned g = 0; g < kGroups; ++g) {
        const uint8_t* q = p + off::kGroups + 4 * g;
        if (p[33] == 0) {
            if ((q[0] & 0x3F) >= kReasonCount) return false;
            d.group[g] = {GV(q[0] >> 6), uint16_t(dv_round(get16(q + 2)) * 100u), q[1], uint8_t(q[0] & 0x3F), 0};
        } else if (!unpack_group(q, d.group[g])) {
            return false;
        }
    }
    for (unsigned i = 0; i < kMaxRanges; ++i) {
        const uint8_t* q = p + off::kRanges + 10 * i;
        if (q[0] >= unsigned(ct::Algorithm::Count)) return false;
        const uint16_t lo = get16(q + 2);
        d.range[i] = {q[0], q[1], p[33] >= 3 ? uint16_t(lo & 0x1FF) : lo, get16(q + 4), get16(q + 6), get16(q + 8),
                      p[33] >= 3 ? uint16_t((lo >> 9) * 50) : uint16_t(0)};
    }
    if (p[33] >= 4) {
        for (unsigned i = 0; i < kEprLevels; ++i) {
            const uint8_t* q = p + off::kEpr + 6 * i;
            d.epr[i].dv = get16(q);
            if (!unpack_group(q + 2, d.epr[i].r)) return false;
        }
    } else {
        for (unsigned i = 0; i < kEprLevels; ++i) d.epr[i] = {0, {GV::None, 0, 0, 0, 0}};
    }
    if (p[33] >= 5) {
        const uint8_t* q = p + off::kCable;
        d.cable = {get32(q), get16(q + 4), get16(q + 6), get16(q + 8), get16(q + 10), q[12], q[13]};
        d.resumed = q[14];
        d.src_flags = q[15];
    }
    if (p[33] >= 6) {
        for (unsigned i = 0; i < kMaxUfcs; ++i) {
            const uint8_t* q = p + off::kUfcs + 3 * i;
            const uint32_t v = q[0] | uint32_t(q[1]) << 8 | uint32_t(q[2]) << 16;
            if ((v >> 8 & 0x1FF) != 0) d.ufcs[i] = {uint16_t(v & 0xFF), uint16_t(v >> 8 & 0x1FF), uint16_t((v >> 17) * 50)};
        }
    }
    if (p[33] >= 7) {
        const uint8_t* q = p + off::kRepeat;
        d.times = q[0] ? q[0] : uint8_t(1);
        d.last_boots = get16(q + 1);
        d.last_uptime_s = q[3] | uint32_t(q[4]) << 8 | uint32_t(q[5]) << 16;
    }
    if (p[33] >= 8 && payload.size() >= off::kV8 + 1) d.rebalanced = p[off::kV8];
    if (p[33] >= 8 && payload.size() >= off::kV8 + 2 && p[off::kV8 + 1] < ct::kLevelCount) d.level = ct::Level(p[off::kV8 + 1]);
    if (p[33] >= 9 && payload.size() >= off::kV9 + kMaxRanges * kSpreadLevels)
        for (unsigned i = 0; i < kMaxRanges; ++i)
            for (unsigned k = 0; k < kSpreadLevels; ++k)
                d.spread[i][k] = int8_t(p[off::kV9 + kSpreadLevels * i + k]);
    if (p[33] >= 10 && payload.size() >= kPayload)
        for (unsigned i = 0; i < kMaxRanges; ++i) {
            const uint8_t* q = p + off::kV10 + kSweepBytes * i;
            const uint32_t lo = get32(q + 3);
            const uint32_t hi = get16(q + 7);
            d.sweep[i] = {int8_t(q[0]), int8_t(q[1]), int8_t(q[2]),
                          uint16_t(lo & 0x1FFu), uint16_t((lo >> 9) & 0x1FFu), uint16_t((lo >> 18) & 0x1FFu),
                          uint16_t(((lo >> 27) | (hi << 5)) & 0x1FFu), uint16_t((hi >> 4) & 0x1FFu)};
        }
    c = d;
    return true;
}

}  // namespace k2::report_card
