// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Host tests for the twin library (k2::twin::Library): one file per twin on littlefs, the record codec with its version
// byte and CRC, and learning a twin from Identify or from a sniffed Request. Over a RAM disk that models NOR flash and
// can lose power at any program or erase.
#include <algorithm>
#include <array>
#include <cstring>
#include <string>
#include <vector>

#include "k2/twin/learn.hpp"
#include "k2/twin/library.hpp"
#include "ramdisk.hpp"
#include "test_util.h"

using namespace k2;
using namespace k2::twin;

namespace {

constexpr std::uint32_t kBlocks = 96;

struct Vol {
    k2test::RamDisk d{kBlocks};
    fs::Fs f{d, kBlocks};
    Vol() { bool fm; f.mount_or_format(fm); }
};

Twin pixel(const char* name = "") {
    Twin t;
    std::strncpy(t.name.data(), name, kNameMax);
    t.vid = 0x18D1;
    t.pid = 0x4EE1;
    t.id_hi = (2u << 4);   // UFP type 2: peripheral
    t.pd_rev = 2;
    t.num_pdos = 3;
    t.pdos = {0x3601912C, 0x0002D12C, 0xC0DC323C};
    return t;
}

std::vector<std::uint8_t> seqs(const Library& l) {
    std::array<std::uint8_t, kCapacity + 1> s{};
    const unsigned n = l.list(s);
    return {s.begin(), s.begin() + n};
}

std::string name_of(const Library& l, std::uint8_t seq) {
    Twin t;
    return l.get(seq, t) ? std::string(t.name.data()) : std::string("<missing>");
}

}  // namespace

static void test_codec_roundtrip() {
    Twin t = pixel("My Pixel");
    t.rdo = 0x4002D12C | (6u << 28);
    t.rdo_pdo = 0xC0DC323C;
    std::array<std::uint8_t, kMaxFileBytes> b{};
    const std::size_t n = encode(t, 7, b);
    CHECK_EQ(n, kHeaderBytes + 3 * 4 + 8 + 2);   // header, three sink PDOs, the Request pair, CRC
    CHECK_EQ(b[0], 'K');
    CHECK_EQ(b[1], '2');
    CHECK_EQ(b[2], 'T');
    CHECK_EQ(b[3], kVersion1);
    std::uint8_t seq = 0;
    Twin u;
    CHECK_EQ(int(decode({b.data(), n}, seq, u)), int(Dec::Ok));
    CHECK_EQ(seq, 7);
    CHECK_STR(u.name.data(), "My Pixel");
    CHECK_EQ(u.vid, 0x18D1);
    CHECK_EQ(u.pid, 0x4EE1);
    CHECK_EQ(u.id_hi, 0x20);
    CHECK_EQ(u.ufp_type(), 2);
    CHECK_EQ(u.pd_rev, 2);
    CHECK_EQ(u.num_pdos, 3);
    CHECK_EQ(u.pdos[2], 0xC0DC323Cu);
    CHECK_EQ(u.rdo, t.rdo);
    CHECK_EQ(u.rdo_pdo, t.rdo_pdo);
    // without a Request the pair is not stored; with seven PDOs and a Request the file is as big as it gets
    t.rdo = t.rdo_pdo = 0;
    CHECK_EQ(encode(t, 1, b), kHeaderBytes + 3 * 4 + 2);
    t.num_pdos = 7;
    t.rdo = 1;
    t.rdo_pdo = 2;
    CHECK_EQ(encode(t, 1, b), kMaxV1Bytes);   // no USB blocks: still a version 1 record
    std::array<std::uint8_t, 10> small{};
    CHECK_EQ(encode(t, 1, small), std::size_t(0));   // no room: nothing written
    // a full 24-character name has no terminator in the file and still reads back whole
    Twin w = pixel("abcdefghijklmnopqrstuvwx");
    const std::size_t m = encode(w, 1, b);
    CHECK_EQ(int(decode({b.data(), m}, seq, u)), int(Dec::Ok));
    CHECK_STR(u.name.data(), "abcdefghijklmnopqrstuvwx");
}

static void test_codec_guards() {
    Twin t = pixel("x");
    std::array<std::uint8_t, kMaxFileBytes> b{};
    const std::size_t n = encode(t, 3, b);
    std::uint8_t seq;
    Twin u;
    // every single flipped bit is caught: by the CRC, or by the magic / version byte it hits first
    for (std::size_t i = 0; i < n; i++) {
        auto c = b;
        c[i] ^= 0x10;
        CHECK_COND(decode({c.data(), n}, seq, u) != Dec::Ok);
    }
    // a record from a newer build (version 9) is refused, not guessed at, even with a valid CRC
    auto c = b;
    c[3] = 9;
    const std::uint16_t crc = bbstore::crc16(0xFFFF, {c.data(), n - 2});
    c[n - 2] = std::uint8_t(crc);
    c[n - 1] = std::uint8_t(crc >> 8);
    CHECK_EQ(int(decode({c.data(), n}, seq, u)), int(Dec::Version));
    // a PDO count over seven with a valid CRC is invalid, a truncated file is short
    c = b;
    c[kOffNumPdos] = 8;
    const std::uint16_t crc2 = bbstore::crc16(0xFFFF, {c.data(), n - 2});
    c[n - 2] = std::uint8_t(crc2);
    c[n - 1] = std::uint8_t(crc2 >> 8);
    CHECK_EQ(int(decode({c.data(), n}, seq, u)), int(Dec::Invalid));
    CHECK_EQ(int(decode({b.data(), n - 5}, seq, u)), int(Dec::Short));
    CHECK_EQ(int(decode({b.data(), std::size_t(3)}, seq, u)), int(Dec::Short));
    c = b;
    c[0] = 'X';
    CHECK_EQ(int(decode({c.data(), n}, seq, u)), int(Dec::Magic));
}

// The version 1 byte layout is a promise to every build that wrote one: this record must keep reading, bit for bit.
static void test_codec_v1_golden() {
    static const std::array<std::uint8_t, 56> kGolden = {
        'K', '2', 'T', 1,                // magic, version
        5,                               // seq
        1,                               // flags: b0 a Request follows the PDOs
        0xD1, 0x18,                      // VID 0x18D1
        0xE1, 0x4E,                      // PID 0x4EE1
        0x20, 0x00,                      // ID Header bits 31:23: UFP type 2
        2,                               // wire revision: PD 3.x
        2,                               // two sink PDOs
        'P', 'i', 'x', 'e', 'l', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,   // name, 24 bytes
        0x2C, 0x91, 0x01, 0x36,          // sink PDO 1 0x3601912C
        0x2C, 0xD1, 0x02, 0x00,          // sink PDO 2 0x0002D12C
        0x2C, 0xD1, 0x02, 0x60,          // the Request, 0x6002D12C
        0x3C, 0x32, 0xDC, 0xC0,          // the source PDO it pointed at, 0xC0DC323C
        0x1A, 0x40,                      // CRC16 CCITT (init 0xFFFF) over everything above, little endian
    };
    std::uint8_t seq = 0;
    Twin t;
    CHECK_EQ(int(decode(kGolden, seq, t)), int(Dec::Ok));
    CHECK_EQ(seq, 5);
    CHECK_STR(t.name.data(), "Pixel");
    CHECK_EQ(t.vid, 0x18D1);
    CHECK_EQ(t.pid, 0x4EE1);
    CHECK_EQ(t.id_hi, 0x20);
    CHECK_EQ(t.pd_rev, 2);
    CHECK_EQ(t.num_pdos, 2);
    CHECK_EQ(t.pdos[0], 0x3601912Cu);
    CHECK_EQ(t.pdos[1], 0x0002D12Cu);
    CHECK_EQ(t.rdo, 0x6002D12Cu);
    CHECK_EQ(t.rdo_pdo, 0xC0DC323Cu);
    // and the writer still produces exactly those bytes
    std::array<std::uint8_t, kMaxFileBytes> b{};
    const std::size_t n = encode(t, 5, b);
    CHECK_EQ(n, kGolden.size());
    CHECK_EQ(std::memcmp(b.data(), kGolden.data(), kGolden.size()), 0);
}

static void test_library_basic() {
    Vol v;
    Library l(v.f);
    CHECK_EQ(l.count(), 0u);
    const Result r = l.save(pixel("My Pixel"));
    CHECK_EQ(int(r.err), int(Err::Ok));
    CHECK_EQ(r.seq, 1);
    CHECK_EQ(r.existed ? 1 : 0, 0);
    Twin t;
    CHECK_EQ(l.get(1, t) ? 1 : 0, 1);
    CHECK_STR(t.name.data(), "My Pixel");
    CHECK_EQ(t.pdos[1], 0x0002D12Cu);
    CHECK_EQ(l.get(2, t) ? 1 : 0, 0);
    // a second device gets the next number; a name is unique, ignoring case
    Twin o = pixel("my pixel");
    o.vid = 0x05AC;
    CHECK_EQ(l.save(o).seq, 2);
    CHECK_STR(name_of(l, 2).c_str(), "my pixel 2");
    // the state lives in the volume only
    Library l2(v.f);
    CHECK_EQ(l2.count(), 2u);
    CHECK_EQ(seqs(l2).size(), 2u);
    CHECK_EQ(l2.find(0x05AC, 0x4EE1), 2);
    CHECK_EQ(l2.find(0x18D1, 0x4EE1), 1);
    CHECK_EQ(l2.find(0x1234, 0x5678), 0);
    CHECK_EQ(l2.find(0, 0), 0);
    // the file is at /twin/NN.twn, its size within the layout bound
    CHECK_EQ(v.f.exists("/twin/01.twn") ? 1 : 0, 1);
    std::size_t sz = 0;
    v.f.size_of("/twin/01.twn", sz);
    CHECK_EQ(sz, kHeaderBytes + 3 * 4 + 2);
    // an invalid twin (a name that breaks the rule, too many PDOs) is not saved
    Twin bad = pixel("bad/name");
    CHECK_EQ(int(l.save(bad).err), int(Err::Invalid));
    bad = pixel("ok");
    bad.num_pdos = 8;
    CHECK_EQ(int(l.save(bad).err), int(Err::Invalid));
    CHECK_EQ(l.count(), 2u);
}

static void test_library_capacity_delete_rename() {
    Vol v;
    Library l(v.f);
    for (unsigned i = 0; i < kCapacity; i++) {
        Twin t = pixel();
        t.vid = std::uint16_t(0x1000 + i);
        CHECK_EQ(int(l.save(t).err), int(Err::Ok));
    }
    CHECK_EQ(l.count(), kCapacity);
    Twin extra = pixel();
    extra.vid = 0x2000;
    CHECK_EQ(int(l.save(extra).err), int(Err::Full));
    CHECK_EQ(l.count(), kCapacity);
    // the same device again is an update, not a new twin: it still works on a full library
    Twin again = pixel();
    again.vid = 0x1003;
    again.pdos[2] = 0xC0DC3264;
    const Result u = l.save(again);
    CHECK_EQ(int(u.err), int(Err::Ok));
    CHECK_EQ(u.existed ? 1 : 0, 1);
    CHECK_EQ(u.seq, 4);
    // delete frees the number, the next save takes the lowest free one
    CHECK_EQ(int(l.remove(4)), int(Err::Ok));
    CHECK_EQ(int(l.remove(4)), int(Err::NotFound));
    CHECK_EQ(l.count(), kCapacity - 1);
    CHECK_EQ(l.save(extra).seq, 4);
    // rename: valid, unchanged, taken (case-insensitive), invalid, missing
    CHECK_EQ(int(l.rename(1, "Desk phone")), int(Err::Ok));
    CHECK_STR(name_of(l, 1).c_str(), "Desk phone");
    CHECK_EQ(int(l.rename(1, "Desk phone")), int(Err::Ok));
    CHECK_EQ(int(l.rename(2, "desk PHONE")), int(Err::Exists));
    CHECK_EQ(int(l.rename(2, "a/b")), int(Err::Invalid));
    CHECK_EQ(int(l.rename(2, "")), int(Err::Invalid));
    CHECK_EQ(int(l.rename(99, "x")), int(Err::NotFound));
    // rename keeps everything else, including the Request
    Twin t;
    CHECK_EQ(l.get(1, t) ? 1 : 0, 1);
    CHECK_EQ(t.vid, 0x1000);
    CHECK_EQ(t.num_pdos, 3);
}

static void test_library_damaged_and_foreign() {
    Vol v;
    Library l(v.f);
    l.save(pixel("a"));
    Twin t2 = pixel("b");
    t2.vid = 0x05AC;
    l.save(t2);
    // a damaged file does not load (get fails) and does not stop the others
    std::array<std::uint8_t, kMaxFileBytes> raw{};
    std::size_t n = 0;
    v.f.read_file("/twin/01.twn", raw, n);
    raw[20] ^= 0xFF;
    v.f.write_file("/twin/01.twn", {raw.data(), n});
    Twin t;
    CHECK_EQ(l.get(1, t) ? 1 : 0, 0);
    CHECK_EQ(l.get(2, t) ? 1 : 0, 1);
    // a record from a future build is left alone: not loaded, not overwritten by a list or a count
    v.f.read_file("/twin/02.twn", raw, n);
    raw[3] = 9;
    const std::uint16_t crc = bbstore::crc16(0xFFFF, {raw.data(), n - 2});
    raw[n - 2] = std::uint8_t(crc);
    raw[n - 1] = std::uint8_t(crc >> 8);
    v.f.write_file("/twin/02.twn", {raw.data(), n});
    CHECK_EQ(l.get(2, t) ? 1 : 0, 0);
    CHECK_EQ(l.count(), 2u);
    // foreign files in the directory are ignored
    const std::array<std::uint8_t, 4> junk{1, 2, 3, 4};
    v.f.write_file("/twin/notes.txt", junk);
    v.f.write_file("/twin/00.twn", junk);
    v.f.write_file("/twin/13.twn", junk);
    CHECK_EQ(l.count(), 2u);
}

static void test_library_power_loss() {
    // a save and an update of an existing twin, cut at every program / erase: the twin is the old one or the new one
    for (int pass = 0; pass < 2; pass++) {
        int total = 0;
        for (int cut = 0; cut < 400; cut++) {
            Vol v;
            Library l(v.f);
            l.save(pixel("first"));
            Twin upd = pixel();
            upd.pdos[2] = 0xC0DC3264;
            if (pass == 1) upd.vid = 0x05AC;      // a new twin instead of an update
            v.d.reset_counters();
            v.d.cut_after(cut);
            l.save(upd);
            const bool finished = !v.d.dead();
            v.d.revive();
            v.f.unmount();
            bool fm;
            v.f.mount_or_format(fm);
            Library after(v.f);
            Twin t;
            CHECK_EQ(after.get(1, t) ? 1 : 0, 1);
            CHECK_STR(t.name.data(), "first");
            CHECK_EQ(t.pdos[2] == 0xC0DC323Cu || (pass == 0 && t.pdos[2] == 0xC0DC3264u), 1);
            CHECK_EQ(after.count() <= (pass == 0 ? 1u : 2u), 1);
            CHECK_EQ(int(after.save(pixel("later")).err), int(Err::Ok));   // the library still takes a save after the torn write
            total = cut;
            if (finished) break;
        }
        CHECK_EQ(total >= 1, 1);
    }
}

// ---- learning ----

static pd::Identity identity_of(std::uint16_t vid, std::uint16_t pid, std::uint32_t id_header) {
    pd::Identity id;
    id.valid = true;
    id.vid = vid;
    id.pid = pid;
    id.id_header = id_header;
    return id;
}

static pd::IdStatus done_with_caps(std::initializer_list<std::uint32_t> pdos) {
    pd::IdStatus st;
    st.state = pd::IdState::Done;
    st.num_sink_pdos = std::uint8_t(pdos.size());
    std::copy(pdos.begin(), pdos.end(), st.sink_pdos.begin());
    st.ok_mask = std::uint8_t((1u << unsigned(pd::IdStep::Identity)) | (1u << unsigned(pd::IdStep::SinkCap)));
    return st;
}

static void test_auto_name() {
    CHECK_STR(auto_name(0x18D1, 33u).data(), "Google 33W");
    CHECK_STR(auto_name(0x17EF, 65u).data(), "Lenovo 65W");
    CHECK_STR(auto_name(0x17EF, 0u).data(), "Lenovo");                   // no sink level read
    CHECK_STR(auto_name(0x1234, 33u).data(), "VID 1234 33W");           // an unlisted VID names itself
    CHECK_STR(auto_name(0x0ABC, 0u).data(), "VID 0ABC");
    CHECK_STR(auto_name(0, 27u).data(), "Device 27W");
    CHECK_STR(auto_name(0, 0u).data(), "Device");
    CHECK_EQ(emlib::name_valid(auto_name(0x05AC, 140u).data()) ? 1 : 0, 1);   // always a name the rename rule accepts
    CHECK_EQ(std::strlen(auto_name(0x05AC, 240u).data()) + 3 <= kNameMax ? 1 : 0, 1);   // room for the " 12" suffix
    Twin t;   // Pixel: 5 V 3 A, 9 V 3 A, PPS 5-11 V 3 A -> 33 W
    t.vid = 0x18D1;
    t.num_pdos = 3;
    t.pdos = {0x3601912Cu, 0x0002D12Cu, 0xC0DC323Cu};
    CHECK_EQ(want_w(t), 33u);
    CHECK_STR(auto_name(t).data(), "Google 33W");
}

static void test_from_identify() {
    const pd::Identity id = identity_of(0x18D1, 0x4EE1, (1u << 30) | (2u << 27));   // USB device capable, UFP type peripheral
    const pd::IdStatus st = done_with_caps({0x3601912C, 0x0002D12C, 0xC0DC323C});
    Twin t;
    CHECK_EQ(from_identify(id, st, 2, t) ? 1 : 0, 1);
    CHECK_EQ(t.vid, 0x18D1);
    CHECK_EQ(t.pid, 0x4EE1);
    CHECK_EQ(t.id_hi, 0xA0);
    CHECK_EQ(t.ufp_type(), 2);
    CHECK_EQ(t.pd_rev, 2);
    CHECK_EQ(t.num_pdos, 3);
    CHECK_EQ(t.pdos[2], 0xC0DC323Cu);
    CHECK_EQ(t.has_request() ? 1 : 0, 0);
    CHECK_EQ(t.name[0], 0);   // the library names it
    // a run that did not finish or learned no sink PDOs is not a twin
    pd::IdStatus part = st;
    part.state = pd::IdState::Aborted;
    CHECK_EQ(from_identify(id, part, 2, t) ? 1 : 0, 0);
    part = st;
    part.state = pd::IdState::Running;
    CHECK_EQ(from_identify(id, part, 2, t) ? 1 : 0, 0);
    part = done_with_caps({});
    CHECK_EQ(from_identify(id, part, 2, t) ? 1 : 0, 0);
    // identity missing but sink PDOs read: still a twin, with no VID
    CHECK_EQ(from_identify(pd::Identity{}, st, 2, t) ? 1 : 0, 1);
    CHECK_EQ(t.vid, 0);
    CHECK_EQ(t.num_pdos, 3);
}

static void test_learn_names_and_rename_wins() {
    Vol v;
    Library l(v.f);
    const pd::Identity id = identity_of(0x18D1, 0x4EE1, 2u << 27);
    const pd::IdStatus st = done_with_caps({0x3601912C, 0x0002D12C, 0xC0DC323C});
    Twin t;
    CHECK_EQ(from_identify(id, st, 2, t) ? 1 : 0, 1);
    Result r = l.save(t);
    CHECK_EQ(int(r.err), int(Err::Ok));
    CHECK_STR(name_of(l, r.seq).c_str(), "Google 33W");
    // the same device learned again: no new twin, nothing written when nothing changed
    r = l.save(t);
    CHECK_EQ(r.seq, 1);
    CHECK_EQ(r.existed ? 1 : 0, 1);
    CHECK_EQ(r.unchanged ? 1 : 0, 1);
    CHECK_EQ(l.count(), 1u);
    // rename wins over the auto name, also after a later learn that changes the PDOs
    CHECK_EQ(int(l.rename(1, "Pixel 9")), int(Err::Ok));
    Twin t2;
    const pd::IdStatus st2 = done_with_caps({0x3601912C, 0x0002D12C, 0x0003C12C, 0xC0DC3264});
    CHECK_EQ(from_identify(id, st2, 2, t2) ? 1 : 0, 1);
    r = l.save(t2);
    CHECK_EQ(r.seq, 1);
    CHECK_STR(name_of(l, 1).c_str(), "Pixel 9");
    Twin got;
    l.get(1, got);
    CHECK_EQ(got.num_pdos, 4);
    // a different device with the same auto name gets a numbered one
    const pd::Identity id2 = identity_of(0x18D1, 0x4EE2, 2u << 27);
    Twin t3;
    from_identify(id2, st, 2, t3);
    r = l.save(t3);
    CHECK_EQ(r.seq, 2);
    CHECK_STR(name_of(l, 2).c_str(), "Google 33W");
    Twin t4;
    from_identify(identity_of(0x18D1, 0x4EE3, 2u << 27), st, 2, t4);
    r = l.save(t4);
    CHECK_STR(name_of(l, r.seq).c_str(), "Google 33W 2");
    // a device with no VID is never matched to an identified one, even with their PDO list; learned again it is matched by its
    // sink PDO list (same_device), not added a second time
    Twin anon;
    from_identify(pd::Identity{}, st, 2, anon);
    CHECK_EQ(l.save(anon).seq, 4);
    r = l.save(anon);
    CHECK_EQ(r.seq, 4);
    CHECK_EQ(r.unchanged ? 1 : 0, 1);
    CHECK_STR(name_of(l, 4).c_str(), "Device 33W");
    CHECK_EQ(l.count(), 4u);
    // another anonymous device (another PDO list) is a twin of its own
    Twin anon2;
    from_identify(pd::Identity{}, done_with_caps({0x3601912C, 0x0002D12C}), 2, anon2);
    CHECK_EQ(l.save(anon2).seq, 5);
    CHECK_STR(name_of(l, 5).c_str(), "Device 27W");
}

static pd::SourceCaps source_with(std::initializer_list<std::uint32_t> words) {
    pd::SourceCaps c;
    c.valid = true;
    c.spec_rev = 2;
    for (std::uint32_t w : words) c.pdos[c.num_pdos++] = pd::decode_pdo(w);
    return c;
}

static pd::Contract contract_at(std::uint8_t pos, std::uint32_t rdo) {
    pd::Contract c;
    c.valid = true;
    c.ready = true;
    c.pos = pos;
    c.rdo = rdo;
    return c;
}

static void test_from_sniffed() {
    const pd::SourceCaps caps = source_with({0x0A01912C, 0x0002D12C, 0xC8DC323C});
    pd::VdmInfo vdm;
    vdm.identity = identity_of(0x18D1, 0x4EE1, 2u << 27);
    // Request for the PPS PDO (position 3): 11 V (550 x 20 mV) at 3 A (60 x 50 mA)
    const std::uint32_t rdo = (3u << 28) | (550u << 9) | 60u;
    Twin t;
    CHECK_EQ(from_sniffed(contract_at(3, rdo), caps, vdm, pd::SinkCapsSeen{}, t) ? 1 : 0, 1);
    CHECK_EQ(t.vid, 0x18D1);
    CHECK_EQ(t.pid, 0x4EE1);
    CHECK_EQ(t.rdo, rdo);
    CHECK_EQ(t.rdo_pdo, 0xC8DC323Cu);
    CHECK_EQ(t.num_pdos, 0);
    // the sink's own Sink_Capabilities, when the source asked for them while we watched, ride along
    pd::SinkCapsSeen seen;
    seen.valid = true;
    seen.n = 2;
    seen.raw = {0x3601912C, 0xC0DC323C};
    CHECK_EQ(from_sniffed(contract_at(3, rdo), caps, vdm, seen, t) ? 1 : 0, 1);
    CHECK_EQ(t.num_pdos, 2);
    CHECK_EQ(t.pdos[0], 0x3601912Cu);
    // no Request seen, a position outside the list, a Request not yet answered with PS_RDY: nothing to learn
    CHECK_EQ(from_sniffed(pd::Contract{}, caps, vdm, pd::SinkCapsSeen{}, t) ? 1 : 0, 0);
    CHECK_EQ(from_sniffed(contract_at(5, rdo), caps, vdm, pd::SinkCapsSeen{}, t) ? 1 : 0, 0);
    pd::Contract unready = contract_at(3, rdo);
    unready.ready = false;
    CHECK_EQ(from_sniffed(unready, caps, vdm, pd::SinkCapsSeen{}, t) ? 1 : 0, 0);
}

// The kind preference: which kind the device chose when both a fixed level above 5 V and a PPS range were on offer.
static void test_from_sniffed_kind_pref() {
    const pd::SourceCaps both = source_with({0x0A01912C, 0x0002D12C, 0xC8DC323C});   // 5 V, 9 V 3 A, PPS 3.3-11 V 3 A
    pd::VdmInfo vdm;
    const std::uint32_t pps_rdo = (3u << 28) | (430u << 9) | 60u;    // PPS at 8.6 V 3 A
    const std::uint32_t fix_rdo = (2u << 28) | (300u << 10) | 300u;   // 9 V 3 A
    Twin t;
    CHECK_EQ(from_sniffed(contract_at(3, pps_rdo), both, vdm, pd::SinkCapsSeen{}, t) ? 1 : 0, 1);
    CHECK_EQ(int(t.pref), int(KindPref::Pps));      // took PPS while 9 V was listed
    CHECK_EQ(from_sniffed(contract_at(2, fix_rdo), both, vdm, pd::SinkCapsSeen{}, t) ? 1 : 0, 1);
    CHECK_EQ(int(t.pref), int(KindPref::Fixed));    // took fixed 9 V while PPS was listed
    // one kind only on offer: the device had no choice, nothing is learned
    CHECK_EQ(from_sniffed(contract_at(2, fix_rdo), source_with({0x0A01912C, 0x0002D12C}), vdm, pd::SinkCapsSeen{}, t) ? 1 : 0, 1);
    CHECK_EQ(int(t.pref), int(KindPref::None));
    CHECK_EQ(from_sniffed(contract_at(2, pps_rdo), source_with({0x0A01912C, 0xC8DC323C}), vdm, pd::SinkCapsSeen{}, t) ? 1 : 0, 1);
    CHECK_EQ(int(t.pref), int(KindPref::None));     // a 5 V level beside a PPS is no choice either
    // taking the 5 V level while both were on offer says nothing about the kind
    CHECK_EQ(from_sniffed(contract_at(1, (1u << 28) | (300u << 10) | 300u), both, vdm, pd::SinkCapsSeen{}, t) ? 1 : 0, 1);
    CHECK_EQ(int(t.pref), int(KindPref::None));
}

static void test_learn_sniffed_attaches() {
    Vol v;
    Library l(v.f);
    const pd::Identity id = identity_of(0x18D1, 0x4EE1, 2u << 27);
    Twin base;
    from_identify(id, done_with_caps({0x3601912C, 0x0002D12C, 0xC0DC323C}), 2, base);
    l.save(base);
    l.rename(1, "Pixel 9");
    // a sniffed Request for the same VID / PID lands on that twin, keeping its name and its PDOs
    const pd::SourceCaps caps = source_with({0x0A01912C, 0xC8DC323C});
    pd::VdmInfo vdm;
    vdm.identity = id;
    const std::uint32_t rdo = (2u << 28) | (550u << 9) | 60u;
    Twin s;
    CHECK_EQ(from_sniffed(contract_at(2, rdo), caps, vdm, pd::SinkCapsSeen{}, s) ? 1 : 0, 1);
    Result r = l.save(s);
    CHECK_EQ(r.seq, 1);
    CHECK_EQ(l.count(), 1u);
    Twin t;
    l.get(1, t);
    CHECK_STR(t.name.data(), "Pixel 9");
    CHECK_EQ(t.num_pdos, 3);
    CHECK_EQ(t.rdo, rdo);
    CHECK_EQ(t.rdo_pdo, 0xC8DC323Cu);
    // an Identify after that keeps the Request
    Twin again;
    from_identify(id, done_with_caps({0x3601912C, 0x0002D12C, 0xC0DC323C}), 2, again);
    l.save(again);
    l.get(1, t);
    CHECK_EQ(t.rdo, rdo);
    // a sniffed Request from a device the library does not know creates a twin (named by VID, no sink PDOs)
    pd::VdmInfo other;
    other.identity = identity_of(0x05AC, 0x1234, 2u << 27);
    Twin s2;
    from_sniffed(contract_at(2, rdo), caps, other, pd::SinkCapsSeen{}, s2);
    r = l.save(s2);
    CHECK_EQ(r.seq, 2);
    CHECK_STR(name_of(l, 2).c_str(), "Apple");
    l.get(2, t);
    CHECK_EQ(t.num_pdos, 0);
    CHECK_EQ(t.has_request() ? 1 : 0, 1);
    // a sniffed twin with no identity at all has nothing to match on: it is a new anonymous twin
    pd::VdmInfo blind;
    Twin s3;
    CHECK_EQ(from_sniffed(contract_at(2, rdo), caps, blind, pd::SinkCapsSeen{}, s3) ? 1 : 0, 1);
    CHECK_EQ(s3.vid, 0);
    CHECK_EQ(l.save(s3).seq, 3);
}

// The merge key (learn.hpp same_device): the identity when both twins have one; without it a fingerprint. Two devices that
// never said who they are are not the same device just because both read 0000:0000.
static Twin anon_req(std::uint32_t rdo, std::uint32_t src_pdo) {
    Twin t;
    t.rdo = rdo;
    t.rdo_pdo = src_pdo;
    return t;
}

static void test_same_device() {
    const std::uint32_t canon_rdo = 0x4344B12Cu, src15 = 0x0004B12Cu;
    // both with an identity: VID and PID decide, nothing else
    Twin a = pixel(), b = pixel();
    b.pdos[0] = 0x3601912D;
    CHECK_EQ(same_device(a, b) ? 1 : 0, 1);
    b.pid = 0x4EE2;
    CHECK_EQ(same_device(a, b) ? 1 : 0, 0);
    // neither with an identity: the sink PDO lists when both have one
    Twin x = pixel(), y = pixel();
    x.vid = x.pid = y.vid = y.pid = 0;
    CHECK_EQ(same_device(x, y) ? 1 : 0, 1);
    y.pdos[2] = 0xC0DC323D;
    CHECK_EQ(same_device(x, y) ? 1 : 0, 0);
    y.pdos[2] = x.pdos[2];
    y.num_pdos = 2;
    CHECK_EQ(same_device(x, y) ? 1 : 0, 0);
    // ... else the sniffed Request with the source PDO it pointed at
    CHECK_EQ(same_device(anon_req(canon_rdo, src15), anon_req(canon_rdo, src15)) ? 1 : 0, 1);
    CHECK_EQ(same_device(anon_req(canon_rdo, src15), anon_req(canon_rdo ^ 1, src15)) ? 1 : 0, 0);
    CHECK_EQ(same_device(anon_req(canon_rdo, src15), anon_req(canon_rdo, 0x0006412Cu)) ? 1 : 0, 0);
    Twin req_and_pdos = anon_req(canon_rdo, src15);
    req_and_pdos.num_pdos = 1;
    req_and_pdos.pdos[0] = 0x0001912C;
    CHECK_EQ(same_device(anon_req(canon_rdo, src15), req_and_pdos) ? 1 : 0, 1);   // a later interrogation of the sniffed device
    // nothing to compare: never the same
    CHECK_EQ(same_device(Twin{}, Twin{}) ? 1 : 0, 0);
    // one with an identity, one without: never (neither a Request nor an equal PDO list says WHICH identified device it was)
    Twin named = anon_req(canon_rdo, src15);
    named.vid = 0x05AC;
    named.pid = 0x1234;
    CHECK_EQ(same_device(named, anon_req(canon_rdo, src15)) ? 1 : 0, 0);
    Twin p_named = pixel(), p_anon = pixel();
    p_anon.vid = p_anon.pid = 0;
    CHECK_EQ(same_device(p_named, p_anon) ? 1 : 0, 0);
    CHECK_EQ(same_device(p_anon, p_named) ? 1 : 0, 0);
}

static void test_library_merges_anonymous_by_fingerprint() {
    Vol v;
    Library l(v.f);
    const std::uint32_t canon_rdo = 0x4344B12Cu, src15 = 0x0004B12Cu;
    // the Canon learned twice by sniffing: one twin, "unchanged" the second time
    CHECK_EQ(l.save(anon_req(canon_rdo, src15)).seq, 1);
    Result r = l.save(anon_req(canon_rdo, src15));
    CHECK_EQ(r.seq, 1);
    CHECK_EQ(r.existed ? 1 : 0, 1);
    CHECK_EQ(r.unchanged ? 1 : 0, 1);
    // another anonymous device with another Request is a second twin, not a merge into the first
    CHECK_EQ(l.save(anon_req(0x2304B12Cu, 0x0002D12Cu)).seq, 2);
    // an interrogation of the Canon that found sink PDOs but no identity lands on twin 1
    Twin q = anon_req(canon_rdo, src15);
    q.num_pdos = 2;
    q.pdos = {0x0001912C, 0x0004B12C};
    r = l.save(q);
    CHECK_EQ(r.seq, 1);
    CHECK_EQ(r.existed ? 1 : 0, 1);
    Twin t;
    l.get(1, t);
    CHECK_EQ(t.num_pdos, 2);
    CHECK_EQ(l.count(), 2u);
    // a learn that read an identity is another twin, even with the same sink PDOs: the anonymous one stays as it was
    Twin named = q;
    named.vid = 0x04A9;
    named.pid = 0x3001;
    r = l.save(named);
    CHECK_EQ(r.seq, 3);
    CHECK_EQ(r.existed ? 1 : 0, 0);
    l.get(1, t);
    CHECK_EQ(t.vid, 0);
}

// Version 3 adds what an inline interrogation reads from PD 3 devices: the Revision data object and the
// Sink_Capabilities_Extended block. Only a twin that holds one of them is written as version 3.
static void test_codec_v3() {
    std::array<std::uint8_t, kMaxFileBytes> b{};
    std::uint8_t seq = 0;
    Twin u;
    // without them: the old versions, byte for byte
    Twin p = pixel("Pixel");
    CHECK_EQ(int(encode(p, 1, b) != 0), 1);
    CHECK_EQ(b[3], kVersion1);
    p.decl.n_svids = 1;
    p.decl.svids[0] = 0xFF01;
    encode(p, 1, b);
    CHECK_EQ(b[3], kVersion2);

    // the Revision data object alone: flag b3, four bytes after the USB blocks
    Twin t = pixel("Rev");
    t.revision = 0x32100000u;   // PD 3.2, version 1.0
    std::size_t n = encode(t, 2, b);
    CHECK_EQ(b[3], kVersion3);
    CHECK_EQ(b[kOffFlags], kFlagRevision);
    CHECK_EQ(n, kHeaderBytes + 3 * 4 + 4 + 2);
    CHECK_EQ(int(decode({b.data(), n}, seq, u)), int(Dec::Ok));
    CHECK_EQ(u.revision, 0x32100000u);
    CHECK_EQ(int(u.sink_ext_len), 0);

    // the Sink_Capabilities_Extended block alone: flag b4, a length byte and that many bytes
    Twin e = pixel("Ext");
    e.sink_ext_len = 21;
    for (std::size_t i = 0; i < 21; ++i) e.sink_ext[i] = std::uint8_t(0xA0 + i);
    n = encode(e, 3, b);
    CHECK_EQ(b[3], kVersion3);
    CHECK_EQ(b[kOffFlags], kFlagSinkExt);
    CHECK_EQ(n, kHeaderBytes + 3 * 4 + 1 + 21 + 2);
    CHECK_EQ(int(decode({b.data(), n}, seq, u)), int(Dec::Ok));
    CHECK_EQ(int(u.sink_ext_len), 21);
    CHECK_EQ(u.sink_ext[0], 0xA0);
    CHECK_EQ(u.sink_ext[20], 0xB4);
    CHECK_EQ(u.sink_ext[21], 0);
    CHECK_EQ(u.revision, 0u);

    // a phone with a Request and both blocks: 89 bytes, everything kept
    Twin ph = pixel("Phone");
    ph.rdo = 0x4344B12Cu;
    ph.rdo_pdo = 0x0004B12Cu;
    ph.revision = 0x31000000u;
    ph.sink_ext_len = kSinkExtBytes;
    ph.sink_ext.fill(0x5A);
    n = encode(ph, 4, b);
    CHECK_EQ(n, std::size_t(89));
    CHECK_EQ(b[kOffFlags], kFlagRequest | kFlagRevision | kFlagSinkExt);
    CHECK_EQ(int(decode({b.data(), n}, seq, u)), int(Dec::Ok));
    CHECK_EQ(int(u.sink_ext_len), int(kSinkExtBytes));
    CHECK_EQ(u.sink_ext[kSinkExtBytes - 1], 0x5A);

    // the record never grows past kMaxFileBytes (the volume budget): with every other block full, the extended block stays out,
    // then the Revision; what is written still reads back
    Twin big = ph;
    big.num_pdos = 7;
    big.decl.n_pt = kMaxProductVdos;
    big.decl.n_svids = kMaxDeclSvids;
    big.decl.n_modes = kMaxDeclModes;
    big.obs.valid = true;
    n = encode(big, 4, b);
    CHECK_EQ(n, kMaxFileBytes);
    CHECK_EQ(b[3], kVersion2);
    CHECK_EQ(int(decode({b.data(), n}, seq, u)), int(Dec::Ok));
    CHECK_EQ(u.revision, 0u);
    CHECK_EQ(int(u.sink_ext_len), 0);
    big.num_pdos = 6;   // four bytes free: the Revision goes in, the extended block still does not
    n = encode(big, 4, b);
    CHECK_EQ(n, kMaxFileBytes);
    CHECK_EQ(b[kOffFlags], kFlagRequest | kFlagDeclared | kFlagObserved | kFlagRevision);
    CHECK_EQ(int(decode({b.data(), n}, seq, u)), int(Dec::Ok));
    CHECK_EQ(u.revision, 0x31000000u);
    big.num_pdos = 0;   // 28 bytes free: an extended block of 23 bytes fits after the Revision, one of 24 does not
    big.sink_ext_len = 23;
    CHECK_EQ(encode(big, 4, b), kMaxFileBytes);
    big.sink_ext_len = 24;
    CHECK_EQ(encode(big, 4, b), kMaxFileBytes - 24);

    // more extended bytes than the block holds: nothing written
    Twin over = e;
    over.sink_ext_len = kSinkExtBytes + 1;
    CHECK_EQ(encode(over, 1, b), std::size_t(0));

    auto recrc = [](std::array<std::uint8_t, kMaxFileBytes>& r, std::size_t len) {
        detail::put16(&r[len - 2], bbstore::crc16(0xFFFF, {r.data(), len - 2}));
    };
    // a version 2 record with a version 3 flag is damage, not a new format
    std::array<std::uint8_t, kMaxFileBytes> c{};
    n = encode(t, 2, c);
    c[3] = kVersion2;
    recrc(c, n);
    CHECK_EQ(int(decode({c.data(), n}, seq, u)), int(Dec::Invalid));
    // an extended block of length 0 or over the maximum is damage
    n = encode(e, 3, c);
    const std::size_t ext_at = kHeaderBytes + 3 * 4;
    c[ext_at] = 0;
    recrc(c, n);
    CHECK_EQ(int(decode({c.data(), n}, seq, u)) != int(Dec::Ok), true);
    n = encode(e, 3, c);
    c[ext_at] = std::uint8_t(kSinkExtBytes + 1);
    recrc(c, n);
    CHECK_EQ(int(decode({c.data(), n}, seq, u)) != int(Dec::Ok), true);
    // a version 4 record is refused, never guessed
    n = encode(big, 4, c);
    c[3] = 4;
    recrc(c, n);
    CHECK_EQ(int(decode({c.data(), n}, seq, u)), int(Dec::Version));

    // merge keeps them the way it keeps everything else: what the new observation knows replaces, the rest stays
    Twin saved = pixel("Saved");
    merge(saved, t);
    CHECK_EQ(saved.revision, 0x32100000u);
    merge(saved, e);
    CHECK_EQ(saved.revision, 0x32100000u);
    CHECK_EQ(int(saved.sink_ext_len), 21);
    merge(saved, pixel(""));
    CHECK_EQ(int(saved.sink_ext_len), 21);
}

// The kind preference rides in two flag bits of a version 3 record (b5 PPS, b6 fixed): no byte added, and a record without them
// reads as "no preference".
static void test_kind_pref_record() {
    std::array<std::uint8_t, kMaxFileBytes> b{};
    std::uint8_t seq = 0;
    Twin p = pixel("Pixel");
    p.rdo = 0x4344B12Cu;
    p.rdo_pdo = 0x0004B12Cu;
    const std::size_t plain = encode(p, 1, b);
    CHECK_EQ(b[3], kVersion1);                      // no preference: byte-identical to the old record
    Twin u;
    CHECK_EQ(int(decode({b.data(), plain}, seq, u)), int(Dec::Ok));
    CHECK_EQ(int(u.pref), int(KindPref::None));
    p.pref = KindPref::Pps;
    std::size_t n = encode(p, 1, b);
    CHECK_EQ(n, plain);                             // same size
    CHECK_EQ(b[3], kVersion3);
    CHECK_EQ(b[kOffFlags], kFlagRequest | kFlagPrefPps);
    CHECK_EQ(int(decode({b.data(), n}, seq, u)), int(Dec::Ok));
    CHECK_EQ(int(u.pref), int(KindPref::Pps));
    p.pref = KindPref::Fixed;
    n = encode(p, 1, b);
    CHECK_EQ(b[kOffFlags], kFlagRequest | kFlagPrefFixed);
    CHECK_EQ(int(decode({b.data(), n}, seq, u)), int(Dec::Ok));
    CHECK_EQ(int(u.pref), int(KindPref::Fixed));
    // both bits, or a bit in a version 1 / 2 record, is damage
    auto recrc = [](std::array<std::uint8_t, kMaxFileBytes>& r, std::size_t len) {
        detail::put16(&r[len - 2], bbstore::crc16(0xFFFF, {r.data(), len - 2}));
    };
    b[kOffFlags] = kFlagRequest | kFlagPrefPps | kFlagPrefFixed;
    recrc(b, n);
    CHECK_EQ(int(decode({b.data(), n}, seq, u)), int(Dec::Invalid));
    b[kOffFlags] = kFlagRequest | kFlagPrefFixed;
    b[3] = kVersion2;
    recrc(b, n);
    CHECK_EQ(int(decode({b.data(), n}, seq, u)), int(Dec::Invalid));
    // the full record (every block) still fits kMaxFileBytes with a preference: the bits take no room
    Twin big = pixel("Big");
    big.rdo = 0x4344B12Cu;
    big.rdo_pdo = 0x0004B12Cu;
    big.pref = KindPref::Fixed;
    CHECK_EQ(encode(big, 2, b) <= kMaxFileBytes ? 1 : 0, 1);
    // merge: a sniff that saw a choice replaces the preference, one that did not leaves it
    Twin saved = pixel("Saved");
    saved.pref = KindPref::Pps;
    Twin in = pixel("");
    merge(saved, in);
    CHECK_EQ(int(saved.pref), int(KindPref::Pps));
    in.pref = KindPref::Fixed;
    merge(saved, in);
    CHECK_EQ(int(saved.pref), int(KindPref::Fixed));
}

// An inline interrogation's answers as a twin: identity, sink PDOs, the PD 3 answers, the USB words, and the contract it
// sniffed before asking (the Canon on its own charger, runs/clone-canon).
static void test_from_interrogated() {
    pd::IqStatus st;
    st.phase = pd::IqPhase::Done;
    st.rev = 2;
    st.first = {true, 4, 2, 0x4344B12Cu, 0x0004B12Cu};
    st.second = st.first;
    st.num_sink_pdos = 2;
    st.sink_pdos = {0x0001912C, 0x0004B12C};
    st.revision = 0x32100000u;
    st.sink_ext_len = 21;
    st.sink_ext[0] = 0xA9;
    st.sink_ext[20] = 0x3C;
    Twin t;
    CHECK_EQ(from_interrogated(st, t) ? 1 : 0, 1);
    CHECK_EQ(t.vid, 0);
    CHECK_EQ(t.pd_rev, 2);
    CHECK_EQ(t.num_pdos, 2);
    CHECK_EQ(t.pdos[1], 0x0004B12Cu);
    CHECK_EQ(t.rdo, 0x4344B12Cu);
    CHECK_EQ(t.rdo_pdo, 0x0004B12Cu);
    CHECK_EQ(t.revision, 0x32100000u);
    CHECK_EQ(int(t.sink_ext_len), 21);
    CHECK_EQ(t.sink_ext[20], 0x3C);
    CHECK_EQ(t.decl.any() ? 1 : 0, 0);
    CHECK_EQ(t.name[0], 0);

    // with an identity and USB words: the same fields from_identify keeps
    st.id = identity_of(0x04A9, 0x3001, (1u << 30) | (1u << 26) | (2u << 27));
    st.id.vdos[4] = 0x11000000u;
    st.id.num_vdos = 5;
    st.num_svids = 2;
    st.svids[0] = 0xFF01;
    st.svids[1] = 0x04A9;
    st.num_modes = 1;
    st.mode_svid[0] = 0xFF01;
    st.mode_vdo[0] = 0x000C0045u;
    CHECK_EQ(from_interrogated(st, t) ? 1 : 0, 1);
    CHECK_EQ(t.vid, 0x04A9);
    CHECK_EQ(t.pid, 0x3001);
    CHECK_EQ(t.id_hi, id_hi_of(st.id.id_header));
    CHECK_EQ(int(t.decl.n_pt), 1);
    CHECK_EQ(t.decl.pt[0], 0x11000000u);
    CHECK_EQ(int(t.decl.n_svids), 2);
    CHECK_EQ(int(t.decl.svids[1]), 0x04A9);
    CHECK_EQ(int(t.decl.n_modes), 1);
    CHECK_EQ(t.decl.mode_vdo[0], 0x000C0045u);

    // no sniffed first contract (the run began with its own reset and the device asked after): the second one is the Request
    pd::IqStatus s2 = st;
    s2.first = {};
    CHECK_EQ(from_interrogated(s2, t) ? 1 : 0, 1);
    CHECK_EQ(t.rdo, 0x4344B12Cu);
    // the device's own revision is its Request's, not the one the questions went out in
    s2.first = st.first;
    s2.first.dev_rev = 1;
    s2.rev = 1;
    CHECK_EQ(from_interrogated(s2, t) ? 1 : 0, 1);
    CHECK_EQ(t.pd_rev, 1);

    // a run that is still going, or one that learned neither a contract nor sink PDOs, is not a twin
    pd::IqStatus run = st;
    run.phase = pd::IqPhase::Ask;
    CHECK_EQ(from_interrogated(run, t) ? 1 : 0, 0);
    pd::IqStatus none;
    none.phase = pd::IqPhase::Aborted;
    none.id = st.id;
    CHECK_EQ(from_interrogated(none, t) ? 1 : 0, 0);
    // an aborted run keeps what it did read
    pd::IqStatus cut = st;
    cut.phase = pd::IqPhase::Aborted;
    cut.why = pd::IqWhy::Timeout;
    CHECK_EQ(from_interrogated(cut, t) ? 1 : 0, 1);
    CHECK_EQ(t.num_pdos, 2);
}

// OK on "Learn from device": alone -> Identify, inline -> interrogate, a current Identify -> learn from it, dark -> no power.
static void test_learn_path() {
    using P = LearnPath;
    CHECK_EQ(int(learn_path(true, false, false, true)), int(P::Identify));
    CHECK_EQ(int(learn_path(true, true, true, false)), int(P::Identify));        // alone wins (the inline latch is stale then)
    CHECK_EQ(int(learn_path(false, true, false, false)), int(P::Interrogate));
    CHECK_EQ(int(learn_path(false, true, true, true)), int(P::Interrogate));     // inline: ask it, even with an older Identify
    CHECK_EQ(int(learn_path(false, false, true, true)), int(P::LastIdentify));   // identified a moment ago, released since
    CHECK_EQ(int(learn_path(false, false, false, false)), int(P::LastIdentify)); // VBUS up, not inline (the K2 is the sink)
    CHECK_EQ(int(learn_path(false, false, false, true)), int(P::NoPower));       // a device with no charger, not a phone alone
}

int main() {
    test_learn_path();
    test_from_interrogated();
    test_codec_v3();
    test_same_device();
    test_library_merges_anonymous_by_fingerprint();
    test_codec_roundtrip();
    test_codec_guards();
    test_codec_v1_golden();
    test_library_basic();
    test_library_capacity_delete_rename();
    test_library_damaged_and_foreign();
    test_library_power_loss();
    test_auto_name();
    test_from_identify();
    test_learn_names_and_rename_wins();
    test_from_sniffed();
    test_from_sniffed_kind_pref();
    test_kind_pref_record();
    test_learn_sniffed_attaches();
    return test_report("test_twin_lib");
}
