// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#include "k2/pd/pd_link.hpp"
#include "k2/drivers/power_calc.hpp"

namespace k2::pd {

void Link::set_attached(bool on, std::uint32_t at_ms) {
    if (on == attached_) return;
    attached_ = on;
    if (!on) {
        detach_at_ = at_ms;
        rx_at_detach_ = last_rx_;
        waiting_ = false;
        // inline_ stays: a collapse of VBUS is not proof that the sink left (see kInlineHoldMs)
    } else {
        waiting_ = true;  // old data stays until new Source_Capabilities arrive (or kWaitingCapsMs pass)
        new_source_ = true;
        attach_at_ = at_ms;
        // The count starts at the detach, not at this edge: the edge is the first tick that saw VBUS, and the driver may have
        // decoded the new source's caps before it (a paint held the tick, or the VBUS sample lagged the CC attach).
        rx_attach_ = rx_at_detach_;
        verdict_ = PdVerdict::Pending;
        none_why_ = PdNoneWhy::None;
        // A held inline state keeps the quiet it had before the gap; the gap itself is paused (update(), vbus_cut()).
    }
}

void Link::update(const LinkInput& in) {
    const std::uint32_t dt = in.now_ms - last_now_;   // since the previous tick: the step a paused quiet window moves by
    last_rx_ = in.caps_rx;
    last_now_ = in.now_ms;
    vbus_known_ = in.vbus_known;
    vbus_mv_ = in.vbus_mv;
    // The waiting classifier's trace: only while the K2 is passive do the lines show the far port's own Rp
    // (our Rd/Ra, an inline sink or a live VBUS make the question void: the trace is dropped, not accumulated).
    if (in.passive && in.cc1_mv >= 0 && in.cc2_mv >= 0) {
        wait_trace_.add(in.now_ms, in.cc1_mv >= raguard::kRpMinMv || in.cc2_mv >= raguard::kRpMinMv);
        drp_trace_.add(in.now_ms, std::max(in.cc1_mv, in.cc2_mv));
    } else {
        wait_trace_.clear();
        drp_trace_.clear();
    }
    // The crossed-CC trace: the same passive lines, and only while VBUS is dark, nothing of ours holds a line (the Wake hold),
    // no sink is inline and the K2 has no contract. Any gate dropping starts the observation over.
    if (in.passive && !in.line_held && !in.own_contract && !inline_ && in.vbus_known && in.vbus_mv < raguard::kVbusAttachedMv &&
        in.cc1_mv >= 0 && in.cc2_mv >= 0)
        cross_trace_.add(in.now_ms, in.cc1_mv, in.cc2_mv);
    else
        cross_trace_.clear();
    if (forced_) return;
    if (!attached_) requests_at_attach_ = in.requests;  // only Requests seen while attached count

    // VBUS trigger with hysteresis: low below vSafe0V, back at kVbusAttachMinMv (covers 3.3V PPS).
    if (in.vbus_known) {
        if (in.vbus_mv < kVSafe0Mv) {
            if (!low_timing_) { low_timing_ = true; low_since_ = in.now_ms; }
            if (!vbus_low_ && in.now_ms - low_since_ > kDetachDebounceMs) vbus_low_ = true;
        } else {
            low_timing_ = false;
            if (in.vbus_mv >= kVbusAttachMinMv) vbus_low_ = false;
        }
    } else {
        low_timing_ = false;
        vbus_low_ = false;
    }

    // CC trigger.
    const bool was_gone = cc_gone_;
    if (in.cc == 0) {
        if (!gone_timing_) { gone_timing_ = true; gone_since_ = in.now_ms; }
        if (!cc_gone_ && in.now_ms - gone_since_ > kDetachDebounceMs) {
            cc_gone_ = true;
            cable_dirty_ = true;  // CC detach: the cable may have been swapped
        }
    } else {
        gone_timing_ = false;
        cc_gone_ = false;
    }

    const bool vbus_legacy = in.vbus_known && !vbus_low_ && in.vbus_mv >= kLegacyVbusMinMv;
    const bool want = in.vbus_known ? (!vbus_low_ && (!cc_gone_ || vbus_legacy)) : !cc_gone_;
    if (!want && attached_) {
        // The detach time is the first sample of whichever trigger fired.
        std::uint32_t at = in.now_ms;
        if (vbus_low_ && cc_gone_) {
            const std::uint32_t cc_at = was_gone ? in.now_ms : gone_since_;
            at = low_since_ < cc_at ? low_since_ : cc_at;
        } else if (vbus_low_) {
            at = low_since_;
        } else {
            at = was_gone ? in.now_ms : gone_since_;
        }
        set_attached(false, at);
    } else if (want && !attached_) {
        set_attached(true, in.now_ms);
    }
    if (!attached_ && inline_ && in.now_ms - detach_at_ > kInlineHoldMs) {
        inline_ = false;   // gone for real
        open_rp_timing_ = false;
        paused_seen_ = false;
    }
    if (waiting_ && cc_gone_) waiting_ = false;                      // no active CC line: Source_Capabilities cannot arrive
    else if (waiting_ && in.caps_rx != rx_at_detach_) waiting_ = false;   // new Source_Capabilities arrived
    else if (waiting_ && in.now_ms - attach_at_ > kWaitingCapsMs) waiting_ = false;   // a non-PD source

    // The one PD verdict per attach, computed on every tick so it settles at kPdVerdictSettleMs sharp.
    {
        const PdVerdictOut v = ::k2::pd::pd_verdict({in.now_ms, attach_at_, attached_, in.any_pd_msg,
                                                     last_rx_ - rx_attach_, in.sink_answered});
        verdict_ = v.verdict;
        none_why_ = v.why;
    }

    // A VBUS cut or a detach inside the hold: the quiet window pauses (it moves along with the clock, never past it).
    const bool up_now = in.vbus_known && in.vbus_mv >= kVSafe5MinMv;
    if (up_now) { vbus_up_ms_ = in.now_ms; up_seen_ = true; }
    const bool cut = vbus_cut(in.vbus_known, in.vbus_mv, up_seen_, in.now_ms, vbus_up_ms_);
    if (inline_ && (!attached_ || cut)) {
        quiet_since_ = in.now_ms - quiet_since_ > dt ? quiet_since_ + dt : in.now_ms;
        paused_at_ = in.now_ms;
        paused_seen_ = true;
    }

    const std::int32_t mag = power::abs_sat(in.current_ma);
    const bool loaded = in.current_known && mag > kInlineCurrentMa + (in.self_supply_ma > 0 ? in.self_supply_ma : 0);
    const bool new_request = in.requests != requests_seen_;
    requests_seen_ = in.requests;

    if (attached_) {
        // Only evidence that is certain latches: a load beyond the K2's own supply, or a sink's PD Request that the K2 did not
        // send. Level evidence never latches (user decision 2026-11-15): a sink-band level on CC (a floating CC2 reads ~1.75 V on
        // a USB-A port with nothing plugged in), a VBUS above 5.5 V and a VBUS rise can all be explained without a device behind
        // the K2. The causes that named them (InlineCause::Rise / Vbus / FarRd) stay in the enum for the wire and the journal only.
        if (!inline_) {
            const bool request = in.requests != requests_at_attach_;
            if (loaded || request) {
                inline_ = true;
                quiet_since_ = in.now_ms;
                paused_seen_ = false;
                open_rp_timing_ = false;
                cause_ = loaded ? InlineCause::Load : InlineCause::Request;
            }
        } else if (loaded || new_request) {
            quiet_since_ = in.now_ms;    // live traffic: keep the guard
        } else if (in.now_ms - quiet_since_ > kInlineQuietMs && (!paused_seen_ || in.now_ms - paused_at_ >= kInlineResumeMs)) {
            inline_ = false;             // the sink left without a CC detach: Auto-CRC comes back
            requests_at_attach_ = in.requests;
            open_rp_timing_ = false;
            paused_seen_ = false;
        }
    }

    if (inline_) {
        bool open_rp = false;
        if (in.cc1_mv >= 0 && in.cc2_mv >= 0 && !in.line_held && !in.own_contract) {
            if (in.cc == 1) open_rp = in.cc1_mv >= raguard::kRpMinMv;
            else if (in.cc == 2) open_rp = in.cc2_mv >= raguard::kRpMinMv;
            else open_rp = in.cc1_mv >= raguard::kRpMinMv || in.cc2_mv >= raguard::kRpMinMv;
        }
        if (!loaded && !new_request && open_rp) {
            if (!open_rp_timing_) { open_rp_timing_ = true; open_rp_since_ = in.now_ms; }
            if (in.now_ms - open_rp_since_ > kInlineOpenRpMs &&
                (!attached_ || !paused_seen_ || in.now_ms - paused_at_ >= kInlineResumeMs)) {
                inline_ = false;
                requests_at_attach_ = in.requests;
                open_rp_timing_ = false;
                paused_seen_ = false;
            }
        } else {
            open_rp_timing_ = false;
        }
    } else {
        open_rp_timing_ = false;
    }
}

void Link::force_attached(std::uint32_t now_ms) {
    forced_ = true;
    if (!attached_) set_attached(true, now_ms);
    waiting_ = false;
    new_source_ = false;
    verdict_ = PdVerdict::Present;   // a forced attach shows live PD data by definition
    none_why_ = PdNoneWhy::None;
}

void Link::force_detached(std::uint32_t now_ms) {
    forced_ = true;
    if (attached_) set_attached(false, now_ms);
    inline_ = false;   // an explicit debug detach is a statement, not a VBUS gap
    open_rp_timing_ = false;
    paused_seen_ = false;
}

bool Link::take_new_source() {
    const bool r = new_source_;
    new_source_ = false;
    return r;
}

void Link::emarker_scanned(std::uint32_t now_ms) {
    scanned_ = true;
    cable_dirty_ = false;
    scan_at_ = now_ms;
}

void format_ago(std::uint32_t s, char* out, std::size_t n) {
    char tmp[16];
    std::size_t k = 0;
    auto put_u = [&](std::uint32_t v, int digits) {
        char d[10];
        int c = 0;
        do { d[c++] = char('0' + v % 10); v /= 10; } while (v);
        while (c < digits) d[c++] = '0';
        while (c) tmp[k++] = d[--c];
    };
    if (s >= 99 * 3600) {
        put_u(99, 0); tmp[k++] = 'h'; tmp[k++] = '+';
    } else if (s >= 3600) {
        put_u(s / 3600, 0); tmp[k++] = 'h'; tmp[k++] = ' '; put_u(s % 3600 / 60, 2); tmp[k++] = 'm';
    } else if (s >= 60) {
        put_u(s / 60, 0); tmp[k++] = 'm'; tmp[k++] = ' '; put_u(s % 60, 0); tmp[k++] = 's';
    } else {
        put_u(s, 0); tmp[k++] = 's';
    }
    tmp[k] = '\0';
    std::size_t i = 0;
    for (; i + 1 < n && tmp[i]; i++) out[i] = tmp[i];
    if (n) out[i] = '\0';
}

}  // namespace k2::pd
