// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// PD limiter, pure logic: see include/k2/pd/pd_limiter.hpp.
#include "k2/pd/pd_limiter.hpp"

#include <algorithm>

#include "k2/pd/pd.hpp"

namespace k2::pd {

namespace {

using namespace wire;

constexpr std::uint8_t kExtEprSourceCap = 0x11;
constexpr std::uint8_t kEprActionAck = 2, kEprActionSucceeded = 3;   // EPR_Mode: the contract is (about to be) EPR

bool due(std::uint32_t now, std::uint32_t t, std::uint32_t span) { return std::uint32_t(now - t) >= span; }

bool valid_tech(const LimTech& t, std::uint16_t min_mv, std::uint16_t mv_unit, bool avs) {
    if (t.max_ma != 0 && (t.max_ma < kLimStepMa || t.max_ma > kLimMaxMa || t.max_ma % kLimStepMa != 0)) return false;
    if (t.max_mv == 0) return true;
    if (avs) return t.max_mv == 15000 || t.max_mv == 20000;
    return t.max_mv >= min_mv && t.max_mv <= kLimMaxMv && (t.max_mv % mv_unit) == 0;
}

std::size_t append_str(std::span<char> out, std::size_t at, const char* s) {
    while (*s && at < out.size()) out[at++] = *s++;
    return at;
}
std::size_t append_u(std::span<char> out, std::size_t at, std::uint32_t v) {
    char tmp[10];
    std::size_t n = 0;
    do { tmp[n++] = static_cast<char>('0' + v % 10); v /= 10; } while (v && n < sizeof tmp);
    while (n && at < out.size()) out[at++] = tmp[--n];
    return at;
}
std::size_t append_tenth(std::span<char> out, std::size_t at, std::uint16_t milli, char unit) {
    const std::uint32_t t = (milli + 50u) / 100u;
    at = append_u(out, at, t / 10u);
    if (t % 10u != 0u && at + 2 <= out.size()) {
        out[at++] = '.';
        out[at++] = static_cast<char>('0' + (t % 10u));
    }
    if (unit && at < out.size()) out[at++] = unit;
    return at;
}
bool tech_constrained(const LimTech& t) { return !t.enabled || t.max_mv != 0 || t.max_ma != 0; }

}  // namespace

bool lim_config_valid(const LimConfig& c) {
    if (!lim_config_active(c)) return false;
    if (!c.fixed.enabled && !c.pps.enabled && !c.avs.enabled) return false;
    if (c.limit_mw != 0 && (c.limit_mw < kLimMinMw || c.limit_mw > kLimMaxMw)) return false;
    return valid_tech(c.fixed, 5000, 50, false) && valid_tech(c.pps, kLimMinMv, 100, false) && valid_tech(c.avs, 15000, 100, true);
}

void lim_pack(const LimConfig& c, std::span<std::uint8_t, kLimPackBytes> out) {
    const LimTech* t[3] = {&c.fixed, &c.pps, &c.avs};
    std::uint16_t h[8];
    out[0] = 0;
    for (unsigned i = 0; i < 3; i++) {
        out[0] |= std::uint8_t(t[i]->enabled << i);
        h[2 * i] = t[i]->max_mv;
        h[2 * i + 1] = t[i]->max_ma;
    }
    h[6] = std::uint16_t(c.limit_mw);
    h[7] = std::uint16_t(c.limit_mw >> 16);
    for (unsigned i = 0; i < 8; i++) {
        out[1 + 2 * i] = std::uint8_t(h[i]);
        out[2 + 2 * i] = std::uint8_t(h[i] >> 8);
    }
}

bool lim_unpack(std::span<const std::uint8_t, kLimPackBytes> in, LimConfig& c) {
    if (in[0] & ~0x07u) return false;
    std::uint16_t h[8];
    for (unsigned i = 0; i < 8; i++) h[i] = std::uint16_t(in[1 + 2 * i] | (in[2 + 2 * i] << 8));
    LimTech* t[3] = {&c.fixed, &c.pps, &c.avs};
    for (unsigned i = 0; i < 3; i++) *t[i] = {((in[0] >> i) & 1u) != 0, h[2 * i], h[2 * i + 1]};
    c.limit_mw = h[6] | (std::uint32_t(h[7]) << 16);
    return lim_config_valid(c);
}

const char* lim_state_name(LimState s) {
    switch (s) {
        case LimState::Off: return "off";
        case LimState::Armed: return "armed";
        case LimState::Negotiating: return "negotiating";
        case LimState::Pending: return "pending";
        case LimState::Injected: return "injected";
        case LimState::Limited: return "limited";
    }
    return "?";
}

const char* lim_why_name(LimWhy w) {
    switch (w) {
        case LimWhy::None: return "none";
        case LimWhy::User: return "turned off";
        case LimWhy::Detached: return "detached";
        case LimWhy::Epr: return "EPR is not covered";
        case LimWhy::Rate: return "too many lists";
        case LimWhy::ChargerResets: return "the charger resets";
        case LimWhy::SinkResets: return "the sink resets";
        case LimWhy::SinkIgnores: return "the sink ignores it";
    }
    return "?";
}

const char* lim_refusal_name(LimRefusal r) {
    switch (r) {
        case LimRefusal::None: return "none";
        case LimRefusal::BadLimit: return "limit outside 5..100 W";
        case LimRefusal::Busy: return "another feature is running";
    }
    return "?";
}

LimRefusal lim_gate(const LimFacts& f, const LimConfig& cfg) {
    if (!lim_config_valid(cfg)) return LimRefusal::BadLimit;
    return f.busy ? LimRefusal::Busy : LimRefusal::None;
}

LimRefusal lim_gate(const LimFacts& f, std::uint32_t limit_mw) {
    return lim_gate(f, lim_config_watts(limit_mw));
}

std::size_t lim_label(const LimConfig& cfg, std::span<char> out) {
    if (out.empty()) return 0;
    const std::size_t cap = std::min(out.size(), kLimLabelMax);
    std::span<char> dst = out.first(cap);
    const bool fx = cfg.fixed.enabled, pp = cfg.pps.enabled, av = cfg.avs.enabled;
    const bool fx_c = tech_constrained(cfg.fixed), pp_c = tech_constrained(cfg.pps), av_c = tech_constrained(cfg.avs);
    auto fmt_va = [&](std::uint16_t mv, std::uint16_t ma, const char* fallback) -> std::size_t {
        if (mv && ma) {
            char b[8]{};
            std::size_t n = append_tenth(b, 0, mv, 'V');
            n = append_tenth(b, n, ma, 'A');
            // Drop trailing 'A' when 7 chars (e.g. "20V1.5A" -> "20V1.5") so both numbers stay visible in 6 chars.
            if (n <= cap || (n == cap + 1 && b[n - 1] == 'A')) {
                const std::size_t copy_n = std::min(n, cap);
                for (std::size_t i = 0; i < copy_n; ++i) dst[i] = b[i];
                return copy_n;
            }
            return append_tenth(dst, 0, mv, 'V');
        }
        if (mv) return append_tenth(dst, 0, mv, 'V');
        if (ma) return append_tenth(dst, 0, ma, 'A');
        return append_str(dst, 0, fallback);
    };
    auto fmt_tag = [&](const char* tag, const LimTech& t) -> std::size_t {
        const std::size_t n = append_str(dst, 0, tag);
        if (t.max_mv) return append_u(dst, n, t.max_mv / 1000u);
        return t.max_ma ? append_u(dst, n, (t.max_ma + 500u) / 1000u) : n;
    };
    if ((fx && !pp && !av) || (fx_c && !pp_c && !av_c)) return fmt_va(cfg.fixed.max_mv, cfg.fixed.max_ma, "FIX");
    if ((!fx && pp && !av) || (pp_c && !fx_c && !av_c)) return fmt_tag("PPS", cfg.pps);
    if ((!fx && !pp && av) || (av_c && !fx_c && !pp_c)) return fmt_tag("AVS", cfg.avs);
    if (cfg.limit_mw != 0) {
        std::size_t n = append_u(dst, 0, lim_watts(cfg.limit_mw));
        if (n < cap) dst[n++] = 'W';
        return n;
    }
    if (cfg.fixed.enabled && cfg.fixed.max_mv) return append_tenth(dst, 0, cfg.fixed.max_mv, 'V');
    if (cfg.fixed.enabled && cfg.fixed.max_ma) return append_tenth(dst, 0, cfg.fixed.max_ma, 'A');
    return append_str(dst, 0, "LIMIT");
}

std::size_t lim_picks(std::span<const Pdo> pdos, std::span<LimPick> out) {
    auto whole_w = [](std::uint32_t uw) { return (uw + 999999) / 1000000 * 1000u; };   // microwatts -> mW, up to the whole watt
    std::uint32_t best = 0;
    LimPick c[kMaxPdos];
    std::size_t n = 0;
    for (const Pdo& p : pdos) {
        best = std::max(best, whole_w(p.max_mw * 1000));
        if (p.type != PdoType::Fixed || n >= std::size(c)) continue;
        c[n++] = {whole_w(std::uint32_t(p.max_mv) * p.max_ma), p.max_mv, p.max_ma};
    }
    // Insertion sort, stable: std::stable_sort allocates (new/delete + the unwinder, +15 KB).
    for (std::size_t i = 1; i < n; ++i)
        for (std::size_t j = i; j > 0 && c[j].mw < c[j - 1].mw; --j) std::swap(c[j], c[j - 1]);
    std::size_t k = 0;
    for (std::size_t i = 0; i < n && k < out.size(); ++i) {
        if (c[i].mw < kLimMinMw || c[i].mw > kLimMaxMw || c[i].mw >= best) continue;
        if (k && out[k - 1].mw == c[i].mw) continue;
        out[k++] = c[i];
    }
    return k;
}

std::uint32_t request_mw(std::uint32_t rdo, std::span<const std::uint32_t> pdos) {
    const unsigned pos = rdo >> 28;
    if (pos < 1 || pos > pdos.size()) return 0;
    const std::uint32_t raw = pdos[pos - 1];
    const Pdo p = decode_pdo(raw);
    if (p.type == PdoType::Battery) return ((rdo >> 10) & 0x3FF) * 250u;   // operating power
    const Rdo r = decode_rdo(rdo, &p);
    return std::uint32_t(r.mv) * r.op_ma / 1000;
}

bool request_complies(std::uint32_t rdo, std::span<const std::uint32_t> real_pdos,
                      std::span<const std::uint32_t> forged_pdos, const LimConfig& cfg) {
    const unsigned pos = rdo >> 28;
    if (pos < 1 || pos > forged_pdos.size() || pos > real_pdos.size()) return false;
    const Pdo fp = decode_pdo(forged_pdos[pos - 1]);
    if (fp.type == PdoType::Unknown) return false;
    if (fp.type == PdoType::Battery) {
        if (fp.max_mw == 0) return false;
        const std::uint32_t op_mw = ((rdo >> 10) & 0x3FFu) * 250u;
        const std::uint32_t max_mw_r = (rdo & 0x3FFu) * 250u;
        const bool gb = (rdo & (1u << 27)) != 0;
        if (op_mw > fp.max_mw || (!gb && max_mw_r > fp.max_mw)) return false;
    } else {
        if (fp.max_ma == 0) return false;
        const Rdo r = decode_rdo(rdo, &fp);
        if (fp.type == PdoType::Pps) {
            if (r.mv < fp.min_mv || r.mv > fp.max_mv || r.op_ma > fp.max_ma) return false;
        } else if (fp.type == PdoType::Avs) {
            const std::uint16_t lim_ma = current_limit_ma(fp, r.mv);
            if (r.mv > fp.max_mv || lim_ma == 0 || r.op_ma > lim_ma) return false;
        } else {
            const std::uint16_t req_ma = (r.flags & kRdoFGiveBack) ? r.op_ma : std::max(r.op_ma, r.max_ma);
            if (req_ma > fp.max_ma) return false;
        }
    }
    if (cfg.limit_mw != 0 && request_mw(rdo, real_pdos) > cfg.limit_mw) return false;
    return true;
}

std::size_t forge_caps(std::span<const std::uint32_t> real, const LimConfig& cfg, std::span<std::uint32_t> out) {
    const std::size_t n = std::min(std::min(real.size(), out.size()), kMaxPdos);
    if (n == 0) return 0;
    std::array<std::uint32_t, kMaxPdos> words{};
    std::array<bool, kMaxPdos> keep{};
    for (std::size_t i = 0; i < n; i++) {
        std::uint32_t w = real[i];
        if (w == 0) {
            words[i] = 0;
            keep[i] = false;
            continue;
        }
        const Pdo p = decode_pdo(w);
        auto clamp10 = [](std::uint32_t cur, std::uint32_t shift, std::uint32_t max_ma) {
            const std::uint32_t orig = (cur >> shift) & 0x3FFu;
            const std::uint32_t lim = max_ma / 10u;
            return orig > lim ? ((cur & ~(0x3FFu << shift)) | (lim << shift)) : cur;
        };
        const bool within_w = cfg.limit_mw == 0 || p.max_mw <= cfg.limit_mw;
        const bool fx_ok = cfg.fixed.enabled && (cfg.fixed.max_mv == 0 || p.max_mv <= cfg.fixed.max_mv) && within_w;
        switch (w >> 30) {
            case 0: { // Fixed
                if (i == 0) {
                    w &= ~(1u << 23);   // never invite EPR while capped
                    keep[i] = true;
                    std::uint32_t cap_ma = p.max_ma;
                    if (!cfg.fixed.enabled) cap_ma = std::min<std::uint32_t>(cap_ma, kLimMinFixedMa);
                    else {
                        if (cfg.fixed.max_ma) cap_ma = std::min<std::uint32_t>(cap_ma, cfg.fixed.max_ma);
                        if (cfg.limit_mw && p.max_mv)
                            cap_ma = std::min<std::uint32_t>(cap_ma, std::max<std::uint32_t>(kLimMinFixedMa, cfg.limit_mw * 1000u / p.max_mv));
                    }
                    w = clamp10(w, 0, cap_ma);
                } else {
                    keep[i] = fx_ok;
                    if (!fx_ok) w &= ~0x3FFu;   // neutered placeholder (0 mA) if followed by an enabled slot
                    else if (cfg.fixed.max_ma) w = clamp10(w, 0, cfg.fixed.max_ma);
                }
                break;
            }
            case 1: { // Battery
                keep[i] = fx_ok;
                if (!fx_ok) {
                    w &= ~0x3FFu;
                } else if (cfg.fixed.max_ma && p.max_mv) {
                    const std::uint32_t lim_mw = std::uint32_t(p.max_mv) * cfg.fixed.max_ma / 1000u;
                    const std::uint32_t lim = lim_mw / 250u;
                    if ((w & 0x3FFu) > lim) w = (w & ~0x3FFu) | lim;
                }
                break;
            }
            case 2: { // Variable
                keep[i] = fx_ok;
                if (!fx_ok) w &= ~0x3FFu;
                else if (cfg.fixed.max_ma) w = clamp10(w, 0, cfg.fixed.max_ma);
                break;
            }
            default: {
                const unsigned apdo = (w >> 28) & 3u;
                if (apdo == 0) {   // PPS
                    const bool ok = cfg.pps.enabled && (cfg.pps.max_mv == 0 || cfg.pps.max_mv >= p.min_mv) && within_w;
                    keep[i] = ok;
                    if (!ok) {
                        w &= ~0x7Fu;
                    } else {
                        if (cfg.pps.max_mv && cfg.pps.max_mv < p.max_mv)
                            w = (w & ~(0xFFu << 17)) | (std::uint32_t(cfg.pps.max_mv / 100u) << 17);
                        if (cfg.pps.max_ma) {
                            const std::uint32_t lim = cfg.pps.max_ma / 50u;
                            if ((w & 0x7Fu) > lim) w = (w & ~0x7Fu) | lim;
                        }
                    }
                } else if (apdo == 2) {   // SPR AVS
                    const bool ok = cfg.avs.enabled && (cfg.avs.max_mv == 0 || cfg.avs.max_mv >= 15000) && within_w;
                    keep[i] = ok;
                    if (!ok) {
                        w &= ~0xFFFFFu;
                    } else {
                        if (cfg.avs.max_mv && cfg.avs.max_mv < 20000) w &= ~0x3FFu;   // leaves valid 9..15 V range
                        if (cfg.avs.max_ma) {
                            w = clamp10(w, 10, cfg.avs.max_ma);
                            if ((w & 0x3FFu) != 0) w = clamp10(w, 0, cfg.avs.max_ma);
                        }
                    }
                } else {
                    keep[i] = within_w;
                }
                break;
            }
        }
        words[i] = w;
    }
    std::size_t last_kept = 0;
    bool any_kept = false;
    for (std::size_t i = n; i > 0; i--) {
        if (keep[i - 1]) {
            last_kept = i - 1;
            any_kept = true;
            break;
        }
    }
    if (!any_kept) return 0;
    const std::size_t out_n = last_kept + 1;
    for (std::size_t i = 0; i < out_n; i++) out[i] = words[i];
    return out_n;
}

std::size_t forge_caps(std::span<const std::uint32_t> real, std::uint32_t limit_mw, std::span<std::uint32_t> out) {
    return forge_caps(real, lim_config_watts(limit_mw), out);
}

bool forge_differs(std::span<const std::uint32_t> real, std::span<const std::uint32_t> forged) {
    if (real.size() != forged.size()) return true;
    for (std::size_t i = 0; i < real.size(); i++)
        if (real[i] != forged[i]) return true;
    return false;
}

bool Limiter::forged_differs_from_watch(std::array<std::uint32_t, 7>& f, std::size_t& cnt) const {
    const auto real = watch_.pdos();
    cnt = forge_caps(real, st_.cfg, f);
    return cnt > 0 && forge_differs(real, {f.data(), cnt});
}

bool Limiter::arm(const LimConfig& cfg, std::uint32_t now, const IqWatch* seed) {
    if (!lim_config_valid(cfg)) return false;
    st_ = LimStatus{};
    st_.state = LimState::Armed;
    st_.cfg = cfg;
    st_.limit_mw = cfg.limit_mw;
    watch_.reset();
    last_at_ = now;
    inj_streak_ = 0;
    sink_asked_caps_ = false;
    if (seed && seed->contract().ready) {   // armed mid-contract: the list, the Request and the MessageID are already known
        watch_ = *seed;
        last_at_ = watch_.last_frame_at();
        const IqContract& c = watch_.contract();
        st_.req_rdo = c.rdo;
        st_.req_pos = c.pos;
        st_.req_mw = request_mw(c.rdo, watch_.pdos());
        std::array<std::uint32_t, 7> f{};
        std::size_t cnt = 0;
        if (forged_differs_from_watch(f, cnt)) {
            st_.state = LimState::Pending;
            ready_at_ = now;
        } else {
            st_.state = LimState::Limited;
            inj_at_ = now - kLimBlameMs - 1;   // nothing to forge: a reset now is nobody's rejection
        }
    }
    return true;
}

bool Limiter::arm(std::uint32_t limit_mw, std::uint32_t now, const IqWatch* seed) {
    return arm(lim_config_watts(limit_mw), now, seed);
}

void Limiter::stop(LimWhy why) {
    if (!armed()) return;
    st_.state = LimState::Off;
    st_.why = why;
}

void Limiter::on_detach() {
    if (!armed()) return;   // an off limiter's last status stays as the stop left it
    restart();
    st_.req_pos = 0;
    st_.req_rdo = 0;
    st_.req_mw = 0;
    st_.forged_n = 0;
    st_.forged.fill(0);
}

void Limiter::restart() {
    if (!armed()) return;
    watch_.reset();
    st_.state = LimState::Armed;
    inj_streak_ = 0;
    sink_asked_caps_ = false;
}

void Limiter::reject(LimWhy cause) {
    if (++st_.rejects >= kLimMaxRejects) stop(cause);
}

void Limiter::blame_check(LimWhy cause, std::uint32_t now) {
    if ((st_.state == LimState::Injected || st_.state == LimState::Limited) && !due(now, inj_at_, kLimBlameMs + 1)) reject(cause);
}

void Limiter::saw_request(std::uint32_t rdo, std::uint32_t now) {
    if ((rdo >> 28) >= 8) return stop(LimWhy::Epr);   // an EPR object position: only in an EPR contract
    st_.req_rdo = rdo;
    st_.req_pos = std::uint8_t(rdo >> 28);
    st_.req_mw = request_mw(rdo, watch_.pdos());
    std::array<std::uint32_t, 7> f{};
    const std::size_t cnt = forge_caps(watch_.pdos(), st_.cfg, f);
    const bool ok = cnt > 0 && request_complies(rdo, watch_.pdos(), {f.data(), cnt}, st_.cfg);
    auto restart_neg = [&] {
        st_.state = LimState::Negotiating;
        neg_at_ = now;
    };
    switch (st_.state) {
        case LimState::Armed:
            if (!watch_.pdos().empty() && !ok) restart_neg();
            break;
        case LimState::Injected:
            if (ok) {
                st_.state = LimState::Limited;
            } else {
                if (++st_.ignores >= kLimMaxRejects) return stop(LimWhy::SinkIgnores);
                restart_neg();
            }
            break;
        case LimState::Limited:
            if (!ok) restart_neg();
            break;
        case LimState::Negotiating:
        case LimState::Pending:
            restart_neg();
            break;
        default: break;
    }
}

void Limiter::on_message(Sop sop, std::uint16_t h, std::span<const std::uint8_t> payload, std::uint32_t now) {
    if (!armed() || sop != Sop::Sop) return;
    last_at_ = now;
    watch_.on_message(sop, h, payload, now);
    const std::uint8_t t = type_of(h);
    const unsigned n = ndo_of(h);
    const bool src = from_source(h);
    if (src && (ext_of(h) || n > 0 || t != kCtlGoodCrc)) inj_streak_ = 0;
    if (ext_of(h)) {
        if (src && t == kExtEprSourceCap) stop(LimWhy::Epr);
        return;
    }
    if (n > 0) {
        if (t == kDataEprMode && payload.size() >= 4) {
            const std::uint8_t a = payload[3];
            if (a == kEprActionAck || a == kEprActionSucceeded) stop(LimWhy::Epr);
        } else if (src && t == kDataSourceCap) {
            if (!sink_asked_caps_) blame_check(LimWhy::ChargerResets, now);   // ignore a charger list answered to Get_Source_Cap
            sink_asked_caps_ = false;
            if (!armed()) return;
            st_.state = LimState::Negotiating;
            neg_at_ = now;
        } else if (!src && t == kDataRequest && payload.size() >= 4) {
            saw_request(le32(payload.data()), now);
        }
        return;
    }
    if (t == kCtlGoodCrc) return;
    if (!src && t == kCtlGetSourceCap) sink_asked_caps_ = true;
    if (t == kCtlSoftReset) {
        blame_check(src ? LimWhy::ChargerResets : LimWhy::SinkResets, now);
        restart();
    } else if (src && t == kCtlPsRdy && st_.state == LimState::Negotiating && watch_.contract().ready) {
        std::array<std::uint32_t, 7> f{};
        std::size_t cnt = 0;
        if (forged_differs_from_watch(f, cnt)) {
            st_.state = LimState::Pending;
            ready_at_ = now;
        } else {
            st_.state = LimState::Limited;
            inj_at_ = now - kLimBlameMs - 1;
        }
    }
}

// Who reset is a guess: a reset after the sink answered our list (it asked again, Limited) is the charger refusing that
// Request; a reset with no answer is the sink refusing the list.
void Limiter::on_hard_reset(std::uint32_t now) {
    if (!armed()) return;
    blame_check(st_.state == LimState::Limited ? LimWhy::ChargerResets : LimWhy::SinkResets, now);
    restart();
}

void Limiter::tick(std::uint32_t now) {
    switch (st_.state) {
        case LimState::Negotiating:
            if (due(now, neg_at_, kLimNegMs + 1)) st_.state = LimState::Armed;
            break;
        case LimState::Pending:
            if (due(now, ready_at_, kLimPendMs + 1)) st_.state = LimState::Armed;
            break;
        case LimState::Injected:
            if (due(now, inj_at_, kLimVerifyMs + 1)) {   // the sink never answered: retry instead of stalling in Armed
                if (++st_.ignores >= kLimMaxRejects) stop(LimWhy::SinkIgnores);
                else {
                    st_.state = LimState::Pending;
                    ready_at_ = now;
                }
            }
            break;
        case LimState::Limited:
            if (due(now, inj_at_, kLimBlameMs + 1)) st_.rejects = st_.ignores = 0;   // it held: the count starts over
            break;
        default: break;
    }
}

bool Limiter::take_tx(TxMsg& m, std::uint32_t now) {
    if (st_.state != LimState::Pending) return false;
    if (st_.injections >= kLimMaxInjections) {
        stop(LimWhy::Rate);
        return false;
    }
    if (!due(now, last_at_, kLimQuietMs)) return false;
    if (st_.injections > 0 && !due(now, inj_at_, kLimGapMs)) return false;
    std::array<std::uint32_t, 7> f{};
    std::size_t cnt = 0;
    if (!forged_differs_from_watch(f, cnt)) {
        st_.state = cnt == 0 ? LimState::Armed : LimState::Limited;
        if (cnt > 0) inj_at_ = now - kLimBlameMs - 1;
        return false;
    }
    const std::uint8_t drev = watch_.contract().dev_rev;
    const std::uint8_t rev = (drev > 0 && drev < watch_.src_rev()) ? drev : watch_.src_rev();
    const std::uint8_t skip = static_cast<std::uint8_t>(1u + (inj_streak_ % 6u));
    inj_streak_++;
    m = TxMsg{};
    m.sop = Sop::Sop;
    m.inline_tx = true;
    m.header = watch_.stamp(make_header(kDataSourceCap, std::uint8_t(cnt), rev), skip);
    for (std::size_t i = 0; i < cnt; i++)
        for (unsigned b = 0; b < 4; b++) m.data[4 * i + b] = std::uint8_t(f[i] >> (8 * b));
    m.len = std::uint8_t(4 * cnt);
    st_.forged = f;
    st_.forged_n = std::uint8_t(cnt);
    st_.injections++;
    inj_at_ = now;
    st_.state = LimState::Injected;
    return true;
}

}  // namespace k2::pd
