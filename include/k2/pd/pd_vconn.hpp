// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// VCONN_Swap, sink side (pure logic, host-testable). Experiment for the captive-cable top port:
// a source that never sees Ra answers EPR_Mode Enter with Enter_Failed "could not become VCONN
// source". The K2 asks for the VCONN source role with a VCONN_Swap, powers VCONN itself on the
// unused CC (the driver does that when want_on() is true) and tells the partner with PS_RDY.
//
// Spec R3.2: VCONN_Swap is control message type 11 (6.3.11); the present VCONN source must not
// Reject it (Accept, Wait or Not_Supported), the new source turns VCONN on after the Accept and
// sends PS_RDY within tVconnSourceOn, the old source turns VCONN off after that PS_RDY (7.13).
// The reverse direction (the partner asks us to give the role back) is Accept now, VCONN off
// when the partner's PS_RDY arrives.
//
// The driver feeds control messages and the clock in, takes at most one message out per tick.
#pragma once
#include <cstdint>

#include "k2/pd/pd_sink.hpp"

namespace k2::pd {

inline constexpr std::uint8_t kCtlVconnSwap = 11;

enum class VconnState : std::uint8_t {
    Off,        // the partner is the VCONN source (or nobody): nothing driven
    SwapSent,   // our VCONN_Swap is out, waiting for Accept
    Settling,   // Accept seen, VCONN switched on, waiting for it to rise before PS_RDY
    On,         // PS_RDY sent: we are the VCONN source
    Releasing,  // the partner asked for the role back, Accept sent, waiting for its PS_RDY
    Failed,     // refused / not answered; VCONN is off
};

enum class VconnWhy : std::uint8_t { None, Accepted, Rejected, Wait, NotSupported, Timeout, ReleasedByPartner, Refused, Abort };

inline constexpr std::uint32_t kVconnResponseMs = 100;   // wait for Accept (spec tSenderResponse is 24-30 ms; margin for the bit-banged poll)
inline constexpr std::uint32_t kVconnSettleMs = 20;      // VCONN on before our PS_RDY (tVconnSourceOn allows 100 ms after Accept)
inline constexpr std::uint32_t kVconnReleaseMs = 250;    // the partner's PS_RDY after our Accept; then VCONN goes off regardless

class Vconn {
public:
    VconnState state() const { return st_; }
    VconnWhy why() const { return why_; }
    bool want_on() const { return on_; }
    std::uint8_t swaps_rx() const { return swaps_rx_; }

    // Ask the partner for the VCONN source role. False when a swap is already running or we hold it.
    bool request(std::uint32_t now) {
        if (st_ != VconnState::Off && st_ != VconnState::Failed) return false;
        st_ = VconnState::SwapSent;
        why_ = VconnWhy::None;
        deadline_ = now + kVconnResponseMs;
        queue(kCtlVconnSwap);
        return true;
    }

    void on_control(std::uint8_t type, std::uint32_t now) {
        switch (st_) {
            case VconnState::SwapSent:
                if (type == kCtlAccept) {
                    st_ = VconnState::Settling;
                    why_ = VconnWhy::Accepted;
                    on_ = true;
                    deadline_ = now + kVconnSettleMs;
                } else if (type == kCtlReject || type == kCtlWait || type == kCtlNotSupported) {
                    st_ = VconnState::Failed;
                    why_ = type == kCtlReject ? VconnWhy::Rejected : type == kCtlWait ? VconnWhy::Wait : VconnWhy::NotSupported;
                }
                break;
            case VconnState::On:
                if (type == kCtlVconnSwap) {
                    swaps_rx_++;
                    st_ = VconnState::Releasing;
                    deadline_ = now + kVconnReleaseMs;
                    queue(kCtlAccept);
                }
                break;
            case VconnState::Releasing:
                if (type == kCtlPsRdy) release(VconnWhy::ReleasedByPartner);
                break;
            default:
                // Not the source: we do not take the role over on the partner's request.
                if (type == kCtlVconnSwap) {
                    swaps_rx_++;
                    queue(kCtlNotSupported);
                }
                break;
        }
    }

    void tick(std::uint32_t now) {
        if (st_ == VconnState::Off || st_ == VconnState::Failed || st_ == VconnState::On) return;
        if (static_cast<std::int32_t>(now - deadline_) < 0) return;
        switch (st_) {
            case VconnState::SwapSent: st_ = VconnState::Failed; why_ = VconnWhy::Timeout; break;
            case VconnState::Settling: st_ = VconnState::On; queue(kCtlPsRdy); break;
            case VconnState::Releasing: release(VconnWhy::Timeout); break;
            default: break;
        }
    }

    // VCONN off now and everything forgotten (detach, hard reset, remote abort).
    void reset(VconnWhy w = VconnWhy::Abort) {
        st_ = VconnState::Off;
        why_ = w;
        on_ = false;
        pending_ = 0;
    }

    bool take_tx(TxMsg& m, std::uint8_t rev) {
        if (pending_ == 0) return false;
        m = {};
        m.header = make_header(pending_ == 0xFF ? 0 : pending_, 0, rev);
        pending_ = 0;
        return true;
    }

private:
    void queue(std::uint8_t type) { pending_ = type; }
    void release(VconnWhy w) {
        st_ = VconnState::Off;
        why_ = w;
        on_ = false;
    }

    VconnState st_ = VconnState::Off;
    VconnWhy why_ = VconnWhy::None;
    bool on_ = false;
    std::uint8_t pending_ = 0;   // control type to send; 0 = none (type 0 is reserved)
    std::uint8_t swaps_rx_ = 0;
    std::uint32_t deadline_ = 0;
};

}  // namespace k2::pd
