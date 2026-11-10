// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Lab-only Apple charger readout engine. STUB for the red test commit: the state machine is not
// implemented yet (tests/test_apple_readout.cpp fails against it); the name tables are real.
#include "k2/pd/apple_readout.hpp"

namespace k2::pd {

const char* apple_why_name(AppleWhy w) {
    switch (w) {
        case AppleWhy::Running: return "running";
        case AppleWhy::Done: return "done";
        case AppleWhy::Nak: return "nak";
        case AppleWhy::Timeout: return "timeout";
        case AppleWhy::Range: return "range";
        case AppleWhy::Cap: return "cap";
        case AppleWhy::NoMode: return "nomode";
    }
    return "?";
}

const char* apple_refusal_name(AppleRefusal r) {
    switch (r) {
        case AppleRefusal::None: return "none";
        case AppleRefusal::Inline: return "inline";
        case AppleRefusal::NoContract: return "no contract";
        case AppleRefusal::NotPartner: return "not partner";
        case AppleRefusal::Busy: return "busy";
    }
    return "?";
}

void AppleReadout::reset() {
    state_ = State::Idle;
    info_ = {};
}

void AppleReadout::arm(std::uint32_t now) {
    state_ = State::Armed;
    armed_at_ = now;
    info_ = {};
}

AppleVdm AppleReadout::on_vdm(const AppleVdm&, std::uint32_t) { return {}; }
AppleVdm AppleReadout::tick(std::uint32_t) { return {}; }

void AppleReadout::stop(AppleWhy, std::uint32_t) {}
AppleVdm AppleReadout::next_read(std::uint32_t) { return {}; }
void AppleReadout::on_answer(const AppleVdm&, std::uint32_t) {}
void AppleReadout::dir_entry(std::uint32_t, std::uint32_t) {}
void AppleReadout::plan_finish(std::uint32_t) {}
void AppleReadout::str_append(const AppleVdm&) {}
std::array<char, kAppleStr + 1>& AppleReadout::field_of(AppleInfo& i, std::uint8_t) { return i.maker; }

}  // namespace k2::pd
