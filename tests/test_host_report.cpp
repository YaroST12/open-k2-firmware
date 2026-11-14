// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Loop probe "Host report", PD half (k2/loopprobe/host_report.hpp): the sequencer against a scripted fake host on the Env seam
// (UFP questions, the DR_Swap path to DFP and back, refusals, timeouts and retries, the soft reset on a protocol error, a host
// Hard Reset, the journal resume), the never-drop-VBUS invariant over every scenario, the record codec and the journal.
#include "test_util.h"

#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <string>
#include <map>
#include <vector>

#include "k2/loopprobe/host_report.hpp"

using namespace k2;
using namespace k2::loopprobe::host;
namespace usbfp = k2::loopprobe::usbfp;

namespace {

// ---- PD frames as a host sends them (USB PD 3.1 6.2.1.1: type 4:0, data role 5, rev 7:6, power role 8, #DO 14:12, ext 15)

std::uint16_t hdr(std::uint8_t type, std::uint8_t ndo, std::uint8_t rev, bool dfp, bool ext = false) {
    return std::uint16_t(type | (dfp ? 1u << 5 : 0u) | (rev << 6) | (1u << 8) | (ndo << 12) | (ext ? 1u << 15 : 0u));
}
void put32(std::vector<std::uint8_t>& v, std::uint32_t x) { for (int i = 0; i < 4; i++) v.push_back(std::uint8_t(x >> (8 * i))); }

enum class R { Answer, Nak, Busy, NotSup, Reject, Wait, Silent };

struct Frame {
    std::uint16_t h;
    std::vector<std::uint8_t> p;
    std::uint32_t at;
};

struct Host final : Env {
    std::uint32_t now = 0;
    std::uint8_t rev = 2;
    bool host_dfp = true;
    int goodcrc_fail = 0;          // the next n sends get no GoodCRC
    std::uint32_t latency = 5;     // ms to the answer
    std::map<char, std::vector<R>> script;   // per question kind: one entry per attempt, the last one repeats
    std::map<char, int> asked;
    std::vector<pd::TxMsg> sent;
    std::vector<std::uint32_t> sent_at;
    std::vector<int> dfp_calls;
    int soft = 0;
    std::deque<Frame> out;

    R next(char k) {
        const auto& v = script.count(k) ? script[k] : std::vector<R>{R::Answer};
        const int i = asked[k]++;
        return v[std::size_t(i) < v.size() ? std::size_t(i) : v.size() - 1];
    }
    void reply(std::uint16_t h, std::vector<std::uint8_t> p = {}) { out.push_back({h, std::move(p), now + latency}); }
    void ctl(std::uint8_t type) { reply(hdr(type, 0, rev, host_dfp)); }
    void vdm(std::uint16_t svid, std::uint8_t cmd, std::uint8_t ack, std::vector<std::uint32_t> vdos) {
        std::vector<std::uint8_t> p;
        put32(p, (std::uint32_t(svid) << 16) | 0x8000u | (rev >= 2 ? 0x2000u : 0u) | (std::uint32_t(ack) << 6) | cmd);
        for (auto x : vdos) put32(p, x);
        reply(hdr(15, std::uint8_t(p.size() / 4), rev, host_dfp), p);
    }
    void ext(std::uint8_t type, std::vector<std::uint8_t> data) {
        std::vector<std::uint8_t> p{std::uint8_t(data.size()), 0x80};   // chunked, chunk 0
        p.insert(p.end(), data.begin(), data.end());
        while (p.size() % 4) p.push_back(0);
        reply(hdr(type, std::uint8_t(p.size() / 4), rev, host_dfp, true), p);
    }
    bool refuse(R r) {
        switch (r) {
        case R::NotSup: ctl(16); return true;
        case R::Reject: ctl(4); return true;
        case R::Wait: ctl(12); return true;
        case R::Silent: return true;
        default: return false;
        }
    }

    bool send(const pd::TxMsg& m) override {
        sent.push_back(m);
        sent_at.push_back(now);
        if (goodcrc_fail > 0) { goodcrc_fail--; return false; }
        const std::uint8_t type = m.header & 0x1F, ndo = (m.header >> 12) & 7;
        const bool is_ext = m.header & 0x8000;
        if (!is_ext && ndo == 0) {
            if (type == 9) {   // DR_Swap
                const R r = next('D');
                if (r == R::Answer) { ctl(3); host_dfp = !host_dfp; }
                else refuse(r);
            } else if (type == 17 && !refuse(next('X'))) {
                std::vector<std::uint8_t> d(25, 0);
                d[0] = 0xD1; d[1] = 0x18; d[2] = 0x34; d[3] = 0x12; d[23] = 60;
                ext(1, d);
            } else if (type == 24 && !refuse(next('V'))) {
                std::vector<std::uint8_t> p;
                put32(p, 0x31100000u);
                reply(hdr(12, 1, rev, host_dfp), p);
            } else if (type == 18 && !refuse(next('T'))) {
                ext(2, {25, 0, 0, 0, 0, 0, 0});
            }
        } else if (is_ext && type == 6) {
            if (!refuse(next('F'))) ext(7, {0xD1, 0x18, 0x34, 0x12, 'H', 'o', 's', 't'});
        } else if (!is_ext && type == 15) {
            const std::uint32_t vh = std::uint32_t(m.data[0] | m.data[1] << 8 | m.data[2] << 16 | std::uint32_t(m.data[3]) << 24);
            const std::uint8_t cmd = vh & 0x1F;
            const std::uint16_t svid = std::uint16_t(vh >> 16);
            const char k = cmd == 1 ? 'I' : cmd == 2 ? 'S' : 'M';
            const R r = next(k);
            if (r == R::Nak) vdm(svid, cmd, 2, {});
            else if (r == R::Busy) vdm(svid, cmd, 3, {});
            else if (!refuse(r)) {
                if (cmd == 1) vdm(svid, cmd, 1, {0x6C800000u | 0x18D1u, 0, 0x12340100u});
                else if (cmd == 2) vdm(svid, cmd, 1, {0x80870000u | 0xFF01u, 0});
                else vdm(svid, cmd, 1, {svid == 0xFF01 ? 0x00000C45u : 0x00000001u});
            }
        }
        return true;
    }
    void set_dfp(bool d) override { dfp_calls.push_back(d ? 1 : 0); }
    bool soft_reset() override { soft++; return true; }
};

struct Rig {
    Host h;
    Seq s{h};
    std::vector<std::uint32_t> checkpoints;

    void caps(std::uint8_t rev) {
        h.rev = rev;
        std::vector<std::uint8_t> p;
        put32(p, 0x0801912Cu);   // fixed 5 V 3 A
        put32(p, 0x0002D12Cu);   // fixed 9 V 3 A
        s.on_message(hdr(1, 2, rev, true), p, h.now);
    }
    void run_to(std::uint32_t until, bool usb = true, bool idle = true) {
        while (h.now < until) {
            h.now++;
            while (!h.out.empty() && h.out.front().at <= h.now) {
                Frame f = h.out.front();
                h.out.pop_front();
                s.on_message(f.h, f.p, h.now);
            }
            s.tick(h.now, idle, usb);
            if (s.take_checkpoint()) checkpoints.push_back(h.now);
        }
    }
    // A run on a host with a contract Ready at t=10.
    void go(std::uint8_t rev, std::uint32_t until = 5000) {
        s.start(0);
        caps(rev);
        h.now = 10;
        s.on_contract(true, h.now);
        run_to(until);
    }
    Ans ans(Q x) const { return Ans(s.facts().at(x).ans); }
    Skip skip(Q x) const { return Skip(s.facts().at(x).skip); }
};

// The invariant every scenario checks: nothing the sequencer sent can make the host drop VBUS.
void check_never_drops_vbus(const Host& h) {
    for (const pd::TxMsg& m : h.sent) {
        CHECK_COND(!m.hard_reset);
        const std::uint8_t type = m.header & 0x1F, ndo = (m.header >> 12) & 7;
        const bool is_ext = m.header & 0x8000;
        if (!is_ext && ndo == 0) {
            CHECK_COND(type != 10);   // PR_Swap
            CHECK_COND(type != 11);   // VCONN_Swap
            CHECK_COND(type != 13);   // Soft_Reset only through Env::soft_reset (the sink's own)
            CHECK_COND(type != 7);    // no Get_Source_Cap: a re-announce would renegotiate
        }
        if (!is_ext && ndo > 0) {
            CHECK_COND(type != 2);    // Request
            CHECK_COND(type != 9);    // EPR_Request
            CHECK_COND(type != 10);   // EPR_Mode
        }
    }
}

std::uint32_t vdm_hdr_of(const pd::TxMsg& m) {
    return std::uint32_t(m.data[0] | m.data[1] << 8 | m.data[2] << 16 | std::uint32_t(m.data[3]) << 24);
}

// ---- slice 1: a PD 3.x host answers everything; the K2 stays UFP --------------------------------------------------------

void test_pd3_host_answers_as_ufp() {
    Rig r;
    r.go(2);
    const PdFacts& f = r.s.facts();
    CHECK_EQ(int(r.s.phase()), int(Phase::Done));
    CHECK_EQ(int(r.s.end()), int(End::Done));
    CHECK_EQ(f.rev, 2);
    CHECK_EQ(f.n_pdos, 2);
    CHECK_EQ(f.pdos[1], 0x0002D12Cu);
    // Nothing before the contract settled (kSettleMs after Ready at t=10), then Discover Identity first, as UFP, SVDM 2.0.
    CHECK_EQ(r.h.sent.size(), 5u);   // one message per answered question
    if (r.h.sent.size() != 5u) return;
    CHECK_EQ(r.h.sent_at[0], 10 + kSettleMs);
    CHECK_EQ(r.h.sent[0].header & 0x1F, 15);
    CHECK_EQ((r.h.sent[0].header >> 6) & 3, 2);
    CHECK_EQ(vdm_hdr_of(r.h.sent[0]), 0xFF00A001u);
    CHECK_EQ(int(r.ans(Q::IdUfp)), int(Ans::Ack));
    CHECK_EQ(f.n_id, 3);
    CHECK_EQ(f.id[0], 0x6C8018D1u);
    // The PD 3.x extras, answered.
    CHECK_EQ(int(r.ans(Q::SrcCapExt)), int(Ans::Ack));
    CHECK_EQ(f.n_scx, 25);
    CHECK_EQ(f.scx[0], 0xD1);
    CHECK_EQ(f.scx[23], 60);
    CHECK_EQ(int(r.ans(Q::Revision)), int(Ans::Ack));
    CHECK_EQ(f.revision, 0x31100000u);
    CHECK_EQ(int(r.ans(Q::MfrInfo)), int(Ans::Ack));
    CHECK_EQ(f.n_mfr, 8);
    CHECK_EQ(f.mfr[4], 'H');
    CHECK_EQ(int(r.ans(Q::Status)), int(Ans::Ack));
    CHECK_EQ(f.n_status, 7);
    CHECK_EQ(f.status[0], 25);
    // The identity came as UFP: no swap, the DFP-only questions are skipped, nothing to swap back.
    CHECK_EQ(int(r.ans(Q::DrSwap)), int(Ans::Skipped));
    CHECK_EQ(int(r.skip(Q::DrSwap)), int(Skip::HaveId));
    CHECK_EQ(int(r.skip(Q::IdDfp)), int(Skip::HaveId));
    CHECK_EQ(int(r.skip(Q::Svids)), int(Skip::NoDfp));
    CHECK_EQ(int(r.skip(Q::Modes1)), int(Skip::NoDfp));
    CHECK_EQ(int(r.skip(Q::DrBack)), int(Skip::NotSwapped));
    CHECK_COND(r.h.dfp_calls.empty());
    CHECK_EQ(r.h.soft, 0);
    // Get_Manufacturer_Info goes chunked (the hardware rule for extended messages): ext header data size 2, chunked bit.
    const pd::TxMsg& mi = r.h.sent[3];
    CHECK_EQ(mi.header & 0x801F, 0x8006);
    CHECK_EQ(mi.data[0], 2);
    CHECK_EQ(mi.data[1], 0x80);
    check_never_drops_vbus(r.h);
}


// ---- slice 2: a PD 2.0 host: no UFP VDM, DR_Swap, then the DFP questions, then the swap back ---------------------------

void test_pd2_host_swaps_to_dfp_and_back() {
    Rig r;
    r.go(1);
    const PdFacts& f = r.s.facts();
    CHECK_EQ(int(r.s.end()), int(End::Done));
    CHECK_EQ(int(r.skip(Q::IdUfp)), int(Skip::Pd2));
    CHECK_EQ(int(r.skip(Q::SrcCapExt)), int(Skip::Pd2));
    CHECK_EQ(int(r.skip(Q::Status)), int(Skip::Pd2));
    CHECK_EQ(int(r.ans(Q::DrSwap)), int(Ans::Ack));
    CHECK_EQ(int(r.ans(Q::IdDfp)), int(Ans::Ack));
    CHECK_EQ(int(r.ans(Q::Svids)), int(Ans::Ack));
    CHECK_EQ(f.n_svids, 2);
    CHECK_EQ(f.svids[0], 0x8087);
    CHECK_EQ(f.svids[1], 0xFF01);
    // DisplayPort first, then Thunderbolt (pd::id_modes_svid order).
    CHECK_EQ(int(r.ans(Q::Modes1)), int(Ans::Ack));
    CHECK_EQ(f.mode_svid[0], 0xFF01);
    CHECK_EQ(f.mode_vdo[0], 0x00000C45u);
    CHECK_EQ(f.mode_svid[1], 0x8087);
    CHECK_EQ(int(r.ans(Q::DrBack)), int(Ans::Ack));
    CHECK_EQ(f.dfp, 0);
    CHECK_EQ(r.h.dfp_calls.size(), 2u);
    if (r.h.dfp_calls.size() == 2u) { CHECK_EQ(r.h.dfp_calls[0], 1); CHECK_EQ(r.h.dfp_calls[1], 0); }
    CHECK_COND(r.h.host_dfp);   // the host is the USB host again
    // Wire revision 1 (PD 2.0) on every message, SVDM 1.0 on the VDMs.
    for (const pd::TxMsg& m : r.h.sent) CHECK_EQ((m.header >> 6) & 3, 1);
    CHECK_EQ(vdm_hdr_of(r.h.sent[1]), 0xFF008001u);
    // The journal checkpoint came once, a tick before the DR_Swap went out.
    CHECK_EQ(r.checkpoints.size(), 1u);
    CHECK_EQ(r.h.sent[0].header & 0x1F, 9);
    if (!r.checkpoints.empty()) CHECK_EQ(r.h.sent_at[0], r.checkpoints[0] + 1);
    check_never_drops_vbus(r.h);
}

// The swap waits for the host's USB enumeration (usb_settled), at most kUsbWaitMs after Ready.
void test_swap_waits_for_usb() {
    Rig r;
    r.s.start(0);
    r.caps(1);
    r.h.now = 10;
    r.s.on_contract(true, 10);
    r.run_to(3000, false);
    CHECK_COND(r.h.sent.empty());
    r.run_to(10 + kUsbWaitMs + 5, false);
    CHECK_COND(!r.h.sent.empty());
    if (!r.h.sent.empty()) CHECK_EQ(r.h.sent_at[0], 10 + kUsbWaitMs + 1);
    check_never_drops_vbus(r.h);
}

// A PD 3.x host that ignores the UFP's Discover Identity (silence, then NAK on the retry): DR_Swap and ask as DFP.
void test_pd3_no_ufp_answer_then_dfp() {
    Rig r;
    r.h.script['I'] = {R::Silent, R::Silent, R::Silent, R::Answer};
    r.go(2);
    CHECK_EQ(int(r.ans(Q::IdUfp)), int(Ans::Timeout));
    CHECK_EQ(r.s.facts().at(Q::IdUfp).attempts, kMaxAttempts);
    // Retries back off: attempt n+1 goes out kRespMs + n x kBackoffMs after attempt n.
    CHECK_EQ(r.h.sent_at[1] - r.h.sent_at[0], kRespMs + kBackoffMs);
    CHECK_EQ(r.h.sent_at[2] - r.h.sent_at[1], kRespMs + 2 * kBackoffMs);
    CHECK_EQ(int(r.ans(Q::DrSwap)), int(Ans::Ack));
    CHECK_EQ(int(r.ans(Q::IdDfp)), int(Ans::Ack));
    CHECK_EQ(int(r.ans(Q::DrBack)), int(Ans::Ack));
    check_never_drops_vbus(r.h);
}

// ---- slice 3: refusals are data -----------------------------------------------------------------------------------------

void test_refusals_are_recorded() {
    Rig r;
    r.h.script['I'] = {R::Nak};
    r.h.script['X'] = {R::NotSup};
    r.h.script['V'] = {R::NotSup};
    r.h.script['F'] = {R::Reject};
    r.h.script['T'] = {R::Silent};
    r.h.script['D'] = {R::Reject};
    r.go(2);
    CHECK_EQ(int(r.ans(Q::IdUfp)), int(Ans::Nak));
    CHECK_EQ(r.s.facts().at(Q::IdUfp).attempts, 1);
    CHECK_EQ(int(r.ans(Q::SrcCapExt)), int(Ans::NotSupported));
    CHECK_EQ(int(r.ans(Q::Revision)), int(Ans::NotSupported));
    CHECK_EQ(int(r.ans(Q::MfrInfo)), int(Ans::Reject));
    CHECK_EQ(int(r.ans(Q::Status)), int(Ans::Timeout));
    CHECK_EQ(int(r.ans(Q::DrSwap)), int(Ans::Reject));
    CHECK_EQ(int(r.skip(Q::IdDfp)), int(Skip::NoDfp));
    CHECK_EQ(int(r.skip(Q::Svids)), int(Skip::NoDfp));
    CHECK_EQ(int(r.skip(Q::DrBack)), int(Skip::NotSwapped));
    CHECK_COND(r.h.dfp_calls.empty());
    CHECK_EQ(int(r.s.end()), int(End::Done));
    check_never_drops_vbus(r.h);
}

// BUSY and Wait are retried after the back-off; the answer that follows counts.
void test_busy_and_wait_are_retried() {
    Rig r;
    r.h.script['I'] = {R::Silent, R::Silent, R::Silent};
    r.h.script['D'] = {R::Wait, R::Answer};
    r.h.script['S'] = {R::Busy, R::Answer};
    r.go(2);
    CHECK_EQ(int(r.ans(Q::DrSwap)), int(Ans::Ack));
    CHECK_EQ(r.s.facts().at(Q::DrSwap).attempts, 2);
    CHECK_EQ(int(r.ans(Q::Svids)), int(Ans::Ack));
    CHECK_EQ(r.s.facts().at(Q::Svids).attempts, 2);
    check_never_drops_vbus(r.h);
}

// A swap the host keeps answering with Wait ends as Wait, the K2 stays UFP.
void test_wait_exhausted() {
    Rig r;
    r.h.script['I'] = {R::NotSup};
    r.h.script['D'] = {R::Wait};
    r.go(2);
    CHECK_EQ(int(r.ans(Q::DrSwap)), int(Ans::Wait));
    CHECK_EQ(r.s.facts().at(Q::DrSwap).attempts, kMaxAttempts);
    CHECK_EQ(int(r.skip(Q::IdDfp)), int(Skip::NoDfp));
    check_never_drops_vbus(r.h);
}

// ---- slice 4: a protocol error (no GoodCRC) gets a Soft_Reset, never a Hard Reset ---------------------------------------

void test_tx_failure_soft_resets_and_retries() {
    Rig r;
    r.h.goodcrc_fail = kMaxAttempts;   // every attempt at the first question goes unacknowledged
    r.s.start(0);
    r.caps(2);
    r.h.now = 10;
    r.s.on_contract(true, 10);
    r.run_to(800);
    CHECK_EQ(r.h.soft, 1);
    CHECK_EQ(r.s.facts().soft_resets, 1);
    CHECK_EQ(r.s.facts().tx_fails, kMaxAttempts);
    CHECK_EQ(int(r.s.phase()), int(Phase::WaitContract));
    CHECK_EQ(r.h.sent_at[1] - r.h.sent_at[0], kBackoffMs);       // unacknowledged attempts back off too
    CHECK_EQ(r.h.sent_at[2] - r.h.sent_at[1], 2 * kBackoffMs);
    // The sink's Soft_Reset dropped the contract; the new one is Ready, the question is asked again and answered.
    r.s.on_contract(false, 800);
    r.s.on_contract(true, 900);
    r.run_to(5000);
    CHECK_EQ(int(r.ans(Q::IdUfp)), int(Ans::Ack));
    CHECK_EQ(int(r.s.end()), int(End::Done));
    check_never_drops_vbus(r.h);
}

// A dead line: the soft resets are bounded, then the question ends TxFail and the run goes on.
void test_soft_resets_bounded() {
    Rig r;
    r.h.goodcrc_fail = 1000;
    r.s.start(0);
    r.caps(2);
    std::uint32_t t = 10;
    r.h.now = t;
    r.s.on_contract(true, t);
    for (int i = 0; i < 20 && r.s.running(); i++) {
        r.run_to(r.h.now + 600);
        if (r.s.phase() == Phase::WaitContract) {
            r.s.on_contract(false, r.h.now);
            r.s.on_contract(true, r.h.now);
        }
    }
    CHECK_EQ(r.h.soft, kMaxSoftResets);
    CHECK_EQ(int(r.ans(Q::IdUfp)), int(Ans::TxFail));
    CHECK_EQ(int(r.s.end()), int(End::Done));
    check_never_drops_vbus(r.h);
}

// ---- slice 5: the host sends a Hard Reset while the K2 is DFP ----------------------------------------------------------

void test_host_hard_reset_drops_dfp() {
    Rig r;
    r.h.script['I'] = {R::NotSup};
    r.h.script['S'] = {R::Silent};
    r.s.start(0);
    r.caps(2);
    r.h.now = 10;
    r.s.on_contract(true, 10);
    while (r.h.now < 5000 && r.s.current() != Q::Svids) r.run_to(r.h.now + 1);
    r.run_to(r.h.now + 5);   // Discover SVIDs is in flight
    CHECK_EQ(r.s.facts().dfp, 1);
    r.s.on_hard_reset(r.h.now);
    CHECK_EQ(r.s.facts().dfp, 0);
    CHECK_EQ(r.h.dfp_calls.back(), 0);
    CHECK_EQ(int(r.ans(Q::Svids)), int(Ans::Interrupted));
    CHECK_EQ(int(r.skip(Q::Svids)), int(Skip::HardReset));
    CHECK_EQ(int(r.s.phase()), int(Phase::WaitContract));
    r.s.on_contract(true, r.h.now + 900);
    r.h.now += 900;
    r.run_to(r.h.now + 3000);
    CHECK_EQ(int(r.s.end()), int(End::Done));
    CHECK_EQ(int(r.skip(Q::Modes1)), int(Skip::NoDfp));   // no second swap after a Hard Reset
    CHECK_EQ(int(r.skip(Q::DrBack)), int(Skip::NotSwapped));
    CHECK_EQ(r.s.facts().hard_resets, 1);
    check_never_drops_vbus(r.h);
}

// ---- slice 6: the journal resume ----------------------------------------------------------------------------------------

void test_resume_never_repeats_the_due_question() {
    Rig a;
    a.h.script['I'] = {R::NotSup};
    a.s.start(0);
    a.caps(2);
    a.h.now = 10;
    a.s.on_contract(true, 10);
    while (a.h.now < 5000 && a.checkpoints.empty()) a.run_to(a.h.now + 1);
    CHECK_EQ(a.checkpoints.size(), 1u);
    const PdFacts saved = a.s.facts();   // what the journal holds: the extras answered, DR_Swap not run
    CHECK_EQ(int(Ans(saved.at(Q::DrSwap).ans)), int(Ans::NotRun));
    // The K2 died in the swap. A new boot resumes from the journal.
    Rig b;
    b.s.start(0, &saved);
    b.caps(2);
    b.h.now = 10;
    b.s.on_contract(true, 10);
    b.run_to(5000);
    CHECK_EQ(int(b.ans(Q::DrSwap)), int(Ans::Interrupted));
    CHECK_EQ(int(b.skip(Q::DrSwap)), int(Skip::Rebooted));
    CHECK_EQ(int(b.ans(Q::SrcCapExt)), int(Ans::Ack));   // kept from before the reboot
    CHECK_COND(b.h.sent.empty());                        // nothing left to ask: no second swap, no repeats
    CHECK_COND(b.checkpoints.empty());
    CHECK_EQ(int(b.s.end()), int(End::Done));
    check_never_drops_vbus(b.h);
}

// ---- slice 7: no contract -----------------------------------------------------------------------------------------------

void test_no_contract_ends_the_run() {
    Rig r;
    r.s.start(0);
    r.run_to(kContractWaitMs + 1);
    CHECK_EQ(int(r.s.end()), int(End::NoContract));
    CHECK_COND(r.h.sent.empty());
}

// Nothing goes out while the line is busy (the sink in flight, frames arriving).
void test_waits_for_idle_line() {
    Rig r;
    r.s.start(0);
    r.caps(2);
    r.h.now = 10;
    r.s.on_contract(true, 10);
    r.run_to(1000, true, false);
    CHECK_COND(r.h.sent.empty());
    r.run_to(1001, true, true);
    CHECK_EQ(r.h.sent.size(), 1u);
}

// ---- the start refusal ------------------------------------------------------------------------------------------------------

LoopFacts loop_ok() {
    LoopFacts f;
    f.usb_core = true;
    f.hid_host = true;
    f.vbus_mv = 5050;
    f.ibus_ma = 120;
    f.active_cc = 1;
    return f;
}

void test_refusal() {
    CHECK_EQ(int(refusal(loop_ok())), int(Refuse::None));
    auto with = [](auto fn) { LoopFacts f = loop_ok(); fn(f); return int(refusal(f)); };
    CHECK_EQ(with([](LoopFacts& f) { f.usb_core = false; }), int(Refuse::UsbOff));
    CHECK_EQ(with([](LoopFacts& f) { f.hid_host = false; }), int(Refuse::NoHost));
    CHECK_EQ(with([](LoopFacts& f) { f.active_cc = 0; }), int(Refuse::NoCc));
    CHECK_EQ(with([](LoopFacts& f) { f.vbus_mv = 3000; }), int(Refuse::NoVbus));
    CHECK_EQ(with([](LoopFacts& f) { f.vbus_mv = -1; }), int(Refuse::NoVbus));
    CHECK_EQ(with([](LoopFacts& f) { f.vbus_mv = 5501; }), int(Refuse::VbusHigh));
    CHECK_EQ(with([](LoopFacts& f) { f.vbus_mv = 5500; }), int(Refuse::None));
    CHECK_EQ(with([](LoopFacts& f) { f.ibus_ma = 401; }), int(Refuse::Load));
    CHECK_EQ(with([](LoopFacts& f) { f.ibus_ma = -401; }), int(Refuse::Load));
    CHECK_EQ(with([](LoopFacts& f) { f.far_request = true; }), int(Refuse::FarSink));
    CHECK_EQ(with([](LoopFacts& f) { f.own_rd = true; }), int(Refuse::OwnRd));
    CHECK_EQ(with([](LoopFacts& f) { f.busy = true; }), int(Refuse::Busy));
    CHECK_EQ(with([](LoopFacts& f) { f.contract = true; }), int(Refuse::Contract));
    // A hard reason wins over one the start would wait out.
    CHECK_EQ(with([](LoopFacts& f) { f.hid_host = false; f.far_request = true; }), int(Refuse::FarSink));
    CHECK_COND(transient(Refuse::NoHost) && transient(Refuse::NoCc) && transient(Refuse::NoVbus));
    CHECK_COND(!transient(Refuse::FarSink) && !transient(Refuse::VbusHigh) && !transient(Refuse::UsbOff));
}

// ---- the record, the file, the journal --------------------------------------------------------------------------------------

Report sample_report() {
    Report r;
    r.state = std::uint8_t(JState::Running);
    r.boots = 2;
    r.end = std::uint8_t(End::Done);
    r.os = 4;
    std::memcpy(r.fw.data(), "1.0.1-130-abcdef12", 18);
    r.start_ms = 0x01020304;
    r.cc = 2;
    r.rp = 3;
    r.vbus_mv = 5070;
    r.dp_mv = 3300;
    r.dm_mv = -1;
    r.pd.rev = 2;
    r.pd.n_pdos = 2;
    r.pd.pdos[0] = 0x0801912Cu;
    r.pd.at(Q::DrBack) = {std::uint8_t(Ans::Ack), 1, 0, 0x1234};
    r.pd.n_svids = 1;
    r.pd.svids[0] = 0xFF01;
    r.pd.n_mfr = 3;
    r.pd.mfr[2] = 0xAB;
    r.pd.tx_fails = 7;
    r.usb.n = 1;
    r.usb.total = 300;
    r.usb.flags = usbfp::kFlagSof;
    r.usb.ev[0] = {12, std::uint8_t(usbfp::Ev::Setup), 0x80, 6, 0x0100, 0, 64};
    r.usb.ev[23] = {0xBEEF, 3, 1, 2, 3, 4, 5};
    return r;
}

void test_codec() {
    const Report r = sample_report();
    std::array<std::uint8_t, kReportBytes> buf{};
    CHECK_EQ(encode(r, buf), kReportBytes);
    CHECK_EQ(encode(r, std::span<std::uint8_t>(buf.data(), kReportBytes - 1)), 0u);
    // Fixed offsets from the documented layout.
    CHECK_EQ(buf[0], kVersion);
    CHECK_EQ(buf[1], std::uint8_t(JState::Running));
    CHECK_EQ(buf[2], 2);
    CHECK_EQ(buf[5], 4);
    CHECK_EQ(buf[6], '1');
    CHECK_EQ(buf[36], 0x04);   // start_ms, little endian
    CHECK_EQ(buf[39], 0x01);
    CHECK_EQ(buf[40], 2);      // cc
    CHECK_EQ(buf[41], 3);      // rp
    CHECK_EQ(buf[48], 2);      // pd.rev
    CHECK_EQ(buf[50], 0x2C);   // pdo 1, low byte
    CHECK_EQ(buf[kReportBytes - 1], 0);    // the last event's length, high byte
    CHECK_EQ(buf[kReportBytes - 2], 5);
    const auto d = decode(buf);
    CHECK_COND(d.has_value());
    if (d) {
        CHECK_EQ(d->boots, 2);
        CHECK_EQ(d->start_ms, 0x01020304u);
        CHECK_EQ(d->vbus_mv, 5070);
        CHECK_EQ(d->dm_mv, -1);
        CHECK_STR(d->fw.data(), "1.0.1-130-abcdef12");
        CHECK_EQ(d->pd.pdos[0], 0x0801912Cu);
        CHECK_EQ(d->pd.at(Q::DrBack).t_ms, 0x1234);
        CHECK_EQ(d->pd.at(Q::DrBack).ans, std::uint8_t(Ans::Ack));
        CHECK_EQ(d->pd.svids[0], 0xFF01);
        CHECK_EQ(d->pd.mfr[2], 0xAB);
        CHECK_EQ(d->pd.tx_fails, 7);
        CHECK_EQ(d->usb.total, 300);
        CHECK_EQ(d->usb.ev[0].length, 64);
        CHECK_EQ(d->usb.ev[23].t_ms, 0xBEEF);
        CHECK_EQ(d->usb.ev[23].length, 5);
    }
    buf[0] = kVersion + 1;
    CHECK_COND(!decode(buf).has_value());
    buf[0] = kVersion;
    CHECK_COND(!decode(std::span<const std::uint8_t>(buf.data(), kReportBytes - 1)).has_value());
}

void test_file() {
    const Report r = sample_report();
    std::array<std::uint8_t, kFileBytes> f{};
    CHECK_EQ(encode_file(r, f), kFileBytes);
    CHECK_EQ(f[0], 'L');
    CHECK_EQ(f[1], 'P');
    CHECK_EQ(f[2], 'H');
    CHECK_EQ(f[3], kVersion);
    CHECK_EQ(f[4] | f[5] << 8, int(kReportBytes));
    CHECK_COND(decode_file(f).has_value());
    f[100] ^= 1;
    CHECK_COND(!decode_file(f).has_value());   // the CRC catches it
}

void test_boot_decide() {
    Report r;
    CHECK_EQ(int(boot_decide(r)), int(Boot::Nothing));
    r.state = std::uint8_t(JState::Armed);
    CHECK_EQ(int(boot_decide(r)), int(Boot::Start));
    CHECK_EQ(r.state, std::uint8_t(JState::Running));
    CHECK_EQ(r.boots, 1);
    CHECK_EQ(int(boot_decide(r)), int(Boot::Resume));
    CHECK_EQ(r.boots, 2);
    CHECK_EQ(int(boot_decide(r)), int(Boot::Resume));
    CHECK_EQ(r.boots, 3);
    CHECK_EQ(int(boot_decide(r)), int(Boot::Closed));
    CHECK_EQ(r.state, std::uint8_t(JState::Done));
    CHECK_EQ(r.end, std::uint8_t(End::TooManyBoots));
    CHECK_EQ(int(boot_decide(r)), int(Boot::Nothing));
}

}  // namespace

// The shared fixture: tests/fixtures/host_report.hex is this sample as its flash file, which tests/test_host_report_rec.py
// decodes with the independent Python reader (tools/host_report_rec.py). A layout change shows up on both sides.
void test_fixture() {
    std::array<std::uint8_t, kFileBytes> f{};
    CHECK_EQ(encode_file(sample_report(), f), kFileBytes);
    std::string want;
    for (const std::uint8_t b : f) {
        static constexpr char kHex[] = "0123456789abcdef";
        want += kHex[b >> 4];
        want += kHex[b & 15];
    }
    std::ifstream in("tests/fixtures/host_report.hex");
    std::string got;
    std::getline(in, got);
    CHECK_COND(got == want);
    if (got != want) std::fprintf(stderr, "tests/fixtures/host_report.hex should be:\n%s\n", want.c_str());
}

// The Loop probe page's Host view: five rows of at most kRowChars characters from the record.
void test_rows() {
    Report r;
    std::array<char, kRowChars + 1> b{};
    const auto row = [&](std::size_t i) -> const char* {
        row_text(r, i, b);
        return b.data();
    };
    CHECK_STR(row_key(0), "Host");
    CHECK_STR(row_key(4), "USB");
    CHECK_STR(row(0), "--");
    CHECK_STR(row(1), "no caps");
    CHECK_STR(row(2), "not run");
    r.state = std::uint8_t(JState::Armed);
    CHECK_STR(row(0), "armed: replug");
    r.state = std::uint8_t(JState::Running);
    r.boots = 2;
    CHECK_STR(row(0), "running, boot 2");
    r.state = std::uint8_t(JState::Done);
    r.end = std::uint8_t(End::Done);
    CHECK_STR(row(0), "done");
    r.end = std::uint8_t(End::NoLoop);
    r.refuse = std::uint8_t(Refuse::NoHost);
    CHECK_STR(row(0), "no host on HID");
    r.pd.caps_seen = 3;
    r.pd.rev = 2;
    r.pd.n_pdos = 6;
    r.rp = 3;
    CHECK_STR(row(1), "PD3 6 PDOs Rp3A");
    r.pd.rev = 1;
    r.rp = 1;
    CHECK_STR(row(1), "PD2 6 PDOs Rp-");
    r.pd.at(Q::IdUfp).ans = std::uint8_t(Ans::NotSupported);
    CHECK_STR(row(2), "not supported");
    r.pd.at(Q::IdDfp).ans = std::uint8_t(Ans::Ack);
    r.pd.n_id = 4;
    r.pd.id[0] = 0x6C0005ACu;
    CHECK_STR(row(2), "VID 05AC");
    CHECK_STR(row(3), "not run");
    r.pd.at(Q::DrSwap).ans = std::uint8_t(Ans::Ack);
    CHECK_STR(row(3), "ack");
    r.os = std::uint8_t(usbfp::Os::ChargeOnly);
    r.usb.total = 300;
    CHECK_STR(row(4), "charge-only 300ev");
    for (std::size_t i = 0; i < kRows; i++) CHECK_COND(std::strlen(row(i)) <= kRowChars);
    row_text(r, kRows, b);
    CHECK_STR(row(kRows), "");
}

int main() {
    test_refusal();
    test_fixture();
    test_rows();
    test_codec();
    test_file();
    test_boot_decide();
    test_pd3_host_answers_as_ufp();
    test_pd2_host_swaps_to_dfp_and_back();
    test_swap_waits_for_usb();
    test_pd3_no_ufp_answer_then_dfp();
    test_refusals_are_recorded();
    test_busy_and_wait_are_retried();
    test_wait_exhausted();
    test_tx_failure_soft_resets_and_retries();
    test_soft_resets_bounded();
    test_host_hard_reset_drops_dfp();
    test_resume_never_repeats_the_due_question();
    test_no_contract_ends_the_run();
    test_waits_for_idle_line();
    return test_report("test_host_report");
}
