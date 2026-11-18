// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Host tests for the sink-initiated DR_Swap (Sink::request_dr_swap, the data role in our headers) and the OPPO (SVID 0x22D9) lab
// probe the sink runs on top of it (Sink::oppo_start, k2/pd/oppo_probe.hpp): the messages on the wire with their exact words, one
// AMS at a time (nothing of ours while an answer is outstanding), the time outs, the refusals and the role after resets.
#include <vector>

#include "k2/pd/pd_sink.hpp"
#include "test_util.h"

using namespace k2::pd;

namespace {

constexpr std::uint16_t kRoleBit = 1u << 5;   // header bit 5: Port Data Role (1 = DFP) on SOP

SourceCaps make_caps() {
    SourceCaps c;
    c.valid = true;
    c.spec_rev = 2;
    const std::uint32_t raw[] = {0x00019145, 0x0002D145, 0xC1A43241};   // 5 V 3.25 A, 9 V 3.25 A, PPS 5-21 V 3.25 A
    c.num_pdos = 3;
    for (int i = 0; i < 3; i++) c.pdos[i] = decode_pdo(raw[i]);
    return c;
}

std::uint32_t word(const TxMsg& m, std::size_t i) {
    return std::uint32_t(m.data[4 * i]) | std::uint32_t(m.data[4 * i + 1]) << 8 | std::uint32_t(m.data[4 * i + 2]) << 16 |
           std::uint32_t(m.data[4 * i + 3]) << 24;
}
unsigned type_of(const TxMsg& m) { return m.header & 0x1F; }
unsigned objs_of(const TxMsg& m) { return (m.header >> 12) & 7; }

struct Rig {
    SourceCaps caps = make_caps();
    Sink sink;
    std::uint32_t now = 1000;
    std::vector<TxMsg> sent;
    Rig() {
        sink.bind(&caps);
        sink.set_cap_mv(36000);
        sink.set_proactive_discover(false);
    }
    void pump() {
        TxMsg m;
        while (sink.take_tx(m)) { sent.push_back(m); sink.on_tx_result(true, now); }
    }
    void run(std::uint32_t ms) { for (std::uint32_t i = 0; i < ms; i += 5) { now += 5; sink.tick(now); pump(); } }
    // Tick until a new message goes out (or `ms` pass); returns it, or a default TxMsg (type 0) when none came.
    TxMsg next_tx(std::uint32_t ms = 500) {
        const std::size_t mark = sent.size();
        for (std::uint32_t i = 0; i < ms && sent.size() == mark; i += 5) { now += 5; sink.tick(now); pump(); }
        return sent.size() > mark ? sent[mark] : TxMsg{};
    }
    void ctl(std::uint8_t type) { sink.on_message(Sop::Sop, make_header(type, 0, 2) | (1u << 8), {}, now); pump(); }
    void data(std::uint8_t type, std::span<const std::uint32_t> objs) {
        std::array<std::uint8_t, 28> b{};
        for (std::size_t i = 0; i < objs.size(); i++)
            for (int k = 0; k < 4; k++) b[4 * i + k] = std::uint8_t(objs[i] >> (8 * k));
        sink.on_message(Sop::Sop, make_header(type, std::uint8_t(objs.size()), 2) | (1u << 8),
                        std::span<const std::uint8_t>(b.data(), 4 * objs.size()), now);
        pump();
    }
    void vdm(std::initializer_list<std::uint32_t> objs) { data(kMsgVendorDefined, std::span<const std::uint32_t>(objs.begin(), objs.size())); }
    void ready_at(std::uint8_t pos) {
        sink.trigger({pos, 0, 0}, now);
        pump();
        ctl(kCtlAccept);
        ctl(kCtlPsRdy);
    }
    std::size_t count(unsigned type) const {
        std::size_t n = 0;
        for (const TxMsg& m : sent) n += type_of(m) == type;
        return n;
    }
};

oppo::Config cfg(std::uint8_t steps = oppo::kStepsAll, bool skip_swap = false) {
    oppo::Config c;
    c.challenge = {0x11111111u, 0x22222222u, 0x33333333u, 0x44444444u};
    c.steps = steps;
    c.skip_swap = skip_swap;
    return c;
}

// ---- DR_Swap ----

void test_dr_swap_accept_makes_us_dfp() {
    Rig r;
    r.ready_at(1);
    CHECK_COND(!r.sink.dfp());
    CHECK_EQ(int(r.sink.request_dr_swap(true, r.now)), int(Err::Ok));
    r.pump();
    CHECK_EQ(type_of(r.sent.back()), kCtlDrSwap);
    CHECK_EQ(objs_of(r.sent.back()), 0);
    CHECK_EQ(r.sent.back().header & kRoleBit, 0);   // asked as UFP
    CHECK_EQ(int(r.sink.dr_swap()), int(DrSwap::Pending));
    r.ctl(kCtlAccept);
    CHECK_COND(r.sink.dfp());
    CHECK_EQ(int(r.sink.dr_swap()), int(DrSwap::Accepted));
    CHECK_EQ(int(r.sink.status().state), int(SinkState::Ready));   // the contract is untouched
    r.ctl(kCtlGetSinkCap);                                        // anything we send from now on says DFP
    CHECK_EQ(type_of(r.sent.back()), kDataSinkCap);
    CHECK_EQ(r.sent.back().header & kRoleBit, kRoleBit);
    CHECK_EQ(int(r.sink.request_dr_swap(true, r.now)), int(Err::BadChoice));   // already DFP
    // and back
    CHECK_EQ(int(r.sink.request_dr_swap(false, r.now)), int(Err::Ok));
    r.pump();
    CHECK_EQ(r.sent.back().header & kRoleBit, kRoleBit);   // the swap back still goes out as DFP
    r.ctl(kCtlAccept);
    CHECK_COND(!r.sink.dfp());
    r.ctl(kCtlGetSinkCap);
    CHECK_EQ(r.sent.back().header & kRoleBit, 0);
}

void test_dr_swap_refusals_keep_ufp() {
    const std::uint8_t answers[] = {kCtlReject, kCtlWait, kCtlNotSupported};
    const DrSwap results[] = {DrSwap::Rejected, DrSwap::Wait, DrSwap::NotSupported};
    for (int i = 0; i < 3; i++) {
        Rig r;
        r.ready_at(1);
        CHECK_EQ(int(r.sink.request_dr_swap(true, r.now)), int(Err::Ok));
        r.pump();
        r.ctl(answers[i]);
        CHECK_COND(!r.sink.dfp());
        CHECK_EQ(int(r.sink.dr_swap()), int(results[i]));
        CHECK_EQ(int(r.sink.status().state), int(SinkState::Ready));   // a refused swap is no failed Request
        CHECK_COND(r.sink.status().result != SinkResult::Rejected);
    }
}

void test_dr_swap_times_out_and_is_one_ams() {
    Rig r;
    r.ready_at(1);
    CHECK_EQ(int(r.sink.request_dr_swap(true, r.now)), int(Err::Ok));
    r.pump();
    const std::size_t n = r.sent.size();
    CHECK_EQ(int(r.sink.request_source_caps(r.now)), int(Err::Busy));   // nothing of ours while its answer is open
    CHECK_EQ(int(r.sink.request_dr_swap(true, r.now)), int(Err::Busy));
    r.run(kDrSwapMs - 10);
    CHECK_EQ(int(r.sink.dr_swap()), int(DrSwap::Pending));
    r.run(20);
    CHECK_EQ(int(r.sink.dr_swap()), int(DrSwap::Timeout));
    CHECK_COND(!r.sink.dfp());
    CHECK_EQ(r.sent.size(), n);   // a silent source: no Hard Reset, nothing at all
    CHECK_EQ(r.sink.status().hard_resets, 0);
}

void test_dr_swap_needs_a_contract() {
    Rig r;
    CHECK_EQ(int(r.sink.request_dr_swap(true, r.now)), int(Err::NoCaps));
    CHECK_COND(r.sent.empty());
    Rig i;
    i.ready_at(1);
    i.sink.set_answers_sop(false);   // another sink behind the K2: we never transmit
    CHECK_EQ(int(i.sink.request_dr_swap(true, i.now)), int(Err::Inline));
}

void test_resets_restore_the_role() {
    {   // the source's Hard Reset: both ends are back to their default roles
        Rig r;
        r.ready_at(1);
        r.sink.request_dr_swap(true, r.now);
        r.pump();
        r.ctl(kCtlAccept);
        CHECK_COND(r.sink.dfp());
        r.sink.on_hard_reset_rx(r.now);
        CHECK_COND(!r.sink.dfp());
        CHECK_EQ(int(r.sink.dr_swap()), int(DrSwap::None));
    }
    {   // our own Hard Reset on the wire
        Rig r;
        r.ready_at(1);
        r.sink.request_dr_swap(true, r.now);
        r.pump();
        r.ctl(kCtlAccept);
        r.sink.hard_reset(r.now);
        CHECK_COND(!r.sink.dfp());
    }
    {   // a detach
        Rig r;
        r.ready_at(1);
        r.sink.request_dr_swap(true, r.now);
        r.pump();
        r.ctl(kCtlAccept);
        r.sink.detach();
        CHECK_COND(!r.sink.dfp());
    }
    {   // Soft_Reset keeps the data role (USB PD: a Soft Reset does not change roles)
        Rig r;
        r.ready_at(1);
        r.sink.request_dr_swap(true, r.now);
        r.pump();
        r.ctl(kCtlAccept);
        r.ctl(kCtlSoftReset);
        CHECK_COND(r.sink.dfp());
        CHECK_EQ(type_of(r.sent.back()), kCtlAccept);
        CHECK_EQ(r.sent.back().header & kRoleBit, kRoleBit);
    }
    {   // a Hard Reset during the swap's answer window drops the pending swap
        Rig r;
        r.ready_at(1);
        r.sink.request_dr_swap(true, r.now);
        r.pump();
        r.sink.on_hard_reset_rx(r.now);
        CHECK_EQ(int(r.sink.dr_swap()), int(DrSwap::None));
        r.ctl(kCtlAccept);   // a late Accept is no swap
        CHECK_COND(!r.sink.dfp());
    }
}

// ---- the OPPO probe ----

// The brick that answers everything: Accept the swaps, AUTH ACK + 4 words, POWER and EXAPDO with one word each, its caps.
void test_probe_full_sequence() {
    Rig r;
    r.ready_at(1);
    CHECK_EQ(int(r.sink.oppo_start(cfg(), r.now)), int(Err::Ok));
    CHECK_COND(r.sink.oppo_running());

    TxMsg m = r.next_tx();
    CHECK_EQ(type_of(m), kCtlDrSwap);
    CHECK_EQ(m.header & kRoleBit, 0);
    r.ctl(kCtlAccept);
    CHECK_COND(r.sink.dfp());

    m = r.next_tx();   // AUTH: SVDM header + the 4 challenge words, 5 objects, as DFP
    CHECK_EQ(type_of(m), kMsgVendorDefined);
    CHECK_EQ(objs_of(m), 5);
    CHECK_EQ(m.header & kRoleBit, kRoleBit);
    CHECK_EQ(word(m, 0), 0x22D9A11Fu);
    CHECK_EQ(word(m, 1), 0x11111111u);
    CHECK_EQ(word(m, 4), 0x44444444u);
    r.vdm({0x22D9A15Fu, 0xA1A1A1A1u, 0xB2B2B2B2u, 0xC3C3C3C3u, 0xD4D4D4D4u});

    m = r.next_tx();   // POWER: unstructured cmd 1 + one zero word
    CHECK_EQ(type_of(m), kMsgVendorDefined);
    CHECK_EQ(objs_of(m), 2);
    CHECK_EQ(word(m, 0), 0x22D90001u);
    CHECK_EQ(word(m, 1), 0u);
    const std::size_t before_reply = r.sent.size();
    r.vdm({0x22D90001u, 0x07D00064u});
    CHECK_EQ(r.sent.size(), before_reply);   // the brick's unstructured answer is ours: no Not_Supported for it

    m = r.next_tx();   // EXAPDO: unstructured cmd 2 + one zero word
    CHECK_EQ(objs_of(m), 2);
    CHECK_EQ(word(m, 0), 0x22D90002u);
    CHECK_EQ(word(m, 1), 0u);
    r.vdm({0x22D90002u, 0x47000000u});

    m = r.next_tx();   // back to UFP
    CHECK_EQ(type_of(m), kCtlDrSwap);
    CHECK_EQ(m.header & kRoleBit, kRoleBit);
    r.ctl(kCtlAccept);
    CHECK_COND(!r.sink.dfp());

    m = r.next_tx();   // then the caps again
    CHECK_EQ(type_of(m), kCtlGetSourceCap);
    CHECK_EQ(m.header & kRoleBit, 0);
    const std::uint32_t after[] = {0x00019145, 0x0002D145, 0xC1A43264};   // the PPS current went up
    r.data(kDataSourceCap, after);
    CHECK_EQ(type_of(r.sent.back()), kDataRequest);   // answered like any list, inside tSenderResponse
    CHECK_COND(r.sink.oppo_running());               // the step ends with the contract back
    r.ctl(kCtlAccept);
    r.ctl(kCtlPsRdy);
    CHECK_COND(!r.sink.oppo_running());

    const oppo::Result& res = r.sink.oppo_result();
    CHECK_EQ(int(res.end), int(oppo::End::Done));
    CHECK_EQ(int(res.out[0]), int(oppo::Outcome::Accept));
    CHECK_EQ(int(res.out[1]), int(oppo::Outcome::Ack));
    CHECK_EQ(int(res.out[2]), int(oppo::Outcome::Reply));
    CHECK_EQ(int(res.out[3]), int(oppo::Outcome::Reply));
    CHECK_EQ(int(res.out[4]), int(oppo::Outcome::Accept));
    CHECK_EQ(int(res.out[5]), int(oppo::Outcome::Reply));
    CHECK_EQ(res.vdm[0].n, 5);
    CHECK_EQ(res.vdm[0].w[1], 0xA1A1A1A1u);
    CHECK_EQ(res.vdm[0].w[4], 0xD4D4D4D4u);
    CHECK_EQ(res.vdm[1].n, 2);
    CHECK_EQ(res.vdm[1].w[1], 0x07D00064u);
    CHECK_EQ(res.vdm[2].w[1], 0x47000000u);
    CHECK_EQ(res.before.n, 3);
    CHECK_EQ(res.before.w[2], 0xC1A43241u);
    CHECK_EQ(res.after.n, 3);
    CHECK_EQ(res.after.w[2], 0xC1A43264u);
    CHECK_EQ(res.diff, 0x04);
    CHECK_COND(!res.dfp);
    CHECK_EQ(r.sink.status().hard_resets, 0);
}

// One AMS at a time: while the AUTH answer is outstanding nothing of ours goes out (a source's own request is still answered),
// and a silent brick costs the step its time out, then the probe goes on.
void test_probe_waits_for_each_answer_then_times_out() {
    Rig r;
    r.ready_at(1);
    CHECK_EQ(int(r.sink.oppo_start(cfg(oppo::kStepAuth | oppo::kStepPower, true), r.now)), int(Err::Ok));
    TxMsg m = r.next_tx();
    CHECK_EQ(word(m, 0), 0x22D9A11Fu);   // --skip-swap: AUTH first, as UFP
    CHECK_EQ(m.header & kRoleBit, 0);
    const std::size_t n = r.sent.size();
    CHECK_EQ(int(r.sink.request_source_caps(r.now)), int(Err::Busy));
    CHECK_EQ(int(r.sink.trigger({2, 0, 0}, r.now)), int(Err::Busy));   // no new contract under the probe
    r.run(100);
    CHECK_EQ(r.sent.size(), n);
    r.ctl(kCtlGetSinkCap);              // the source asks: that answer goes out
    CHECK_EQ(r.sent.size(), n + 1);
    CHECK_EQ(type_of(r.sent.back()), kDataSinkCap);
    r.run(oppo::kVdmReplyMs - 120);
    CHECK_EQ(r.sent.size(), n + 1);     // still inside the AUTH window
    m = r.next_tx(400);
    CHECK_EQ(word(m, 0), 0x22D90001u);  // the time out moved on to POWER
    CHECK_EQ(int(r.sink.oppo_result().out[1]), int(oppo::Outcome::Timeout));
    CHECK_EQ(int(r.sink.oppo_result().out[0]), int(oppo::Outcome::Skipped));
    r.ctl(kCtlNotSupported);            // POWER refused
    r.run(200);
    CHECK_COND(!r.sink.oppo_running());
    const oppo::Result& res = r.sink.oppo_result();
    CHECK_EQ(int(res.out[2]), int(oppo::Outcome::NotSupported));
    CHECK_EQ(int(res.out[3]), int(oppo::Outcome::Skipped));
    CHECK_EQ(int(res.out[4]), int(oppo::Outcome::Skipped));   // never swapped
    CHECK_EQ(int(res.out[5]), int(oppo::Outcome::Skipped));
    CHECK_EQ(int(res.end), int(oppo::End::Done));
    CHECK_EQ(r.count(kCtlNotSupported), 0);
}

// The quiet line: a probe message never follows a frame by less than kGapMs.
void test_probe_keeps_a_gap_after_any_frame() {
    Rig r;
    r.ready_at(1);
    r.sink.oppo_start(cfg(oppo::kStepPower, true), r.now);
    const std::uint32_t t0 = r.now;   // the PS_RDY came at t0
    const TxMsg m = r.next_tx();
    CHECK_EQ(word(m, 0), 0x22D90001u);
    CHECK_COND(r.now - t0 >= oppo::kGapMs);
}

void test_probe_rejected_swap_still_asks_as_ufp() {
    Rig r;
    r.ready_at(1);
    r.sink.oppo_start(cfg(oppo::kStepAuth), r.now);
    TxMsg m = r.next_tx();
    CHECK_EQ(type_of(m), kCtlDrSwap);
    r.ctl(kCtlReject);
    m = r.next_tx();
    CHECK_EQ(word(m, 0), 0x22D9A11Fu);
    CHECK_EQ(m.header & kRoleBit, 0);
    r.vdm({0x22D9A19Fu});   // NAK
    r.run(300);
    const oppo::Result& res = r.sink.oppo_result();
    CHECK_EQ(int(res.out[0]), int(oppo::Outcome::Reject));
    CHECK_EQ(int(res.out[1]), int(oppo::Outcome::Nak));
    CHECK_EQ(int(res.out[4]), int(oppo::Outcome::Skipped));   // nothing to swap back
    CHECK_EQ(r.count(kCtlDrSwap), 1);
    CHECK_COND(!r.sink.oppo_running());
}

void test_probe_wait_retries_the_swap() {
    Rig r;
    r.ready_at(1);
    r.sink.oppo_start(cfg(0), r.now);
    for (int i = 0; i < oppo::kSwapTries; i++) {
        const TxMsg m = r.next_tx();
        CHECK_EQ(type_of(m), kCtlDrSwap);
        r.ctl(kCtlWait);
    }
    r.run(300);
    CHECK_EQ(r.count(kCtlDrSwap), oppo::kSwapTries);
    const oppo::Result& res = r.sink.oppo_result();
    CHECK_EQ(int(res.out[0]), int(oppo::Outcome::Wait));
    CHECK_EQ(res.swap_waits, oppo::kSwapTries);
    CHECK_COND(!r.sink.oppo_running());
}

void test_probe_ends_on_hard_reset_as_ufp() {
    Rig r;
    r.ready_at(1);
    r.sink.oppo_start(cfg(), r.now);
    r.next_tx();
    r.ctl(kCtlAccept);
    r.next_tx();   // AUTH out
    r.sink.on_hard_reset_rx(r.now);
    CHECK_COND(!r.sink.oppo_running());
    CHECK_COND(!r.sink.dfp());
    const oppo::Result& res = r.sink.oppo_result();
    CHECK_EQ(int(res.end), int(oppo::End::HardReset));
    CHECK_EQ(int(res.out[1]), int(oppo::Outcome::Lost));
    CHECK_COND(!res.dfp);
    const std::size_t n = r.sent.size();
    r.vdm({0x22D9A15Fu, 1, 2, 3, 4});   // a late answer is dropped quietly
    CHECK_EQ(r.sent.size(), n);
}

void test_probe_stop_and_soft_reset() {
    Rig r;
    r.ready_at(1);
    r.sink.oppo_start(cfg(), r.now);
    r.next_tx();
    r.ctl(kCtlAccept);
    r.sink.oppo_stop(r.now);
    CHECK_EQ(int(r.sink.oppo_result().end), int(oppo::End::Stopped));
    CHECK_COND(r.sink.oppo_result().dfp);   // stopped while DFP: the result says so
    Rig s;
    s.ready_at(1);
    s.sink.oppo_start(cfg(), s.now);
    s.next_tx();
    s.ctl(kCtlSoftReset);
    CHECK_EQ(int(s.sink.oppo_result().end), int(oppo::End::SoftReset));
}

void test_probe_start_refusals() {
    Rig none;
    CHECK_EQ(int(none.sink.oppo_start(cfg(), none.now)), int(Err::NoCaps));
    Rig nine;
    nine.ready_at(2);
    CHECK_EQ(int(nine.sink.oppo_start(cfg(), nine.now)), int(Err::BadVoltage));
    Rig twice;
    twice.ready_at(1);
    CHECK_EQ(int(twice.sink.oppo_start(cfg(), twice.now)), int(Err::Ok));
    CHECK_EQ(int(twice.sink.oppo_start(cfg(), twice.now)), int(Err::Busy));
    Rig in;
    in.ready_at(1);
    in.sink.set_answers_sop(false);
    CHECK_EQ(int(in.sink.oppo_start(cfg(), in.now)), int(Err::Inline));
    CHECK_COND(!in.sink.oppo_running());
}

// A brick that never re-announces after our Get_Source_Cap: the caps step times out, the probe ends.
void test_probe_caps_step_times_out() {
    Rig r;
    r.ready_at(1);
    r.sink.oppo_start(cfg(oppo::kStepCaps, true), r.now);
    const TxMsg m = r.next_tx();
    CHECK_EQ(type_of(m), kCtlGetSourceCap);
    r.run(oppo::kCapsMs + 50);
    CHECK_COND(!r.sink.oppo_running());
    CHECK_EQ(int(r.sink.oppo_result().out[5]), int(oppo::Outcome::Timeout));
    CHECK_EQ(r.sink.oppo_result().after.n, 0);
    CHECK_EQ(r.sink.status().hard_resets, 0);
}

}  // namespace

// A source that sends its caps while our DR_Swap waits ended that AMS: the Accept that follows is the Request's, never the swap's.
void test_dr_swap_ends_when_the_source_restarts() {
    Rig r;
    r.ready_at(1);
    CHECK_EQ(int(r.sink.request_dr_swap(true, r.now)), int(Err::Ok));
    r.pump();
    const std::uint32_t raw[] = {0x00019145, 0x0002D145, 0xC1A43241};
    r.data(kDataSourceCap, raw);
    CHECK_EQ(int(r.sink.dr_swap()), int(DrSwap::Timeout));
    CHECK_EQ(type_of(r.sent.back()), kDataRequest);
    r.ctl(kCtlAccept);
    CHECK_COND(!r.sink.dfp());
    r.ctl(kCtlPsRdy);
    CHECK_EQ(int(r.sink.status().state), int(SinkState::Ready));
}

// pd oppo-power: the advertised point pick_power finds goes into the Request as asked (current never raised).
void test_oppo_power_pick_goes_into_the_rdo() {
    Rig r;
    r.ready_at(1);
    const oppo::Pick f = oppo::pick_power(r.caps, 9000, 2000);
    CHECK_EQ(f.pos, 2);
    CHECK_EQ(int(r.sink.trigger({f.pos, f.mv, f.ma}, r.now)), int(Err::Ok));
    r.pump();
    CHECK_EQ(type_of(r.sent.back()), kDataRequest);
    std::uint32_t rdo = word(r.sent.back(), 0);
    CHECK_EQ(rdo >> 28, 2u);
    CHECK_EQ((rdo >> 10) & 0x3FF, 200u);   // operating 2 A in 10 mA units
    CHECK_EQ(rdo & 0x3FF, 200u);           // maximum the same
    r.ctl(kCtlAccept);
    r.ctl(kCtlPsRdy);
    const oppo::Pick p = oppo::pick_power(r.caps, 7000, 3000);
    CHECK_EQ(p.pos, 3);
    CHECK_EQ(int(r.sink.trigger({p.pos, p.mv, p.ma}, r.now)), int(Err::Ok));
    r.pump();
    rdo = word(r.sent.back(), 0);
    CHECK_EQ(rdo >> 28, 3u);
    CHECK_EQ((rdo >> 9) & 0xFFF, 350u);    // 7 V in 20 mV units
    CHECK_EQ(rdo & 0x7F, 60u);             // 3 A in 50 mA units
    CHECK_EQ(oppo::pick_power(r.caps, 7000, 4000).pos, 0);   // above the offer: nothing is asked
}

int main() {
    test_dr_swap_accept_makes_us_dfp();
    test_dr_swap_refusals_keep_ufp();
    test_dr_swap_times_out_and_is_one_ams();
    test_dr_swap_needs_a_contract();
    test_resets_restore_the_role();
    test_probe_full_sequence();
    test_probe_waits_for_each_answer_then_times_out();
    test_probe_keeps_a_gap_after_any_frame();
    test_probe_rejected_swap_still_asks_as_ufp();
    test_probe_wait_retries_the_swap();
    test_probe_ends_on_hard_reset_as_ufp();
    test_probe_stop_and_soft_reset();
    test_probe_start_refusals();
    test_probe_caps_step_times_out();
    test_dr_swap_ends_when_the_source_restarts();
    test_oppo_power_pick_goes_into_the_rdo();
    return test_report("test_pd_sink_oppo");
}
