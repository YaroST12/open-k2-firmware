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
    CHECK_EQ(encode(t, 1, b), kMaxFileBytes);
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
    CHECK_STR(auto_name(0x05AC, 2u << 4).data(), "Apple Peripheral");
    CHECK_STR(auto_name(0x17EF, 2u << 4).data(), "Lenovo Peripheral");
    CHECK_STR(auto_name(0x17EF, 2u).data(), "Lenovo Host");               // a laptop answers as a DFP host
    CHECK_STR(auto_name(0x17EF, 0).data(), "Lenovo");                     // product type not read
    CHECK_STR(auto_name(0x1234, 2u << 4).data(), "VID 1234");             // an unlisted VID names itself
    CHECK_STR(auto_name(0x0ABC, 0).data(), "VID 0ABC");
    CHECK_STR(auto_name(0, 0).data(), "Device");
    CHECK_EQ(emlib::name_valid(auto_name(0x05AC, 3u << 4).data()) ? 1 : 0, 1);   // always a name the rename rule accepts
    CHECK_STR(auto_name(0x05AC, 3u << 4).data(), "Apple Power bank");
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
    CHECK_STR(name_of(l, r.seq).c_str(), "Google Peripheral");
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
    CHECK_STR(name_of(l, 2).c_str(), "Google Peripheral");
    Twin t4;
    from_identify(identity_of(0x18D1, 0x4EE3, 2u << 27), st, 2, t4);
    r = l.save(t4);
    CHECK_STR(name_of(l, r.seq).c_str(), "Google Peripheral 2");
    // a device with no VID is never matched to another: each learn adds one (named by number)
    Twin anon;
    from_identify(pd::Identity{}, st, 2, anon);
    CHECK_EQ(l.save(anon).seq, 4);
    CHECK_EQ(l.save(anon).seq, 5);
    CHECK_STR(name_of(l, 4).c_str(), "Device");
    CHECK_STR(name_of(l, 5).c_str(), "Device 2");
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
    CHECK_STR(name_of(l, 2).c_str(), "Apple Peripheral");
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

int main() {
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
    test_learn_sniffed_attaches();
    return test_report("test_twin_lib");
}
