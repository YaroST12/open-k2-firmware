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
        own_seen_ = false;   // inline_ stays: a collapse of VBUS is not proof that the sink left (see kInlineHoldMs)
        own_ever_ = false;
        foreign_timing_ = false;
    } else {
        waiting_ = true;  // old data stays until new Source_Capabilities arrive (or kWaitingCapsMs pass)
        new_source_ = true;
        vbus_was_low_ = false;
        attach_at_ = at_ms;
        quiet_since_ = at_ms;   // a held inline state is judged from this attach on, not from the load before the gap
    }
}

void Link::update(const LinkInput& in) {
    last_rx_ = in.caps_rx;
    if (forced_) return;
    if (!attached_) requests_at_attach_ = in.requests;  // only Requests seen while attached count

    // VBUS trigger with hysteresis: low below vSafe0V, back at vSafe5V min.
    if (in.vbus_known) {
        if (in.vbus_mv < kVSafe0Mv) {
            if (!low_timing_) { low_timing_ = true; low_since_ = in.now_ms; }
            if (!vbus_low_ && in.now_ms - low_since_ > kDetachDebounceMs) vbus_low_ = true;
        } else {
            low_timing_ = false;
            if (in.vbus_mv >= kVSafe5MinMv) vbus_low_ = false;
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
    (void)was_gone;

    const bool want = !vbus_low_ && !cc_gone_;
    if (!want && attached_) {
        // The detach time is the first sample of whichever trigger fired.
        std::uint32_t at = in.now_ms;
        if (vbus_low_ && cc_gone_) at = low_since_ < gone_since_ ? low_since_ : gone_since_;
        else if (vbus_low_) at = low_since_;
        else at = gone_since_;
        set_attached(false, at);
    } else if (want && !attached_) {
        set_attached(true, in.now_ms);
    }
    if (!attached_ && inline_ && in.now_ms - detach_at_ > kInlineHoldMs) inline_ = false;   // gone for real
    if (waiting_ && in.caps_rx != rx_at_detach_) waiting_ = false;   // new Source_Capabilities arrived
    else if (waiting_ && in.now_ms - attach_at_ > kWaitingCapsMs) waiting_ = false;   // a non-PD source

    if (attached_) {
        const std::int32_t mag = power::abs_sat(in.current_ma);
        const bool loaded = in.current_known && mag > kInlineCurrentMa + (in.self_supply_ma > 0 ? in.self_supply_ma : 0);
        const bool new_request = in.requests != requests_seen_;
        requests_seen_ = in.requests;
        // A rise above 5.5 V that the K2 did not cause: only counts after VBUS was seen at or
        // below 5.5 V while attached. A charger already sitting at 9 V is not a sink.
        // "Ours" lasts kOwnHoldMs past the end of the K2's trigger: a trigger that gave up releases the data lines and the
        // charger answers a few ms later, when the trigger is already Failed or Idle.
        if (in.own_contract) { own_seen_ = true; own_ever_ = true; own_last_ms_ = in.now_ms; }
        const bool own_recent = in.own_contract || (own_seen_ && in.now_ms - own_last_ms_ <= kOwnHoldMs);
        // The lines alone: a PD-level VBUS nobody on this side asked for, or a sink's Rd on CC (see kInlineForeignMs).
        const bool foreign_now = !own_recent && ((in.vbus_known && in.vbus_mv > kInlineVbusMv && !own_ever_) || in.far_sink_cc);
        bool foreign = false;
        if (foreign_now) {
            if (!foreign_timing_) { foreign_timing_ = true; foreign_since_ = in.now_ms; }
            foreign = in.now_ms - foreign_since_ >= kInlineForeignMs;
        } else {
            foreign_timing_ = false;
        }
        bool rise = false;
        if (in.vbus_known) {
            if (in.vbus_mv <= kInlineVbusMv) vbus_was_low_ = true;
            else if (vbus_was_low_) {
                vbus_was_low_ = false;   // consumed: it needs a new low before the next rise
                rise = !own_recent;
            }
        }
        if (!inline_) {
            if (loaded || in.requests != requests_at_attach_ || rise || foreign) { inline_ = true; quiet_since_ = in.now_ms; }
        } else if (loaded || new_request || rise || foreign_now) {
            quiet_since_ = in.now_ms;    // live traffic: keep the guard
        } else if (in.now_ms - quiet_since_ > kInlineQuietMs) {
            inline_ = false;             // the sink left without a CC detach: Auto-CRC comes back
            requests_at_attach_ = in.requests;
            vbus_was_low_ = false;       // a high VBUS that stays does not re-latch
        }
    }
}

void Link::force_attached(std::uint32_t now_ms) {
    forced_ = true;
    if (!attached_) set_attached(true, now_ms);
    waiting_ = false;
    new_source_ = false;
}

void Link::force_detached(std::uint32_t now_ms) {
    forced_ = true;
    if (attached_) set_attached(false, now_ms);
    inline_ = false;   // an explicit debug detach is a statement, not a VBUS gap
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
