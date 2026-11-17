// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// k2::emk_learn: the learn plan against a fake cable, and the clone it builds answering the same requests.
#include <array>
#include <vector>

#include "k2/pd/emk_learn.hpp"
#include "k2/pd/emk_spoof.hpp"
#include "test_util.h"

using namespace k2;
using emk_learn::Reply;
using emk_learn::Request;
using emk_learn::Step;
namespace q = emk_quirk;

// The firmware keeps one Result in static RAM next to the stack: it borrows the record and the quirk table (from
// the spoof) and holds only a packed transcript, so it stays small (docs/ARCHITECTURE.md, RAM).
static_assert(sizeof(emk_learn::Result) <= 380, "emk_learn::Result must stay small");

enum class How { Ack, Nak, Busy, NotSupported };

struct Behaviour {
    std::uint16_t svid;
    std::uint8_t cmd;
    How how;
    std::vector<std::uint32_t> words;   // VDOs after the VDM header (Ack)
};

struct FakeCable {
    bool pd3 = true;                     // answers a PD 3.x request; false: only PD 2.0 (spec rev field 1)
    bool svdm1_only = false;             // acknowledges only a PD 3.x header with an SVDM 1.0 VDM header (nothing as PD 2.0)
    bool alive = true;                   // false: the cable is not there at all (no answer, GoodCRC or not)
    int fail_after = -1;                 // the exchange reports "no GoodCRC" from this call on
    std::vector<std::uint32_t> id{0xFF00A041, 0x1C00203C, 0x00000123, 0x00010000, 0x00082052};
    std::vector<Behaviour> table;
    std::vector<Request> seen;
};

static Reply vdm_reply(const Request& req, unsigned type, const std::vector<std::uint32_t>& vdos) {
    Reply r;
    r.got = true;
    r.n = static_cast<std::uint8_t>(1 + vdos.size());
    r.header = static_cast<std::uint16_t>(0x0F | 0x100 | (req.spec_rev << 6) | (r.n << 12));
    r.w[0] = (req.w[0] & ~(3u << 6)) | (type << 6);
    for (std::size_t i = 0; i < vdos.size(); i++) r.w[1 + i] = vdos[i];
    return r;
}

static bool fake_exchange(void* ctx, const Request& req, Reply& rep) {
    auto& c = *static_cast<FakeCable*>(ctx);
    c.seen.push_back(req);
    rep = {};
    if (c.fail_after >= 0 && static_cast<int>(c.seen.size()) > c.fail_after) return false;
    if (!c.alive) return true;                         // GoodCRC only (or no cable: the fake does not tell)
    const std::uint32_t h = req.w[0];
    if (c.svdm1_only && (req.spec_rev != 2 || (h & 0x2000u))) return true;   // GoodCRC, no answer
    const std::uint16_t svid = static_cast<std::uint16_t>(h >> 16);
    const unsigned cmd = h & 0x1F;
    if (svid == 0xFF00 && cmd == 1) {
        if (req.spec_rev == 2 && !c.pd3) return true;
        rep = vdm_reply(req, 1, {c.id.begin() + 1, c.id.end()});
        return true;
    }
    for (const Behaviour& b : c.table) {
        if (b.svid != svid || b.cmd != cmd) continue;
        switch (b.how) {
        case How::Ack: rep = vdm_reply(req, 1, b.words); break;
        case How::Nak: rep = vdm_reply(req, 2, {}); break;
        case How::Busy: rep = vdm_reply(req, 3, {}); break;
        case How::NotSupported:
            rep.got = true;
            rep.header = 16 | 0x100 | (req.spec_rev << 6);
            break;
        }
        return true;
    }
    return true;   // an unknown request: GoodCRC, then silence
}

static std::vector<std::uint8_t> bytes(const Request& r) {
    std::vector<std::uint8_t> b;
    for (unsigned i = 0; i < r.n; i++)
        for (int k = 0; k < 4; k++) b.push_back(static_cast<std::uint8_t>(r.w[i] >> (8 * k)));
    return b;
}

static emk_learn::Options none() { return {}; }

static const Behaviour kSvids22D9{0xFF00, 2, How::Ack, {0x22D90000u}};          // one SVID: 0x22D9, then the terminator half
static const Behaviour kModes22D9{0x22D9, 3, How::Ack, {0x00010001u}};
static const Behaviour kOppo{0x22D9, 31, How::Ack, {0xAB01CD02u, 0x00000007u}};

static void test_identity_only() {
    FakeCable c;
    emk_learn::Standalone r;
    CHECK_EQ(emk_learn::learn(&fake_exchange, &c, none(), r), true);
    CHECK_EQ(int(r.outcome), int(emk_learn::Outcome::Learned));
    CHECK_EQ(emrec::valid(r.rec), true);
    for (unsigned i = 0; i < 5; i++) CHECK_EQ(emrec::vdo(r.rec, i), c.id[i]);
    CHECK_EQ(r.rec[0x14], 2);   // pd_spec_rev: PD 3.x
    // The Discover SVIDs probe is silent: no rows, default Ignore.
    CHECK_EQ(r.quirks.count, 0);
    CHECK_EQ(int(r.quirks.unmatched), int(q::Unmatched::Ignore));
    CHECK_EQ(c.seen.size(), 2u);   // Discover Identity, Discover SVIDs
    CHECK_EQ(c.seen[0].w[0], 0xFF00A001u);
    CHECK_EQ(c.seen[0].spec_rev, 2);
    CHECK_EQ(c.seen[1].w[0], 0xFF00A002u);
}

static void test_full_walk_and_clone_answers_the_same() {
    FakeCable c;
    c.table = {kSvids22D9, kModes22D9, kOppo};
    emk_learn::Options o;
    o.seeds[0] = {0x22D9, 31, 0, {}};
    o.nseeds = 1;
    emk_learn::Standalone r;
    CHECK_EQ(emk_learn::learn(&fake_exchange, &c, o, r), true);
    CHECK_EQ(r.svids, 1);
    CHECK_EQ(r.quirks.count, 3);   // SVIDs, Modes 22D9, the seed
    // Order of the plan: DI, SVIDs, Modes(22D9), Seed.
    CHECK_EQ(c.seen.size(), 4u);
    CHECK_EQ(c.seen[2].w[0], 0x22D9A003u);
    CHECK_EQ(c.seen[3].w[0], 0x22D9A01Fu);
    // The transcript keeps every step with its reply.
    CHECK_EQ(r.nlog, 4);
    CHECK_EQ(int(r.entry(0).step), int(Step::Identity));
    CHECK_EQ(int(r.entry(3).step), int(Step::Seed));
    CHECK_EQ(r.entry(3).rep.n, 3);
    CHECK_EQ(r.entry(3).rep.w[1], 0xAB01CD02u);
    // The packed transcript round-trips a whole exchange, request and reply.
    CHECK_EQ(r.entry(3).req.n, 1);
    CHECK_EQ(r.entry(3).req.w[0], 0x22D9A01Fu);
    CHECK_EQ(r.entry(3).rep.got, true);
    CHECK_EQ(r.entry(0).rep.n, 5);
    CHECK_EQ(r.entry(0).rep.w[4], c.id[4]);
    CHECK_EQ(r.log_full, false);
    CHECK_EQ(int(r.entry(4).step), int(Step::Identity));   // past the log: an empty entry
    CHECK_EQ(r.entry(4).rep.got, false);
    // The clone answers the cmd-31 request with the learned words, via the real responder.
    const auto req = bytes(c.seen[3]);
    const auto o2 = emk_spoof::respond(r.rec, r.quirks, q::kSopP, 0x128F, req, 0);
    CHECK_EQ(int(o2.how), int(emk_spoof::Match::Row));
    // Frame: sync(4) PACKSYM hdr(2) 3 words(12) FF 14 FE
    CHECK_EQ(o2.frame.n, 4 + 1 + 2 + 12 + 3);
    const auto& t = o2.frame.t;
    auto word = [&](unsigned i) { return t[7 + 4 * i] | (t[8 + 4 * i] << 8) | (t[9 + 4 * i] << 16) | (std::uint32_t(t[10 + 4 * i]) << 24); };
    CHECK_EQ(word(0), (0x22D9A01Fu & ~(3u << 6)) | (1u << 6));
    CHECK_EQ(word(1), 0xAB01CD02u);
    CHECK_EQ(word(2), 7u);
    // And the modes request too.
    const auto o3 = emk_spoof::respond(r.rec, r.quirks, q::kSopP, 0x128F, bytes(c.seen[2]), 0);
    CHECK_EQ(int(o3.how), int(emk_spoof::Match::Row));
}

static void test_pd2_only_cable() {
    FakeCable c;
    c.pd3 = false;
    emk_learn::Standalone r;
    CHECK_EQ(emk_learn::learn(&fake_exchange, &c, none(), r), true);
    CHECK_EQ(r.rec[0x14], 1);   // PD 2.0
    // The attempt table: PD 3.x + SVDM 2.0, PD 3.x + SVDM 1.0, PD 2.0 + SVDM 1.0.
    CHECK_EQ(c.seen.size(), 4u);
    if (c.seen.size() != 4u) return;
    CHECK_EQ(c.seen[0].spec_rev, 2);
    CHECK_EQ(c.seen[0].w[0], 0xFF00A001u);
    CHECK_EQ(c.seen[1].spec_rev, 2);
    CHECK_EQ(c.seen[1].w[0], 0xFF008001u);
    CHECK_EQ(c.seen[2].spec_rev, 1);
    CHECK_EQ(c.seen[2].w[0], 0xFF008001u);
    // The rest of the walk goes at the cable's variant.
    CHECK_EQ(c.seen[3].spec_rev, 1);
    CHECK_EQ(c.seen[3].w[0], 0xFF008002u);
}

// The Baseus 100 W cable: acknowledges only a PD 3.x header with SVDM 1.0. Found on the second attempt; Discover
// SVIDs, Discover Modes and the seeds then use that header revision AND that SVDM version.
static void test_svdm1_only_cable_learns_and_follows_up_with_its_variant() {
    FakeCable c;
    c.svdm1_only = true;
    c.table = {kSvids22D9, kModes22D9, kOppo};
    emk_learn::Options o;
    o.seeds[0] = {0x22D9, 31, 0, {}};
    o.nseeds = 1;
    emk_learn::Standalone r;
    CHECK_EQ(emk_learn::learn(&fake_exchange, &c, o, r), true);
    CHECK_EQ(int(r.outcome), int(emk_learn::Outcome::Learned));
    CHECK_EQ(c.seen.size(), 5u);   // DI (SVDM 2.0, silent), DI (SVDM 1.0), SVIDs, Modes, seed
    if (c.seen.size() != 5u) return;
    CHECK_EQ(c.seen[0].w[0], 0xFF00A001u);
    CHECK_EQ(c.seen[1].spec_rev, 2);
    CHECK_EQ(c.seen[1].w[0], 0xFF008001u);
    for (unsigned i = 2; i < 5; i++) CHECK_EQ(c.seen[i].spec_rev, 2);       // PD 3.x header
    CHECK_EQ(c.seen[2].w[0], 0xFF008002u);                                  // SVDM 1.0
    CHECK_EQ(c.seen[3].w[0], 0x22D98003u);
    CHECK_EQ(c.seen[4].w[0], 0x22D9801Fu);
    CHECK_EQ(r.svids, 1);
    CHECK_EQ(r.quirks.count, 3);
    CHECK_EQ(r.rec[0x14], 2);   // the cable's answer carried a PD 3.x header
}

// A cable that answers the first variant sees exactly what it saw before: one Discover Identity, then SVDM 2.0.
static void test_first_variant_cable_behaves_as_before() {
    FakeCable c;
    c.table = {kSvids22D9, kModes22D9};
    emk_learn::Standalone r;
    CHECK_EQ(emk_learn::learn(&fake_exchange, &c, none(), r), true);
    CHECK_EQ(c.seen.size(), 3u);   // DI, SVIDs, Modes
    if (c.seen.size() != 3u) return;
    CHECK_EQ(c.seen[0].spec_rev, 2);
    CHECK_EQ(c.seen[0].w[0], 0xFF00A001u);
    CHECK_EQ(c.seen[1].w[0], 0xFF00A002u);
    CHECK_EQ(c.seen[2].w[0], 0x22D9A003u);
}

static void test_no_identity() {
    FakeCable c;
    c.alive = false;
    emk_learn::Standalone r;
    CHECK_EQ(emk_learn::learn(&fake_exchange, &c, none(), r), false);
    CHECK_EQ(int(r.outcome), int(emk_learn::Outcome::NoIdentity));
    CHECK_EQ(c.seen.size(), 3u);   // the three variants of the table, nothing more
    CHECK_EQ(r.quirks.count, 0);
}

static void test_link_lost_aborts() {
    FakeCable c;
    c.table = {kSvids22D9, kModes22D9};
    c.fail_after = 2;   // DI and SVIDs are acknowledged, the Modes request is not
    emk_learn::Standalone r;
    CHECK_EQ(emk_learn::learn(&fake_exchange, &c, none(), r), false);
    CHECK_EQ(int(r.outcome), int(emk_learn::Outcome::Aborted));
    CHECK_EQ(c.seen.size(), 3u);
    CHECK_EQ(emrec::valid(r.rec), true);   // what was read before stays available
    CHECK_EQ(r.quirks.count, 1);
}

static void test_nak_busy_and_not_supported() {
    FakeCable c;
    c.table = {{0xFF00, 2, How::Nak, {}}, {0x1234, 16, How::Busy, {}}, {0x1234, 17, How::NotSupported, {}}, {0x1234, 18, How::Nak, {}}};
    emk_learn::Options o;
    o.seeds[0] = {0x1234, 16, 0, {}};
    o.seeds[1] = {0x1234, 17, 0, {}};
    o.seeds[2] = {0x1234, 18, 0, {}};
    o.nseeds = 3;
    emk_learn::Standalone r;
    CHECK_EQ(emk_learn::learn(&fake_exchange, &c, o, r), true);
    // NAK to Discover SVIDs: a NAK row and no Modes requests.
    CHECK_EQ(r.svids, 0);
    CHECK_EQ(r.quirks.rows[0].kind == q::Kind::Nak, true);
    // BUSY and NAK seeds are rows, Not_Supported is a policy, not a row.
    CHECK_EQ(r.quirks.count, 3);
    CHECK_EQ(r.quirks.rows[1].kind == q::Kind::Busy, true);
    CHECK_EQ(r.quirks.rows[2].kind == q::Kind::Nak, true);
    CHECK_EQ(int(r.quirks.unmatched), int(q::Unmatched::NotSupported));
    CHECK_EQ(r.silent, 0);
}

static void test_seed_with_objects_and_silence() {
    FakeCable c;
    emk_learn::Options o;
    o.seeds[0] = {0x22D9, 31, 2, {0x11111111u, 0x22222222u}};
    o.nseeds = 1;
    emk_learn::Standalone r;
    CHECK_EQ(emk_learn::learn(&fake_exchange, &c, o, r), true);
    CHECK_EQ(c.seen.back().n, 3);
    CHECK_EQ(c.seen.back().w[1], 0x11111111u);
    CHECK_EQ(c.seen.back().w[2], 0x22222222u);
    // Silent seed: no row, counted.
    CHECK_EQ(r.quirks.count, 0);
    CHECK_EQ(r.silent, 2);   // SVIDs and the seed
}

static void test_svid_list_decoding_and_budget() {
    FakeCable c;
    // Four SVIDs in two words (0x1111, 0x2222, 0x3333, 0x4444): only kMaxSvids are explored.
    c.table = {{0xFF00, 2, How::Ack, {0x11112222u, 0x33334444u}}};
    emk_learn::Standalone r;
    CHECK_EQ(emk_learn::learn(&fake_exchange, &c, none(), r), true);
    CHECK_EQ(r.svids, 4);
    CHECK_EQ(c.seen.size(), 2u + emk_learn::kMaxSvids);
    CHECK_EQ(c.seen[2].w[0], 0x1111A003u);
    CHECK_EQ(c.seen[4].w[0], 0x3333A003u);
    // The terminator (a zero half) ends the list.
    FakeCable d;
    d.table = {{0xFF00, 2, How::Ack, {0x22D90000u}}, {0xFF00, 3, How::Nak, {}}};
    emk_learn::Standalone r2;
    CHECK_EQ(emk_learn::learn(&fake_exchange, &d, none(), r2), true);
    CHECK_EQ(r2.svids, 1);
    CHECK_EQ(d.seen.size(), 3u);
}

static void test_sweep_records_acks_and_policy() {
    FakeCable c;
    c.table = {kSvids22D9, {0x22D9, 20, How::Ack, {0xCAFE0001u}}, {0x22D9, 21, How::Nak, {}}};
    emk_learn::Options o;
    o.sweep = true;
    emk_learn::Standalone r;
    CHECK_EQ(emk_learn::learn(&fake_exchange, &c, o, r), true);
    // 16 sweep commands on the one SVID, after DI, SVIDs and Modes.
    CHECK_EQ(c.seen.size(), 3u + 16u);
    // The ACK is a row, the NAK makes the unmatched policy Nak, silence changes nothing.
    bool ack = false;
    for (unsigned i = 0; i < r.quirks.count; i++)
        if (r.quirks.rows[i].hdr_val == q::svdm_req(0x22D9, 20) && r.quirks.rows[i].kind == q::Kind::Ack) ack = true;
    CHECK_EQ(ack, true);
    CHECK_EQ(int(r.quirks.unmatched), int(q::Unmatched::Nak));
    CHECK_EQ(r.quirks.count, 2);   // SVIDs row + cmd 20 (a sweep NAK is covered by the policy)
    // Only answered probes are logged: DI, SVIDs, Modes (silent, but not a sweep step) and the two sweep answers.
    unsigned sweeps = 0;
    for (unsigned i = 0; i < r.nlog; i++) sweeps += r.entry(i).step == Step::Sweep;
    CHECK_EQ(sweeps, 2u);
}

static void test_row_budget_truncates() {
    FakeCable c;
    c.table = {kSvids22D9};
    for (unsigned cmd = 16; cmd < 32; cmd++) c.table.push_back({0x22D9, static_cast<std::uint8_t>(cmd), How::Ack, {cmd}});
    emk_learn::Options o;
    o.sweep = true;
    emk_learn::Standalone r;
    CHECK_EQ(emk_learn::learn(&fake_exchange, &c, o, r), true);
    CHECK_EQ(r.quirks.count, q::kMaxRows);
    CHECK_EQ(r.truncated, true);
    // 16 answered sweep probes do not fit the packed transcript: it stops at the last whole entry and says so.
    CHECK_EQ(r.log_full, true);
    CHECK_EQ(r.nlog < 3 + 16, true);
    CHECK_EQ(r.nlog > 6, true);
    CHECK_EQ(r.log_len <= emk_learn::kLogBytes, true);
    unsigned total = 0;
    for (unsigned i = 0; i < r.nlog; i++) {
        const auto x = r.entry(i);
        CHECK_EQ(x.rep.got || x.step != Step::Sweep, true);   // silent sweep probes are never kept
        total += 7 + 4 * (x.req.n + x.rep.n);
    }
    CHECK_EQ(total, r.log_len);   // no gaps, no partial entry
    std::uint8_t buf[emk_learn::kEntryMax];
    CHECK_EQ(emk_learn::encode_entry(r, r.nlog, buf), 1u);   // not found
    // Seeds are reserved their rows before the sweep fills the table.
    FakeCable d = c;
    emk_learn::Options o2 = o;
    o2.seeds[0] = {0x22D9, 31, 0, {}};
    o2.nseeds = 1;
    emk_learn::Standalone r2;
    CHECK_EQ(emk_learn::learn(&fake_exchange, &d, o2, r2), true);
    bool seed_row = false;
    for (unsigned i = 0; i < r2.quirks.count; i++) seed_row |= r2.quirks.rows[i].hdr_val == q::svdm_req(0x22D9, 31);
    CHECK_EQ(seed_row, true);
}

static void test_vdm_request_header() {
    constexpr emk_variant::Variant pd3_svdm2{2, 2}, pd3_svdm1{2, 1}, pd2_svdm1{1, 1};
    CHECK_EQ(emk_learn::vdm_request(0xFF00, 1, pd3_svdm2), 0xFF00A001u);
    CHECK_EQ(emk_learn::vdm_request(0xFF00, 1, pd2_svdm1), 0xFF008001u);
    CHECK_EQ(emk_learn::vdm_request(0xFF00, 1, pd3_svdm1), 0xFF008001u);   // PD 3.x header, SVDM 1.0
    CHECK_EQ(emk_learn::vdm_request(0x22D9, 31, pd3_svdm2), 0x22D9A01Fu);
}

// ---- remote wire forms ----

static std::uint32_t le32(const std::uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (std::uint32_t(p[3]) << 24); }

static void test_parse_options() {
    emk_learn::Options o;
    bool confirm = false;
    // flags 3 = sweep + confirm, two seeds: 22D9:31 without objects, 1234:17 with one object.
    const std::vector<std::uint8_t> ok = {3, 2, 0xD9, 0x22, 31, 0, 0x34, 0x12, 17, 1, 0x78, 0x56, 0x34, 0x12};
    CHECK_EQ(emk_learn::parse_options(ok, o, confirm), true);
    CHECK_EQ(o.sweep, true);
    CHECK_EQ(confirm, true);
    CHECK_EQ(o.nseeds, 2);
    CHECK_EQ(o.seeds[0].svid, 0x22D9);
    CHECK_EQ(o.seeds[0].cmd, 31);
    CHECK_EQ(o.seeds[0].nvdo, 0);
    CHECK_EQ(o.seeds[1].svid, 0x1234);
    CHECK_EQ(o.seeds[1].nvdo, 1);
    CHECK_EQ(o.seeds[1].vdo[0], 0x12345678u);
    // No seeds, no flags.
    CHECK_EQ(emk_learn::parse_options(std::vector<std::uint8_t>{0, 0}, o, confirm), true);
    CHECK_EQ(o.sweep, false);
    CHECK_EQ(confirm, false);
    CHECK_EQ(o.nseeds, 0);
    // Faults: too short, a seed cut off, trailing bytes, too many seeds, too many objects, a command past 31, unknown flag bits.
    CHECK_EQ(emk_learn::parse_options(std::vector<std::uint8_t>{0}, o, confirm), false);
    CHECK_EQ(emk_learn::parse_options(std::vector<std::uint8_t>{0, 1, 0xD9, 0x22, 31}, o, confirm), false);
    CHECK_EQ(emk_learn::parse_options(std::vector<std::uint8_t>{0, 0, 9}, o, confirm), false);
    std::vector<std::uint8_t> five = {0, 5};
    for (int i = 0; i < 5; i++) for (std::uint8_t b : {0xD9, 0x22, 31, 0}) five.push_back(b);
    CHECK_EQ(emk_learn::parse_options(five, o, confirm), false);
    {   // up to six objects after the header now; seven is a fault
        std::vector<std::uint8_t> six = {0, 1, 0xD9, 0x22, 31, 6};
        six.resize(six.size() + 24, 0x11);
        CHECK_EQ(emk_learn::parse_options(six, o, confirm), true);
        CHECK_EQ(o.seeds[0].nvdo, 6);
        CHECK_EQ(o.seeds[0].vdo[5], 0x11111111u);
        CHECK_EQ(o.seeds[0].raw, false);
        std::vector<std::uint8_t> seven = {0, 1, 0xD9, 0x22, 31, 7};
        seven.resize(seven.size() + 28, 0x11);
        CHECK_EQ(emk_learn::parse_options(seven, o, confirm), false);
    }
    CHECK_EQ(emk_learn::parse_options(std::vector<std::uint8_t>{0, 1, 0xD9, 0x22, 32, 0}, o, confirm), false);
    CHECK_EQ(emk_learn::parse_options(std::vector<std::uint8_t>{4, 0}, o, confirm), false);
    // The longest valid request fits one request frame.
    CHECK_EQ(emk_learn::kOptionsMax <= 57, true);
}

// The brick's cable command, as the K2 saw it: header 0x22D9A11F (position 1), then 0, 0 and four random words.
static const std::uint32_t kBrickHdr = 0x22D9A11Fu;
static const std::array<std::uint32_t, 6> kBrickVdos = {0x00000000u, 0x00000000u, 0x0E4AD139u, 0x2F2DF86Fu, 0x50111FA6u, 0x70F446DCu};

static void push_le32(std::vector<std::uint8_t>& v, std::uint32_t w) {
    for (int k = 0; k < 4; k++) v.push_back(static_cast<std::uint8_t>(w >> (8 * k)));
}

// A raw seed on the wire: [header hi u16, 0xFF, nvdo, header lo u16, vdo u32 * nvdo].
static std::vector<std::uint8_t> raw_seed_bytes(std::uint32_t hdr, const std::vector<std::uint32_t>& vdos) {
    std::vector<std::uint8_t> v = {static_cast<std::uint8_t>(hdr >> 16), static_cast<std::uint8_t>(hdr >> 24), 0xFF,
                                   static_cast<std::uint8_t>(vdos.size()), static_cast<std::uint8_t>(hdr),
                                   static_cast<std::uint8_t>(hdr >> 8)};
    for (std::uint32_t w : vdos) push_le32(v, w);
    return v;
}

static void test_parse_raw_seed() {
    emk_learn::Options o;
    bool confirm = false;
    // The documented seed, byte for byte (tools/k2_remote.py --seed-raw 22D9A11F:0:0:0E4AD139:2F2DF86F:50111FA6:70F446DC).
    const std::vector<std::uint8_t> wire = {0, 1, 0xD9, 0x22, 0xFF, 6, 0x1F, 0xA1,
                                            0, 0, 0, 0, 0, 0, 0, 0, 0x39, 0xD1, 0x4A, 0x0E, 0x6F, 0xF8, 0x2D, 0x2F,
                                            0xA6, 0x1F, 0x11, 0x50, 0xDC, 0x46, 0xF4, 0x70};
    CHECK_EQ(emk_learn::parse_options(wire, o, confirm), true);
    CHECK_EQ(o.nseeds, 1);
    CHECK_EQ(o.seeds[0].raw, true);
    CHECK_EQ(o.seeds[0].hdr, kBrickHdr);
    CHECK_EQ(o.seeds[0].svid, 0x22D9);   // derived from the header: the sweep and the seed table use them
    CHECK_EQ(o.seeds[0].cmd, 31);
    CHECK_EQ(o.seeds[0].nvdo, 6);
    for (unsigned i = 0; i < 6; i++) CHECK_EQ(o.seeds[0].vdo[i], kBrickVdos[i]);
    CHECK_EQ(raw_seed_bytes(kBrickHdr, {kBrickVdos.begin(), kBrickVdos.end()}).size(), 30u);
    // The header is exact: reserved and type bits survive, the command comes from its low five bits.
    auto w2 = raw_seed_bytes(0x12345678u, {});
    w2.insert(w2.begin(), {0, 1});
    CHECK_EQ(emk_learn::parse_options(w2, o, confirm), true);
    CHECK_EQ(o.seeds[0].hdr, 0x12345678u);
    CHECK_EQ(o.seeds[0].cmd, 0x78 & 0x1F);
    CHECK_EQ(o.seeds[0].nvdo, 0);
    // A structured seed and a raw one in one request, in order.
    std::vector<std::uint8_t> mix = {2, 2, 0x34, 0x12, 17, 1, 0x78, 0x56, 0x34, 0x12};
    const auto rs = raw_seed_bytes(kBrickHdr, {1, 2});
    mix.insert(mix.end(), rs.begin(), rs.end());
    mix[0] = 2;   // confirm
    CHECK_EQ(emk_learn::parse_options(mix, o, confirm), true);
    CHECK_EQ(o.nseeds, 2);
    CHECK_EQ(o.seeds[0].raw, false);
    CHECK_EQ(o.seeds[0].vdo[0], 0x12345678u);
    CHECK_EQ(o.seeds[1].raw, true);
    CHECK_EQ(o.seeds[1].nvdo, 2);
    CHECK_EQ(o.seeds[1].vdo[1], 2u);
    // Bounds: seven objects, a cut-off header, cut-off objects, trailing bytes.
    auto bad = raw_seed_bytes(kBrickHdr, {1, 2, 3, 4, 5, 6, 7});
    bad.insert(bad.begin(), {0, 1});
    CHECK_EQ(emk_learn::parse_options(bad, o, confirm), false);
    std::vector<std::uint8_t> cut = {0, 1, 0xD9, 0x22, 0xFF, 0, 0x1F};   // the header's low half is one byte short
    CHECK_EQ(emk_learn::parse_options(cut, o, confirm), false);
    auto cut2 = raw_seed_bytes(kBrickHdr, {1, 2});
    cut2.insert(cut2.begin(), {0, 1});
    cut2.pop_back();
    CHECK_EQ(emk_learn::parse_options(cut2, o, confirm), false);
    auto extra = raw_seed_bytes(kBrickHdr, {1});
    extra.insert(extra.begin(), {0, 1});
    extra.push_back(0);
    CHECK_EQ(emk_learn::parse_options(extra, o, confirm), false);
    // 0xFF is the only marker: 32..254 stay refused.
    CHECK_EQ(emk_learn::parse_options(std::vector<std::uint8_t>{0, 1, 0xD9, 0x22, 0x80, 0}, o, confirm), false);
    // Four seeds of any kind parse; the request is capped by the frame (57 option bytes), not by the parser.
    std::vector<std::uint8_t> four = {0, 4};
    for (int i = 0; i < 4; i++) {
        const auto s = raw_seed_bytes(kBrickHdr, {});
        four.insert(four.end(), s.begin(), s.end());
    }
    CHECK_EQ(emk_learn::parse_options(four, o, confirm), true);
    CHECK_EQ(o.nseeds, 4);
    // One full-size raw seed is the largest request that is meant to fit one frame; the limit is documented.
    CHECK_EQ(emk_learn::kOptionsMax, 57u);
    std::vector<std::uint8_t> frame = {0, 1};
    frame.insert(frame.end(), wire.begin() + 2, wire.end());
    CHECK_EQ(frame.size() <= emk_learn::kOptionsMax, true);
    std::vector<std::uint8_t> toolong = {0, 2};
    for (int i = 0; i < 2; i++) {
        const auto s = raw_seed_bytes(kBrickHdr, {kBrickVdos.begin(), kBrickVdos.end()});
        toolong.insert(toolong.end(), s.begin(), s.end());
    }
    CHECK_EQ(toolong.size() > emk_learn::kOptionsMax, true);
    CHECK_EQ(emk_learn::parse_options(toolong, o, confirm), false);   // two full raw seeds do not fit one frame
}

// The brick's command with a full-size answer: a header with position 1 and 6 VDOs out, a 7-object answer back.
static emk_learn::Seed brick_seed() {
    emk_learn::Seed s;
    s.raw = true;
    s.hdr = kBrickHdr;
    s.svid = 0x22D9;
    s.cmd = 31;
    s.nvdo = 6;
    for (unsigned i = 0; i < 6; i++) s.vdo[i] = kBrickVdos[i];
    return s;
}
static const Behaviour kBrickAnswer{0x22D9, 31, How::Ack, {0xA0000001u, 0xA0000002u, 0xA0000003u, 0xA0000004u, 0xA0000005u, 0xA0000006u}};

static void test_raw_seed_goes_out_exact_and_the_full_reply_is_kept() {
    FakeCable c;
    c.table = {kBrickAnswer};
    emk_learn::Options o;
    o.seeds[0] = brick_seed();
    o.nseeds = 1;
    emk_learn::Standalone r;
    CHECK_EQ(emk_learn::learn(&fake_exchange, &c, o, r), true);
    // Identity, SVIDs (silent), then the seed: 7 objects exactly as given, the header untouched (position 1).
    CHECK_EQ(c.seen.size(), 3u);
    const Request& s = c.seen[2];
    CHECK_EQ(s.n, 7);
    CHECK_EQ(s.w[0], kBrickHdr);
    for (unsigned i = 0; i < 6; i++) CHECK_EQ(s.w[1 + i], kBrickVdos[i]);
    CHECK_EQ(s.spec_rev, 2);
    // The answer comes back whole: header word plus six VDOs, in the transcript and as a row.
    CHECK_EQ(r.nlog, 3);
    CHECK_EQ(r.log_full, false);
    const auto x = r.entry(2);
    CHECK_EQ(int(x.step), int(Step::Seed));
    CHECK_EQ(x.req.n, 7);
    CHECK_EQ(x.rep.got, true);
    CHECK_EQ(x.rep.n, 7);
    CHECK_EQ(x.req.w[6], 0x70F446DCu);
    CHECK_EQ(x.rep.w[0] >> 16, 0x22D9u);
    for (unsigned i = 0; i < 6; i++) CHECK_EQ(x.rep.w[1 + i], 0xA0000001u + i);
    CHECK_EQ(r.quirks.count, 1);
    CHECK_EQ(r.quirks.rows[0].nrep, 6);
    CHECK_EQ(r.quirks.rows[0].rep[5], 0xA0000006u);
    // A silent answer is a kept entry too (the request words, no reply).
    FakeCable d;
    emk_learn::Standalone r2;
    CHECK_EQ(emk_learn::learn(&fake_exchange, &d, o, r2), true);
    CHECK_EQ(r2.entry(2).rep.got, false);
    CHECK_EQ(r2.entry(2).req.n, 7);
    // The sweep leaves the seed's own command alone, as for a structured seed.
    FakeCable e;
    emk_learn::Options os = o;
    os.sweep = true;
    emk_learn::Standalone r3;
    CHECK_EQ(emk_learn::learn(&fake_exchange, &e, os, r3), true);
    for (const Request& q : e.seen) CHECK_EQ(q.n == 7 || q.w[0] != 0x22D9A01Fu, true);
}

static void test_four_full_size_seeds_fit_the_transcript() {
    FakeCable c;
    c.table = {kBrickAnswer};
    emk_learn::Options o;
    for (unsigned i = 0; i < 4; i++) o.seeds[i] = brick_seed();
    o.nseeds = 4;
    emk_learn::Standalone r;
    CHECK_EQ(emk_learn::learn(&fake_exchange, &c, o, r), true);
    CHECK_EQ(r.log_full, false);
    CHECK_EQ(r.nlog, 6);   // identity, SVIDs, four seeds
    for (unsigned i = 2; i < 6; i++) CHECK_EQ(r.entry(i).rep.n, 7);
    CHECK_EQ(r.log_len <= emk_learn::kLogBytes, true);
}

static void test_encode_entry_chunks() {
    FakeCable c;
    c.table = {kBrickAnswer};
    emk_learn::Options o;
    o.seeds[0] = brick_seed();
    o.nseeds = 1;
    emk_learn::Standalone r;
    CHECK_EQ(emk_learn::learn(&fake_exchange, &c, o, r), true);
    const auto x = r.entry(2);
    std::vector<std::uint32_t> all;   // request words, then reply words
    for (unsigned i = 0; i < x.req.n; i++) all.push_back(x.req.w[i]);
    for (unsigned i = 0; i < x.rep.n; i++) all.push_back(x.rep.w[i]);
    CHECK_EQ(all.size(), 14u);
    // A response frame carries 58 bytes: 9 of header and 12 words. The whole entry is 65 bytes.
    CHECK_EQ(emk_learn::kEntryMax, 65u);
    std::array<std::uint8_t, 58> out{};
    std::size_t n = emk_learn::encode_entry(r, 2, out);
    CHECK_EQ(n, 9u + 4 * 12);
    CHECK_EQ(out[4], 7);   // req_n and rep_n are the whole entry's, not the chunk's
    CHECK_EQ(out[6], 7);
    std::vector<std::uint32_t> got;
    for (std::size_t at = 9; at < n; at += 4) got.push_back(le32(&out[at]));
    // The second chunk starts at the first word the first one did not carry.
    n = emk_learn::encode_entry(r, 2, out, got.size());
    CHECK_EQ(n, 9u + 4 * 2);
    CHECK_EQ(out[4], 7);
    CHECK_EQ(out[6], 7);
    for (std::size_t at = 9; at < n; at += 4) got.push_back(le32(&out[at]));
    CHECK_EQ(got == all, true);
    // Past the last word: the header alone. A bigger buffer takes everything in one go.
    CHECK_EQ(emk_learn::encode_entry(r, 2, out, 14), 9u);
    CHECK_EQ(emk_learn::encode_entry(r, 2, out, 200), 9u);
    std::array<std::uint8_t, emk_learn::kEntryMax> big{};
    CHECK_EQ(emk_learn::encode_entry(r, 2, big), 9u + 4 * 14);
    // Too small even for the header: nothing.
    std::array<std::uint8_t, 8> tiny{};
    CHECK_EQ(emk_learn::encode_entry(r, 2, tiny), 0u);
    // Past the log is still "not found".
    CHECK_EQ(emk_learn::encode_entry(r, 9, out, 0), 1u);
    CHECK_EQ(out[0], 3);
}

static void test_summary_and_entry_encoding() {
    std::array<std::uint8_t, 64> out{};
    // Nothing learned yet: zero outcome, the rest zero.
    std::size_t n = emk_learn::encode_summary(nullptr, out);
    CHECK_EQ(n, emk_learn::kSummaryBytes);
    CHECK_EQ(out[0], 0);   // err
    CHECK_EQ(out[1], 0);   // outcome: none
    for (std::size_t i = 2; i < n; i++) CHECK_EQ(out[i], 0);
    CHECK_EQ(emk_learn::kSummaryBytes, 20u);   // 13 bytes, the answered tail (4), the read tail (2), then the pull-down verdict (1)

    FakeCable c;
    c.table = {kSvids22D9, kModes22D9, kOppo};
    emk_learn::Options o;
    o.seeds[0] = {0x22D9, 31, 0, {}};
    o.nseeds = 1;
    emk_learn::Standalone r;
    r.answered = k2::emk_variant::answered_from(2, 1);   // orientation B, second row (PD 3.x header, SVDM 1.0)
    r.read_mode = 2;                                     // the scan ran with a source attached
    r.read_acks = 2;
    r.pull_down = 3;                                     // the Ra probe found a pull-down on CC2
    CHECK_EQ(emk_learn::learn(&fake_exchange, &c, o, r), true);
    CHECK_EQ(r.answered == k2::emk_variant::answered_from(2, 1), true);   // the plan leaves what the scan answered alone
    n = emk_learn::encode_summary(&r, out);
    CHECK_EQ(n, emk_learn::kSummaryBytes);
    CHECK_EQ(out[13], 2);     // orientation B
    CHECK_EQ(out[14], 2);     // PD 3.x header
    CHECK_EQ(out[15], 1);     // SVDM 1.0
    CHECK_EQ(out[16], 2);     // attempt 2
    CHECK_EQ(out[17], 2);     // read mode: attached
    CHECK_EQ(out[18], 2);     // variants acknowledged
    CHECK_EQ(out[19], 3);     // pull-down verdict: CC2
    CHECK_EQ(out[0], 0);
    CHECK_EQ(out[1], 1);      // learned
    CHECK_EQ(out[2], 1);      // svids
    CHECK_EQ(out[3], 4);      // probes
    CHECK_EQ(out[4], 0);      // silent
    CHECK_EQ(out[5], 0);      // flags
    CHECK_EQ(out[6], 4);      // log entries
    CHECK_EQ(out[7], 3);      // quirk rows
    CHECK_EQ(out[8], 0);      // unmatched policy: ignore
    CHECK_EQ(out[9] | (out[10] << 8), 0x203C);    // vid = low half of the ID header VDO
    CHECK_EQ(out[11] | (out[12] << 8), 0x0001);   // pid = high half of the product VDO

    // Entry 3: the seed. [err, i, step, spec_rev, req_n, got, rep_n, header u16, req words, rep words]
    n = emk_learn::encode_entry(r, 3, out);
    CHECK_EQ(n, 9u + 4 + 12);
    CHECK_EQ(out[0], 0);
    CHECK_EQ(out[1], 3);
    CHECK_EQ(out[2], int(Step::Seed));
    CHECK_EQ(out[3], 2);
    CHECK_EQ(out[4], 1);
    CHECK_EQ(out[5], 1);
    CHECK_EQ(out[6], 3);
    CHECK_EQ(out[7] | (out[8] << 8), r.entry(3).rep.header);
    CHECK_EQ(le32(&out[9]), 0x22D9A01Fu);
    CHECK_EQ(le32(&out[13]), (0x22D9A01Fu & ~(3u << 6)) | (1u << 6));
    CHECK_EQ(le32(&out[17]), 0xAB01CD02u);
    CHECK_EQ(le32(&out[21]), 7u);
    // Past the log is "not found" (one byte).
    n = emk_learn::encode_entry(r, 4, out);
    CHECK_EQ(n, 1u);
    CHECK_EQ(out[0], 3);
    // The longest whole entry (65 bytes) is more than a response frame: it comes in chunks (test_encode_entry_chunks).
    CHECK_EQ(emk_learn::kEntryMax, 9u + 4 * 14);
}

static void test_seen_encoding() {
    q::SeenLog log;
    std::array<std::uint8_t, 64> out{};
    std::size_t n = q::encode_seen(log, q::kSeenTotals, out);
    CHECK_EQ(n, 4u);               // [err, total u16, held]
    CHECK_EQ(out[0], 0);
    CHECK_EQ(out[1] | (out[2] << 8), 0);
    CHECK_EQ(out[3], 0);
    const std::uint8_t p1[] = {0x1F, 0xA0, 0xD9, 0x22, 0x02, 0x00, 0x00, 0x00};
    const std::uint8_t p2[] = {0x11, 0xA0, 0xD9, 0x22};
    log.push(q::kSopP, 0x128F, p1);
    log.push(q::kSopPP, 0x148F, p2);
    n = q::encode_seen(log, q::kSeenTotals, out);
    CHECK_EQ(n, 4u);
    CHECK_EQ(out[1] | (out[2] << 8), 2);
    CHECK_EQ(out[3], 2);
    // Newest first: entry 0 is the SOP'' request.  [err, total u16, held, sop, n, header u16, words]
    n = q::encode_seen(log, 0, out);
    CHECK_EQ(n, 4u + 2 + 2 + 4);
    CHECK_EQ(out[4], q::kSopPP);
    CHECK_EQ(out[5], 1);
    CHECK_EQ(out[6] | (out[7] << 8), 0x148F);
    CHECK_EQ(le32(&out[8]), 0x22D9A011u);
    n = q::encode_seen(log, 1, out);
    CHECK_EQ(n, 4u + 2 + 2 + 8);
    CHECK_EQ(out[4], q::kSopP);
    CHECK_EQ(out[5], 2);
    // Past the held entries: not found.
    n = q::encode_seen(log, 2, out);
    CHECK_EQ(n, 1u);
    CHECK_EQ(out[0], 3);
    CHECK_EQ(q::kSeenMax <= 58, true);
}

static void test_acked_no_answer_outcome_goes_on_the_wire() {
    std::array<std::uint8_t, 64> out{};
    emk_learn::Standalone r;
    r.outcome = emk_learn::Outcome::AckedNoAnswer;   // the chip acknowledged the query and never answered it
    r.read_mode = 1;
    r.read_acks = 3;
    CHECK_EQ(emk_learn::encode_summary(&r, out), emk_learn::kSummaryBytes);
    CHECK_EQ(out[1], 4);      // outcome + 1: learned 1, no identity 2, aborted 3, acked-no-answer 4
    CHECK_EQ(out[17], 1);     // alone
    CHECK_EQ(out[18], 3);
    // A plain silent cable stays "no identity".
    emk_learn::Standalone q;
    CHECK_EQ(emk_learn::encode_summary(&q, out), emk_learn::kSummaryBytes);
    CHECK_EQ(out[1], 2);
    CHECK_EQ(out[17], 0);
    CHECK_EQ(out[19], 0);     // never probed
}

int main() {
    test_parse_options();
    test_parse_raw_seed();
    test_raw_seed_goes_out_exact_and_the_full_reply_is_kept();
    test_four_full_size_seeds_fit_the_transcript();
    test_encode_entry_chunks();
    test_summary_and_entry_encoding();
    test_acked_no_answer_outcome_goes_on_the_wire();
    test_seen_encoding();
    test_vdm_request_header();
    test_identity_only();
    test_full_walk_and_clone_answers_the_same();
    test_pd2_only_cable();
    test_svdm1_only_cable_learns_and_follows_up_with_its_variant();
    test_first_variant_cable_behaves_as_before();
    test_no_identity();
    test_link_lost_aborts();
    test_nak_busy_and_not_supported();
    test_seed_with_objects_and_silence();
    test_svid_list_decoding_and_budget();
    test_sweep_records_acks_and_policy();
    test_row_budget_truncates();
    return test_report("test_emk_learn");
}
