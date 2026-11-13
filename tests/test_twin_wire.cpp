// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Host tests for the device twin's payload layouts (k2/twin/wire.hpp): every response fits the remote frame, the record
// reassembles from its parts, the run request is validated, the status and result bytes mirror the runner.
#include <array>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <string_view>
#include <vector>

#include "k2/twin/wire.hpp"
#include "test_util.h"

using namespace k2;
using namespace k2::twin;

namespace {

Twin big_twin() {
    Twin t;
    std::strcpy(t.name.data(), "Dock with a long nam");   // 20 characters
    t.vid = 0x17EF;
    t.pid = 0x3071;
    t.id_hi = std::uint16_t((0x4C000000u | 0x17EF) >> 23);   // a dock: device capable, UFP hub, modal
    t.pd_rev = 2;
    t.num_pdos = 4;
    t.pdos = {0x3601912C, 0x0002D12C, 0xC0DC323C, 0x0004B12C};
    t.rdo = 0x1004B12C;
    t.rdo_pdo = 0x0004B12C;
    t.decl.n_pt = 1;
    t.decl.pt = {0x6D800000u | (1u << 3) | 3, 0};   // USB4 capable, TBT3 alt mode, code 3 (40G)
    t.decl.n_svids = 6;
    t.decl.svids = {0xFF01, 0x8087, 0x1234, 0x2345, 0x3456, 0x4567};
    t.decl.n_modes = 2;
    t.decl.mode_svid = {0xFF01, 0x8087};
    t.decl.mode_vdo = {0x000C0045u, 3u << 16};
    t.obs.valid = true;
    t.obs.partner_vid = 0x05AC;
    t.obs.family = std::uint8_t(usblink::Family::Usb4);
    t.obs.tbt = std::uint8_t(usblink::Tbt::Tbt4);
    t.obs.mbps = 40000;
    return t;
}

void test_list() {
    std::array<std::uint8_t, wire::kMaxRsp> out{};
    const std::array<std::uint8_t, 3> s = {1, 4, 9};
    CHECK_EQ(wire::encode_list(s, out), 5u);
    CHECK_EQ(int(out[0]), 0);
    CHECK_EQ(int(out[1]), 3);
    CHECK_EQ(int(out[2]), 1);
    CHECK_EQ(int(out[4]), 9);
    CHECK_EQ(wire::encode_list({}, out), 2u);
    std::array<std::uint8_t, 1> tiny{};
    CHECK_EQ(wire::encode_list(s, tiny), 0u);
    // more than the library can hold is cut, never overflows the frame
    std::array<std::uint8_t, 20> many{};
    CHECK_EQ(wire::encode_list(many, out), 2u + kCapacity);
    CHECK_EQ(int(out[1]), int(kCapacity));
}

void test_record_chunks_reassemble() {
    const Twin t = big_twin();
    std::array<std::uint8_t, kMaxFileBytes> rec{};
    const std::size_t n = encode(t, 7, rec);
    CHECK_EQ(n > wire::kChunk, true);
    std::array<std::uint8_t, kMaxFileBytes> got{};
    std::size_t have = 0;
    std::array<std::uint8_t, wire::kMaxRsp> out{};
    unsigned parts = 0;
    for (unsigned p = 0; p == 0 || p < parts; ++p) {
        const std::size_t m = wire::encode_chunk(7, std::uint8_t(p), std::span(rec.data(), n), out);
        CHECK_EQ(m > 5, true);
        CHECK_EQ(m <= wire::kMaxRsp, true);
        CHECK_EQ(int(out[0]), 0);
        CHECK_EQ(int(out[1]), 7);
        CHECK_EQ(int(out[2]), int(p));
        parts = out[3];
        CHECK_EQ(int(out[4]), int(m - 5));
        std::memcpy(got.data() + have, &out[5], out[4]);
        have += out[4];
    }
    CHECK_EQ(have, n);
    CHECK_EQ(int(parts), int((n + wire::kChunk - 1) / wire::kChunk));
    Twin back;
    std::uint8_t seq = 0;
    CHECK_EQ(int(decode(std::span<const std::uint8_t>(got.data(), have), seq, back)), int(Dec::Ok));
    CHECK_EQ(int(seq), 7);
    CHECK_STR(back.name.data(), "Dock with a long nam");
    CHECK_EQ(int(back.obs.mbps), 40000);
    // a part past the end is an error, not a short frame
    CHECK_EQ(wire::encode_chunk(7, std::uint8_t(parts), std::span(rec.data(), n), out), 1u);
    CHECK_EQ(int(out[0]), int(Err::Invalid));
}

void test_predict_bytes() {
    Outcome o;
    o.kind = Kind::Pps;
    o.pos = 5;
    o.mv = 11000;
    o.ma = 3000;
    o.mw = 33000;
    o.want_mw = 55000;
    o.mismatch = true;
    o.limiter = Limiter::Cable;
    Facts f;
    f.claim_w = 100;
    f.cable_ma = 3000;
    f.cable_mv = 20000;
    std::array<std::uint8_t, wire::kMaxRsp> out{};
    CHECK_EQ(wire::encode_predict(3, o, f, out), wire::kPredictBytes);
    CHECK_EQ(int(out[0]), 0);
    CHECK_EQ(int(out[1]), 3);
    CHECK_EQ(int(out[2]), int(Kind::Pps));
    CHECK_EQ(int(out[3]), int(Limiter::Cable));
    CHECK_EQ(int(out[4]), int(wire::kPredMismatch));
    CHECK_EQ(int(out[5]), 5);
    CHECK_EQ(int(out[6] | out[7] << 8), 11000);
    CHECK_EQ(int(out[8] | out[9] << 8), 3000);
    CHECK_EQ(int(out[10] | out[11] << 8), 33000);
    CHECK_EQ(int(out[14] | out[15] << 8), 55000);
    CHECK_EQ(int(out[18]), 100);
    CHECK_EQ(int(out[20] | out[21] << 8), 3000);
    CHECK_EQ(int(out[22] | out[23] << 8), 20000);
    o.observed = true;
    o.mismatch = false;
    wire::encode_predict(3, o, f, out);
    CHECK_EQ(int(out[4]), int(wire::kPredObserved));
    std::array<std::uint8_t, 8> tiny{};
    CHECK_EQ(wire::encode_predict(3, o, f, tiny), 0u);
}

void test_usb_text() {
    Twin t = big_twin();
    char b[wire::kUsbText + 1];
    wire::usb_text(t, wire::kUsbDeclared, b);
    CHECK_STR(b, "USB4 40G, DP, TB TB4 class");
    wire::usb_text(t, wire::kUsbObserved, b);
    CHECK_STR(b, "USB4 40G TBT4");
    std::array<std::uint8_t, wire::kMaxRsp> out{};
    const std::size_t n = wire::encode_usb(2, wire::kUsbObserved, t, out);
    CHECK_EQ(n, 4u + 13u);
    CHECK_EQ(int(out[3]), 13);
    CHECK_EQ(std::memcmp(&out[4], "USB4 40G TBT4", 13), 0);
    // nothing held: an empty text, not "-"
    const Twin e;
    CHECK_EQ(wire::encode_usb(2, wire::kUsbDeclared, e, out), 4u);
    CHECK_EQ(int(out[3]), 0);
    CHECK_EQ(wire::encode_usb(2, wire::kUsbObserved, e, out), 4u);
    // the longest text still fits the frame
    CHECK_EQ(4 + wire::kUsbText <= wire::kMaxRsp, true);
}

void test_learn_bytes() {
    Twin t = big_twin();
    Result r;
    r.seq = 5;
    r.existed = true;
    std::array<std::uint8_t, wire::kMaxRsp> out{};
    CHECK_EQ(wire::encode_learn(0, 0, r, t, out), wire::kLearnBytes);
    CHECK_EQ(int(out[0]), 0);
    CHECK_EQ(int(out[2]), 5);
    CHECK_EQ(int(out[3]), 1);
    CHECK_EQ(int(out[5] | out[6] << 8), 0x17EF);
    CHECK_EQ(int(out[7] | out[8] << 8), 0x3071);
    CHECK_EQ(int(out[9]), 4);
    CHECK_EQ(int(out[10]), int(kFlagRequest | kFlagDeclared | kFlagObserved));
    // not ready / not inline / no contract: the err byte is the learn error, `how` says which
    wire::encode_learn(1, 0, r, Twin{}, out);
    CHECK_EQ(int(out[0]), int(wire::kErrLearn));
    CHECK_EQ(int(out[1]), 1);
    // a store failure carries the library's error
    wire::encode_learn(4, std::uint8_t(Err::Full), r, Twin{}, out);
    CHECK_EQ(int(out[0]), int(Err::Full));
}

void test_run_request() {
    wire::RunReq q;
    const std::uint8_t all[] = {wire::kRunConfirm, 0};
    CHECK_EQ(wire::parse_run(all, q), true);
    CHECK_EQ(q.confirm, true);
    CHECK_EQ(int(q.n), 0);
    const std::uint8_t two[] = {0, 2, 3, 1};
    CHECK_EQ(wire::parse_run(two, q), true);
    CHECK_EQ(q.confirm, false);
    CHECK_EQ(int(q.n), 2);
    CHECK_EQ(int(q.seqs[0]), 3);
    CHECK_EQ(int(q.seqs[1]), 1);
    const std::uint8_t short_[] = {1};
    const std::uint8_t bad_len[] = {1, 2, 1};
    const std::uint8_t zero[] = {1, 1, 0};
    const std::uint8_t big[] = {1, 1, 13};
    const std::uint8_t dup[] = {1, 2, 4, 4};
    const std::uint8_t flags[] = {2, 0};
    const std::uint8_t toomany[] = {1, 13, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 1};
    CHECK_EQ(wire::parse_run(short_, q), false);
    CHECK_EQ(wire::parse_run(bad_len, q), false);
    CHECK_EQ(wire::parse_run(zero, q), false);
    CHECK_EQ(wire::parse_run(big, q), false);
    CHECK_EQ(wire::parse_run(dup, q), false);
    CHECK_EQ(wire::parse_run(flags, q), false);
    CHECK_EQ(wire::parse_run(toomany, q), false);
    std::array<std::uint8_t, wire::kMaxRsp> out{};
    CHECK_EQ(wire::encode_run(live::Refusal::NoSource, 3, out), wire::kRunBytes);
    CHECK_EQ(int(out[0]), int(live::Refusal::NoSource));
    CHECK_EQ(int(out[1]), 3);
}

void test_status_and_slot_bytes() {
    live::Runner r;
    std::array<std::uint8_t, wire::kMaxRsp> out{};
    CHECK_EQ(wire::encode_status(r, out), wire::kStatusBytes);
    CHECK_EQ(int(out[0]), 0);
    CHECK_EQ(int(out[1]), 0);   // not running
    CHECK_EQ(int(out[3]), 0);   // count
    CHECK_EQ(int(out[7] | out[8] << 8), 0);
    CHECK_EQ(wire::verdict_mask(r), 0u);
    live::Slot s;
    s.verdict = live::Verdict::Granted;
    s.measured_mv = 11030;
    CHECK_EQ(wire::encode_slot(6, s, out), wire::kSlotBytes);
    CHECK_EQ(int(out[0]), 0);
    CHECK_EQ(int(out[1]), 6);
    CHECK_EQ(int(out[2]), int(live::Verdict::Granted));
    CHECK_EQ(int(out[4] | out[5] << 8), 11030);
    s.verdict = live::Verdict::Skipped;
    s.skip = live::Skip::AboveCeiling;
    wire::encode_slot(6, s, out);
    CHECK_EQ(int(out[3]), int(live::Skip::AboveCeiling));
}

void test_every_response_fits() {
    CHECK_EQ(wire::kPredictBytes <= wire::kMaxRsp, true);
    CHECK_EQ(wire::kLearnBytes <= wire::kMaxRsp, true);
    CHECK_EQ(wire::kStatusBytes <= wire::kMaxRsp, true);
    CHECK_EQ(5 + wire::kChunk <= wire::kMaxRsp, true);
    CHECK_EQ(wire::kMaxParts, 3u);
}

// ---- the handler over a fake Api ----

struct FakeApi {
    std::array<Twin, kCapacity + 1> lib{};
    std::array<bool, kCapacity + 1> held{};
    bool have_caps = true;
    live::Refusal refusal = live::Refusal::None;
    std::array<std::uint8_t, kCapacity> run_seqs{};
    unsigned run_n = 0;
    bool run_confirm = false;
    int predicts = 0, logged = 0, cancels = 0;
    bool is_running = false;
    twin_hw::Learned learned{};
    bool learn_save = false;
    std::uint8_t learn_how = 99;
    char learn_name[32]{};
    live::Runner r;

    unsigned list(std::span<std::uint8_t> o) {
        unsigned n = 0;
        for (unsigned s = 1; s <= kCapacity && n < o.size(); ++s)
            if (held[s]) o[n++] = std::uint8_t(s);
        return n;
    }
    bool get(std::uint8_t s, Twin& t) {
        if (s == 0 || s > kCapacity || !held[s]) return false;
        t = lib[s];
        return true;
    }
    bool predict(const Twin&, Outcome& o, Facts& f) {
        ++predicts;
        if (!have_caps) return false;
        o.kind = Kind::Pps;
        o.mv = 11000;
        o.ma = 3000;
        o.mw = 33000;
        o.want_mw = 33000;
        f.claim_w = 60;
        return true;
    }
    void log_predict(std::uint8_t, const Twin&, const Outcome&) { ++logged; }
    twin_hw::Learned learn(std::uint8_t how, std::string_view name, bool save) {
        learn_how = how;
        learn_save = save;
        std::memset(learn_name, 0, sizeof learn_name);
        std::memcpy(learn_name, name.data(), std::min(name.size(), sizeof learn_name - 1));
        return learned;
    }
    Err rename(std::uint8_t s, std::string_view name) {
        if (s == 0 || !held[s]) return Err::NotFound;
        std::memset(lib[s].name.data(), 0, lib[s].name.size());
        std::memcpy(lib[s].name.data(), name.data(), name.size());
        return Err::Ok;
    }
    Err remove(std::uint8_t s) {
        if (s == 0 || !held[s]) return Err::NotFound;
        held[s] = false;
        return Err::Ok;
    }
    live::Refusal run(std::span<const std::uint8_t> seqs, bool confirmed) {
        run_n = unsigned(seqs.size());
        for (unsigned i = 0; i < run_n; ++i) run_seqs[i] = seqs[i];
        run_confirm = confirmed;
        return refusal;
    }
    const live::Runner& runner() const { return r; }
    bool running() const { return is_running; }
    void cancel() { ++cancels; is_running = false; }
    // the inline interrogation
    pd::IqRefusal iq_refusal = pd::IqRefusal::None;
    bool iq_running = false, iq_save = false, iq_confirmed = false;
    int iq_starts = 0, iq_cancels = 0;
    char iq_name[32]{};
    twin_hw::InlineLearn iq_learn{};
    pd::IqStatus iq{};
    wire::InlineSpoof iq_spoof{};
    pd::IqRefusal inline_start(std::string_view name, bool save, bool confirmed, const wire::InlineSpoof& spoof) {
        ++iq_starts;
        iq_save = save;
        iq_confirmed = confirmed;
        iq_spoof = spoof;
        std::memset(iq_name, 0, sizeof iq_name);
        std::memcpy(iq_name, name.data(), std::min(name.size(), sizeof iq_name - 1));
        if (iq_refusal == pd::IqRefusal::None) iq_running = true;
        return iq_refusal;
    }
    void inline_cancel() { ++iq_cancels; iq_running = false; }
    bool inline_running() const { return iq_running; }
    const twin_hw::InlineLearn& inline_learn() const { return iq_learn; }
    const pd::IqStatus& inline_status() const { return iq; }
    void put(std::uint8_t s, const char* name) {
        held[s] = true;
        lib[s] = big_twin();
        std::memset(lib[s].name.data(), 0, lib[s].name.size());
        std::strcpy(lib[s].name.data(), name);
    }
};

std::size_t call(FakeApi& a, std::initializer_list<std::uint8_t> req, std::array<std::uint8_t, wire::kMaxRsp>& out) {
    std::vector<std::uint8_t> v(req);
    out.fill(0xEE);
    return wire::handle(a, v, out);
}

void test_handler_list_get_predict() {
    FakeApi a;
    a.put(2, "Pixel");
    a.put(5, "Laptop");
    std::array<std::uint8_t, wire::kMaxRsp> out{};
    CHECK_EQ(call(a, {wire::kOpList}, out), 4u);
    CHECK_EQ(int(out[1]), 2);
    CHECK_EQ(int(out[2]), 2);
    CHECK_EQ(int(out[3]), 5);
    // GET: part 0 of twin 5, then a missing twin
    CHECK_EQ(call(a, {wire::kOpGet, 5, 0}, out) > 5, true);
    CHECK_EQ(int(out[1]), 5);
    CHECK_EQ(call(a, {wire::kOpGet, 6, 0}, out), 1u);
    CHECK_EQ(int(out[0]), int(Err::NotFound));
    // PREDICT: one log line per prediction, none when it could not predict
    CHECK_EQ(call(a, {wire::kOpPredict, 2}, out), wire::kPredictBytes);
    CHECK_EQ(int(out[2]), int(Kind::Pps));
    CHECK_EQ(a.logged, 1);
    a.have_caps = false;
    CHECK_EQ(call(a, {wire::kOpPredict, 2}, out), 1u);
    CHECK_EQ(int(out[0]), int(wire::kErrNoCaps));
    CHECK_EQ(a.logged, 1);
    CHECK_EQ(call(a, {wire::kOpPredict, 9}, out), 1u);
    CHECK_EQ(int(out[0]), int(Err::NotFound));
    // USB text of a held twin
    CHECK_EQ(call(a, {wire::kOpUsb, 2, wire::kUsbDeclared}, out) > 4, true);
    CHECK_EQ(int(out[1]), 2);
}

void test_handler_writes() {
    FakeApi a;
    a.put(3, "Camera");
    std::array<std::uint8_t, wire::kMaxRsp> out{};
    CHECK_EQ(call(a, {wire::kOpRename, 3, 'C', 'a', 'm', '2'}, out), 2u);
    CHECK_EQ(int(out[0]), 0);
    CHECK_STR(a.lib[3].name.data(), "Cam2");
    CHECK_EQ(call(a, {wire::kOpRename, 4, 'X'}, out), 2u);
    CHECK_EQ(int(out[0]), int(Err::NotFound));
    CHECK_EQ(call(a, {wire::kOpDelete, 3, 0}, out), 1u);   // no confirm byte, nothing deleted
    CHECK_EQ(a.held[3], true);
    CHECK_EQ(call(a, {wire::kOpDelete, 3, 1}, out), 2u);
    CHECK_EQ(int(out[0]), 0);
    CHECK_EQ(a.held[3], false);
    // LEARN passes how, the dry bit and the name through
    a.learned.how = twin_hw::Learn::Ok;
    a.learned.store.seq = 4;
    a.learned.twin = big_twin();
    CHECK_EQ(call(a, {wire::kOpLearn, 1, 0, 'P', 'x'}, out), wire::kLearnBytes);
    CHECK_EQ(int(a.learn_how), 1);
    CHECK_EQ(a.learn_save, true);
    CHECK_STR(a.learn_name, "Px");
    CHECK_EQ(int(out[0]), 0);
    CHECK_EQ(int(out[2]), 4);
    call(a, {wire::kOpLearn, 0, 1}, out);
    CHECK_EQ(a.learn_save, false);
    a.learned.how = twin_hw::Learn::NotReady;
    call(a, {wire::kOpLearn, 0, 0}, out);
    CHECK_EQ(int(out[0]), int(wire::kErrLearn));
    CHECK_EQ(int(out[1]), int(twin_hw::Learn::NotReady));
}

void test_handler_run_status_cancel() {
    FakeApi a;
    a.put(1, "A");
    a.put(7, "B");
    std::array<std::uint8_t, wire::kMaxRsp> out{};
    // n = 0 means every twin in the library
    CHECK_EQ(call(a, {wire::kOpRun, wire::kRunConfirm, 0}, out), wire::kRunBytes);
    CHECK_EQ(int(out[0]), 0);
    CHECK_EQ(int(out[1]), 2);
    CHECK_EQ(a.run_n, 2u);
    CHECK_EQ(int(a.run_seqs[0]), 1);
    CHECK_EQ(int(a.run_seqs[1]), 7);
    CHECK_EQ(a.run_confirm, true);
    // a named list keeps its order; a refusal is passed on
    a.refusal = live::Refusal::NoSource;
    CHECK_EQ(call(a, {wire::kOpRun, wire::kRunConfirm, 1, 7}, out), wire::kRunBytes);
    CHECK_EQ(int(out[0]), int(live::Refusal::NoSource));
    CHECK_EQ(a.run_n, 1u);
    CHECK_EQ(int(a.run_seqs[0]), 7);
    // a malformed run request touches nothing
    a.run_n = 0;
    CHECK_EQ(call(a, {wire::kOpRun, wire::kRunConfirm, 2, 7}, out), 1u);
    CHECK_EQ(int(out[0]), int(Err::Invalid));
    CHECK_EQ(a.run_n, 0u);
    // status, result, cancel
    CHECK_EQ(call(a, {wire::kOpStatus}, out), wire::kStatusBytes);
    CHECK_EQ(int(out[1]), 0);
    CHECK_EQ(call(a, {wire::kOpResult, 7}, out), wire::kSlotBytes);
    CHECK_EQ(int(out[1]), 7);
    a.is_running = true;
    CHECK_EQ(call(a, {wire::kOpCancel}, out), 2u);
    CHECK_EQ(int(out[1]), 1);
    CHECK_EQ(a.cancels, 1);
    CHECK_EQ(call(a, {wire::kOpCancel}, out), 2u);
    CHECK_EQ(int(out[1]), 0);
    // unknown op and short requests answer Invalid, never read past the payload
    CHECK_EQ(call(a, {99}, out), 1u);
    CHECK_EQ(int(out[0]), int(Err::Invalid));
    CHECK_EQ(call(a, {wire::kOpGet, 1}, out), 1u);
    CHECK_EQ(int(out[0]), int(Err::Invalid));
    std::array<std::uint8_t, 4> tiny{};
    std::vector<std::uint8_t> v{wire::kOpList};
    CHECK_EQ(wire::handle(a, v, tiny), 0u);
}

// INLINE: start passes the name, the dry bit and the confirm; a refusal says which; status, abort and the answers read back.
void test_handler_inline() {
    FakeApi a;
    std::array<std::uint8_t, wire::kMaxRsp> out{};
    CHECK_EQ(call(a, {wire::kOpInline, wire::kInlineStart, wire::kInlineConfirm, 'C', 'a', 'n'}, out), 3u);
    CHECK_EQ(int(out[0]), 0);
    CHECK_EQ(int(out[1]), int(wire::kInlineStart));
    CHECK_EQ(int(out[2]), int(pd::IqRefusal::None));
    CHECK_EQ(a.iq_save, true);
    CHECK_EQ(a.iq_confirmed, true);
    CHECK_STR(a.iq_name, "Can");
    a.iq_running = false;
    CHECK_EQ(call(a, {wire::kOpInline, wire::kInlineStart, wire::kInlineDry}, out), 3u);   // the handler passes no confirm on
    CHECK_EQ(a.iq_save, false);
    CHECK_EQ(a.iq_confirmed, false);
    a.iq_refusal = pd::IqRefusal::OwnContract;
    CHECK_EQ(call(a, {wire::kOpInline, wire::kInlineStart, wire::kInlineConfirm}, out), 3u);
    CHECK_EQ(int(out[0]), int(wire::kErrIqRefused));
    CHECK_EQ(int(out[2]), int(pd::IqRefusal::OwnContract));
    CHECK_EQ(call(a, {wire::kOpInline, wire::kInlineStart, 8}, out), 1u);   // an unknown flag bit
    CHECK_EQ(int(out[0]), int(Err::Invalid));
    CHECK_EQ(a.iq_starts, 3);
    CHECK_EQ(a.iq_spoof.on, false);                                         // no spoof bit, no spoof

    // START with the spoof bit: kind, id and then the name; a short head or an unknown kind is invalid
    a.iq_refusal = pd::IqRefusal::None;
    a.iq_running = false;
    CHECK_EQ(call(a, {wire::kOpInline, wire::kInlineStart, wire::kInlineConfirm | wire::kInlineSpoof, 1, 0x34, 0x12, 'X', 'y'}, out), 3u);
    CHECK_EQ(int(out[0]), 0);
    CHECK_EQ(a.iq_spoof.on, true);
    CHECK_EQ(int(a.iq_spoof.kind), int(wire::kInlineSpoofSaved));
    CHECK_EQ(int(a.iq_spoof.id), 0x1234);
    CHECK_STR(a.iq_name, "Xy");                                             // the name starts after the 6-byte head
    CHECK_EQ(a.iq_confirmed, true);
    a.iq_running = false;
    CHECK_EQ(call(a, {wire::kOpInline, wire::kInlineStart, wire::kInlineConfirm | wire::kInlineSpoof, 0, 3, 0}, out), 3u);   // built-in 3, no name
    CHECK_EQ(int(a.iq_spoof.kind), int(wire::kInlineSpoofBuiltin));
    CHECK_EQ(int(a.iq_spoof.id), 3);
    CHECK_EQ(a.iq_name[0], 0);
    CHECK_EQ(call(a, {wire::kOpInline, wire::kInlineStart, wire::kInlineConfirm | wire::kInlineSpoof, 0, 3}, out), 1u);   // head too short
    CHECK_EQ(int(out[0]), int(Err::Invalid));
    CHECK_EQ(call(a, {wire::kOpInline, wire::kInlineStart, wire::kInlineConfirm | wire::kInlineSpoof, 2, 3, 0}, out), 1u);   // unknown kind
    CHECK_EQ(int(out[0]), int(Err::Invalid));
    a.iq_refusal = pd::IqRefusal::BadSpoof;
    CHECK_EQ(call(a, {wire::kOpInline, wire::kInlineStart, wire::kInlineConfirm | wire::kInlineSpoof, 0, 3, 0}, out), 3u);
    CHECK_EQ(int(out[0]), int(wire::kErrIqRefused));
    CHECK_EQ(int(out[2]), int(pd::IqRefusal::BadSpoof));
    a.iq_refusal = pd::IqRefusal::None;
    a.iq_spoof = {};
    a.iq_starts = 3;

    // STATUS: the run, the masks, the identity, both contracts and what was learned
    a.iq_running = true;
    a.iq_learn.pending = true;
    a.iq.phase = pd::IqPhase::Done;
    a.iq.step = pd::IqStep::Done;
    a.iq.match = pd::IqMatch::Differs;
    a.iq.rev = 2;
    a.iq.asked_mask = 0x3F;
    a.iq.ok_mask = 0x1B;
    a.iq.refused_mask = 0x04;
    a.iq.silent_mask = 0x20;
    a.iq.sent = 7;
    a.iq.dev_acks = 6;
    a.iq.resets_sent = 2;
    a.iq.id.valid = true;
    a.iq.id.vid = 0x04A9;
    a.iq.id.pid = 0x3254;
    a.iq.num_svids = 1;
    a.iq.num_sink_pdos = 2;
    a.iq.sink_ext_len = 24;
    a.iq.revision = 0x32100000u;
    a.iq.first = {true, 4, 2, 0x4344B12Cu, 0x0004B12Cu};
    a.iq.second = {true, 1, 2, 0x1304B12Cu, 0x0001912Cu};
    a.iq.late_mask = 0x08;
    a.iq.spoof = true;
    a.iq.epr_partial = true;
    a.iq.sopp_queries = 3;
    a.iq.sopp_replies = 2;
    a.iq.num_epr_pdos = 9;
    a.iq.started_at = 1000;
    a.iq.ended_at = 4500;
    a.iq_learn.result.how = twin_hw::Learn::Ok;
    a.iq_learn.result.store.seq = 3;
    a.iq_learn.result.store.existed = true;
    CHECK_EQ(call(a, {wire::kOpInline, wire::kInlineStatus}, out), wire::kIqStatusBytes);
    CHECK_EQ(int(out[2]), 1);
    CHECK_EQ(int(out[3]), 1);
    CHECK_EQ(int(out[5]), int(pd::IqPhase::Done));
    CHECK_EQ(int(out[8]), int(pd::IqMatch::Differs));
    CHECK_EQ(int(out[10]), 0x3F);
    CHECK_EQ(int(out[11]), 0x1B);
    CHECK_EQ(int(out[12]), 0x04);
    CHECK_EQ(int(out[13]), 0x20);
    CHECK_EQ(int(out[19] | out[20] << 8), 0x04A9);
    CHECK_EQ(int(out[21] | out[22] << 8), 0x3254);
    CHECK_EQ(int(out[25]), 2);
    CHECK_EQ(int(out[26]), 24);
    CHECK_EQ(int(out[30]), 0x32);
    CHECK_EQ(int(out[31]), 1);
    CHECK_EQ(int(out[32]), 4);
    CHECK_EQ(int(out[36]), 0x43);
    CHECK_EQ(int(out[38]), 1);
    CHECK_EQ(int(out[45]), 3);
    CHECK_EQ(int(out[46]), 1);
    CHECK_EQ(int(out[48] | out[49] << 8), 3500);
    CHECK_EQ(int(out[50]), 0x08);                      // late mask
    CHECK_EQ(int(out[51]), 3);                         // spoof | EPR partial
    CHECK_EQ(int(out[52]), 3);                         // SOP' queries
    CHECK_EQ(int(out[53]), 2);                         // spoof replies
    CHECK_EQ(int(out[54]), 9);                         // EPR sink PDOs

    // ANSWERS: the sink PDOs, the extended caps bytes, the identity VDOs, the SVIDs and modes; part 4 does not exist
    a.iq.sink_pdos[0] = 0x2601912Cu;
    a.iq.sink_pdos[1] = 0x0002D12Cu;
    for (std::size_t i = 0; i < 24; ++i) a.iq.sink_ext[i] = std::uint8_t(i + 1);
    a.iq.id.num_vdos = 3;
    a.iq.id.vdos[2] = 0xAABBCCDDu;
    a.iq.svids[0] = 0xFF01;
    a.iq.num_modes = 1;
    a.iq.mode_svid[0] = 0xFF01;
    a.iq.mode_vdo[0] = 0x00000405u;
    CHECK_EQ(call(a, {wire::kOpInline, wire::kInlineAnswers, 0}, out), 4u + 8u);
    CHECK_EQ(int(out[3]), 2);
    CHECK_EQ(int(out[7]), 0x26);
    CHECK_EQ(call(a, {wire::kOpInline, wire::kInlineAnswers, 1}, out), 4u + 24u);
    CHECK_EQ(int(out[4]), 1);
    CHECK_EQ(int(out[27]), 24);
    CHECK_EQ(call(a, {wire::kOpInline, wire::kInlineAnswers, 2}, out), 5u + 12u);
    CHECK_EQ(int(out[3]), 1);
    CHECK_EQ(int(out[4]), 3);
    CHECK_EQ(int(out[13]), 0xDD);
    CHECK_EQ(call(a, {wire::kOpInline, wire::kInlineAnswers, 3}, out), 4u + 2u + 1u + 6u);
    CHECK_EQ(int(out[4] | out[5] << 8), 0xFF01);
    CHECK_EQ(int(out[6]), 1);
    CHECK_EQ(int(out[9]), 0x05);
    // part 4: the EPR sink PDOs [n, partial, u32 * n]; part 5 does not exist
    for (std::size_t i = 0; i < 9; ++i) a.iq.epr_pdos[i] = 0xA0000000u + std::uint32_t(i);
    CHECK_EQ(call(a, {wire::kOpInline, wire::kInlineAnswers, 4}, out), 3u + 2u + 9u * 4u);
    CHECK_EQ(int(out[0]), 0);
    CHECK_EQ(int(out[3]), 9);
    CHECK_EQ(int(out[4]), 1);                          // partial
    CHECK_EQ(int(out[5]), 0x00);
    CHECK_EQ(int(out[8]), 0xA0);
    CHECK_EQ(int(out[5 + 8 * 4]), 0x08);               // the ninth PDO
    a.iq.num_epr_pdos = 13;
    CHECK_EQ(call(a, {wire::kOpInline, wire::kInlineAnswers, 4}, out), 3u + 2u + 13u * 4u);   // the most the 58-byte frame holds
    a.iq.num_epr_pdos = 0;
    CHECK_EQ(call(a, {wire::kOpInline, wire::kInlineAnswers, 4}, out), 5u);                    // none
    CHECK_EQ(int(out[3]), 0);
    CHECK_EQ(call(a, {wire::kOpInline, wire::kInlineAnswers, 5}, out), 1u);
    CHECK_EQ(int(out[0]), int(Err::Invalid));

    // ABORT: says whether a run was going
    CHECK_EQ(call(a, {wire::kOpInline, wire::kInlineAbort}, out), 3u);
    CHECK_EQ(int(out[2]), 1);
    CHECK_EQ(a.iq_cancels, 1);
    CHECK_EQ(call(a, {wire::kOpInline, wire::kInlineAbort}, out), 3u);
    CHECK_EQ(int(out[2]), 0);
    CHECK_EQ(call(a, {wire::kOpInline}, out), 1u);
    CHECK_EQ(call(a, {wire::kOpInline, 9}, out), 1u);
    CHECK_EQ(int(out[0]), int(Err::Invalid));
}

}  // namespace

int main() {
    test_handler_list_get_predict();
    test_handler_writes();
    test_handler_run_status_cancel();
    test_handler_inline();
    test_list();
    test_record_chunks_reassemble();
    test_predict_bytes();
    test_usb_text();
    test_learn_bytes();
    test_run_request();
    test_status_and_slot_bytes();
    test_every_response_fits();
    return test_report("test_twin_wire");
}
