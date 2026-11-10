// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Lab-only Apple charger readout engine (see the header for the protocol): the source drives a
// 0x05AC discovery after the re-request, we enter the mode and read its register file through
// Attention + 05ACnn02 + addr, one read per ~20 ms, per-read timeout 50 ms with one retry, at most
// 16 reads per run. Pure logic: the Sink routes VDMs in/out (pd_sink.cpp), host tests replay the
// sniffed register file (tests/test_apple_readout.cpp).
#include <cstring>

#include "k2/core/log.hpp"
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
    phase_ = Phase::Dir;
    info_ = {};
    dir_i_ = 0;
    plan_ = {};
    plan_n_ = 0;
    str_i_ = 0;
    str_addr_ = 0;
    str_got_ = 0;
    awaiting_ = false;
    retries_ = 0;
    deadline_ = 0;
    next_at_ = 0;
    cur_nn_ = 0;
    cur_addr_ = 0;
}

void AppleReadout::arm(std::uint32_t now) {
    reset();
    state_ = State::Armed;
    armed_at_ = now;
    info_.valid = true;
    info_.why = AppleWhy::Running;
    K2LOG("[APPLE] armed\n");
}

// One Attention read request: Attention + 05ACnn02 + addr (three words, as sniffed).
static AppleVdm read_vdm(std::uint8_t nn, std::uint32_t addr) {
    AppleVdm tx;
    tx.w[0] = kAppleAttention;
    tx.w[1] = apple_read_obj(nn);
    tx.w[2] = addr;
    tx.n = 3;
    return tx;
}

AppleVdm AppleReadout::on_vdm(const AppleVdm& rx, std::uint32_t now) {
    if (state_ == State::Idle || rx.n == 0) return {};
    const std::uint32_t h = rx.w[0];
    if (std::uint16_t(h >> 16) != kAppleSvid) return {};
    const bool structured = (h >> 15) & 1;
    const std::uint8_t cmd_type = std::uint8_t((h >> 6) & 3);
    const std::uint8_t cmd = std::uint8_t(h & 0x1F);
    if (structured) {
        if (cmd_type == 2) {   // NAK: the run stops on the first one
            if (state_ == State::Reading && awaiting_) stop(AppleWhy::Nak, now);
            return {};
        }
        if (cmd_type == 0 && state_ == State::Armed && cmd == 3) {   // Discover Modes REQ
            AppleVdm tx;
            tx.w[0] = kAppleDiscoverModesAck;
            tx.w[1] = kAppleModesVdo;
            tx.n = 2;
            return tx;
        }
        if (cmd_type == 0 && state_ == State::Armed && cmd == 4) {   // Enter_Mode REQ
            state_ = State::Entered;
            entered_at_ = now;
            K2LOG("[APPLE] modes entered\n");
            AppleVdm tx;
            tx.w[0] = kAppleEnterModeAck;
            tx.n = 1;
            return tx;
        }
        return {};
    }
    // unstructured
    if (state_ == State::Entered && h == kAppleHandshakeReq) {   // 05AC0005: the mode is live, start reading
        state_ = State::Reading;
        phase_ = Phase::Dir;
        dir_i_ = 0;
        plan_n_ = 0;
        str_i_ = 0;
        str_got_ = 0;
        awaiting_ = false;
        retries_ = 0;
        next_at_ = now;   // the first read may go at once
        AppleVdm tx;
        tx.w[0] = kAppleHandshakeAck;
        tx.n = 1;
        return tx;
    }
    if (state_ == State::Reading && awaiting_ && h == apple_read_obj(cur_nn_) && rx.n >= 2u + cur_nn_ && rx.w[1] == cur_addr_) {
        on_answer(rx, now);
        AppleVdm tx;   // the 05ACnn12 ack of the answer, as sniffed
        tx.w[0] = apple_read_ack(cur_nn_);
        tx.n = 1;
        return tx;
    }
    return {};
}

AppleVdm AppleReadout::tick(std::uint32_t now) {
    // The source drives its discovery ~1.4 s after the re-request: give up when it never comes.
    if (state_ == State::Armed && std::int32_t(now - armed_at_) >= std::int32_t(kAppleArmWaitMs)) {
        stop(AppleWhy::NoMode, now);
        return {};
    }
    if (state_ == State::Entered && std::int32_t(now - entered_at_) >= std::int32_t(kAppleEnterWaitMs)) {
        stop(AppleWhy::NoMode, now);
        return {};
    }
    if (state_ != State::Reading) return {};
    if (awaiting_ && std::int32_t(now - deadline_) >= 0) {
        if (retries_ < kAppleReadRetries) {   // one retry: resend the same read at once
            retries_++;
            const AppleVdm tx = read_vdm(cur_nn_, cur_addr_);
            info_.reads++;
            deadline_ = now + kAppleReadTimeoutMs;
            next_at_ = now + kAppleReadPeriodMs;
            return tx;
        }
        stop(AppleWhy::Timeout, now);
        return {};
    }
    if (!awaiting_ && std::int32_t(now - next_at_) >= 0) {
        const AppleVdm tx = next_read(now);
        if (!tx.n) return {};   // the run ended inside next_read (stop logged it)
        info_.reads++;
        awaiting_ = true;
        deadline_ = now + kAppleReadTimeoutMs;
        next_at_ = now + kAppleReadPeriodMs;
        return tx;
    }
    return {};
}

void AppleReadout::stop(AppleWhy why, std::uint32_t) {
    state_ = State::Idle;
    awaiting_ = false;
    info_.why = why;
    K2LOG("[APPLE] done reads=%d ok=%d why=%s\n", int(info_.reads), int(info_.ok), apple_why_name(why));
}

AppleVdm AppleReadout::next_read(std::uint32_t now) {
    if (phase_ == Phase::Dir && dir_i_ >= kAppleDirEntries) {   // the directory is in: plan the strings
        phase_ = Phase::Str;
        str_i_ = 0;
        str_got_ = 0;
        plan_finish(now);
    }
    // Completion is checked before the cap, so a run that ends exactly at the cap reports done.
    if (phase_ == Phase::Str && str_i_ >= plan_n_) {
        stop(AppleWhy::Done, now);
        return {};
    }
    if (info_.reads >= kAppleMaxReads) {
        stop(AppleWhy::Cap, now);
        return {};
    }
    if (phase_ == Phase::Dir) {
        cur_addr_ = kAppleDirFirst + kAppleDirStep * dir_i_++;
        cur_nn_ = 1;   // one word per directory entry
    } else {
        const Entry& e = plan_[str_i_];
        const unsigned rem = e.len - str_got_;
        cur_nn_ = rem > 4 * kAppleMaxWordsPerRead ? kAppleMaxWordsPerRead : std::uint8_t((rem + 3) / 4);
        cur_addr_ = e.addr + str_got_;
    }
    return read_vdm(cur_nn_, cur_addr_);
}

void AppleReadout::on_answer(const AppleVdm& rx, std::uint32_t now) {
    awaiting_ = false;
    retries_ = 0;
    if (phase_ == Phase::Dir) {
        const std::uint32_t w = rx.w[2];
        const std::uint8_t idx = std::uint8_t(w >> 24);
        const std::uint8_t len = std::uint8_t((w >> 16) & 0xFF);
        const std::uint32_t addr = w & 0xFFFFu;
        const bool field = idx == 1 || idx == 2 || idx == 4 || idx == 5 || idx == 7 || idx == 8;
        // A field entry outside the bank-0 window (or empty) stops the run before it is read; the
        // answer does not count as ok.
        if (field && (len == 0 || addr + len > kAppleRegWindow)) {
            stop(AppleWhy::Range, now);
            return;
        }
        info_.ok++;
        dir_entry(w, now);
    } else {
        info_.ok++;
        str_append(rx);
    }
}

void AppleReadout::dir_entry(std::uint32_t w, std::uint32_t) {
    const std::uint8_t idx = std::uint8_t(w >> 24);
    if (idx == 1 || idx == 2 || idx == 4 || idx == 5 || idx == 7 || idx == 8) {
        if (plan_n_ < plan_.size())
            plan_[plan_n_++] = {idx, std::uint8_t((w >> 16) & 0xFF), std::uint16_t(w & 0xFFFFu)};
    }
}

void AppleReadout::plan_finish(std::uint32_t) {
    K2LOG("[APPLE] dir n=%d\n", int(plan_n_));
}

void AppleReadout::str_append(const AppleVdm& rx) {
    Entry& e = plan_[str_i_];
    auto& dst = field_of(info_, e.idx);
    bool done = false;
    for (unsigned i = 0; i < cur_nn_ && !done; i++) {
        const std::uint32_t w = rx.w[2 + i];
        for (unsigned b = 0; b < 4; b++) {
            const unsigned pos = str_got_ + 4 * i + b;
            if (pos >= e.len) { done = true; break; }   // the field is fully read
            const char c = char(w >> (8 * b));
            if (c == 0) { done = true; break; }          // strings are NUL-padded ASCII, little endian per word
            if (pos < kAppleStr) dst[pos] = c;
        }
    }
    str_got_ += cur_nn_ * 4;
    if (str_got_ >= e.len) done = true;
    if (done) {
        // The serial never goes into a binlog %s (the cut is 12 bytes anyway): length only.
        if (e.idx == 1) K2LOG("[APPLE] serial len=%d\n", int(std::strlen(info_.serial.data())));
        str_i_++;
        str_got_ = 0;
    }
}

std::array<char, kAppleStr + 1>& AppleReadout::field_of(AppleInfo& i, std::uint8_t idx) {
    switch (idx) {
        case 1: return i.serial;
        case 2: return i.maker;
        case 4: return i.product;
        case 5: return i.field90;
        case 7: return i.fw;
        case 8: return i.hw;
        default: return i.maker;
    }
}

}  // namespace k2::pd
