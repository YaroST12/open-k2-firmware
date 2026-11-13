// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The inline interrogation sequencer (k2/pd/pd_interrogate.hpp). The wire is replayed from the bench capture of 2026-11-13
// (runs/clone-canon/reneg_pdtrace.txt: OnePlus 120 W charger, Canon dual-battery charger as the device, K2 inline): after a
// Hard Reset the charger lists 6 PDOs (id 0, PD 3.x, src/dfp), the Canon asks for PDO 4 (RDO 0x4344B12C, id 0, PD 3.x),
// Accept (id 1), PS_RDY (id 2), every message GoodCRC'd by the other side with a PD 2.0 header.
#include <string>
#include <vector>

#include "k2/pd/pd_interrogate.hpp"
#include "test_util.h"

using namespace k2::pd;

namespace {

constexpr std::uint16_t kSrc = 0x0120, kSnk = 0x0000;   // power role bit 8, data role bit 5
constexpr std::uint16_t hdr(std::uint8_t type, std::uint8_t ndo, std::uint8_t rev, std::uint16_t roles, std::uint8_t id,
                            bool ext = false) {
    return std::uint16_t(type | (rev << 6) | roles | ((id & 7) << 9) | (ndo << 12) | (ext ? 0x8000u : 0u));
}
constexpr std::uint32_t kCaps[6] = {0x0A01912Cu, 0x0002D12Cu, 0x0003C12Cu, 0x0004B12Cu, 0x0006412Cu, 0xC1A4323Cu};
constexpr std::uint32_t kCanonRdo = 0x4344B12Cu;

void put32(std::vector<std::uint8_t>& b, std::uint32_t w) {
    for (int i = 0; i < 4; i++) b.push_back(std::uint8_t(w >> (8 * i)));
}
std::uint32_t word(const TxMsg& m, unsigned i = 0) {
    return m.data[4 * i] | (m.data[4 * i + 1] << 8) | (m.data[4 * i + 2] << 16) | (std::uint32_t(m.data[4 * i + 3]) << 24);
}

struct Rig {
    Interrogate iq;
    std::uint32_t now = 10000;
    std::vector<TxMsg> sent;
    bool tx_ok = true;

    void rx(std::uint16_t header, std::vector<std::uint8_t> payload = {}) {
        iq.on_message(Sop::Sop, header, payload, now);
        now += 1;
    }
    // The capture's renegotiation, source ids starting at `id0`, the Request in `dev_rev`.
    void contract(std::uint8_t id0 = 0, std::uint8_t dev_rev = 2, std::uint32_t rdo = kCanonRdo) {
        std::vector<std::uint8_t> caps;
        for (std::uint32_t w : kCaps) put32(caps, w);
        rx(hdr(1, 6, 2, kSrc, id0), caps);
        rx(hdr(1, 0, 1, kSnk, id0));   // GoodCRC
        std::vector<std::uint8_t> req;
        put32(req, rdo);
        rx(hdr(2, 1, dev_rev, kSnk, 0), req);
        rx(hdr(1, 0, 1, kSrc, 0));
        rx(hdr(3, 0, 2, kSrc, std::uint8_t(id0 + 1)));   // Accept
        rx(hdr(1, 0, 1, kSnk, std::uint8_t(id0 + 1)));
        rx(hdr(6, 0, 2, kSrc, std::uint8_t(id0 + 2)));   // PS_RDY
        rx(hdr(1, 0, 1, kSnk, std::uint8_t(id0 + 2)));
    }
    IqRefusal start() {
        IqFacts f;
        f.cc = f.inline_link = f.confirmed = true;
        return iq.start(f, now);
    }
    // One 5 ms tick; true when something went out.
    bool step() {
        iq.tick(now);
        TxMsg m;
        bool out = false;
        if (iq.take_tx(m)) {
            sent.push_back(m);
            if (!m.hard_reset) iq.on_tx_result(tx_ok, now);
            out = true;
        }
        now += 5;
        return out;
    }
    bool next(unsigned limit_ms = 6000) {
        for (unsigned t = 0; t < limit_ms; t += 5)
            if (step()) return true;
        return false;
    }
    const TxMsg& last() const {
        static const TxMsg none{};
        return sent.empty() ? none : sent.back();
    }
    unsigned last_id() const { return (last().header >> 9) & 7; }
    unsigned last_type() const { return last().header & 0x1F; }
    // The device's answers (sink role, its own MessageIDs).
    std::uint8_t dev_id = 3;
    void dev(std::uint8_t type, std::vector<std::uint32_t> words, std::uint8_t rev = 2) {
        std::vector<std::uint8_t> p;
        for (std::uint32_t w : words) put32(p, w);
        rx(hdr(1, 0, 1, kSrc, dev_id));   // the charger's GoodCRC of it
        iq.on_message(Sop::Sop, hdr(type, std::uint8_t(words.size()), rev, kSnk, dev_id++), p, now);
        now += 1;
    }
    void dev_ctl(std::uint8_t type) {
        iq.on_message(Sop::Sop, hdr(type, 0, 2, kSnk, dev_id++), {}, now);
        now += 1;
    }
    void dev_goodcrc(std::uint8_t id) { rx(hdr(1, 0, 1, kSnk, id)); }
    // A Discover Identity ACK: ID Header (VID `vid`, modal bit when `modal`), cert stat, product VDO (PID).
    void identity_ack(std::uint16_t vid, std::uint16_t pid, bool modal = false, std::uint8_t rev = 2) {
        const std::uint32_t idh = (2u << 27) | (modal ? 1u << 26 : 0u) | vid;   // UFP peripheral
        dev(kMsgVendorDefined, {0xFF00A041u, idh, 0u, (std::uint32_t(pid) << 16) | 0x0100u}, rev);
    }
    void sink_ext_ack() {
        std::vector<std::uint8_t> p = {24, 0x80};   // ext header: data size 24, chunked, chunk 0
        for (int i = 0; i < 24; i++) p.push_back(std::uint8_t(i + 1));
        p.push_back(0);
        p.push_back(0);   // pad to 7 objects
        rx(hdr(1, 0, 1, kSrc, dev_id));
        iq.on_message(Sop::Sop, hdr(kExtSinkCapsExt, 7, 2, kSnk, dev_id++, true), p, now);
        now += 1;
    }
    // One chunk of an EPR_Sink_Capabilities answer: ext header (total `size`, chunk number) + `data`, padded to whole objects.
    void epr_chunk(unsigned chunk, unsigned size, const std::vector<std::uint8_t>& data) {
        std::vector<std::uint8_t> p;
        const std::uint16_t eh = std::uint16_t(size | (chunk << 11) | 0x8000u);
        p.push_back(std::uint8_t(eh));
        p.push_back(std::uint8_t(eh >> 8));
        p.insert(p.end(), data.begin(), data.end());
        while (p.size() % 4) p.push_back(0);
        rx(hdr(1, 0, 1, kSrc, dev_id));   // the charger's GoodCRC
        iq.on_message(Sop::Sop, hdr(kExtEprSinkCaps, std::uint8_t(p.size() / 4), 2, kSnk, dev_id++, true), p, now);
        now += 1;
    }
    // The charger's SOP' frame: a Discover Identity request to the cable (structured SVDM request, command 1).
    void sopp_query() {
        std::vector<std::uint8_t> p;
        put32(p, 0xFF00A001u);
        iq.on_message(Sop::SopP, hdr(kMsgVendorDefined, 1, 2, kSrc, 0), p, now);
        now += 1;
    }
    // A real e-marker chip in the charger's cable answers the query: Discover Identity ACK on SOP'.
    void sopp_real_answer() {
        std::vector<std::uint8_t> p;
        put32(p, 0xFF00A041u);
        iq.on_message(Sop::SopP, hdr(kMsgVendorDefined, 1, 2, kSrc, 1), p, now);
        now += 1;
    }
    // Run until the run is over (or `limit_ms`), answering nothing.
    void drain(unsigned limit_ms = 20000) {
        for (unsigned t = 0; t < limit_ms && iq.running(); t += 5) step();
    }
};

constexpr std::uint32_t kSinkPdo1 = 0x0001912Cu, kSinkPdo2 = 0x0004B12Cu;   // 5 V 3 A, 15 V 3 A

}  // namespace

int main() {
    // ---- a fresh sniffed contract: no first reset, the first question as the source, MessageID after PS_RDY's ----
    {
        Rig r;
        r.contract();
        CHECK_EQ(int(r.start()), int(IqRefusal::None));
        CHECK_EQ(int(r.iq.status().phase), int(IqPhase::Settle));
        CHECK_EQ(bool(r.iq.status().skipped_first_reset), true);
        CHECK_EQ(bool(r.iq.status().first.ready), true);
        CHECK_EQ(int(r.iq.status().first.pos), 4);
        CHECK_EQ(r.iq.status().first.rdo, kCanonRdo);
        CHECK_EQ(r.iq.status().first.src_pdo, kCaps[3]);
        const std::uint32_t t0 = r.now;
        CHECK_EQ(bool(r.next()), true);
        CHECK_COND(r.now - t0 >= kIqSettleMs);
        CHECK_EQ(bool(r.last().hard_reset), false);
        CHECK_EQ(bool(r.last().inline_tx), true);
        CHECK_EQ(int(r.last().header & 0x0120), int(kSrc));    // source, DFP
        CHECK_EQ(int((r.last().header >> 6) & 3), 2);          // PD 3.x, the contract's revision
        CHECK_EQ(r.last_id(), 3u);                             // PS_RDY was id 2
        CHECK_EQ(r.last_type(), unsigned(kCtlGetSinkCap));     // the plain questions first: a VDM is the one a source may resent
        CHECK_EQ(int(r.iq.status().phase), int(IqPhase::Ask));
    }

    // ---- the whole round: every answer kept, one fresh MessageID per question, the final reset, the contract compared ----
    {
        Rig r;
        r.contract();
        r.start();
        CHECK_EQ(bool(r.next()), true);                     // Get_Sink_Cap, id 3
        CHECK_EQ(r.last_type(), unsigned(kCtlGetSinkCap));
        r.dev_goodcrc(3);
        r.dev(kDataSinkCap, {kSinkPdo1, kSinkPdo2});
        CHECK_EQ(int(r.iq.status().dev_acks), 1);
        CHECK_EQ(bool(r.next()), true);
        CHECK_EQ(r.last_type(), unsigned(kCtlGetSinkCapExt));
        CHECK_EQ(r.last_id(), 4u);
        CHECK_EQ(int(r.last().header & 0x0120), int(kSrc));
        r.sink_ext_ack();
        CHECK_EQ(bool(r.next()), true);
        CHECK_EQ(r.last_type(), unsigned(kCtlGetRevision));
        CHECK_EQ(r.last_id(), 5u);
        r.dev_ctl(kCtlNotSupported);                        // a PD 3.0 device: no Revision message
        CHECK_EQ(bool(r.next()), true);                     // EPR_Get_Sink_Cap (a plain question), id 6
        CHECK_EQ(r.last_type(), unsigned(kExtControl));
        CHECK_EQ(r.last_id(), 6u);
        r.dev_ctl(kCtlNotSupported);                        // not an EPR sink
        CHECK_EQ(bool(r.next()), true);                     // the VDM last: Discover Identity, id 7
        CHECK_EQ(word(r.last()), 0xFF00A001u);              // SVDM 2.0
        CHECK_EQ(r.last_id(), 7u);
        r.identity_ack(0x04A9, 0x1234);                     // not modal: no SVIDs, no Modes
        CHECK_EQ(bool(r.next()), true);                     // nothing left to ask: the final Hard Reset
        CHECK_EQ(bool(r.last().hard_reset), true);
        CHECK_EQ(int(r.iq.status().phase), int(IqPhase::Sniff2));
        r.contract();
        r.step();
        const IqStatus& s = r.iq.status();
        CHECK_EQ(int(s.phase), int(IqPhase::Done));
        CHECK_EQ(int(s.why), int(IqWhy::None));
        CHECK_EQ(int(s.match), int(IqMatch::Same));
        CHECK_EQ(int(s.sent), 5);
        CHECK_EQ(int(s.resets_sent), 1);
        CHECK_EQ(int(s.ok_mask), int(iq_bit(IqStep::Identity) | iq_bit(IqStep::SinkCap) | iq_bit(IqStep::SinkCapExt)));
        CHECK_EQ(int(s.refused_mask), int(iq_bit(IqStep::Revision) | iq_bit(IqStep::EprSinkCap)));
        CHECK_EQ(bool(s.id.valid), true);
        CHECK_EQ(int(s.id.vid), 0x04A9);
        CHECK_EQ(int(s.id.pid), 0x1234);
        CHECK_EQ(int(s.num_sink_pdos), 2);
        CHECK_EQ(s.sink_pdos[1], kSinkPdo2);
        CHECK_EQ(int(s.sink_ext_len), 24);
        CHECK_EQ(int(s.sink_ext[0]), 1);
        CHECK_EQ(int(s.sink_ext[23]), 24);
        CHECK_EQ(s.second.rdo, kCanonRdo);
        CHECK_EQ(bool(r.iq.running()), false);
    }

    // ---- MessageID: the next after the newest source-role message, the charger's own included; it wraps 7 -> 0 ----
    {
        Rig r;
        r.contract(5);                                      // caps id 5, Accept 6, PS_RDY 7
        r.start();
        CHECK_EQ(bool(r.next()), true);
        CHECK_EQ(r.last_id(), 0u);                          // 7 + 1 wraps
        r.dev(kDataSinkCap, {kSinkPdo1});
        // The charger starts something of its own (a Discover Identity request, id 1) before our next question.
        std::vector<std::uint8_t> di;
        put32(di, 0xFF00A001u);
        r.rx(hdr(kMsgVendorDefined, 1, 2, kSrc, 1), di);
        CHECK_EQ(bool(r.next()), true);
        CHECK_EQ(r.last_type(), unsigned(kCtlGetSinkCapExt));
        CHECK_EQ(r.last_id(), 2u);
        r.dev_ctl(kCtlNotSupported);
        CHECK_EQ(bool(r.next()), true);
        CHECK_EQ(r.last_type(), unsigned(kCtlGetRevision));
        CHECK_EQ(r.last_id(), 3u);
        // The charger answering our question itself (a source-role Revision, as on the bench) is no answer: Get_Revision
        // times out and is asked again.
        std::vector<std::uint8_t> rv;
        put32(rv, 0x30000000u);
        r.rx(hdr(kDataRevision, 1, 2, kSrc, 4), rv);
        CHECK_EQ(r.iq.status().revision, 0u);
        CHECK_EQ(bool(r.next()), true);
        CHECK_EQ(r.last_type(), unsigned(kCtlGetRevision));
        CHECK_EQ(r.last_id(), 5u);                          // fresh id: after the charger's 4
        CHECK_EQ(int(r.iq.status().q.result), int(IqResult::ChargerOnly));
    }

    // ---- silence: one retry with a fresh id, then the step is marked silent and the next one is asked ----
    {
        Rig r;
        r.contract();
        r.start();
        r.tx_ok = false;                                    // collided GoodCRCs: the transmit result never counts
        CHECK_EQ(bool(r.next()), true);
        CHECK_EQ(r.last_id(), 3u);
        const std::uint32_t t0 = r.now;
        CHECK_EQ(bool(r.next()), true);                     // Get_Sink_Cap again
        CHECK_COND(r.now - t0 >= kIqRespMs);
        CHECK_EQ(r.last_type(), unsigned(kCtlGetSinkCap));
        CHECK_EQ(r.last_id(), 4u);
        CHECK_EQ(bool(r.next()), true);
        CHECK_EQ(r.last_type(), unsigned(kCtlGetSinkCapExt));
        CHECK_EQ(r.last_id(), 5u);
        CHECK_EQ(int(r.iq.status().silent_mask), int(iq_bit(IqStep::SinkCap)));
        CHECK_EQ(int(r.iq.status().asked_mask), int(iq_bit(IqStep::SinkCap) | iq_bit(IqStep::SinkCapExt)));
    }

    // ---- what came back after each question: the sink's answer, the charger's, a bare GoodCRC, or nothing ----
    {
        Rig r;
        r.contract();
        r.start();
        r.next();                                           // Get_Sink_Cap, id 3
        CHECK_EQ(int(r.iq.status().closed), 0);
        r.dev(kDataSinkCap, {kSinkPdo1});
        const IqQuestion& q = r.iq.status().q;
        CHECK_EQ(int(r.iq.status().closed), 1);
        CHECK_EQ(int(q.result), int(IqResult::Answered));
        CHECK_EQ(int(q.step), int(IqStep::SinkCap));
        CHECK_EQ(int(q.id), 3);
        CHECK_EQ(int(q.attempt), 1);
        CHECK_EQ(int(q.dev_msgs), 1);
        r.next();                                           // Get_Sink_Cap_Extended, id 4: the device only GoodCRCs
        r.dev_goodcrc(4);
        r.next();                                           // asked again after the window
        CHECK_EQ(int(r.iq.status().closed), 2);
        CHECK_EQ(int(q.result), int(IqResult::GoodCrcOnly));
        CHECK_EQ(int(q.step), int(IqStep::SinkCapExt));
        CHECK_EQ(int(q.dev_msgs), 0);
        CHECK_EQ(bool(q.dev_goodcrc), true);
        r.next();                                           // the second attempt got nothing at all; Get_Revision, id 6
        CHECK_EQ(int(r.iq.status().closed), 3);
        CHECK_EQ(int(q.result), int(IqResult::Nothing));
        CHECK_EQ(int(q.attempt), 2);
        CHECK_EQ(r.last_type(), unsigned(kCtlGetRevision));
        r.dev_ctl(kCtlNotSupported);
        CHECK_EQ(int(q.result), int(IqResult::Refused));
        CHECK_EQ(int(r.iq.status().closed), 4);
    }

    // ---- no fresh contract: the first Hard Reset, the renegotiation sniffed, then the questions ----
    {
        Rig r;
        CHECK_EQ(int(r.start()), int(IqRefusal::None));
        CHECK_EQ(int(r.iq.status().phase), int(IqPhase::Reset1));
        CHECK_EQ(bool(r.next()), true);
        CHECK_EQ(bool(r.last().hard_reset), true);
        CHECK_EQ(int(r.iq.status().phase), int(IqPhase::Sniff1));
        CHECK_EQ(bool(r.iq.status().skipped_first_reset), false);
        r.now += 1100;                                      // the charger drops VBUS and comes back (bench: ~1 s)
        r.contract();
        CHECK_EQ(bool(r.next()), true);
        CHECK_EQ(r.last_type(), unsigned(kCtlGetSinkCap));
        CHECK_EQ(r.last_id(), 3u);
        CHECK_EQ(int(r.iq.status().first.pos), 4);
        CHECK_EQ(int(r.iq.status().resets_sent), 1);
    }

    // ---- the reset gets no renegotiation: aborted, nothing asked, no second reset ----
    {
        Rig r;
        r.start();
        r.next();
        r.drain();
        CHECK_EQ(int(r.iq.status().phase), int(IqPhase::Aborted));
        CHECK_EQ(int(r.iq.status().why), int(IqWhy::NoContract));
        CHECK_EQ(int(r.iq.status().resets_sent), 1);
        CHECK_EQ(int(r.iq.status().sent), 0);
    }

    // ---- a PD 2.0 device: SVDM 1.0, no Get_Sink_Cap_Extended, no Get_Revision ----
    {
        Rig r;
        r.contract(0, 1);
        r.start();
        CHECK_EQ(bool(r.next()), true);
        CHECK_EQ(r.last_type(), unsigned(kCtlGetSinkCap));
        CHECK_EQ(int((r.last().header >> 6) & 3), 1);
        r.dev(kDataSinkCap, {kSinkPdo1}, 1);
        CHECK_EQ(bool(r.next()), true);                     // no Ext, no Revision: straight to the VDM
        CHECK_EQ(word(r.last()), 0xFF008001u);
        CHECK_EQ(int((r.last().header >> 6) & 3), 1);
        r.dev_ctl(kCtlNotSupported);                        // whatever the device says to a VDM it does not do
        CHECK_EQ(bool(r.next()), true);
        CHECK_EQ(bool(r.last().hard_reset), true);
        CHECK_EQ(int(r.iq.status().rev), 1);
    }

    // ---- modal Identity: Discover SVIDs, then Discover Modes, DisplayPort first ----
    {
        Rig r;
        r.contract();
        r.start();
        r.next();
        r.dev(kDataSinkCap, {kSinkPdo1});
        r.next();
        r.dev_ctl(kCtlNotSupported);   // no Sink_Capabilities_Extended
        r.next();
        r.dev_ctl(kCtlNotSupported);   // no Revision
        r.next();
        r.dev_ctl(kCtlNotSupported);   // no EPR sink
        CHECK_EQ(bool(r.next()), true);
        CHECK_EQ(word(r.last()), 0xFF00A001u);   // the VDMs last: Discover Identity
        r.identity_ack(0x05AC, 0x7513, true);
        CHECK_EQ(bool(r.next()), true);
        CHECK_EQ(word(r.last()), 0xFF00A002u);
        r.dev(kMsgVendorDefined, {0xFF00A042u, 0x05ACFF01u});
        CHECK_EQ(bool(r.next()), true);
        CHECK_EQ(word(r.last()), 0xFF01A003u);
        r.dev(kMsgVendorDefined, {0xFF01A043u, 0x000C0045u});
        CHECK_EQ(bool(r.next()), true);
        CHECK_EQ(word(r.last()), 0x05ACA003u);
        r.dev(kMsgVendorDefined, {0x05ACA083u});            // NAK
        CHECK_EQ(bool(r.next()), true);
        CHECK_EQ(bool(r.last().hard_reset), true);
        const IqStatus& s = r.iq.status();
        CHECK_EQ(int(s.num_svids), 2);
        CHECK_EQ(int(s.num_modes), 1);
        CHECK_EQ(int(s.mode_svid[0]), 0xFF01);
        CHECK_EQ(s.mode_vdo[0], 0x000C0045u);
        CHECK_COND(s.ok_mask & iq_bit(IqStep::Modes));
    }

    // ---- the device answered the charger's own questions during the settle: those are not asked again ----
    {
        Rig r;
        r.contract();
        r.start();
        r.step();
        std::vector<std::uint8_t> di;
        put32(di, 0xFF00A001u);
        r.rx(hdr(kMsgVendorDefined, 1, 2, kSrc, 3), di);   // the charger's Discover Identity
        r.identity_ack(0x2B01, 0x0001);
        CHECK_EQ(int(r.iq.status().phase), int(IqPhase::Settle));
        CHECK_EQ(bool(r.next()), true);
        CHECK_EQ(r.last_type(), unsigned(kCtlGetSinkCap));
        CHECK_EQ(r.last_id(), 4u);
        // Identity is known already: it is never asked, whatever the order.
        r.dev(kDataSinkCap, {kSinkPdo1});
        r.next();
        r.dev_ctl(kCtlNotSupported);
        r.next();
        r.dev_ctl(kCtlNotSupported);
        r.next();
        r.dev_ctl(kCtlNotSupported);   // no EPR sink
        CHECK_EQ(bool(r.next()), true);
        CHECK_EQ(bool(r.last().hard_reset), true);
        CHECK_EQ(int(r.iq.status().asked_mask),
                 int(iq_bit(IqStep::SinkCap) | iq_bit(IqStep::SinkCapExt) | iq_bit(IqStep::Revision) | iq_bit(IqStep::EprSinkCap)));
    }

    // ---- a Hard Reset from the charger that comes long after our last question: re-sniff and go on; a third one ends the run ----
    {
        Rig r;
        r.contract();
        r.start();
        r.next();
        r.now += kIqBlameMs + 500;                          // nothing to do with our question
        r.iq.on_hard_reset(r.now);
        CHECK_EQ(int(r.iq.status().phase), int(IqPhase::Sniff1));
        r.contract();
        CHECK_EQ(bool(r.next()), true);
        CHECK_EQ(r.last_type(), unsigned(kCtlGetSinkCap));  // the question that was cut is asked again
        CHECK_EQ(r.last_id(), 3u);
        r.now += kIqBlameMs + 500;
        r.iq.on_hard_reset(r.now);
        r.contract();
        r.next();
        r.now += kIqBlameMs + 500;
        r.iq.on_hard_reset(r.now);
        CHECK_EQ(int(r.iq.status().phase), int(IqPhase::Aborted));
        CHECK_EQ(int(r.iq.status().why), int(IqWhy::Resets));
        CHECK_EQ(int(r.iq.status().resets_seen), 3);
    }

    // ---- a source that Hard Resets right after our question (a laptop port on the bench: within 3 ms of the Discover Identity,
    //      then again, every ~650 ms): the first one ends the run. No second question, no final reset of ours, no resync loop. ----
    {
        Rig r;
        r.contract();
        r.start();
        r.next();
        r.dev(kDataSinkCap, {kSinkPdo1});                   // what the device did say is kept
        r.next();                                           // Get_Sink_Cap_Extended
        r.now += 3;
        r.iq.on_hard_reset(r.now);
        const IqStatus& s = r.iq.status();
        CHECK_EQ(int(s.phase), int(IqPhase::Aborted));
        CHECK_EQ(int(s.why), int(IqWhy::SourceResets));
        CHECK_EQ(int(s.resets_seen), 1);
        CHECK_EQ(int(s.resets_sent), 0);
        CHECK_EQ(int(s.num_sink_pdos), 1);
        CHECK_EQ(int(s.q.result), int(IqResult::SourceReset));
        CHECK_EQ(int(s.q.step), int(IqStep::SinkCapExt));
        CHECK_EQ(bool(r.iq.running()), false);
        const std::size_t n = r.sent.size();
        r.contract();                                       // the renegotiation after it, and the escalation 7.5 ms later
        r.iq.on_hard_reset(r.now + 8);
        CHECK_EQ(bool(r.next(8000)), false);
        CHECK_EQ(r.sent.size(), n);
        CHECK_EQ(int(r.iq.status().why), int(IqWhy::SourceResets));
        CHECK_EQ(int(r.iq.status().resets_seen), 1);

        // The very first question already provokes it.
        Rig d;
        d.contract();
        d.start();
        d.next();
        d.now += 1;
        d.iq.on_hard_reset(d.now);
        CHECK_EQ(int(d.iq.status().why), int(IqWhy::SourceResets));
        CHECK_EQ(int(d.iq.status().resets_sent), 0);
        CHECK_EQ(bool(d.next(2000)), false);
    }

    // ---- the device and the charger talk to each other after our question (laptop on the bench, 2026-11-13: its VCONN_Swap
    //      came 125 ms after our Get_Sink_Cap, then the charger's Not_Supported, Soft_Resets and a Hard Reset): that reset
    //      follows their exchange, it is not "charger resets when asked". The run resyncs and asks again within the budget. ----
    {
        Rig r;
        r.contract();
        r.start();
        r.next();                                           // Get_Sink_Cap
        r.now += 125;
        r.dev_ctl(11);                                      // the device's VCONN_Swap to the charger
        r.now += 12;
        r.iq.on_hard_reset(r.now);
        const IqStatus& s = r.iq.status();
        CHECK_COND(s.why != IqWhy::SourceResets);
        CHECK_EQ(bool(r.iq.running()), true);
        CHECK_EQ(int(s.resets_seen), 1);
        CHECK_EQ(int(s.resets_sent), 0);
        r.contract();                                       // the renegotiation after it
        CHECK_EQ(bool(r.next(8000)), true);                 // asked again
        CHECK_EQ(r.last_type(), 8u);

        // The same reset with nothing said by the device is still the question's doing (the rule above stays).
        Rig q;
        q.contract();
        q.start();
        q.next();
        q.now += 125;
        q.iq.on_hard_reset(q.now);
        CHECK_EQ(int(q.iq.status().why), int(IqWhy::SourceResets));

        // An exchange before the question does not excuse a reset after it.
        Rig p;
        p.contract();
        p.start();
        p.dev_ctl(11);
        p.next();
        p.now += 5;
        p.iq.on_hard_reset(p.now);
        CHECK_EQ(int(p.iq.status().why), int(IqWhy::SourceResets));
    }

    // ---- the charger Soft_Resets the device mid-run (an answer it did not expect): wait for their new contract, go on, and
    //      the final reset is still owed ----
    {
        Rig r;
        r.contract();
        r.start();
        r.next();
        r.dev_ctl(kCtlNotSupported);                        // Get_Sink_Cap refused
        r.rx(hdr(kCtlSoftReset, 0, 2, kSrc, 0));
        r.dev_ctl(kCtlAccept);
        r.step();
        CHECK_EQ(int(r.iq.status().phase), int(IqPhase::Sniff1));
        r.contract();
        CHECK_EQ(bool(r.next()), true);
        CHECK_EQ(r.last_type(), unsigned(kCtlGetSinkCapExt));
        CHECK_EQ(r.last_id(), 3u);
        r.dev_ctl(kCtlNotSupported);
        r.next();
        r.dev_ctl(kCtlNotSupported);
        r.next();                                           // EPR_Get_Sink_Cap
        r.dev_ctl(kCtlNotSupported);
        r.next();                                           // Discover Identity
        r.dev_ctl(kCtlNotSupported);
        CHECK_EQ(bool(r.next()), true);
        CHECK_EQ(bool(r.last().hard_reset), true);
    }

    // ---- a stop after a question: the final reset still goes, the answers stay; a detach ends at once ----
    {
        Rig r;
        r.contract();
        r.start();
        r.next();
        r.identity_ack(0x04A9, 0x1234);
        r.iq.abort(IqWhy::Stopped, r.now);
        CHECK_EQ(int(r.iq.status().phase), int(IqPhase::Reset2));
        CHECK_EQ(bool(r.next()), true);
        CHECK_EQ(bool(r.last().hard_reset), true);
        r.contract();
        r.step();
        CHECK_EQ(int(r.iq.status().phase), int(IqPhase::Aborted));
        CHECK_EQ(int(r.iq.status().why), int(IqWhy::Stopped));
        CHECK_EQ(int(r.iq.status().match), int(IqMatch::Same));
        CHECK_EQ(int(r.iq.status().id.vid), 0x04A9);

        Rig d;
        d.contract();
        d.start();
        d.next();
        d.iq.abort(IqWhy::Detached, d.now);
        CHECK_EQ(int(d.iq.status().phase), int(IqPhase::Aborted));
        CHECK_EQ(bool(d.next(500)), false);
    }

    // ---- the second contract differs, or never comes ----
    {
        Rig r;
        r.contract();
        r.start();
        r.next();
        r.dev_ctl(kCtlNotSupported);
        for (int i = 0; i < 4; i++) {
            r.next();
            r.dev_ctl(kCtlNotSupported);
        }
        CHECK_EQ(bool(r.next()), true);
        CHECK_EQ(bool(r.last().hard_reset), true);
        r.contract(0, 2, 0x2304B12Cu);                      // PDO 2 this time
        r.step();
        CHECK_EQ(int(r.iq.status().match), int(IqMatch::Differs));

        Rig q;
        q.contract();
        q.start();
        q.next();
        q.iq.abort(IqWhy::Stopped, q.now);
        q.drain();
        CHECK_EQ(int(q.iq.status().match), int(IqMatch::NoSecond));
        CHECK_COND(q.iq.status().ended_at - q.iq.status().started_at <= kIqTotalMs + kIqRenegMs);
    }

    // ---- the time limit: no question after kIqTotalMs; the final reset still goes ----
    {
        Rig r;
        r.contract();
        r.start();
        r.drain(kIqTotalMs + kIqRenegMs + 1000);
        const IqStatus& s = r.iq.status();
        CHECK_EQ(int(s.phase), int(IqPhase::Done));           // every step asked twice, all silent: done, nothing learned
        CHECK_EQ(int(s.silent_mask), int(iq_bit(IqStep::Identity) | iq_bit(IqStep::SinkCap) | iq_bit(IqStep::SinkCapExt) |
                                          iq_bit(IqStep::Revision) | iq_bit(IqStep::EprSinkCap)));
        CHECK_EQ(int(s.resets_sent), 1);
        CHECK_COND(s.ended_at - s.started_at <= kIqTotalMs + kIqRenegMs + 10);
    }

    // ---- a source that renegotiates after our question (laptop port, bench 2026-11-13: nobody GoodCRCs the question, ~130 ms
    //      later Source_Capabilities id 0 and a fresh Request/Accept/PS_RDY; then the laptop's own Discover Identity, which the
    //      Canon NAKs): the run ends at once, and the NAK is not blamed on our question ----
    {
        std::vector<std::uint8_t> caps, di, nak;
        for (std::uint32_t w : kCaps) put32(caps, w);
        put32(di, 0xFF00A001u);
        put32(nak, 0xFF00A081u);
        Rig r;
        r.contract();
        r.start();
        r.next();                                           // Get_Sink_Cap, id 3: no GoodCRC, no answer
        r.now += 130;
        r.rx(hdr(1, 6, 2, kSrc, 0), caps);
        const IqStatus& s = r.iq.status();
        CHECK_EQ(int(s.phase), int(IqPhase::Aborted));
        CHECK_EQ(int(s.why), int(IqWhy::SourceRestarts));
        CHECK_EQ(int(s.q.result), int(IqResult::SourceRestart));
        CHECK_EQ(int(s.q.step), int(IqStep::SinkCap));
        CHECK_EQ(int(s.q.dev_msgs), 0);
        CHECK_EQ(int(s.resets_sent), 0);
        CHECK_EQ(int(s.resets_seen), 0);                    // a restart is not a Hard Reset
        CHECK_EQ(bool(r.iq.running()), false);
        r.rx(hdr(kMsgVendorDefined, 1, 2, kSrc, 1), di);    // the laptop's own question ...
        r.iq.on_message(Sop::Sop, hdr(kMsgVendorDefined, 1, 2, kSnk, 0), nak, r.now);   // ... and the Canon's NAK of it
        CHECK_EQ(int(r.iq.status().refused_mask), 0);
        CHECK_EQ(int(r.iq.status().why), int(IqWhy::SourceRestarts));
        const std::size_t n = r.sent.size();
        CHECK_EQ(bool(r.next(3000)), false);                // no second try, no final reset
        CHECK_EQ(r.sent.size(), n);

        // Later than the blame window it is the source's own business: the run goes on once the contract is back.
        Rig l;
        l.contract();
        l.start();
        l.next();
        l.now += kIqRenegBlameMs + 100;
        l.rx(hdr(1, 6, 2, kSrc, 0), caps);
        CHECK_EQ(bool(l.iq.running()), true);
        CHECK_COND(l.iq.status().why != IqWhy::SourceRestarts);
    }

    // ---- an answer counts only when it matches the question: the device's NAK of the charger's own Discover Identity is not a
    //      refusal of our Get_Sink_Cap, and a Revision nobody asked for is kept but is no answer ----
    {
        std::vector<std::uint8_t> di, nak;
        put32(di, 0xFF00A001u);
        put32(nak, 0xFF00A081u);
        Rig r;
        r.contract();
        r.start();
        r.next();                                           // Get_Sink_Cap, id 3
        r.rx(hdr(kMsgVendorDefined, 1, 2, kSrc, 1), di);
        r.iq.on_message(Sop::Sop, hdr(kMsgVendorDefined, 1, 2, kSnk, 0), nak, r.now);
        r.next();                                           // the window ends: asked again
        CHECK_EQ(r.last_type(), unsigned(kCtlGetSinkCap));
        CHECK_EQ(int(r.iq.status().closed), 1);
        CHECK_EQ(int(r.iq.status().q.step), int(IqStep::SinkCap));
        CHECK_EQ(int(r.iq.status().q.dev_msgs), 0);
        CHECK_EQ(int(r.iq.status().q.result), int(IqResult::ChargerOnly));   // only the charger's question was heard
        CHECK_EQ(int(r.iq.status().refused_mask), 0);

        Rig s;
        s.contract();
        s.start();
        s.next();                                           // Get_Sink_Cap, id 3
        s.dev(kDataRevision, {0x30000000u});                // not what was asked
        CHECK_EQ(int(s.iq.status().ok_mask), int(iq_bit(IqStep::Revision)));   // kept
        CHECK_EQ(int(s.iq.status().closed), 0);             // the question is still open
        s.next();
        CHECK_EQ(int(s.iq.status().q.dev_msgs), 0);
        CHECK_EQ(int(s.iq.status().q.result), int(IqResult::Nothing));

        // The device's NAK of the charger's request that arrives while we ask the same thing: the charger's, not ours; the
        // ACK after it answers us.
        Rig d;
        d.contract();
        d.start();
        d.next();
        d.dev(kDataSinkCap, {kSinkPdo1});
        d.next();
        d.dev_ctl(kCtlNotSupported);
        d.next();
        d.dev_ctl(kCtlNotSupported);
        d.next();
        d.dev_ctl(kCtlNotSupported);                        // EPR_Get_Sink_Cap
        CHECK_EQ(bool(d.next()), true);                     // Discover Identity
        CHECK_EQ(d.last_type(), unsigned(kMsgVendorDefined));
        d.rx(hdr(kMsgVendorDefined, 1, 2, kSrc, 4), di);    // the charger asks the same
        d.iq.on_message(Sop::Sop, hdr(kMsgVendorDefined, 1, 2, kSnk, 0), nak, d.now);   // the device NAKs the charger
        CHECK_EQ(int(d.iq.status().refused_mask & iq_bit(IqStep::Identity)), 0);
        CHECK_EQ(int(d.iq.status().ok_mask & iq_bit(IqStep::Identity)), 0);
        d.identity_ack(0x04A9, 0x1234);                     // the answer to ours
        CHECK_EQ(int(d.iq.status().ok_mask & iq_bit(IqStep::Identity)), int(iq_bit(IqStep::Identity)));
        CHECK_EQ(int(d.iq.status().q.result), int(IqResult::Answered));
        CHECK_EQ(int(d.iq.status().q.step), int(IqStep::Identity));
    }

    // ---- EPR_Get_Sink_Cap (PD 3.x only): an Extended_Control question after the plain ones, the answer in two chunks ----
    {
        auto to_epr = [](Rig& r) {   // Get_Sink_Cap answered, Sink_Capabilities_Extended and Get_Revision refused
            r.next();
            r.dev(kDataSinkCap, {kSinkPdo1});
            r.next();
            r.dev_ctl(kCtlNotSupported);
            r.next();
            r.dev_ctl(kCtlNotSupported);
        };
        // 13 objects: SPR 5 V 3 A .. then EPR 28 V 5 A (0xC1A4323C-style words); object 7 straddles the two chunks.
        std::vector<std::uint8_t> all;
        for (unsigned i = 0; i < 13; i++) put32(all, 0xA0000000u + i);
        const std::vector<std::uint8_t> c0(all.begin(), all.begin() + 26), c1(all.begin() + 26, all.end());

        Rig r;
        r.contract();
        r.start();
        to_epr(r);
        CHECK_EQ(bool(r.next()), true);
        CHECK_EQ(r.last_type(), unsigned(kExtControl));
        CHECK_EQ(int((r.last().header >> 15) & 1), 1);        // extended
        CHECK_EQ(int((r.last().header >> 12) & 7), 1);        // one data object: the ext header and the ECDB
        CHECK_EQ(int(r.last().header & 0x0120), int(kSrc));
        CHECK_EQ(int((r.last().header >> 6) & 3), 2);
        CHECK_EQ(r.last_id(), 6u);
        CHECK_EQ(int(r.last().len), 4);
        CHECK_EQ(int(r.last().data[0]), 0x02);                // ext header 0x8002: chunked, 2 bytes
        CHECK_EQ(int(r.last().data[1]), 0x80);
        CHECK_EQ(int(r.last().data[2]), int(kEprGetSinkCap)); // ECDB type 2
        CHECK_EQ(int(r.last().data[3]), 0);
        CHECK_EQ(int(r.iq.status().step), int(IqStep::EprSinkCap));
        r.epr_chunk(0, 52, c0);
        CHECK_EQ(int(r.iq.status().num_epr_pdos), 6);         // the first chunk is already kept
        CHECK_EQ(bool(r.iq.status().epr_partial), true);
        CHECK_EQ(int(r.iq.status().closed), 3);               // the question stays open until the last chunk
        CHECK_EQ(bool(r.next()), true);                       // the chunk request
        CHECK_EQ(r.last_type(), unsigned(kExtEprSinkCaps));
        CHECK_EQ(int((r.last().header >> 15) & 1), 1);
        CHECK_EQ(int(r.last().len), 4);
        CHECK_EQ(int(r.last().data[0]), 0x00);                // request, chunk 1: 0x8C00
        CHECK_EQ(int(r.last().data[1]), 0x8C);
        CHECK_EQ(int(r.last().header & 0x0120), int(kSrc));
        CHECK_EQ(r.last_id(), 7u);
        CHECK_EQ(int(r.iq.status().sent), 4);                 // the chunk request is no new question
        r.epr_chunk(1, 52, c1);
        const IqStatus& s = r.iq.status();
        CHECK_EQ(int(s.num_epr_pdos), 13);
        CHECK_EQ(bool(s.epr_partial), false);
        CHECK_EQ(s.epr_pdos[0], 0xA0000000u);
        CHECK_EQ(s.epr_pdos[6], 0xA0000006u);                 // the straddling one
        CHECK_EQ(s.epr_pdos[12], 0xA000000Cu);
        CHECK_COND(s.ok_mask & iq_bit(IqStep::EprSinkCap));
        CHECK_EQ(int(s.closed), 4);
        CHECK_EQ(int(s.q.step), int(IqStep::EprSinkCap));
        CHECK_EQ(int(s.q.result), int(IqResult::Answered));
        CHECK_EQ(bool(r.next()), true);
        CHECK_EQ(word(r.last()), 0xFF00A001u);                // then the VDMs
        CHECK_EQ(int(r.iq.status().late_mask), 0);

        // One chunk is the whole answer (24 bytes): no chunk request.
        Rig o;
        o.contract();
        o.start();
        to_epr(o);
        o.next();
        o.epr_chunk(0, 24, std::vector<std::uint8_t>(all.begin(), all.begin() + 24));
        CHECK_EQ(int(o.iq.status().num_epr_pdos), 6);
        CHECK_EQ(bool(o.iq.status().epr_partial), false);
        CHECK_EQ(bool(o.next()), true);
        CHECK_EQ(word(o.last()), 0xFF00A001u);

        // The second chunk never comes: the first is kept and the step counts as answered, marked partial, never asked twice.
        Rig p;
        p.contract();
        p.start();
        to_epr(p);
        p.next();
        p.epr_chunk(0, 52, c0);
        CHECK_EQ(bool(p.next()), true);                       // the chunk request
        CHECK_EQ(p.last_type(), unsigned(kExtEprSinkCaps));
        CHECK_EQ(bool(p.next()), true);                       // after the window: the VDM, not a second EPR question
        CHECK_EQ(word(p.last()), 0xFF00A001u);
        CHECK_EQ(int(p.iq.status().num_epr_pdos), 6);
        CHECK_EQ(bool(p.iq.status().epr_partial), true);
        CHECK_COND(p.iq.status().ok_mask & iq_bit(IqStep::EprSinkCap));
        CHECK_EQ(int(p.iq.status().silent_mask & iq_bit(IqStep::EprSinkCap)), 0);

        // Not_Supported: refused (PD 3.0 devices without EPR), and nothing is stored.
        Rig n;
        n.contract();
        n.start();
        to_epr(n);
        n.next();
        CHECK_EQ(n.last_type(), unsigned(kExtControl));
        n.dev_ctl(kCtlNotSupported);
        CHECK_EQ(int(n.iq.status().refused_mask & iq_bit(IqStep::EprSinkCap)), int(iq_bit(IqStep::EprSinkCap)));
        CHECK_EQ(int(n.iq.status().ok_mask & iq_bit(IqStep::EprSinkCap)), 0);
        CHECK_EQ(int(n.iq.status().num_epr_pdos), 0);
        CHECK_EQ(int(n.iq.status().q.result), int(IqResult::Refused));

        // Only a matching answer counts: the charger's own EPR_Sink_Capabilities (source role) is nothing, and the device's
        // answer to something else does not close the question in flight.
        Rig m;
        m.contract();
        m.start();
        m.next();                                             // Get_Sink_Cap in flight
        std::vector<std::uint8_t> sp = {24, 0x80};
        for (int i = 0; i < 26; i++) sp.push_back(1);
        m.rx(hdr(kExtEprSinkCaps, 7, 2, kSrc, 2, true), sp);
        CHECK_EQ(int(m.iq.status().num_epr_pdos), 0);
        m.epr_chunk(0, 24, std::vector<std::uint8_t>(all.begin(), all.begin() + 24));
        CHECK_EQ(int(m.iq.status().closed), 0);               // kept, but Get_Sink_Cap is still open
        CHECK_COND(m.iq.status().ok_mask & iq_bit(IqStep::EprSinkCap));
        CHECK_EQ(int(m.iq.status().step), int(IqStep::SinkCap));

        // A PD 2.0 device is never asked.
        Rig v;
        v.contract(0, 1);
        v.start();
        v.next();
        v.dev(kDataSinkCap, {kSinkPdo1}, 1);
        v.next();
        v.drain();
        CHECK_EQ(int(v.iq.status().asked_mask & iq_bit(IqStep::EprSinkCap)), 0);
    }

    // ---- a late answer is an answer: Discover Identity asked twice with no reply, the ACK arrives during the final renegotiation ----
    {
        Rig r;
        r.contract();
        r.start();
        r.next();
        r.dev(kDataSinkCap, {kSinkPdo1});
        for (int i = 0; i < 3; i++) {                         // Ext, Revision, EPR: refused
            r.next();
            r.dev_ctl(kCtlNotSupported);
        }
        CHECK_EQ(bool(r.next()), true);
        CHECK_EQ(word(r.last()), 0xFF00A001u);                // Discover Identity, attempt 1
        CHECK_EQ(bool(r.next()), true);
        CHECK_EQ(word(r.last()), 0xFF00A001u);                // attempt 2
        CHECK_EQ(bool(r.next()), true);
        CHECK_EQ(bool(r.last().hard_reset), true);
        CHECK_EQ(int(r.iq.status().silent_mask), int(iq_bit(IqStep::Identity)));
        r.now += 1260 - 1;                                    // bench: the identity came 1.26 s later
        r.identity_ack(0x04A9, 0x1234);
        const IqStatus& s = r.iq.status();
        CHECK_EQ(int(s.ok_mask & iq_bit(IqStep::Identity)), int(iq_bit(IqStep::Identity)));
        CHECK_EQ(int(s.late_mask), int(iq_bit(IqStep::Identity)));
        CHECK_EQ(int(s.silent_mask & iq_bit(IqStep::Identity)), 0);   // not "silent" any more
        CHECK_EQ(bool(s.id.valid), true);
        CHECK_EQ(int(s.id.vid), 0x04A9);
        r.contract();
        r.step();
        CHECK_EQ(int(r.iq.status().phase), int(IqPhase::Done));
        CHECK_EQ(int(r.iq.status().late_mask), int(iq_bit(IqStep::Identity)));
    }

    // ---- the e-marker spoof run (IqFacts::spoof): the first Hard Reset is never skipped, SOP' queries open a bounded window ----
    {
        auto sp_start = [](Rig& r) {
            IqFacts f;
            f.cc = f.inline_link = f.confirmed = f.spoof = true;
            return r.iq.start(f, r.now);
        };
        Rig r;
        r.contract();                                         // a fresh contract: a plain run would skip the reset
        CHECK_EQ(int(sp_start(r)), int(IqRefusal::None));
        CHECK_EQ(int(r.iq.status().phase), int(IqPhase::Reset1));
        CHECK_EQ(bool(r.iq.status().skipped_first_reset), false);
        CHECK_EQ(bool(r.iq.status().spoof), true);
        CHECK_EQ(bool(r.next()), true);
        CHECK_EQ(bool(r.last().hard_reset), true);
        CHECK_EQ(bool(r.iq.sopp_window(r.now)), false);       // nothing is asked of Auto-CRC before a query was seen
        r.now += 1100;
        r.contract();
        r.iq.tick(r.now);
        CHECK_EQ(int(r.iq.status().phase), int(IqPhase::Settle));
        CHECK_EQ(bool(r.iq.sopp_window(r.now)), false);

        // A SOP' query opens the window; our reply shortens it; the questions wait for it to close.
        r.sopp_query();
        CHECK_EQ(int(r.iq.status().sopp_queries), 1);
        CHECK_EQ(bool(r.iq.sopp_window(r.now)), true);
        const std::uint32_t tq = r.now;
        r.now += 5;
        r.iq.on_sopp_reply(r.now);
        CHECK_EQ(int(r.iq.status().sopp_replies), 1);
        CHECK_EQ(bool(r.iq.sopp_window(tq + 60)), true);
        CHECK_EQ(bool(r.iq.sopp_window(tq + 5 + kIqSoppReplyMs + 1)), false);
        CHECK_EQ(bool(r.next()), true);
        CHECK_EQ(r.last_type(), unsigned(kCtlGetSinkCap));
        CHECK_COND(r.now - tq >= 5 + kIqSoppReplyMs);         // the question did not go out on top of the window
        CHECK_EQ(bool(r.iq.sopp_window(r.now)), false);       // Ask: Auto-CRC off
        r.sopp_query();                                       // a late query in Ask still counts, but opens nothing
        CHECK_EQ(int(r.iq.status().sopp_queries), 2);
        CHECK_EQ(bool(r.iq.sopp_window(r.now)), false);

        // Without a reply the window is kIqSoppWindowMs; a retry re-opens it but never past kIqSoppMaxMs from the first query.
        Rig w;
        w.contract();
        sp_start(w);
        w.next();
        w.now += 1100;
        w.contract();
        w.iq.tick(w.now);
        w.sopp_query();
        const std::uint32_t t0 = w.now;
        CHECK_EQ(bool(w.iq.sopp_window(t0 + kIqSoppWindowMs - 5)), true);
        CHECK_EQ(bool(w.iq.sopp_window(t0 + kIqSoppWindowMs + 5)), false);
        for (int i = 0; i < 20; i++) {
            w.now += 100;
            w.sopp_query();
        }
        CHECK_EQ(int(w.iq.status().sopp_queries), 21);
        CHECK_EQ(bool(w.iq.sopp_window(w.now + kIqSoppWindowMs - 5)), false);   // 2 s since the first: capped
        CHECK_COND(w.now - t0 > kIqSoppMaxMs);

        // Source_Capabilities (a fresh negotiation) closes it.
        Rig c;
        c.contract();
        sp_start(c);
        c.next();
        c.now += 1100;
        c.contract();
        c.iq.tick(c.now);
        c.sopp_query();
        CHECK_EQ(bool(c.iq.sopp_window(c.now)), true);
        std::vector<std::uint8_t> caps;
        for (std::uint32_t wd : kCaps) put32(caps, wd);
        c.rx(hdr(1, 6, 2, kSrc, 0), caps);
        CHECK_EQ(bool(c.iq.sopp_window(c.now)), false);

        // A run without the spoof sees SOP' frames as nothing.
        Rig n;
        n.contract();
        n.start();
        n.sopp_query();
        CHECK_EQ(int(n.iq.status().sopp_queries), 0);
        CHECK_EQ(bool(n.iq.sopp_window(n.now)), false);
        Rig idle;
        idle.sopp_query();
        CHECK_EQ(bool(idle.iq.sopp_window(idle.now)), false);

        // A real e-marked cable on the charger side answers the query itself (hardware 2026-11-13): the spoof stays silent and
        // the status says a real chip answered, so "0 replies" is not read as "the charger never got an answer".
        Rig real;
        real.contract();
        sp_start(real);
        real.next();
        real.now += 1100;
        real.contract();
        real.iq.tick(real.now);
        CHECK_EQ(bool(real.iq.status().sopp_real), false);
        real.sopp_query();
        CHECK_EQ(bool(real.iq.status().sopp_real), false);
        real.sopp_real_answer();
        CHECK_EQ(bool(real.iq.status().sopp_real), true);
        CHECK_EQ(int(real.iq.status().sopp_queries), 1);      // the answer is no query
        CHECK_EQ(int(real.iq.status().sopp_replies), 0);
        Rig nospoof;
        nospoof.contract();
        nospoof.start();
        nospoof.sopp_real_answer();
        CHECK_EQ(bool(nospoof.iq.status().sopp_real), false);   // only a spoof run watches the cable
    }

    // ---- spoof run, after the final reset: the contract is taken after the settle, so the charger's second negotiation (it
    //      re-lists once it knows the cable) is the one compared ----
    {
        Rig r;
        r.contract();
        IqFacts f;
        f.cc = f.inline_link = f.confirmed = f.spoof = true;
        r.iq.start(f, r.now);
        r.next();                                             // Hard Reset 1
        r.now += 1100;
        r.contract();
        r.next();
        r.dev(kDataSinkCap, {kSinkPdo1});
        for (int i = 0; i < 3; i++) {
            r.next();
            r.dev_ctl(kCtlNotSupported);
        }
        r.next();
        r.identity_ack(0x04A9, 0x1234);
        CHECK_EQ(bool(r.next()), true);
        CHECK_EQ(bool(r.last().hard_reset), true);            // Hard Reset 2
        r.now += 1100;
        r.contract();                                         // PS_RDY of the first renegotiation
        r.step();
        CHECK_EQ(int(r.iq.status().phase), int(IqPhase::Sniff2));   // not over yet: the cable query may follow
        r.now += 20;
        r.contract(0, 2, 0x2304B12Cu);                        // the charger re-lists (5 A cable known) and the device re-requests
        for (int i = 0; i < 400 && r.iq.running(); i++) r.step();
        CHECK_EQ(int(r.iq.status().phase), int(IqPhase::Done));
        CHECK_EQ(int(r.iq.status().match), int(IqMatch::Differs));
        CHECK_EQ(r.iq.status().second.rdo, 0x2304B12Cu);

        CHECK_COND(std::string(iq_refusal_name(IqRefusal::BadSpoof)) != "?");
        CHECK_COND(std::string(iq_step_name(IqStep::EprSinkCap)) != "?");
    }

    // ---- refusals, in order ----
    {
        IqFacts f;
        CHECK_EQ(int(iq_refusal(f)), int(IqRefusal::NoCc));
        f.cc = true;
        CHECK_EQ(int(iq_refusal(f)), int(IqRefusal::NotInline));
        f.inline_link = true;
        f.busy = true;
        CHECK_EQ(int(iq_refusal(f)), int(IqRefusal::Busy));
        f.busy = false;
        f.own_contract = true;
        CHECK_EQ(int(iq_refusal(f)), int(IqRefusal::OwnContract));
        f.own_contract = false;
        f.self_powered = true;
        CHECK_EQ(int(iq_refusal(f)), int(IqRefusal::SelfPowered));
        f.self_powered = false;
        CHECK_EQ(int(iq_refusal(f)), int(IqRefusal::NeedConfirm));
        f.confirmed = true;
        CHECK_EQ(int(iq_refusal(f)), int(IqRefusal::None));
        f.other_running = true;
        CHECK_EQ(int(iq_refusal(f)), int(IqRefusal::Running));
        Rig r;
        r.start();
        CHECK_EQ(int(r.start()), int(IqRefusal::Running));   // one at a time
    }
    return test_report("test_pd_interrogate");
}
