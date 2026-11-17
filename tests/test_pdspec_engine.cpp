// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// PD Spec Check engine (k2/pdspec/engine.hpp) against a simulated source: a millisecond model of a PD source (Accept / PS_RDY timing,
// VBUS slew, PPS keep-alive and EPR keep-alive timeouts, Hard Reset and detach paths) behind the engine's Env seam, with the real
// capture analysis (analyze.hpp) over the samples the model records. Covers a clean run (score 0), the failing behaviours each check is
// for, refused levels, every stop reason, the start refusals and a cancel at every stage (always ending with the silence off, the Rd
// on, the capture released and a 5 V contract or a dark line).
#include <array>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "k2/chargertest/spec_rows.hpp"
#include "k2/pd/transcap_calc.hpp"
#include "k2/pdspec/engine.hpp"
#include "test_util.h"

using namespace k2;
using namespace k2::pdspec;

namespace {

pd::Pdo fixed(std::uint16_t mv, bool epr_bit = false) {
    pd::Pdo d;
    d.type = pd::PdoType::Fixed;
    d.min_mv = d.max_mv = mv;
    d.max_ma = 3000;
    d.raw = (1u << 26) | (epr_bit ? 1u << 23 : 0u) | (std::uint32_t(mv / 50) << 10) | 300;
    return d;
}
pd::Pdo avs(std::uint16_t lo, std::uint16_t hi) {
    pd::Pdo d;
    d.type = pd::PdoType::Avs;
    d.min_mv = lo;
    d.max_mv = hi;
    d.max_ma = 3000;
    d.raw = 0xD0000000u;
    return d;
}
pd::Pdo pps(std::uint16_t lo, std::uint16_t hi) {
    pd::Pdo d;
    d.type = pd::PdoType::Pps;
    d.min_mv = lo;
    d.max_mv = hi;
    d.max_ma = 3000;
    d.raw = 0xC0000000u;
    return d;
}

// The model. Times in ms; the sink's microsecond stamps are t * 1000 with bit 0 set, samples sit at t * 1000 + 500.
struct Sim final : Env {
    // ---- behaviour ----
    pd::SourceCaps c{};
    std::uint32_t cap = 36000;
    bool proof = false, self = false, inl = false, cap_busy = false;
    int accept_ms = 5, start_ms = 30, pps_start_ms = 5, psrdy_after_ms = 10;
    int early_psrdy_ms = -1;            // >= 0: PS_RDY this long after the Accept, wherever VBUS is
    int pps_offset_mv = 0;              // a PPS output this far off its set point
    int pps_fold_from_mv = 0;           // > 0: a request at or above this folds to pps_fold_to_mv (a fixed setpoint, like the FH22 at 20 V)
    int pps_fold_to_mv = 0;
    int pps_jump_high_mv = 0;           // a programmable move of more than pps_jump_min_mv lands this far HIGH and holds there until the next request
    int pps_jump_min_mv = 500;
    int pps_small_offset_mv = 0;        // a smaller programmable move tracks this far off its request (the UGREEN lands 5-12 mV low)
    int pps_overshoot_mv = 0;           // a programmable step up overshoots its level by this much for pps_overshoot_ms
    int pps_overshoot_ms = 0;
    int pps_status_ms = 3;              // Get_PPS_Status -> PPS_Status
    int pps_status_mode = 0;            // 0 a clean answer, 1 Not_Supported, 2 silence, 3 OMF set, 4 voltage off by 10 %
    std::uint8_t no_psrdy_pos = 0;      // Accept, then no PS_RDY: the sink Hard Resets after tPSTransition
    std::uint8_t wait_pos = 0;          // Wait until the sink gives up (WaitExhausted)
    std::uint8_t stuck_pos = 0;         // no answer at all, and no give-up (a stuck sink)
    bool back_old = false;              // after a reset the sink asks for the level it had before
    int rise = 100, fall = 100;         // mV per ms
    int hr_fall = 100, bleed = 50, dark_ms = 700, reattach_ms = 300;
    std::uint8_t reject_pos = 0;
    bool never_recover = false;
    int pps_timeout_ms = 13000;         // 0 = never
    int epr_ka_timeout_ms = 900;
    int ov_at = -1, load_at = -1, src_hr_at = -1;
    // ---- state ----
    std::uint32_t t = 0;
    Snapshot s{};
    std::int32_t v = 5000, vt = 5000;
    int rate = 100;
    std::uint32_t ramp_at = 0;
    enum class Ev { None, Accept, Reject, Wait, PsRdy, Never, Stuck } ev = Ev::None;
    std::uint32_t ev_at = 0;
    pd::Choice want{};
    bool reached = true;
    std::uint32_t reached_at = 0;
    std::uint32_t last_req = 0, last_ka = 0;
    Silence sil = Silence::Off;
    bool rd_is_off = false, want_release = false, releasing = false;
    std::uint8_t old_pos = 0;
    enum class Rs { None, Falling, Dark, Rising, Caps, Gone } rs = Rs::None;
    std::uint32_t rs_t = 0, rs_until = 0;
    bool cap_on = false;
    std::uint32_t cap_end = 0;
    std::array<transcap::Sample, 64> ring{};
    std::size_t ring_n = 0;
    std::vector<transcap::Sample> cs;
    std::vector<transcap::Marker> cm;
    // ---- what the engine did ----
    int releases = 0, captures = 0, requests = 0, notes = 0, pps_asks = 0, pps_asks_in_ka = 0;
    std::uint32_t pps_status_at = 0, pps_ask_t = 0, over_until = 0;
    std::int32_t over_v = 0;
    std::int32_t max_req_mv = 0;
    bool finished = false;
    Stop stop = Stop::None;

    Sim() {
        s.attached = true;
        s.state = pd::SinkState::Ready;
        s.result = pd::SinkResult::Ok;
        s.pos = 1;
        s.mv = 5000;
        s.vbus_mv = 5000;
    }
    void list(std::initializer_list<pd::Pdo> l) {
        c = {};
        c.valid = true;
        for (const auto& d : l) c.pdos[c.num_pdos++] = d;
    }
    std::int32_t level(const pd::Choice& ch) const {
        const pd::Pdo& d = c.pdos[ch.pos - 1];
        return d.type == pd::PdoType::Fixed ? d.max_mv : ch.mv;
    }
    bool is_pps() const { return s.pos && c.pdos[s.pos - 1].type != pd::PdoType::Fixed; }
    void mark(transcap::Mark m) { if (cap_on) cm.push_back({t * 1000 + 1, std::uint8_t(m), 0, 0}); }
    transcap::View view() const { return {cs, cm}; }

    void start_reset() {
        if (s.pos) old_pos = s.pos;
        s.attached = false;
        s.state = pd::SinkState::Idle;
        s.pos = 0;
        s.mv = 0;
        s.epr_mode = false;
        ev = Ev::None;
        vt = 0;
        ramp_at = t + 25;
        rate = hr_fall;
        reached = false;
        rs = Rs::Falling;
        rs_t = t;
    }
    void contract5() {
        s.state = pd::SinkState::Ready;
        s.result = pd::SinkResult::Ok;
        s.pos = 1;
        s.mv = 5000;
        s.accept_at = t;
        last_req = t;
    }

    void tick(std::uint32_t now) {
        t = now;
        if (int(t) == ov_at) { v = vt = 30000; }
        if (int(t) == load_at) s.ibus_ma = 500;
        if (int(t) == src_hr_at && s.attached) { ++s.source_hard_resets; start_reset(); }
        // The answers to a Request.
        if (ev == Ev::Wait && t >= ev_at) {
            s.waits_rx = std::uint16_t(s.waits_rx + 3);
            s.state = pd::SinkState::Ready;
            s.result = pd::SinkResult::WaitExhausted;
            ev = Ev::None;
        } else if (ev == Ev::Accept && t >= ev_at) {
            s.accept_at = t;
            s.accept_us = (t * 1000) | 1;
            mark(transcap::Mark::Accept);
            s.state = pd::SinkState::Transition;
            const bool p = c.pdos[want.pos - 1].type != pd::PdoType::Fixed;
            vt = level(want) + (p ? pps_offset_mv : 0);
            if (p && pps_fold_from_mv && level(want) >= pps_fold_from_mv) vt = pps_fold_to_mv;
            if (p) vt += std::abs(level(want) - int(s.mv)) > pps_jump_min_mv ? pps_jump_high_mv : pps_small_offset_mv;
            ramp_at = t + std::uint32_t(p ? pps_start_ms : start_ms);
            rate = vt >= v ? rise : fall;
            if (p && pps_overshoot_mv && vt > v) over_v = vt + pps_overshoot_mv;
            reached = vt == v;
            reached_at = t;
            ev = want.pos == no_psrdy_pos ? Ev::Never : Ev::PsRdy;
            ev_at = t;
        } else if (ev == Ev::Never && t >= ev_at + 550) {   // the sink's tPSTransition ran out: its own Hard Reset
            ++s.hard_resets;
            start_reset();
        } else if (ev == Ev::Reject && t >= ev_at) {
            ++s.rejects_rx;
            s.state = pd::SinkState::Ready;
            s.result = pd::SinkResult::Rejected;
            mark(transcap::Mark::Reject);
            ev = Ev::None;
        } else if (ev == Ev::PsRdy &&
                   ((early_psrdy_ms >= 0 && t >= ev_at + std::uint32_t(early_psrdy_ms)) ||
                    (early_psrdy_ms < 0 && reached && t >= reached_at + std::uint32_t(psrdy_after_ms)))) {
            s.ps_rdy_at = t;
            s.ps_rdy_us = (t * 1000) | 1;
            mark(transcap::Mark::PsRdy);
            s.state = pd::SinkState::Ready;
            s.result = pd::SinkResult::Ok;
            s.pos = want.pos;
            s.mv = std::uint16_t(level(want));
            if (want.pos >= 8) s.epr_mode = true;           // EPR stays on until a release, a reset or a detach
            if (releasing) { s.epr_mode = false; releasing = false; }
            last_req = last_ka = t;
            if (s.epr_mode) s.epr_ka_at = t;
            ev = Ev::None;
        }
        // VBUS (an overshoot sits on top of the level for its while once the ramp is there).
        if (over_until && t >= over_until) { over_until = 0; v = vt; }
        if (t >= ramp_at && v != vt && !over_until) {
            v = v < vt ? (vt - v > rate ? v + rate : vt) : (v - vt > rate ? v - rate : vt);
            if (v == vt) {
                reached = true;
                reached_at = t;
                if (over_v) { v = over_v; over_v = 0; over_until = t + std::uint32_t(pps_overshoot_ms); }
            }
        }
        // Get_PPS_Status.
        if (s.pps_ask == pd::PpsAsk::Pending && pps_status_mode == 2 && t >= pps_ask_t + 200) { s.pps_ask = pd::PpsAsk::Timeout; ++s.pps_seq; }
        if (s.pps_ask == pd::PpsAsk::Pending && pps_status_mode != 2 && t >= pps_status_at) {
            s.pps_rx_us = (t * 1000) | 1;
            ++s.pps_seq;
            if (pps_status_mode == 1) {
                s.pps_ask = pd::PpsAsk::NotSupported;
            } else {
                s.pps_ask = pd::PpsAsk::Got;
                s.pps_mv_raw = std::uint16_t((pps_status_mode == 4 ? v * 11 / 10 : v) / 20);
                s.pps_ma_raw = 0;
                s.pps_flags = std::uint8_t(1 << 1 | (pps_status_mode == 3 ? 8 : 0));
            }
        }
        // Keep-alives (the sink's) and the source's timers.
        if (s.attached && s.state == pd::SinkState::Ready) {
            if (is_pps() && sil != Silence::Pps && t - last_req >= 5000) { last_req = t; s.accept_at = t; }
            if (s.epr_mode && sil != Silence::Epr && t - last_ka >= 400) { last_ka = t; s.epr_ka_at = t; }
            if (is_pps() && pps_timeout_ms > 0 && t - last_req >= std::uint32_t(pps_timeout_ms)) {
                ++s.source_hard_resets;
                start_reset();
            } else if (s.epr_mode && t - last_ka >= std::uint32_t(epr_ka_timeout_ms)) {
                ++s.source_hard_resets;
                start_reset();
            }
        }
        // The reset / detach path.
        switch (rs) {
            case Rs::Falling:
                if (v == 0) { rs = never_recover ? Rs::Gone : Rs::Dark; rs_t = t; rs_until = t + std::uint32_t(dark_ms); }
                break;
            case Rs::Dark:
                if (t >= rs_until && !rd_is_off) { vt = 5000; ramp_at = t; rate = 100; rs = Rs::Rising; rs_t = t; }
                break;
            case Rs::Rising:
                if (t - rs_t >= 150) { ++s.caps_rx; s.attached = true; rs = Rs::Caps; rs_t = t; }
                break;
            case Rs::Caps:
                if (t - rs_t >= 20) {
                    contract5();
                    if (back_old && old_pos > 1 && c.pdos[old_pos - 1].type == pd::PdoType::Fixed) {
                        s.pos = old_pos;
                        s.mv = c.pdos[old_pos - 1].max_mv;
                        vt = s.mv;
                        ramp_at = t;
                        rate = rise;
                    }
                    rs = Rs::None;
                }
                break;
            case Rs::None:
            case Rs::Gone: break;
        }
        if (want_release && s.attached && s.state == pd::SinkState::Ready && ev == Ev::None) {
            want_release = false;
            if (s.pos != 1) { pd::Choice ch; ch.pos = 1; if (request(ch) == pd::Err::Ok) releasing = true; }
            else s.epr_mode = false;
        }
        s.vbus_mv = v;
        s.now_us = t * 1000;
        const transcap::Sample smp{t * 1000 + 500, std::uint16_t(v), 0, 0};
        ring[ring_n++ % ring.size()] = smp;
        if (cap_on && t * 1000 <= cap_end) cs.push_back(smp);
    }

    // ---- Env ----
    Snapshot snapshot() override {
        Snapshot r = s;
        r.self_powered = self;
        r.inline_link = inl;
        return r;
    }
    const pd::SourceCaps& caps() override { return c; }
    std::uint32_t cap_mv() override { return cap; }
    bool epr_proof() override { return proof; }
    pd::Err request(const pd::Choice& ch) override {
        if (inl) return pd::Err::Inline;
        if (!s.attached || s.state != pd::SinkState::Ready || ev != Ev::None) return pd::Err::Busy;
        if (ch.pos == 0 || ch.pos > c.num_pdos) return pd::Err::BadPos;
        if (std::uint32_t(level(ch)) > cap) return pd::Err::OverCap;
        ++requests;
        if (level(ch) > max_req_mv) max_req_mv = level(ch);
        s.state = pd::SinkState::Requesting;
        s.req_us = (t * 1000) | 1;
        s.accept_us = s.ps_rdy_us = 0;
        mark(transcap::Mark::TxRequest);
        want = ch;
        if (ch.pos == reject_pos) ev = Ev::Reject;
        else if (ch.pos == wait_pos) ev = Ev::Wait;
        else if (ch.pos == stuck_pos) ev = Ev::Stuck;      // never answered, never given up
        else ev = Ev::Accept;
        ev_at = t + std::uint32_t(ev == Ev::Wait ? 300 : accept_ms);
        return pd::Err::Ok;
    }
    void release() override {
        ++releases;
        want_release = true;
    }
    bool hard_reset() override {
        if (!s.attached) return false;
        ++s.hard_resets;
        start_reset();
        return true;
    }
    void rd_off(bool off) override {
        rd_is_off = off;
        if (off) {
            s.attached = false;
            s.state = pd::SinkState::Idle;
            s.pos = 0;
            s.mv = 0;
            s.epr_mode = false;
            ev = Ev::None;
            vt = 0;
            ramp_at = t + 20;
            rate = bleed;
            reached = false;
            rs = Rs::Gone;
        } else if (!never_recover) {
            rs = Rs::Dark;
            rs_until = t + std::uint32_t(reattach_ms);
        }
    }
    void keepalive_silence(Silence k) override { sil = k; }
    pd::Err ask_pps_status() override {
        if (!s.attached || s.state != pd::SinkState::Ready || ev != Ev::None || s.pps_ask == pd::PpsAsk::Pending) return pd::Err::Busy;
        ++pps_asks;
        if (sil == Silence::Pps) ++pps_asks_in_ka;
        s.pps_ask = pd::PpsAsk::Pending;
        s.pps_ask_us = (t * 1000) | 1;
        s.pps_rx_us = 0;
        pps_ask_t = t;
        pps_status_at = t + std::uint32_t(pps_status_ms);
        return pd::Err::Ok;
    }
    bool capture_start(std::uint32_t post_us) override {
        if (cap_busy || cap_on) return false;
        ++captures;
        cap_on = true;
        cs.clear();
        cm.clear();
        const std::size_t n = ring_n < ring.size() ? ring_n : ring.size();
        for (std::size_t i = ring_n - n; i < ring_n; ++i) cs.push_back(ring[i % ring.size()]);
        cap_end = t * 1000 + post_us;
        return true;
    }
    bool capture_frozen() override { return cap_on && t * 1000 >= cap_end; }
    Edges capture_edges(std::uint32_t from_us) override { return find_edges(view(), from_us); }
    TransitionOut capture_transition(const TransitionIn& in) override { return analyze_transition(view(), in); }
    StepOut capture_step(const StepIn& in) override { return analyze_step(view(), in); }
    void capture_stop() override { cap_on = false; }
    void on_note(const char*) override { ++notes; }
    void on_finish(Stop st) override { finished = true; stop = st; }
    // The item counter as the firmware logs it ("item <family> idx+1/total"): never past its total.
    int items_over = 0;
    std::array<int, kFamilies> item_last{}, item_total{};
    void on_item(Family f, std::uint8_t idx, std::uint8_t total) override {
        if (idx >= total) ++items_over;
        item_last[std::size_t(f)] = idx + 1;
        item_total[std::size_t(f)] = total;
    }
};

std::uint8_t bit(Family f) { return std::uint8_t(1u << std::uint8_t(f)); }

void typical(Sim& m) { m.list({fixed(5000), fixed(9000), fixed(15000), fixed(20000), pps(3300, 11000)}); }

// Runs until the engine ends (or limit ms); hook(t) runs before every poll. Checks the ceiling rule on the way.
template <class F>
std::uint32_t drive(Sim& m, Engine& e, std::uint32_t limit, F hook) {
    std::uint32_t t = m.t;
    while (e.running() && t < limit) {
        ++t;
        m.tick(t);
        hook(t);
        e.poll(t);
        if (std::uint32_t(m.max_req_mv) > m.cap) { CHECK_COND(false); break; }
    }
    return t;
}
std::uint32_t drive(Sim& m, Engine& e, std::uint32_t limit = 3600000) { return drive(m, e, limit, [](std::uint32_t) {}); }

// After any end: nothing silenced, the Rd on, the capture released, the restore asked for, the end reported once.
void check_safe_end(const Sim& m, const Engine& e) {
    CHECK_COND(!e.running());
    CHECK_COND(m.finished);
    CHECK_COND(m.sil == Silence::Off);
    CHECK_COND(!m.rd_is_off);
    CHECK_COND(!m.cap_on);
    CHECK_COND(m.releases >= 1);
    CHECK_COND(!e.release_unconfirmed());
    // At 5 V: a PDO 1 contract, or a source back at vSafe5V with no contract of ours yet (the engine's own end rule).
    const bool at5 = m.s.attached && m.v <= 5500 && m.v >= 800 && (m.s.pos == 1 || m.s.state == pd::SinkState::Idle);
    const bool dark = !m.s.attached && m.v < 800;
    CHECK_COND(at5 || dark);
    if (!(at5 || dark))
        std::printf("  end: attached=%d pos=%d state=%d v=%d stop=%s t=%u\n", int(m.s.attached), int(m.s.pos), int(m.s.state), int(m.v),
                    stop_name(e.stop()), unsigned(m.t));
}

Verdict verdict(const Engine& e, Check c) { return verdict(c, e.agg(c)); }

void clean_source_scores_zero() {
    Sim m;
    typical(m);
    Engine e(m);
    CHECK_COND(e.start(0) == StartResult::Ok);
    drive(m, e);
    check_safe_end(m, e);
    CHECK_COND(e.stop() == Stop::Done);
    CHECK_EQ(int(e.score()), 0);
    // 4 fixed levels: 12 transitions, each with its timings.
    CHECK_EQ(int(e.trans().size()), 12);
    CHECK_EQ(int(e.agg(Check::FallTime).n), 6);
    CHECK_EQ(int(e.agg(Check::RiseTime).n), 6);
    CHECK_EQ(int(e.agg(Check::PsRdyEarly).n), 6);
    CHECK_EQ(int(e.agg(Check::PsRdyLow).n), 6);
    CHECK_EQ(int(e.agg(Check::Accuracy).n), 12);
    // 5 -> 20 V: 30 ms dead time + 150 ms at 100 mV/ms; the band (19 V) 10 ms before the top, PS_RDY 10 ms after it.
    for (const TransRec& r : e.trans()) {
        if (r.from == 1 && r.to == 4) {
            CHECK_EQ(int(r.band_ms), 169);    // the ramp's first step 30 ms after the Accept, 19 V on the 140th
            CHECK_EQ(int(r.psrdy_ms), 189);
            CHECK_EQ(int(r.final_mv), 20000);
        }
    }
    CHECK_COND(verdict(e, Check::AcceptTime) == Verdict::Pass);
    CHECK_EQ(e.agg(Check::AcceptTime).worst, 5000);
    // PPS 3.3-11 V: four passes over the window (plan.hpp): 20 mV in three 10-step windows up and three down (60 moves), 100 mV (154),
    // 500 mV (32) and the two slams; every move measured and judged.
    CHECK_EQ(int(e.passes().size()), 4);
    CHECK_EQ(int(e.agg(Check::PpsAccuracy).n), 248);
    // The monotonic check skips the first point of every entry into a window that is not reached by a step of the pass (the pass's start
    // and the four setup moves between the 20 mV windows: that point is the first reference, not a comparison): 248 - 5.
    CHECK_EQ(int(e.agg(Check::PpsMonotonic).n), 243);
    CHECK_EQ(int(e.agg(Check::PpsPsRdy).n), 248);
    CHECK_EQ(int(e.agg(Check::PpsSettle).n), 60);
    CHECK_EQ(int(e.agg(Check::PpsBand).n), 188);
    CHECK_COND(verdict(e, Check::PpsMonotonic) == Verdict::Pass);
    CHECK_COND(verdict(e, Check::PpsBand) == Verdict::Pass);
    CHECK_COND(verdict(e, Check::PpsPsRdy) == Verdict::Pass);
    CHECK_COND(verdict(e, Check::PpsOvershoot) == Verdict::Pass);
    CHECK_COND(verdict(e, Check::PpsTransient) == Verdict::Pass);
    CHECK_COND(verdict(e, Check::PpsSettle) == Verdict::Unverified);   // under the meter's resolution: reported, never judged
    CHECK_EQ(int(e.agg(Check::PpsSettle).fails), 0);
    // Get_PPS_Status at the top and the end of every pass, clean, answered in 3 ms; never inside the keep-alive silence.
    CHECK_EQ(int(e.agg(Check::PpsStatus).n), 8);
    CHECK_COND(verdict(e, Check::PpsStatus) == Verdict::Pass);
    CHECK_EQ(e.agg(Check::PpsStatusTime).worst, 3000);
    CHECK_EQ(m.pps_asks, 8);
    CHECK_EQ(m.pps_asks_in_ka, 0);
    const PassRec& p0 = e.passes()[0];
    CHECK_EQ(int(p0.pos), 5);
    CHECK_EQ(int(p0.cls), 0);
    CHECK_EQ(int(p0.lo_mv), 3300);
    CHECK_EQ(int(p0.hi_mv), 11000);
    CHECK_EQ(int(p0.steps), 60);
    CHECK_EQ(int(e.passes()[3].cls), 3);
    CHECK_EQ(int(e.passes()[3].steps), 2);
    // A 3.3 -> 11 V slam: the ramp's first 100 mV 5 ms after the Accept, 11 V on its 77th, PS_RDY 10 ms after: 91 ms, inside
    // tPpsSrcTransLarge.
    CHECK_EQ(int(e.passes()[3].psrdy_100us), 910);
    // No AVS in the list: its checks say so, and the caps note is the advisory one.
    CHECK_COND(e.agg(Check::AvsBand).skip == Skip::NoAvs);
    CHECK_COND(e.caps_note() == CapsNote::PpsNoAvs);
    // The timers.
    CHECK_COND(verdict(e, Check::PpsKeepAlive) == Verdict::Pass);
    CHECK_COND(e.timers().pps_ka_ms >= 12900 && e.timers().pps_ka_ms <= 13100);
    CHECK_COND(verdict(e, Check::EprKeepAlive) == Verdict::Skip);
    CHECK_COND(e.agg(Check::EprKeepAlive).skip == Skip::NoEpr);
    for (const Check c : {Check::HrSafe0V, Check::HrRecover, Check::HrTurnOn, Check::HrCaps}) CHECK_COND(verdict(e, c) == Verdict::Pass);
    // The detach limits (Type-C R2.5): tSRCDisconnect + tVBUSOFF to vSafe0V; Rd back -> vSafe5V within tCCDebounce + tBulkDischarge +
    // tVBUSON (750 ms from 20 V), the capabilities within tFirstSourceCap of it. The model: 300 ms dark, 5 V 50 ms into the rise, the
    // capabilities 150 ms after the rise starts.
    for (const Check c : {Check::DetachSafe0V, Check::Reattach, Check::ReattachCaps}) CHECK_COND(verdict(e, c) == Verdict::Pass);
    CHECK_EQ(e.agg(Check::Reattach).hi, 750);
    CHECK_COND(e.agg(Check::Reattach).worst >= 340 && e.agg(Check::Reattach).worst <= 360);
    CHECK_COND(e.agg(Check::ReattachCaps).worst >= 90 && e.agg(Check::ReattachCaps).worst <= 110);
    // 20 V at 100 mV/ms after 25 ms: under 800 mV at 217 ms; 700 ms dark; 5 V at +43 ms, the capabilities 150 ms after the rise.
    CHECK_COND(e.timers().hr_off_ms >= 215 && e.timers().hr_off_ms <= 219);
    CHECK_COND(e.timers().hr_dark_ms >= 700 && e.timers().hr_dark_ms <= 720);
    // The bleeder: 20 V at 50 mV/ms after 20 ms: 404 ms.
    CHECK_COND(e.timers().det_off_ms >= 400 && e.timers().det_off_ms <= 410);
    CHECK_COND(e.timers().det_back_ms >= 470 && e.timers().det_back_ms <= 500);
    CHECK_COND(verdict(e, Check::SourceReset) == Verdict::Pass);
    CHECK_EQ(int(e.timers().poll_gap_ms), 1);
}

void unloaded_slow_fall_and_early_ps_rdy() {
    // No discharge: 10 mV/ms down, PS_RDY 100 ms after the Accept whatever VBUS does.
    Sim m;
    typical(m);
    m.fall = 10;
    m.early_psrdy_ms = 100;
    Engine e(m);
    CHECK_COND(e.start(0) == StartResult::Ok);
    drive(m, e);
    check_safe_end(m, e);
    CHECK_COND(e.stop() == Stop::Done);
    CHECK_COND(verdict(e, Check::FallTime) == Verdict::Fail);
    CHECK_COND(verdict(e, Check::PsRdyEarly) == Verdict::Fail);
    CHECK_EQ(int(e.agg(Check::PsRdyEarly).fails), 6);
    // Up-steps reach their band in time but PS_RDY at 100 ms comes before VBUS is there on 5 -> 15 / 20 V: PS_RDY low.
    CHECK_COND(verdict(e, Check::PsRdyLow) == Verdict::Fail);
    CHECK_COND(verdict(e, Check::RiseTime) == Verdict::Pass);
    // 20 -> 5 V: (20000 - 5250) / 10 = 1475 ms + 30 ms: past the 1.5 s window, followed live.
    bool live = false;
    for (const TransRec& r : e.trans())
        if (r.from == 4 && r.to == 1) {
            live = (r.flags & kTrLiveBand) != 0;
            CHECK_COND(r.band_ms >= 1500 && r.band_ms <= 1510);
        }
    CHECK_COND(live);
    CHECK_COND(e.score() > 0);
}

void pps_offset_and_slow_settle() {
    // 300 mV high (over 5 % of 5 V, inside 5 % of 7.94 V and 10.9 V) and 40 ms before a step starts to move.
    Sim m;
    typical(m);
    m.pps_offset_mv = 300;
    m.pps_start_ms = 40;
    Engine e(m);
    CHECK_COND(e.start(0) == StartResult::Ok);
    drive(m, e);
    check_safe_end(m, e);
    CHECK_COND(e.stop() == Stop::Done);
    // Under 6 V the 300 mV are outside vPpsNew: the level, settled where it is, fails its accuracy (and never enters the band).
    CHECK_COND(verdict(e, Check::PpsAccuracy) == Verdict::Fail);
    CHECK_COND(e.agg(Check::PpsAccuracy).fails > 0);
    CHECK_COND(e.passes()[0].acc_fails > 0);
    CHECK_EQ(int(e.passes()[0].worst_err_mv), 300);
    CHECK_COND(verdict(e, Check::PpsBand) == Verdict::Fail);           // 40 ms dead time: late for tPpsSrcTransSmall (verified)
    CHECK_COND(verdict(e, Check::PpsSettle) == Verdict::Unverified);   // slow, but the 20 mV settle is unverified: no fail, no penalty
    CHECK_COND(e.agg(Check::PpsSettle).fails > 0);                     // the count over it is kept
    CHECK_COND(verdict(e, Check::PpsMonotonic) == Verdict::Pass);
    CHECK_COND(verdict(e, Check::Accuracy) == Verdict::Pass);   // the fixed levels are not touched
}

void pps_fold_is_a_finding() {
    // Every request from 9.0 V up lands on 8.59 V (a fixed setpoint): the first step of each upward pass that reaches it is a fold
    // or a break, named once per pass; the same sweep without the fold is clean.
    Sim m;
    typical(m);
    m.pps_fold_from_mv = 9000;
    m.pps_fold_to_mv = 8590;
    Engine e(m);
    CHECK_COND(e.start(0) == StartResult::Ok);
    drive(m, e);
    check_safe_end(m, e);
    CHECK_COND(e.stop() == Stop::Done);
    CHECK_COND(verdict(e, Check::PpsTrack) == Verdict::Fail);
    CHECK_COND(e.agg(Check::PpsTrack).n > 0);
    CHECK_COND(e.agg(Check::PpsTrack).fails > 0);
    int flagged = 0, flagged_up = 0;
    for (const PassRec& r : e.passes()) {
        flagged += r.track;
        flagged_up += r.track && r.cls < 3;
    }
    CHECK_COND(flagged > 0);
    CHECK_EQ(flagged, flagged_up);
    CHECK_COND(e.agg(Check::PpsTrack).fails <= std::uint32_t(flagged));   // one per pass, never one per step of the plateau
    CHECK_COND(e.score() > 0);
    CHECK_COND(verdict(e, Check::AvsTrack) != Verdict::Fail);
    // The clean source: judged on every upward step, never failing.
    Sim c;
    typical(c);
    Engine ec(c);
    CHECK_COND(ec.start(0) == StartResult::Ok);
    drive(c, ec);
    CHECK_COND(ec.stop() == Stop::Done);
    CHECK_COND(ec.agg(Check::PpsTrack).n > 0);
    CHECK_EQ(int(ec.agg(Check::PpsTrack).fails), 0);
    CHECK_COND(verdict(ec, Check::PpsTrack) == Verdict::Pass);
    // The AVS sweep: the same rule under its own check.
    Sim a;
    a.list({fixed(5000), fixed(9000), fixed(15000), fixed(20000), avs(9000, 20000)});
    a.pps_fold_from_mv = 15000;
    a.pps_fold_to_mv = 14500;
    Engine ea(a);
    CHECK_COND(ea.start(0, bit(Family::Pps)) == StartResult::Ok);
    drive(a, ea);
    CHECK_COND(ea.stop() == Stop::Done);
    CHECK_COND(verdict(ea, Check::AvsTrack) == Verdict::Fail);
    CHECK_COND(ea.agg(Check::AvsTrack).fails > 0);
    CHECK_COND(verdict(ea, Check::PpsTrack) != Verdict::Fail);
}

void pps_window_entry_is_not_a_reference() {
    // The UGREEN Nexode Air (2026-11-15): a large jump (the entry of a 20 mV window, a jump between windows, a slam) lands 95 mV high,
    // inside vPpsValid, and holds there; every following small step tracks within 10 mV of its request (10.82 V -> 10.815 V). The
    // fall from the entry's landing to the first in-window step is the landing, not a fold, so neither the monotonic nor the track
    // check may judge it: the first in-window result is the first reference.
    Sim m;
    typical(m);
    m.pps_jump_high_mv = 95;
    m.pps_small_offset_mv = -5;
    Engine e(m);
    CHECK_COND(e.start(0) == StartResult::Ok);
    drive(m, e);
    check_safe_end(m, e);
    CHECK_COND(e.stop() == Stop::Done);
    CHECK_COND(e.agg(Check::PpsTrack).n > 0);
    CHECK_COND(e.agg(Check::PpsMonotonic).n > 0);
    CHECK_EQ(int(e.agg(Check::PpsTrack).fails), 0);
    CHECK_EQ(int(e.agg(Check::PpsMonotonic).fails), 0);
    CHECK_COND(verdict(e, Check::PpsTrack) == Verdict::Pass);
    CHECK_COND(verdict(e, Check::PpsMonotonic) == Verdict::Pass);
    for (const PassRec& r : e.passes()) {
        CHECK_EQ(int(r.track), 0);
        CHECK_COND(r.min_delta_mv >= -8);
    }
}

void pps_fold_inside_a_window_is_still_a_finding() {
    // The FH22 shape at 20 mV: two points of one window (10.92 V and 10.94 V of the top window), the second folding back, is judged by
    // both checks even with the large-jump landing of the same source (the window entry is skipped, the in-window points are not).
    Sim m;
    typical(m);
    m.pps_jump_high_mv = 95;
    m.pps_fold_from_mv = 10940;
    m.pps_fold_to_mv = 10500;
    Engine e(m);
    CHECK_COND(e.start(0) == StartResult::Ok);
    drive(m, e);
    CHECK_COND(e.stop() == Stop::Done);
    CHECK_EQ(int(e.passes()[0].cls), 0);
    CHECK_EQ(int(e.passes()[0].track), 1);
    CHECK_COND(e.passes()[0].mono_fails > 0);
    CHECK_COND(verdict(e, Check::PpsTrack) == Verdict::Fail);
    CHECK_COND(verdict(e, Check::PpsMonotonic) == Verdict::Fail);
}

void epr_and_pps_on_one_charger() {
    // EPR stays on at 5 V after the walk (the restore is a Request, not a release): the PPS timeout is timed with the EPR_KeepAlive
    // still running, and the EPR timeout from the sink's own keep-alive stamp.
    Sim m;
    m.list({fixed(5000, true), fixed(9000), fixed(15000), fixed(20000), pps(3300, 11000)});
    m.c.pdos[7] = fixed(28000);
    m.c.num_pdos = 8;
    m.proof = true;
    Engine e(m);
    CHECK_COND(e.start(0) == StartResult::Ok);
    drive(m, e);
    check_safe_end(m, e);
    CHECK_COND(e.stop() == Stop::Done);
    CHECK_EQ(int(e.score()), 0);
    CHECK_COND(e.timers().pps_ka_ms >= 12900 && e.timers().pps_ka_ms <= 13100);
    CHECK_COND(e.timers().epr_ka_ms >= 899 && e.timers().epr_ka_ms <= 903);
    CHECK_COND(!m.s.epr_mode);   // the final release left EPR
}

void source_back_at_its_old_level() {
    // After each reset the sink re-asks the level it had (20 V after the Hard Reset): the engine brings it to 5 V before going on.
    Sim m;
    typical(m);
    m.back_old = true;
    Engine e(m);
    CHECK_COND(e.start(0) == StartResult::Ok);
    drive(m, e);
    check_safe_end(m, e);
    CHECK_COND(e.stop() == Stop::Done);
    CHECK_EQ(int(e.score()), 0);
    CHECK_COND(verdict(e, Check::Reattach) == Verdict::Pass);
}

void capture_busy_falls_back_to_stamps_and_polling() {
    Sim m;
    typical(m);
    m.cap_busy = true;
    Engine e(m);
    CHECK_COND(e.start(0) == StartResult::Ok);
    drive(m, e);
    check_safe_end(m, e);
    CHECK_COND(e.stop() == Stop::Done);
    CHECK_EQ(int(e.score()), 0);
    for (const Check c : {Check::SrcStart, Check::PsRdyEarly, Check::PsRdyLow, Check::Overshoot, Check::PpsAccuracy})
        CHECK_COND(e.agg(c).skip == Skip::NoCapture);
    CHECK_EQ(int(e.agg(Check::AcceptTime).n), 12 + 248);
    CHECK_EQ(int(e.agg(Check::RiseTime).n), 6);
    CHECK_EQ(int(e.agg(Check::FallTime).n), 6);
    for (const TransRec& r : e.trans()) CHECK_COND((r.flags & kTrNoCapture) && (r.flags & kTrLiveBand));
}

void wait_exhausted_is_a_refusal() {
    Sim m;
    typical(m);
    m.wait_pos = 3;
    Engine e(m);
    CHECK_COND(e.start(0) == StartResult::Ok);
    drive(m, e);
    check_safe_end(m, e);
    CHECK_COND(e.stop() == Stop::Done);
    CHECK_EQ(int(e.trans().size()), 7);
}

void missing_ps_rdy_is_a_measurement() {
    // 15 V Accepted, no PS_RDY: the sink's own Hard Reset after tPSTransition. PS_RDY time fails ("none"), the walk ends there and
    // the run goes on with the next family.
    Sim m;
    typical(m);
    m.no_psrdy_pos = 3;
    Engine e(m);
    CHECK_COND(e.start(0) == StartResult::Ok);
    drive(m, e);
    check_safe_end(m, e);
    CHECK_COND(e.stop() == Stop::Done);
    CHECK_COND(verdict(e, Check::PsRdyTime) == Verdict::Fail);
    CHECK_EQ(e.agg(Check::PsRdyTime).worst, kNever);
    CHECK_COND(verdict(e, Check::SourceReset) == Verdict::Pass);   // the K2 reset it, not the source
    CHECK_COND(verdict(e, Check::PpsKeepAlive) == Verdict::Pass);
}

void stuck_request_stops() {
    Sim m;
    typical(m);
    m.stuck_pos = 2;
    Engine e(m);
    CHECK_COND(e.start(0) == StartResult::Ok);
    drive(m, e);
    CHECK_COND(e.stop() == Stop::Stuck);
    CHECK_COND(m.releases >= 1 && m.sil == Silence::Off && !m.cap_on);
    CHECK_COND(e.release_unconfirmed());   // the model never leaves Requesting: the end says so
}

void jittered_polls_still_score_zero() {
    // The main loop is not 1 ms on the device: poll at 1..20 ms gaps (a fixed pseudo-random sequence).
    Sim m;
    typical(m);
    Engine e(m);
    CHECK_COND(e.start(0) == StartResult::Ok);
    std::uint32_t lcg = 12345, next = 1, t = 0;
    while (e.running() && t < 3600000) {
        ++t;
        m.tick(t);
        if (t >= next) {
            e.poll(t);
            lcg = lcg * 1103515245u + 12345u;
            next = t + 1 + (lcg >> 16) % 20;
        }
    }
    check_safe_end(m, e);
    CHECK_COND(e.stop() == Stop::Done);
    CHECK_EQ(int(e.score()), 0);
    CHECK_COND(e.timers().poll_gap_ms >= 15 && e.timers().poll_gap_ms <= 20);
}

void refused_level_is_left_out() {
    Sim m;
    typical(m);
    m.reject_pos = 3;   // 15 V
    Engine e(m);
    CHECK_COND(e.start(0) == StartResult::Ok);
    drive(m, e);
    check_safe_end(m, e);
    CHECK_COND(e.stop() == Stop::Done);
    // 6 pairs without 15 V measured, the first 15 V pair recorded as refused, the rest left out.
    CHECK_EQ(int(e.trans().size()), 7);
    int refused = 0;
    for (const TransRec& r : e.trans()) {
        if (r.flags & kTrRefused) ++refused;
        else CHECK_COND(r.from != 3 && r.to != 3);
    }
    CHECK_EQ(refused, 1);
    CHECK_EQ(int(e.score()), 0);
}

// The item log of every family ends at its total ("Transitions 12/12", "PPS steps 3/3"), never one past it, also when refused levels
// leave the walk's last pairs out.
void item_counter_never_passes_its_total() {
    for (const std::uint8_t reject : {std::uint8_t(0), std::uint8_t(4)}) {
        Sim m;
        typical(m);
        m.reject_pos = reject;   // 0: none; 4: 20 V, the walk's last pairs skipped
        Engine e(m);
        CHECK_COND(e.start(0) == StartResult::Ok);
        drive(m, e);
        check_safe_end(m, e);
        CHECK_EQ(m.items_over, 0);
        CHECK_EQ(m.item_total[std::size_t(Family::Transitions)], 12);
        CHECK_EQ(m.item_last[std::size_t(Family::Transitions)], 12);
        CHECK_EQ(m.item_total[std::size_t(Family::Pps)], 4);   // one APDO window, four passes
        CHECK_EQ(m.item_last[std::size_t(Family::Pps)], 4);
    }
}

void ceiling_keeps_levels_out() {
    Sim m;
    typical(m);
    m.cap = 15000;
    Engine e(m);
    CHECK_COND(e.start(0) == StartResult::Ok);
    drive(m, e);
    CHECK_COND(e.stop() == Stop::Done);
    CHECK_COND(m.max_req_mv <= 15000);
    CHECK_EQ(int(e.trans().size()), 6);
}

void epr_level_and_keepalive() {
    Sim m;
    m.list({fixed(5000, true), fixed(9000), fixed(15000), fixed(20000)});
    m.c.pdos[7] = fixed(28000);
    m.c.pdos[8] = fixed(48000);
    m.c.num_pdos = 9;
    {
        Engine e(m);
        CHECK_COND(e.start(0) == StartResult::Ok);
        drive(m, e);
        CHECK_COND(e.stop() == Stop::Done);
        CHECK_COND(e.agg(Check::EprKeepAlive).skip == Skip::NoEprProof);
        CHECK_COND(e.agg(Check::PpsKeepAlive).skip == Skip::NoPps);
        CHECK_EQ(int(e.trans().size()), 12);
    }
    Sim p;
    p.c = m.c;
    p.proof = true;
    Engine e(p);
    CHECK_COND(e.start(0) == StartResult::Ok);
    drive(p, e);
    check_safe_end(p, e);
    CHECK_COND(e.stop() == Stop::Done);
    CHECK_COND(p.max_req_mv == 28000);   // 48 V is above the ceiling: never asked
    CHECK_EQ(int(e.trans().size()), 20);  // 5 levels
    CHECK_COND(verdict(e, Check::EprKeepAlive) == Verdict::Pass);
    // The source times out 900 ms after the last EPR_KeepAlive (the sink's stamp).
    CHECK_COND(e.timers().epr_ka_ms >= 899 && e.timers().epr_ka_ms <= 903);
    CHECK_EQ(int(e.score()), 0);
}

void pps_keepalive_never_times_out() {
    Sim m;
    typical(m);
    m.pps_timeout_ms = 0;
    Engine e(m);
    CHECK_COND(e.start(0) == StartResult::Ok);
    drive(m, e);
    check_safe_end(m, e);
    CHECK_COND(e.stop() == Stop::Done);
    CHECK_COND(verdict(e, Check::PpsKeepAlive) == Verdict::Fail);
    CHECK_EQ(int(e.score()), int(def(Check::PpsKeepAlive).weight));
}

void source_reset_mid_walk_is_counted() {
    Sim m;
    typical(m);
    m.src_hr_at = 1500;
    Engine e(m);
    CHECK_COND(e.start(0) == StartResult::Ok);
    drive(m, e);
    check_safe_end(m, e);
    CHECK_COND(e.stop() == Stop::Done);
    CHECK_COND(verdict(e, Check::SourceReset) == Verdict::Fail);
    CHECK_EQ(e.agg(Check::SourceReset).worst, 1);
    // The walk stopped there; the later families still ran.
    CHECK_COND(e.trans().size() < 12);
    CHECK_COND(verdict(e, Check::PpsKeepAlive) == Verdict::Pass);
    CHECK_COND(verdict(e, Check::HrSafe0V) == Verdict::Pass);
}

void stops() {
    {   // VBUS far above the level in flight: overvoltage stop.
        Sim m;
        typical(m);
        m.ov_at = 400;
        Engine e(m);
        CHECK_COND(e.start(0) == StartResult::Ok);
        drive(m, e, 60000);
        CHECK_COND(e.stop() == Stop::Overvoltage);
        CHECK_COND(m.finished && m.sil == Silence::Off && !m.rd_is_off && !m.cap_on && m.releases >= 1);
    }
    {   // A load on the output.
        Sim m;
        typical(m);
        m.load_at = 700;
        Engine e(m);
        CHECK_COND(e.start(0) == StartResult::Ok);
        drive(m, e, 60000);
        CHECK_COND(e.stop() == Stop::Load);
        CHECK_COND(m.releases >= 1 && !m.cap_on);
    }
    {   // The source never comes back after the K2's Hard Reset (the PPS timeout's reset before it still recovers).
        Sim m;
        typical(m);
        Engine e(m);
        CHECK_COND(e.start(0) == StartResult::Ok);
        drive(m, e, 3600000, [&](std::uint32_t) { if (e.family() == Family::HardReset) m.never_recover = true; });
        check_safe_end(m, e);
        CHECK_COND(e.stop() == Stop::NoRecovery);
        CHECK_COND(verdict(e, Check::HrRecover) == Verdict::Fail);
        CHECK_COND(e.agg(Check::DetachSafe0V).skip == Skip::NotReached);
    }
    {   // An inline link appears mid-run.
        Sim m;
        typical(m);
        Engine e(m);
        CHECK_COND(e.start(0) == StartResult::Ok);
        drive(m, e, 3600000, [&](std::uint32_t t) { if (t == 900) m.inl = true; });
        CHECK_COND(e.stop() == Stop::Inline);
    }
}

void family_selection() {
    // Only the Hard Reset and the detach: no Request above 5 V is ever made, the rest reads "not selected".
    Sim m;
    typical(m);
    Engine e(m);
    const auto bit = [](Family f) { return std::uint8_t(1u << std::uint8_t(f)); };
    CHECK_COND(e.start(0, bit(Family::HardReset) | bit(Family::Detach)) == StartResult::Ok);
    drive(m, e);
    check_safe_end(m, e);
    CHECK_COND(e.stop() == Stop::Done);
    CHECK_EQ(int(e.trans().size()), 0);
    CHECK_COND(e.agg(Check::AcceptTime).skip == Skip::NotSelected);
    CHECK_COND(e.agg(Check::PpsKeepAlive).skip == Skip::NotSelected);
    CHECK_COND(verdict(e, Check::HrSafe0V) == Verdict::Pass);
    CHECK_COND(verdict(e, Check::DetachSafe0V) == Verdict::Pass);
    CHECK_EQ(m.max_req_mv, 20000);   // the top fixed level the reset is made from
}

void start_refusals() {
    Sim a;
    typical(a);
    a.self = true;
    CHECK_COND(Engine(a).start(0) == StartResult::SelfPowered);
    Sim b;
    typical(b);
    b.inl = true;
    CHECK_COND(Engine(b).start(0) == StartResult::Inline);
    Sim c;
    CHECK_COND(Engine(c).start(0) == StartResult::NoCaps);
    Sim d;
    typical(d);
    Engine e(d);
    CHECK_COND(e.start(0) == StartResult::Ok);
    CHECK_COND(e.start(1) == StartResult::Running);
}

void cancel_anywhere_ends_safe() {
    // Cancel every 700 ms of a clean run, each time on a fresh run.
    Sim probe;
    typical(probe);
    Engine pe(probe);
    pe.start(0);
    const std::uint32_t len = drive(probe, pe);
    int checked = 0;
    for (std::uint32_t at = 50; at < len; at += 700) {
        Sim m;
        typical(m);
        Engine e(m);
        e.start(0);
        drive(m, e, 3600000, [&](std::uint32_t t) { if (t == at) e.cancel(); });
        check_safe_end(m, e);
        CHECK_COND(e.stop() == Stop::Cancelled);
        ++checked;
    }
    CHECK_COND(checked > 50);
    // The short windows a stride can step over: the detach gap, the keep-alive silences, the Hard Reset's dark time.
    enum class Where { RdOff, PpsSilence, EprSilence, Dark };
    for (const Where w : {Where::RdOff, Where::PpsSilence, Where::EprSilence, Where::Dark}) {
        for (const std::uint32_t d : {0u, 50u, 300u, 3000u}) {
            if (w == Where::RdOff && d == 3000) continue;   // the detach is the last family: the run is over by then
            Sim m;
            m.list({fixed(5000, true), fixed(9000), fixed(20000), pps(3300, 11000)});
            m.c.pdos[7] = fixed(28000);
            m.c.num_pdos = 8;
            m.proof = true;
            Engine e(m);
            e.start(0);
            std::uint32_t seen = 0;
            drive(m, e, 3600000, [&](std::uint32_t t) {
                const bool in = w == Where::RdOff ? m.rd_is_off
                              : w == Where::PpsSilence ? m.sil == Silence::Pps
                              : w == Where::EprSilence ? m.sil == Silence::Epr
                              : m.rs == Sim::Rs::Dark;
                if (in && !seen) seen = t;
                if (seen && t == seen + d) e.cancel();
            });
            CHECK_COND(seen != 0);
            check_safe_end(m, e);
            CHECK_COND(e.stop() == Stop::Cancelled);
            if (e.stop() != Stop::Cancelled) std::printf("  where=%d d=%u stop=%s\n", int(w), unsigned(d), stop_name(e.stop()));
        }
    }
}

// A source that drains slowly after the detach and comes back late: both are over the Type-C limits (tSRCDisconnect + tVBUSOFF, 670 ms;
// tCCDebounce + tBulkDischarge + tVBUSON, 750 ms from 20 V): two verified fails, their weights in the score.
void slow_detach_fails_the_type_c_limits() {
    Sim m;
    typical(m);
    m.bleed = 20;          // 20 V at 20 mV/ms: about 960 ms to vSafe0V
    m.reattach_ms = 6000;
    Engine e(m);
    CHECK_COND(e.start(0) == StartResult::Ok);
    drive(m, e);
    check_safe_end(m, e);
    CHECK_COND(e.stop() == Stop::Done);
    CHECK_COND(e.timers().det_off_ms >= 900);
    CHECK_COND(e.agg(Check::DetachSafe0V).fails == 1);
    CHECK_COND(e.agg(Check::Reattach).fails == 1);
    CHECK_COND(verdict(e, Check::DetachSafe0V) == Verdict::Fail);
    CHECK_COND(verdict(e, Check::Reattach) == Verdict::Fail);
    CHECK_COND(verdict(e, Check::ReattachCaps) == Verdict::Pass);
    CHECK_EQ(int(e.score()), int(def(Check::DetachSafe0V).weight + def(Check::Reattach).weight));
}

// Get_PPS_Status answers: a refusal (it is required of a PPS source), silence, OMF set at no load, a voltage 10 % off. Each is a verified
// fail with its fault bit; the answer time is timed only when an answer came.
void pps_status_answers() {
    const struct { int mode; std::int32_t bits; } cases[] = {{1, kPsNotSupported}, {2, kPsNoAnswer}, {3, kPsOmf}, {4, kPsVoltage}};
    for (const auto& k : cases) {
        Sim m;
        m.list({fixed(5000), pps(3300, 5900)});
        m.pps_status_mode = k.mode;
        Engine e(m);
        CHECK_COND(e.start(0, bit(Family::Pps)) == StartResult::Ok);
        drive(m, e);
        check_safe_end(m, e);
        CHECK_COND(e.stop() == Stop::Done);
        CHECK_COND(verdict(e, Check::PpsStatus) == Verdict::Fail);
        CHECK_EQ(e.agg(Check::PpsStatus).worst, k.bits);
        CHECK_EQ(int(e.agg(Check::PpsStatus).n), 8);
        CHECK_EQ(int(e.agg(Check::PpsStatus).fails), 8);
        CHECK_EQ(int(e.passes()[0].status), int(k.bits));
        CHECK_EQ(int(e.agg(Check::PpsStatusTime).n), k.mode == 2 ? 0 : 8);
        CHECK_EQ(int(e.score()), 8 * int(def(Check::PpsStatus).weight));
    }
    // The pure judgement: 0xFFFF / 0xFF are "not supported" fields, PTF 1 is normal, +-3 % rounded to 20 mV.
    CHECK_EQ(pps_status_faults(pd::PpsAsk::Got, 0xFFFF, 0xFF, 0x02, 5000), 0);
    CHECK_EQ(pps_status_faults(pd::PpsAsk::Got, 5160 / 20, 3, 0x00, 5000), 0);       // +160 mV: 3 % of 5 V is 150, rounded to 160
    CHECK_EQ(pps_status_faults(pd::PpsAsk::Got, 5180 / 20, 4, 0x01, 5000), kPsVoltage | kPsCurrent | kPsFlags);
    CHECK_EQ(pps_status_faults(pd::PpsAsk::Got, 250, 0, 0x06, 5000), kPsFlags);       // PTF 3
}

// A step up that overshoots its level by 300 mV for 10 ms: beyond vPpsNew + vPpsValid on the low levels (5 % of 3.3 V is 165 mV),
// inside it above 8 V; back inside vPpsNew well within tPpsTransient.
void pps_overshoot_fails_low_levels() {
    Sim m;
    m.list({fixed(5000), pps(3300, 11000)});
    m.pps_overshoot_mv = 300;
    m.pps_overshoot_ms = 10;
    Engine e(m);
    CHECK_COND(e.start(0, bit(Family::Pps)) == StartResult::Ok);
    drive(m, e);
    check_safe_end(m, e);
    CHECK_COND(e.stop() == Stop::Done);
    CHECK_COND(verdict(e, Check::PpsOvershoot) == Verdict::Fail);
    CHECK_COND(e.agg(Check::PpsOvershoot).fails > 0);
    CHECK_COND(verdict(e, Check::PpsTransient) == Verdict::Pass);
    CHECK_COND(e.agg(Check::PpsTransient).worst > 0);
    CHECK_COND(verdict(e, Check::PpsAccuracy) == Verdict::Pass);
}

// An unloaded source that falls at 20 mV/ms: the down slam (7.7 V) takes 390 ms, past tPpsSrcTransLarge, and a 500 mV step down
// 30 ms, past tPpsSrcTransSmall; the run waits for each level before the next move, so the later moves stay clean.
void slow_pps_fall_fails_the_band() {
    Sim m;
    m.list({fixed(5000), pps(3300, 11000)});
    m.fall = 20;
    Engine e(m);
    CHECK_COND(e.start(0, bit(Family::Pps)) == StartResult::Ok);
    drive(m, e);
    check_safe_end(m, e);
    CHECK_COND(e.stop() == Stop::Done);
    CHECK_COND(verdict(e, Check::PpsBand) == Verdict::Fail);
    CHECK_COND(verdict(e, Check::PpsMonotonic) == Verdict::Pass);
    CHECK_COND(verdict(e, Check::PpsAccuracy) == Verdict::Pass);
    const PassRec& slam = e.passes()[3];
    CHECK_EQ(int(slam.band_fails), 1);                   // the way down: 7.535 V to the top of vPpsNew (3465 mV), 377 ms + 5
    CHECK_COND(slam.band_100us >= 3800 && slam.band_100us <= 3850);
    // A 500 mV step down enters vPpsNew (+-5 %) after at most 335 mV of the fall (22 ms): in band in time, but PS_RDY comes 10 ms after
    // the level, 40 ms after the Accept: the PS_RDY check is the one that tells (the 200 mV last step is 25 ms, on the limit).
    CHECK_EQ(int(e.passes()[2].band_fails), 0);
    CHECK_EQ(int(e.passes()[2].psrdy_fails), 15);
    CHECK_EQ(int(e.passes()[1].band_fails), 0);          // 100 mV down: 10 ms
    CHECK_COND(verdict(e, Check::PpsPsRdy) == Verdict::Fail);
}

// An SPR AVS APDO beside the PPS one: the AVS window (9-20 V) is swept at 100 / 500 / 1000 mV and slammed, against tAvsSrcTransSmall /
// the large-step 325 ms, vAvsNew and vAvsValid; no Get_PPS_Status for it. The caps note says both kinds are there.
// PS_RDY is stamped when its frame has been received, so the reading carries the frame's own wire time: a small PPS step's PS_RDY
// is judged against tPpsSrcTransSmall (25 ms) plus the 2 ms wire allowance (pd_spec_timing.hpp kPsRdyWireOverheadMs). 26 ms after
// the Accept passes, 28 ms fails on every small step; the slams (large steps, 275 + 2 ms) pass both.
void pps_ps_rdy_wire_allowance() {
    for (const int at : {26, 28}) {
        Sim m;
        m.list({fixed(5000), pps(3300, 11000)});
        m.early_psrdy_ms = at;
        Engine e(m);
        CHECK_COND(e.start(0, bit(Family::Pps)) == StartResult::Ok);
        drive(m, e);
        check_safe_end(m, e);
        CHECK_COND(e.stop() == Stop::Done);
        CHECK_EQ(int(e.agg(Check::PpsPsRdy).hi), 27000);    // the worst is a small step's: 25 ms + 2
        CHECK_EQ(int(e.passes()[3].psrdy_fails), 0);
        if (at == 26) {
            CHECK_COND(verdict(e, Check::PpsPsRdy) == Verdict::Pass);
            CHECK_EQ(int(e.passes()[1].psrdy_fails), 0);
        } else {
            CHECK_COND(verdict(e, Check::PpsPsRdy) == Verdict::Fail);
            CHECK_EQ(int(e.passes()[1].psrdy_fails), int(e.passes()[1].steps));
            CHECK_EQ(int(e.agg(Check::PpsPsRdy).worst), 28000);
        }
    }
}

void avs_sweep_and_caps_note() {
    Sim m;
    m.list({fixed(5000), fixed(9000), fixed(15000), fixed(20000), pps(3300, 11000), avs(9000, 20000)});
    Engine e(m);
    CHECK_COND(e.start(0, bit(Family::Pps)) == StartResult::Ok);
    drive(m, e);
    check_safe_end(m, e);
    CHECK_COND(e.stop() == Stop::Done);
    CHECK_EQ(int(e.score()), 0);
    CHECK_COND(e.caps_note() == CapsNote::PpsAvs);
    CHECK_EQ(int(e.passes().size()), 8);
    CHECK_EQ(int(e.passes()[4].cls), int(kPassAvs));
    CHECK_EQ(int(e.passes()[4].pos), 6);
    CHECK_EQ(int(e.agg(Check::AvsAccuracy).n), 220 + 44 + 22 + 2);
    CHECK_EQ(int(e.agg(Check::AvsBand).n), 220 + 44 + 22 + 2);
    for (const Check c : {Check::AvsMonotonic, Check::AvsAccuracy, Check::AvsBand, Check::AvsPsRdy, Check::AvsOvershoot, Check::AvsTransient})
        CHECK_COND(verdict(e, c) == Verdict::Pass);
    CHECK_EQ(int(e.agg(Check::PpsStatus).n), 8);         // the PPS passes only
    CHECK_EQ(int(e.agg(Check::PpsAccuracy).n), 248);
    // An AVS-only list: the PPS checks say why they are empty.
    Sim m2;
    m2.list({fixed(5000), fixed(9000), avs(9000, 15000)});
    Engine e2(m2);
    CHECK_COND(e2.start(0, bit(Family::Pps)) == StartResult::Ok);
    drive(m2, e2);
    CHECK_COND(e2.caps_note() == CapsNote::AvsOnly);
    CHECK_COND(e2.agg(Check::PpsBand).skip == Skip::NoPps);
    CHECK_COND(e2.agg(Check::PpsStatus).skip == Skip::NoPps);
    CHECK_COND(verdict(e2, Check::AvsBand) == Verdict::Pass);
    CHECK_EQ(m2.pps_asks, 0);
}

// The full 20 mV option sweeps the whole window at 20 mV (385 up, 385 down) instead of three 10-step windows each way.
void full_20mv_sweep_option() {
    Sim m;
    m.list({fixed(5000), pps(3300, 11000)});
    Engine e(m);
    CHECK_COND(e.start(0, bit(Family::Pps), kOptFull20) == StartResult::Ok);
    drive(m, e);
    check_safe_end(m, e);
    CHECK_COND(e.stop() == Stop::Done);
    CHECK_EQ(int(e.opts()), int(kOptFull20));
    CHECK_EQ(int(e.agg(Check::PpsSettle).n), 770);
    CHECK_EQ(int(e.passes()[0].steps), 770);
    CHECK_COND(verdict(e, Check::PpsMonotonic) == Verdict::Pass);
}

// The per-transition rows the Deep result lists (spec_rows.hpp) are the engine's own judgements, one per measurement: a fixed check has
// a row per transition that measured it, a programmable check one per pass; the failing ones add up to the aggregate's count.
void rows_follow_the_aggregates_one_run(Sim& m, std::uint8_t families, bool expect_fail) {
    Engine e(m);
    CHECK_COND(e.start(0, families) == StartResult::Ok);
    drive(m, e);
    check_safe_end(m, e);
    CHECK_COND(e.stop() == Stop::Done);
    std::size_t listed = 0, failing_rows = 0;
    for (std::size_t i = 0; i < specsec::kCells; ++i) {
        const Check c = specsec::kOrder[i];
        const Agg& a = e.agg(c);
        const std::size_t n = specsec::row_count(e, i);
        if (!specsec::has_list(i)) { CHECK_EQ(int(n), 0); continue; }
        std::size_t bad = 0, fixed_rows = 0, prog_rows = 0;
        for (std::size_t k = 0; k < n; ++k) {
            specsec::Row r;
            CHECK_COND(specsec::row_at(e, i, k, r));
            CHECK_EQ(int(r.check), int(c));
            if (r.flags & specsec::kRowProg) { ++prog_rows; bad += std::size_t(r.margin); }
            else { ++fixed_rows; if (r.flags & specsec::kRowFail) ++bad; }
            if (r.flags & specsec::kRowFail) ++failing_rows;
        }
        CHECK_COND(fixed_rows == 0 || prog_rows == 0);   // a check lists one kind of row
        if (c == Check::AcceptTime) {
            // The aggregate also counts the programmable steps' Accepts: the rows are the fixed transitions'.
            CHECK_EQ(int(n), int(e.trans().size()));
            CHECK_COND(bad <= a.fails);
        } else if (fixed_rows) {
            CHECK_EQ(int(bad), int(a.fails));
            CHECK_COND(n <= a.n);   // every row has a measurement behind it
        } else if (prog_rows) {
            CHECK_EQ(int(bad), int(a.fails));
            CHECK_COND(n <= e.passes().size());
        }
        listed += n;
        // The worst row: a failing one when any failed.
        if (n) {
            const std::size_t w = specsec::worst_row(e, i);
            CHECK_COND(w < n);
            specsec::Row r;
            CHECK_COND(specsec::row_at(e, i, w, r));
            if (a.fails) CHECK_COND((r.flags & specsec::kRowFail) != 0);
        }
    }
    CHECK_COND(listed > 0);
    if (expect_fail) CHECK_COND(failing_rows > 0); else CHECK_EQ(int(failing_rows), 0);
    // The file the journal writes: the header, then the rows in cell order, read back in 64-byte pieces.
    const std::size_t size = specsec::rows_file_size(e);
    CHECK_EQ(int(size), int(specsec::kRowsHdr + listed * specsec::kRowBytes));
    std::array<std::uint8_t, 6144> file{};
    CHECK_COND(size <= file.size());
    for (std::size_t off = 0; off < size; off += 64)
        specsec::rows_fill(e, 0xC0FFEEu, off, file.data() + off, std::min<std::size_t>(64, size - off));
    specsec::RowsIndex ix;
    CHECK_COND(specsec::rows_parse(file.data(), size, 0xC0FFEEu, ix));
    CHECK_EQ(int(ix.total()), int(listed));
    for (std::size_t i = 0; i < specsec::kCells; ++i) {
        CHECK_EQ(int(ix.count(i)), int(specsec::row_count(e, i)));
        if (ix.count(i)) CHECK_EQ(int(ix.worst(i)), int(specsec::worst_row(e, i)));
        for (std::size_t k = 0; k < ix.count(i); ++k) {
            specsec::Row want, got;
            CHECK_COND(specsec::row_at(e, i, k, want));
            specsec::row_decode(file.data() + ix.offset(i, k), got);
            CHECK_COND(got == want);
        }
    }
}

void rows_follow_the_aggregates() {
    {   // a clean run: every row passes
        Sim m;
        typical(m);
        rows_follow_the_aggregates_one_run(m, 0, false);
    }
    {   // an unloaded source: slow falls and a PS_RDY that comes early
        Sim m;
        typical(m);
        m.fall = 10;
        m.early_psrdy_ms = 100;
        rows_follow_the_aggregates_one_run(m, bit(Family::Transitions), true);
    }
    {   // a PPS source that overshoots the low levels
        Sim m;
        m.list({fixed(5000), pps(3300, 11000)});
        m.pps_overshoot_mv = 300;
        m.pps_overshoot_ms = 10;
        rows_follow_the_aggregates_one_run(m, bit(Family::Pps), true);
    }
    {   // a PPS source that falls slowly: the band fails
        Sim m;
        m.list({fixed(5000), pps(3300, 11000)});
        m.fall = 20;
        rows_follow_the_aggregates_one_run(m, bit(Family::Pps), true);
    }
    {   // PPS and AVS together
        Sim m;
        m.list({fixed(5000), fixed(9000), fixed(15000), fixed(20000), pps(3300, 11000), avs(9000, 20000)});
        rows_follow_the_aggregates_one_run(m, bit(Family::Pps), false);
    }
}

// What a row says: the transition in volts and the value and margin the check judged.
void rows_name_the_transition() {
    Sim m;
    typical(m);
    m.fall = 10;
    m.early_psrdy_ms = 100;
    Engine e(m);
    CHECK_COND(e.start(0, bit(Family::Transitions)) == StartResult::Ok);
    drive(m, e);
    check_safe_end(m, e);
    std::size_t cell = 0;
    for (std::size_t i = 0; i < specsec::kCells; ++i) if (specsec::kOrder[i] == Check::PsRdyEarly) cell = i;
    bool seen_20_5 = false;
    for (std::size_t k = 0; k < specsec::row_count(e, cell); ++k) {
        specsec::Row r;
        CHECK_COND(specsec::row_at(e, cell, k, r));
        CHECK_COND(r.flags & specsec::kRowFail);                  // every down-step fails the check here
        CHECK_COND(r.a > r.b);                                    // a down-step: from above to below
        CHECK_COND(r.margin > 0);                                 // positive = past the limit
        if (r.a == 20000 && r.b == 5000) {
            seen_20_5 = true;
            CHECK_COND(r.value > 1000);                           // VBUS at PS_RDY, in 10 mV
        }
    }
    CHECK_COND(seen_20_5);
}

// A record the pool refused is only counted: the rows are what the pool kept, never more than the aggregates counted, and the file
// stays inside its cap.
void rows_never_outnumber_the_measurements() {
    Sim m;
    m.list({fixed(5000), fixed(9000), fixed(12000), fixed(15000), fixed(20000), fixed(28000, true), pps(3300, 11000)});
    m.cap = 36000;
    Engine e(m);
    CHECK_COND(e.start(0) == StartResult::Ok);
    drive(m, e);
    check_safe_end(m, e);
    for (std::size_t i = 0; i < specsec::kCells; ++i) CHECK_COND(specsec::row_count(e, i) <= e.agg(specsec::kOrder[i]).n);
    CHECK_COND(specsec::rows_file_size(e) <= specsec::kRowsMaxFile);
}

}  // namespace

int main() {
    rows_follow_the_aggregates();
    rows_name_the_transition();
    rows_never_outnumber_the_measurements();
    clean_source_scores_zero();
    unloaded_slow_fall_and_early_ps_rdy();
    pps_offset_and_slow_settle();
    pps_fold_is_a_finding();
    pps_window_entry_is_not_a_reference();
    pps_fold_inside_a_window_is_still_a_finding();
    epr_and_pps_on_one_charger();
    source_back_at_its_old_level();
    capture_busy_falls_back_to_stamps_and_polling();
    wait_exhausted_is_a_refusal();
    missing_ps_rdy_is_a_measurement();
    stuck_request_stops();
    jittered_polls_still_score_zero();
    refused_level_is_left_out();
    ceiling_keeps_levels_out();
    epr_level_and_keepalive();
    pps_keepalive_never_times_out();
    source_reset_mid_walk_is_counted();
    stops();
    family_selection();
    start_refusals();
    cancel_anywhere_ends_safe();
    slow_detach_fails_the_type_c_limits();
    item_counter_never_passes_its_total();
    pps_status_answers();
    pps_overshoot_fails_low_levels();
    slow_pps_fall_fails_the_band();
    avs_sweep_and_caps_note();
    full_20mv_sweep_option();
    pps_ps_rdy_wire_allowance();
    return test_report("test_pdspec_engine");
}
