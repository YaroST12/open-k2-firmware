// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Host tests for the lab-only Apple charger readout (k2/pd/apple_readout.hpp): the sniffed register file
// (with a MADE-UP serial - the real one never reaches a tracked file) replayed through the engine, the
// fault cases, the SVIDs-ACK word, the arm gate, and the Sink integration (SVIDs ACK, VDM routing, detach).
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <vector>

#include "k2/pd/apple_readout.hpp"
#include "k2/pd/pd_sink.hpp"
#include "k2/pd/pd_vdm.hpp"
#include "test_util.h"

using namespace k2::pd;

namespace {

// The sniffed Apple 30W register file, bank 0, with a made-up serial (TESTSERIAL0000000, 17 chars).
// Wire values little endian per word, exactly as decoded from the inline sniff.
void load_sniffed(std::array<std::uint8_t, 0x100>& reg) {
    reg.fill(0);
    auto word = [&](std::uint32_t addr, std::uint32_t w) {
        for (int b = 0; b < 4; b++) reg[addr + b] = std::uint8_t(w >> (8 * b));
    };
    word(0x00, 0x20000006);
    word(0x04, 0x00090200);
    word(0x08, 0x01180030);   // idx 1: serial   @0x30 len 0x18
    word(0x0C, 0x02100050);   // idx 2: maker    @0x50 len 0x10
    word(0x10, 0x03100060);   // idx 3: unknown, not read
    word(0x14, 0x04180070);   // idx 4: product  @0x70 len 0x18
    word(0x18, 0x05100090);   // idx 5: 0x90 field @0x90 len 0x10
    word(0x1C, 0x061000A0);   // idx 6: unknown, not read
    word(0x20, 0x071000B0);   // idx 7: fw       @0xB0 len 0x10
    word(0x24, 0x081000C0);   // idx 8: hw       @0xC0 len 0x10
    // serial @0x30, 24 B field: "TESTSERIAL0000000" + NUL pad
    word(0x30, 0x54534554);   // "TEST"
    word(0x34, 0x49524553);   // "SERI"
    word(0x38, 0x30304C41);   // "AL00"
    word(0x3C, 0x30303030);   // "0000"
    word(0x40, 0x00000030);   // "0\0\0\0"
    // maker @0x50: "Apple Inc."
    word(0x50, 0x6C707041);
    word(0x54, 0x6E492065);
    word(0x58, 0x00002E63);
    // product @0x70 len 0x18: "30W USB-C Power Adapter" (24 B over two reads: 0x70 x5 words, 0x84 x1)
    word(0x70, 0x20573033);
    word(0x74, 0x2D425355);
    word(0x78, 0x6F502043);
    word(0x7C, 0x20726577);
    word(0x80, 0x70616441);
    word(0x84, 0x00726574);
    // 0x90 field: "0x7003"
    word(0x90, 0x30377830);
    word(0x94, 0x00003330);
    // fw @0xB0: "01030054"
    word(0xB0, 0x33303130);
    word(0xB4, 0x34353030);
    // hw @0xC0: "1.0"
    word(0xC0, 0x00302E31);
}

// A fake charger: answers the engine's read requests from the register file, with fault injection.
struct Fake {
    std::array<std::uint8_t, 0x100> reg{};
    AppleReadout eng;
    std::uint32_t now = 1000;
    std::vector<AppleVdm> sent;
    std::vector<std::uint32_t> sent_at;
    std::vector<AppleVdm> rx_q;
    std::vector<std::uint32_t> rx_at;
    int nak_dir_at = -1;        // NAK the directory read of this dir index
    int silent_reads = 0;       // answer nothing for this many reads (timeout case)
    std::uint32_t answer_delay = 4;

    Fake() { load_sniffed(reg); }
    std::uint32_t word_at(std::uint32_t addr) const {
        return std::uint32_t(reg[addr]) | (std::uint32_t(reg[addr + 1]) << 8) |
               (std::uint32_t(reg[addr + 2]) << 16) | (std::uint32_t(reg[addr + 3]) << 24);
    }
    void feed(const AppleVdm& rx) {   // an unsolicited source -> sink VDM (discovery, handshake)
        const AppleVdm tx = eng.on_vdm(rx, now);
        if (tx.n) { sent.push_back(tx); sent_at.push_back(now); }
    }
    void answer(const AppleVdm& tx) {
        if (tx.n == 3 && tx.w[0] == kAppleAttention) {   // a read request: Attention + 05ACnn02 + addr
            const std::uint8_t nn = std::uint8_t(tx.w[1] >> 8);
            const std::uint32_t addr = tx.w[2];
            if (silent_reads > 0) { silent_reads--; return; }
            if (nak_dir_at >= 0 && addr == kAppleDirFirst + 4 * unsigned(nak_dir_at)) {
                AppleVdm nak;
                nak.w[0] = 0x05AC8182;   // structured NAK (cmd type 2), SVID 0x05AC
                nak.n = 1;
                rx_q.push_back(nak); rx_at.push_back(now + answer_delay);
                return;
            }
            AppleVdm ans;
            ans.w[0] = tx.w[1]; ans.w[1] = addr;
            for (int i = 0; i < nn; i++) ans.w[2 + i] = word_at(addr + 4 * i);
            ans.n = std::uint8_t(2 + nn);
            rx_q.push_back(ans); rx_at.push_back(now + answer_delay);
        }
        // the 05ACnn12 ack and anything else: no answer
    }
    void step() {
        now++;
        for (std::size_t i = 0; i < rx_q.size();) {
            if (std::int32_t(now - rx_at[i]) >= 0) {
                const AppleVdm rx = rx_q[i];
                rx_q.erase(rx_q.begin() + i); rx_at.erase(rx_at.begin() + i);
                feed(rx);
            } else i++;
        }
        const AppleVdm tx = eng.tick(now);
        if (tx.n) { sent.push_back(tx); sent_at.push_back(now); answer(tx); }
    }
    void run(std::uint32_t ms) { for (std::uint32_t i = 0; i < ms; i++) step(); }
    // The full sniffed discovery sequence, then the reads.
    void discover_and_run(std::uint32_t ms = 2000) {
        eng.arm(now);
        AppleVdm dm; dm.w[0] = kAppleDiscoverModesReq; dm.n = 1; feed(dm);
        AppleVdm em; em.w[0] = kAppleEnterModeReq; em.n = 1; feed(em);
        AppleVdm hs; hs.w[0] = kAppleHandshakeReq; hs.n = 1; feed(hs);
        run(ms);
    }
    // The addr of every read request sent, in order.
    std::vector<std::uint32_t> read_addrs() const {
        std::vector<std::uint32_t> out;
        for (const AppleVdm& t : sent)
            if (t.n == 3 && t.w[0] == kAppleAttention) out.push_back(t.w[2]);
        return out;
    }
    const AppleVdm* reply_to(std::uint32_t word0) const {
        for (const AppleVdm& t : sent)
            if (t.n >= 1 && t.w[0] == word0) return &t;
        return nullptr;
    }
};

std::uint32_t vdm_word(const TxMsg& m, unsigned i) {
    return std::uint32_t(m.data[4 * i]) | (std::uint32_t(m.data[4 * i + 1]) << 8) |
           (std::uint32_t(m.data[4 * i + 2]) << 16) | (std::uint32_t(m.data[4 * i + 3]) << 24);
}

struct Rig {
    SourceCaps caps;
    Sink sink;
    std::uint32_t now = 1000;
    std::vector<TxMsg> sent;
    Rig() {
        caps.valid = true;
        caps.spec_rev = 2;
        const std::uint32_t raw[] = {0x0001912C, 0x0002D12C, 0x0004B12C, 0x000640C8};
        caps.num_pdos = 4;
        for (int i = 0; i < 4; i++) caps.pdos[i] = decode_pdo(raw[i]);
        sink.bind(&caps);
        sink.set_cap_mv(36000);
        sink.set_proactive_discover(false);
    }
    void pump() {
        TxMsg m;
        while (sink.take_tx(m)) { sent.push_back(m); sink.on_tx_result(true, now); }
    }
    void ctl(std::uint8_t type) { sink.on_message(Sop::Sop, make_header(type, 0, 2) | (1u << 8), {}, now); pump(); }
    void vdm(std::initializer_list<std::uint32_t> words) {
        std::uint8_t payload[28] = {};
        unsigned n = 0;
        for (std::uint32_t w : words) {
            for (int b = 0; b < 4; b++) payload[4 * n + b] = std::uint8_t(w >> (8 * b));
            n++;
        }
        sink.on_message(Sop::Sop, make_header(kMsgVendorDefined, std::uint8_t(n), 2) | (1u << 8),
                        {payload, 4 * n}, now);
        pump();
    }
    void contract() {
        CHECK_EQ(sink.trigger({1, 5000, 1000}, now), Err::Ok);
        pump();
        ctl(kCtlAccept);
        ctl(kCtlPsRdy);
    }
    // The SVID word of the last Discover SVIDs ACK we sent (0 if none).
    std::uint32_t last_svid_word() const {
        for (auto it = sent.rbegin(); it != sent.rend(); ++it)
            if ((it->header & 0x1F) == kMsgVendorDefined && it->len >= 8) return vdm_word(*it, 1);
        return 0;
    }
};

}  // namespace

int main() {
    // ---- the gate (pure) ----
    {
        AppleFacts f{false, false, false, true, true};
        CHECK_EQ(apple_read_refusal(f), AppleRefusal::None);
        f.charger_test_busy = true;
        CHECK_EQ(apple_read_refusal(f), AppleRefusal::Busy);
        f = {false, true, false, true, true};
        CHECK_EQ(apple_read_refusal(f), AppleRefusal::Inline);
        f = {false, false, true, true, true};
        CHECK_EQ(apple_read_refusal(f), AppleRefusal::Inline);
        f = {false, false, false, true, false};
        CHECK_EQ(apple_read_refusal(f), AppleRefusal::NotPartner);
        f = {false, false, false, false, true};
        CHECK_EQ(apple_read_refusal(f), AppleRefusal::NoContract);
        CHECK_STR(apple_refusal_name(AppleRefusal::NoContract), "no contract");
        CHECK_STR(apple_why_name(AppleWhy::NoMode), "nomode");
    }

    // ---- the SVIDs ACK word: [0x2717] unarmed, [0x2717, 0x05AC] armed ----
    {
        AppleReadout e;
        CHECK_EQ(e.svid_ack_word(), 0x27170000u);
        e.arm(1000);
        CHECK(e.armed());
        CHECK_EQ(e.svid_ack_word(), 0x271705ACu);
        e.reset();
        CHECK(!e.armed());
        CHECK_EQ(e.svid_ack_word(), 0x27170000u);
    }

    // ---- replay of the sniffed exchange: directory first, then the strings ----
    {
        Fake f;
        f.discover_and_run(2500);
        const AppleInfo& i = f.eng.info();
        CHECK(i.valid);
        CHECK_EQ(i.why, AppleWhy::Done);
        CHECK_EQ(i.reads, 16);   // 9 directory + serial 1 + maker 1 + product 2 + 0x90 1 + fw 1 + hw 1
        CHECK_EQ(i.ok, 16);
        CHECK_STR(i.maker.data(), "Apple Inc.");
        CHECK_STR(i.product.data(), "30W USB-C Power Adapter");
        CHECK_STR(i.fw.data(), "01030054");
        CHECK_STR(i.hw.data(), "1.0");
        CHECK_STR(i.field90.data(), "0x7003");
        CHECK_STR(i.serial.data(), "TESTSERIAL0000000");
        // discovery replies
        const AppleVdm* dm = f.reply_to(kAppleDiscoverModesAck);
        CHECK(dm && dm->n == 2 && dm->w[1] == kAppleModesVdo);
        CHECK(f.reply_to(kAppleEnterModeAck) != nullptr);
        const AppleVdm* hs = f.reply_to(kAppleHandshakeAck);
        CHECK(hs && hs->n == 1);
        // read order: the directory 0x04..0x24, then the strings
        const std::vector<std::uint32_t> want = {0x04, 0x08, 0x0C, 0x10, 0x14, 0x18, 0x1C, 0x20, 0x24,
                                                 0x30, 0x50, 0x70, 0x84, 0x90, 0xB0, 0xC0};
        const std::vector<std::uint32_t> got = f.read_addrs();
        CHECK_EQ(got.size(), want.size());
        if (got.size() == want.size())
            for (std::size_t k = 0; k < want.size(); k++) CHECK_EQ(got[k], want[k]);
        // pacing: one read per ~20 ms (the first read of a run may go at once)
        for (std::size_t k = 1; k < f.sent_at.size(); k++) {
            const bool both_reads = f.sent[k].n == 3 && f.sent[k - 1].n == 3 &&
                                    f.sent[k].w[0] == kAppleAttention && f.sent[k - 1].w[0] == kAppleAttention;
            if (both_reads) CHECK(std::int32_t(f.sent_at[k] - f.sent_at[k - 1]) >= 20);
        }
    }

    // ---- a NAK mid-directory stops the run and keeps what was read ----
    {
        Fake f;
        f.nak_dir_at = 4;   // the directory read of addr 0x14 is NAKed
        f.discover_and_run(1500);
        const AppleInfo& i = f.eng.info();
        CHECK(i.valid);
        CHECK_EQ(i.why, AppleWhy::Nak);
        CHECK_EQ(i.ok, 4);          // 0x04..0x10 answered
        CHECK_EQ(i.reads, 5);       // the NAKed one counted as a read, not ok
        CHECK_STR(i.maker.data(), "");   // nothing past the directory yet
    }

    // ---- a silent read: 50 ms timeout, one retry, then the run stops ----
    {
        Fake f;
        f.discover_and_run(400);    // let the directory finish, then go silent on a string read
        if (f.eng.info().why == AppleWhy::Running) {
            f.silent_reads = 100;   // every later read is unanswered
            f.run(500);
        }
        const AppleInfo& i = f.eng.info();
        CHECK(i.valid);
        CHECK_EQ(i.why, AppleWhy::Timeout);
        CHECK(i.ok > 0 && i.ok < i.reads);
        // the read was retried once: 2 requests for the same addr at the end
        const auto addrs = f.read_addrs();
        CHECK(addrs.size() >= 2 && addrs[addrs.size() - 1] == addrs[addrs.size() - 2]);
    }

    // ---- a directory entry pointing out of range stops the run before reading it ----
    {
        Fake f;
        // idx 2 (maker) -> addr 0xF0 len 0x18: 0xF0 + 0x18 > the 0x100 register window
        f.reg[0x0C] = 0xF0; f.reg[0x0D] = 0x00; f.reg[0x0E] = 0x18; f.reg[0x0F] = 0x02;
        f.discover_and_run(1500);
        const AppleInfo& i = f.eng.info();
        CHECK(i.valid);
        CHECK_EQ(i.why, AppleWhy::Range);
        CHECK_EQ(i.ok, 2);   // 0x04 and 0x08 answered; 0x0C (the bad entry) parsed, then stopped
        for (std::uint32_t a : f.read_addrs()) CHECK(a != 0xF0);   // the bad address was never read
    }

    // ---- a 24-byte name with no NUL: split over two reads (5 + 1 words) ----
    {
        Fake f;
        const char* name = "ABCDEFGHIJKLMNOPQRSTUVWX";   // exactly 24 chars
        for (int k = 0; k < 24; k++) f.reg[0x70 + k] = std::uint8_t(name[k]);
        f.discover_and_run(2500);
        const AppleInfo& i = f.eng.info();
        CHECK_EQ(i.why, AppleWhy::Done);
        CHECK_STR(i.product.data(), name);
        CHECK_EQ(std::strlen(i.product.data()), 24u);
        // the product was read at 0x70 (nn=5) then 0x84 (nn=1)
        bool seen70 = false, seen84 = false;
        for (const AppleVdm& t : f.sent) {
            if (t.n == 3 && t.w[0] == kAppleAttention && t.w[2] == 0x70) { seen70 = true; CHECK_EQ(std::uint8_t(t.w[1] >> 8), 5); }
            if (t.n == 3 && t.w[0] == kAppleAttention && t.w[2] == 0x84) { seen84 = true; CHECK_EQ(std::uint8_t(t.w[1] >> 8), 1); }
        }
        CHECK(seen70 && seen84);
    }

    // ---- no discovery from the source: the arm wait runs out with NoMode ----
    {
        Fake f;
        f.eng.arm(f.now);
        f.run(4000);   // the charger never sends Discover Modes / Enter_Mode
        const AppleInfo& i = f.eng.info();
        CHECK(i.valid);
        CHECK_EQ(i.why, AppleWhy::NoMode);
        CHECK_EQ(i.reads, 0);
    }

    // ---- the 16-read cap bounds a run ----
    {
        Fake f;
        // make every field 24 B so each string takes 2 reads: 9 dir + 6*2 = 21 > 16
        for (std::uint32_t a = 0x08; a <= 0x24; a += 4) { f.reg[a + 2] = 0x18; }
        f.discover_and_run(3000);
        const AppleInfo& i = f.eng.info();
        CHECK(i.valid);
        CHECK_EQ(i.why, AppleWhy::Cap);
        CHECK_EQ(i.reads, 16);
    }

    // ---- Sink integration: the SVIDs ACK and the 0x05AC VDM routing ----
    {
        Rig r;
        r.contract();
        r.vdm({0xFF008002});   // the source's Discover SVIDs REQ
        CHECK_EQ(r.last_svid_word(), 0x27170000u);   // unarmed: exactly [0x2717]
        // arm without a fresh contract is fine here, but arm on a sink with NO contract is refused:
        Rig r2;
        CHECK(r2.sink.arm_apple_read(r2.now) != Err::Ok);
        CHECK(!r2.sink.apple_info().valid);
        // arm the first sink: the op also triggers the re-request (Get_Source_Cap goes out)
        CHECK_EQ(r.sink.arm_apple_read(r.now), Err::Ok);
        bool got_get_cap = false;
        for (const TxMsg& m : r.sent) if ((m.header & 0x1F) == kCtlGetSourceCap) got_get_cap = true;
        CHECK(got_get_cap);
        r.vdm({0xFF008002});
        CHECK_EQ(r.last_svid_word(), 0x271705ACu);   // armed: [0x2717, 0x05AC]
        // the source drives discovery: Discover Modes 0x05AC -> ACK with 00000002
        r.sink.tick(r.now += 10);
        r.pump();
        r.vdm({kAppleDiscoverModesReq});
        bool dm_ack = false;
        for (const TxMsg& m : r.sent)
            if ((m.header & 0x1F) == kMsgVendorDefined && m.len >= 8 && vdm_word(m, 0) == kAppleDiscoverModesAck &&
                vdm_word(m, 1) == kAppleModesVdo)
                dm_ack = true;
        CHECK(dm_ack);
        // detach clears the readout and the SVIDs ACK goes back to [0x2717]
        r.sink.detach();
        CHECK(!r.sink.apple_info().valid);
        r.vdm({0xFF008002});
        CHECK_EQ(r.last_svid_word(), 0x27170000u);
    }

    return test_report("test_apple_readout");
}
