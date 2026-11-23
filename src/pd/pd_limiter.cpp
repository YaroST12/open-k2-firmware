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

}  // namespace

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

LimRefusal lim_gate(const LimFacts& f, std::uint32_t limit_mw) {
    if (limit_mw < kLimMinMw || limit_mw > kLimMaxMw) return LimRefusal::BadLimit;
    return f.busy ? LimRefusal::Busy : LimRefusal::None;
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

std::size_t forge_caps(std::span<const std::uint32_t> real, std::uint32_t limit_mw, std::span<std::uint32_t> out) {
    const std::size_t n = std::min(real.size(), out.size());
    for (std::size_t i = 0; i < n; i++) {
        std::uint32_t w = real[i];
        // current limit in `unit_ma` units at `mv`: P/V rounded down, never above the object's own
        auto cap = [&](unsigned shift, std::uint32_t mask, std::uint32_t mv, std::uint32_t unit_ma) {
            if (mv == 0) return;
            const std::uint32_t lim = std::uint32_t(limit_mw) * 1000u / mv / unit_ma;
            if (((w >> shift) & mask) > lim) w = (w & ~(mask << shift)) | (lim << shift);
        };
        switch (w >> 30) {
            case 0:   // fixed
                cap(0, 0x3FF, ((w >> 10) & 0x3FF) * 50, 10);
                if (i == 0) w &= ~(1u << 23);   // EPR capable
                break;
            case 1: { // battery: power in 250 mW
                const std::uint32_t lim = limit_mw / 250;
                if ((w & 0x3FF) > lim) w = (w & ~0x3FFu) | lim;
                break;
            }
            case 2:   // variable: the current at the top voltage
                cap(0, 0x3FF, ((w >> 20) & 0x3FF) * 50, 10);
                break;
            default:
                if (((w >> 28) & 3) == 0) cap(0, 0x7F, ((w >> 17) & 0xFF) * 100, 50);   // PPS at its top voltage
                else if (((w >> 28) & 3) == 2) {                                             // SPR AVS: 15 V and 20 V limits
                    cap(10, 0x3FF, 15000, 10);
                    cap(0, 0x3FF, 20000, 10);
                }
                break;   // EPR AVS never sits in an SPR list; left alone
        }
        out[i] = w;
    }
    return n;
}

bool Limiter::arm(std::uint32_t limit_mw, std::uint32_t now, const IqWatch* seed) {
    if (limit_mw < kLimMinMw || limit_mw > kLimMaxMw) return false;
    st_ = LimStatus{};
    st_.state = LimState::Armed;
    st_.limit_mw = limit_mw;
    watch_.reset();
    last_at_ = now;
    if (seed && seed->contract().ready) {   // armed mid-contract: the list, the Request and the MessageID are already known
        watch_ = *seed;
        last_at_ = watch_.last_frame_at();
        const IqContract& c = watch_.contract();
        st_.req_rdo = c.rdo;
        st_.req_pos = c.pos;
        st_.req_mw = request_mw(c.rdo, watch_.pdos());
        if (st_.req_mw > limit_mw) {
            st_.state = LimState::Pending;
            ready_at_ = now;
        } else {
            st_.state = LimState::Limited;
            inj_at_ = now - kLimBlameMs - 1;   // no list of ours is in flight: a reset now is nobody's rejection
        }
    }
    return true;
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
    const bool over = st_.req_mw > st_.limit_mw;
    auto restart_neg = [&] {
        st_.state = LimState::Negotiating;
        neg_at_ = now;
    };
    switch (st_.state) {
        case LimState::Injected:
            if (!over) {
                st_.state = LimState::Limited;
            } else {
                if (++st_.ignores >= kLimMaxRejects) return stop(LimWhy::SinkIgnores);
                restart_neg();
            }
            break;
        case LimState::Limited:
            if (over) restart_neg();
            break;
        case LimState::Negotiating:
        case LimState::Pending:
            restart_neg();
            break;
        default: break;   // Armed: no real list seen, nothing to forge from
    }
}

void Limiter::on_message(Sop sop, std::uint16_t h, std::span<const std::uint8_t> payload, std::uint32_t now) {
    if (!armed() || sop != Sop::Sop) return;
    last_at_ = now;
    watch_.on_message(sop, h, payload, now);
    const std::uint8_t t = type_of(h);
    const unsigned n = ndo_of(h);
    const bool src = from_source(h);
    if (ext_of(h)) {
        if (src && t == kExtEprSourceCap) stop(LimWhy::Epr);
        return;
    }
    if (n > 0) {
        if (t == kDataEprMode && payload.size() >= 4) {
            const std::uint8_t a = payload[3];
            if (a == kEprActionAck || a == kEprActionSucceeded) stop(LimWhy::Epr);
        } else if (src && t == kDataSourceCap) {
            blame_check(LimWhy::ChargerResets, now);   // the charger starting over right after our list
            if (!armed()) return;
            st_.state = LimState::Negotiating;
            neg_at_ = now;
        } else if (!src && t == kDataRequest && payload.size() >= 4) {
            saw_request(le32(payload.data()), now);
        }
        return;
    }
    if (t == kCtlGoodCrc) return;
    if (t == kCtlSoftReset) {
        blame_check(src ? LimWhy::ChargerResets : LimWhy::SinkResets, now);
        restart();
    } else if (src && t == kCtlPsRdy && st_.state == LimState::Negotiating && watch_.contract().ready) {
        st_.state = LimState::Pending;
        ready_at_ = now;
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
            if (due(now, inj_at_, kLimVerifyMs + 1)) {   // the sink never answered the list
                st_.state = LimState::Armed;
                if (++st_.ignores >= kLimMaxRejects) stop(LimWhy::SinkIgnores);
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
    const std::size_t cnt = forge_caps(watch_.pdos(), st_.limit_mw, f);
    if (cnt == 0) {
        st_.state = LimState::Armed;
        return false;
    }
    m = TxMsg{};
    m.sop = Sop::Sop;
    m.inline_tx = true;
    m.header = watch_.stamp(make_header(kDataSourceCap, std::uint8_t(cnt), watch_.src_rev()), 1);
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
