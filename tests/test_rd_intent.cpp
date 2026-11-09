// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Host tests for the Rd intent marker (k2/core/rd_intent.hpp): the record format and its damage handling, the cold-boot Rd decision
// (an ordinary plain cold boot never presents an Rd it did not choose), the comeback judge (consume on use, release fast when no
// source shows), the session follower (writes on a state change only) and the store over a RAM disk.
#include <array>
#include <cstdint>

#include "k2/core/bootmode.hpp"
#include "k2/core/fs.hpp"
#include "k2/core/rd_intent.hpp"
#include "ramdisk.hpp"
#include "test_util.h"

using namespace k2;
using namespace k2::rdintent;
using ctcable::Choice;
using ctcable::Kind;

namespace {
constexpr Choice kOff{};
constexpr Choice kB2{Kind::Builtin, 2}, kS7{Kind::Saved, 7};
constexpr Intent kNone{};
constexpr Intent kSession1{IntentKind::Session, 1, kOff};
constexpr Intent kSession2{IntentKind::Session, 2, kOff};
constexpr Intent kReattachB2{IntentKind::Reattach, 2, kB2};
constexpr Intent kReattachS7{IntentKind::Reattach, 2, kS7};
constexpr Intent kReattachBare{IntentKind::Reattach, 2, kOff};

struct Vol {
    k2test::RamDisk d{96};
    fs::Fs f{d, 96};
    Vol() { bool x; f.mount_or_format(x); }
};

void test_record_format() {
    // "K2RI", kind, pin, cable kind, seq low, seq high, check = kind ^ pin ^ cable kind ^ seq low ^ seq high ^ 0x5A
    const std::array<std::uint8_t, 10> session{'K', '2', 'R', 'I', 1, 2, 0, 0, 0, std::uint8_t(1 ^ 2 ^ 0x5A)};
    CHECK_EQ(encode(kSession2) == session, 1);
    CHECK_EQ(decode(session) == kSession2, 1);
    const std::array<std::uint8_t, 10> reattach{'K', '2', 'R', 'I', 2, 2, 1, 2, 0, std::uint8_t(2 ^ 2 ^ 1 ^ 2 ^ 0x5A)};
    CHECK_EQ(encode(kReattachB2) == reattach, 1);
    CHECK_EQ(decode(reattach) == kReattachB2, 1);
    CHECK_EQ(decode(encode(kReattachS7)) == kReattachS7, 1);
    CHECK_EQ(decode(encode(kReattachBare)) == kReattachBare, 1);
    CHECK_EQ(decode(encode(kSession1)) == kSession1, 1);
    CHECK_EQ(decode(encode(kNone)) == kNone, 1);
}

void test_damage_reads_as_none() {
    const auto r = encode(kReattachS7);
    CHECK_EQ(decode(r) == kReattachS7, 1);
    auto bad = r; bad[9] ^= 1;      // check byte
    CHECK_EQ(decode(bad) == kNone, 1);
    bad = r; bad[0] = 'X';          // magic
    CHECK_EQ(decode(bad) == kNone, 1);
    bad = r; bad[5] ^= 3;           // a flipped payload bit: the check no longer matches
    CHECK_EQ(decode(bad) == kNone, 1);
    std::array<std::uint8_t, 10> blank{};
    blank.fill(0xFF);               // a never-written, erased page
    CHECK_EQ(decode(blank) == kNone, 1);
    // Well-formed record, impossible content: an unknown kind, no pin, both pins, a session with a cable, a cable on the CC1 pin
    // (the Ra sits on CC1), a clone without its id, a built-in past the table.
    CHECK_EQ(decode(encode({static_cast<IntentKind>(3), 2, kOff})) == kNone, 1);
    CHECK_EQ(decode(encode({IntentKind::Session, 0, kOff})) == kNone, 1);
    CHECK_EQ(decode(encode({IntentKind::Session, 3, kOff})) == kNone, 1);
    CHECK_EQ(decode(encode({IntentKind::Session, 2, kB2})) == kNone, 1);
    CHECK_EQ(decode(encode({IntentKind::Reattach, 1, kB2})) == kNone, 1);
    CHECK_EQ(decode(encode({IntentKind::Reattach, 2, Choice{Kind::Saved, 0}})) == kNone, 1);
    CHECK_EQ(decode(encode({IntentKind::Reattach, 2, Choice{Kind::Builtin, 40}})) == kNone, 1);
    // A bare Reattach (no cable) may sit on either pin: it only keeps Rd.
    constexpr Intent kBareCc1{IntentKind::Reattach, 1, kOff};
    CHECK_EQ(decode(encode(kBareCc1)) == kBareCc1, 1);
}

void test_the_boot_mode_decision() {
    using bootmode::Mode;
    // A key held decides, whatever the marker says.
    CHECK_EQ(int(boot_mode(Mode::PlusRdCc2, false, kSession1)), int(Mode::PlusRdCc2));
    CHECK_EQ(int(boot_mode(Mode::OkRdCc1, false, kReattachB2)), int(Mode::OkRdCc1));
    CHECK_EQ(int(boot_mode(Mode::OkRdCc1, true, kNone)), int(Mode::OkRdCc1));
    // An ordinary plain cold boot (no marker) never presents an Rd.
    CHECK_EQ(int(boot_mode(Mode::Plain, false, kNone)), int(Mode::Plain));
    // The marker turns a plain COLD boot into the sink mode of its pin ...
    CHECK_EQ(int(boot_mode(Mode::Plain, false, kSession1)), int(Mode::OkRdCc1));
    CHECK_EQ(int(boot_mode(Mode::Plain, false, kSession2)), int(Mode::PlusRdCc2));
    CHECK_EQ(int(boot_mode(Mode::Plain, false, kReattachB2)), int(Mode::PlusRdCc2));
    CHECK_EQ(int(boot_mode(Mode::Plain, false, kReattachBare)), int(Mode::PlusRdCc2));
    // ... and never a warm one: the pin mark owns warm resets, a marker left by an earlier session must not widen them.
    CHECK_EQ(int(boot_mode(Mode::Plain, true, kSession2)), int(Mode::Plain));
    CHECK_EQ(int(boot_mode(Mode::Plain, true, kReattachB2)), int(Mode::Plain));
    // The pin the mode gives is what the marker named, as the driver's initial_rd computes it.
    CHECK_EQ(bootmode::initial_rd(boot_mode(Mode::Plain, false, kSession1), false, 0), 1);
    CHECK_EQ(bootmode::initial_rd(boot_mode(Mode::Plain, false, kSession2), false, 0), 2);
    // Only a cold plain boot reads the marker at all (the read costs a mount).
    CHECK_EQ(reads_marker(Mode::Plain, false), 1);
    CHECK_EQ(reads_marker(Mode::Plain, true), 0);
    CHECK_EQ(reads_marker(Mode::PlusRdCc2, false), 0);
    CHECK_EQ(reads_marker(Mode::OkRdCc1, false), 0);
}

void test_the_comeback_judge() {
    // Caps seen: vouched, whenever.
    CHECK_EQ(int(judge({0, false, true})), int(Verdict::Vouched));
    CHECK_EQ(int(judge({1400, true, true})), int(Verdict::Vouched));
    // Waiting for the source: the Rp is on the line, give it up to kGiveUpMs.
    CHECK_EQ(int(judge({0, false, false})), int(Verdict::Wait));
    CHECK_EQ(int(judge({kNoRpMs - 1, false, false})), int(Verdict::Wait));
    CHECK_EQ(int(judge({kNoRpMs, true, false})), int(Verdict::Wait));
    CHECK_EQ(int(judge({kGiveUpMs - 1, true, false})), int(Verdict::Wait));
    // No Rp on any line after kNoRpMs: nothing is there to hold the Rd for (a laptop's port, a bare table): release fast.
    CHECK_EQ(int(judge({kNoRpMs, false, false})), int(Verdict::Release));
    CHECK_EQ(int(judge({kGiveUpMs - 1, false, false})), int(Verdict::Release));
    // An Rp that never sends capabilities: released at kGiveUpMs.
    CHECK_EQ(int(judge({kGiveUpMs, true, false})), int(Verdict::Release));
    CHECK_EQ(int(judge({60000, true, false})), int(Verdict::Release));
    CHECK_EQ(1, kNoRpMs < kGiveUpMs);
}

void test_what_a_vouched_comeback_leaves() {
    // The record is consumed on use: a planned re-attach becomes an ordinary session, a session stays one, nothing stays a
    // planned re-attach after the boot that served it.
    CHECK_EQ(after_vouch(kReattachB2) == kSession2, 1);
    CHECK_EQ(after_vouch(kReattachBare) == kSession2, 1);
    CHECK_EQ(after_vouch({IntentKind::Reattach, 1, kOff}) == kSession1, 1);
    CHECK_EQ(after_vouch(kSession1) == kSession1, 1);
    CHECK_EQ(after_vouch(kNone) == kNone, 1);
}

void test_the_follower() {
    Follower f;
    // A plain boot that is not self-powered: the first look scrubs (writes None) once, then nothing.
    CHECK_EQ(f.update({false, 0, false, false}) == Follower::Act::Apply, 1);
    CHECK_EQ(f.wanted() == kNone, 1);
    CHECK_EQ(f.update({false, 0, false, false}) == Follower::Act::Hold, 1);
    CHECK_EQ(f.update({false, 0, true, false}) == Follower::Act::Hold, 1);

    Follower g;
    // Pinned, no contract yet: nothing to say (a decision waits for the contract).
    CHECK_EQ(g.update({true, 2, false, false}) == Follower::Act::Hold, 1);
    CHECK_EQ(g.update({true, 2, false, false}) == Follower::Act::Hold, 1);
    // The contract becomes Ready: the session is marked, once.
    CHECK_EQ(g.update({true, 2, true, false}) == Follower::Act::Apply, 1);
    CHECK_EQ(g.wanted() == kSession2, 1);
    for (int i = 0; i < 1000; i++) CHECK_EQ(g.update({true, 2, true, false}) == Follower::Act::Hold, 1);   // no write per loop pass
    // The contract drops and returns (a charger's own Hard Reset, a run's request): no change, no write.
    CHECK_EQ(g.update({true, 2, false, false}) == Follower::Act::Hold, 1);
    CHECK_EQ(g.update({true, 2, true, false}) == Follower::Act::Hold, 1);
    // The pin moves (the charger sits on the other plug orientation): rewritten once.
    CHECK_EQ(g.update({true, 1, true, false}) == Follower::Act::Apply, 1);
    CHECK_EQ(g.wanted() == kSession1, 1);
    CHECK_EQ(g.update({true, 1, true, false}) == Follower::Act::Hold, 1);
    // A host appears (HID or drive link: the K2 no longer lives on the charger): the marker goes.
    CHECK_EQ(g.update({true, 1, true, true}) == Follower::Act::Apply, 1);
    CHECK_EQ(g.wanted() == kNone, 1);
    // The pin is released (nothing resumes on it): the marker stays gone.
    CHECK_EQ(g.update({false, 0, false, true}) == Follower::Act::Hold, 1);
    CHECK_EQ(g.update({false, 0, true, false}) == Follower::Act::Hold, 1);

    // A pinned K2 with a host is not a session, even when its contract is Ready.
    Follower h;
    CHECK_EQ(h.update({true, 2, true, true}) == Follower::Act::Apply, 1);
    CHECK_EQ(h.wanted() == kNone, 1);
}

void test_save_and_reload() {
    Vol v;
    CHECK_EQ(load(v.f) == kNone, 1);                              // a blank volume
    CHECK_EQ(save(v.f, kReattachS7), true);
    CHECK_EQ(load(v.f) == kReattachS7, 1);
    CHECK_EQ(save(v.f, kSession2), true);
    CHECK_EQ(load(v.f) == kSession2, 1);
    CHECK_EQ(v.f.exists(kPath), true);
    CHECK_EQ(save(v.f, kNone), true);                             // None removes the file
    CHECK_EQ(v.f.exists(kPath), false);
    CHECK_EQ(load(v.f) == kNone, 1);
}

void test_save_writes_only_on_change() {
    Vol v;
    CHECK_EQ(save(v.f, kSession2), true);
    v.d.reset_counters();
    CHECK_EQ(save(v.f, kSession2), true);                         // the same record: a read, no write
    CHECK_EQ(save(v.f, kSession2), true);
    CHECK_EQ(int(v.d.progs + v.d.erases), 0);
    CHECK_EQ(save(v.f, kSession1), true);                         // a change is written
    CHECK_EQ(int(v.d.progs) > 0, 1);
    CHECK_EQ(load(v.f) == kSession1, 1);
    CHECK_EQ(save(v.f, kNone), true);
    v.d.reset_counters();
    CHECK_EQ(save(v.f, kNone), true);                             // nothing to remove: no flash traffic
    CHECK_EQ(int(v.d.progs + v.d.erases), 0);
}

void test_a_damaged_file_reads_as_none() {
    Vol v;
    const std::array<std::uint8_t, 4> shortf{'K', '2', 'R', 'I'};
    CHECK_EQ(v.f.write_file(kPath, shortf) == fs::Err::Ok, true);
    CHECK_EQ(load(v.f) == kNone, 1);
    auto r = encode(kSession2);
    std::array<std::uint8_t, 11> longf{};
    for (size_t i = 0; i < r.size(); ++i) longf[i] = r[i];
    CHECK_EQ(v.f.write_file(kPath, longf) == fs::Err::Ok, true);
    CHECK_EQ(load(v.f) == kNone, 1);
    // save() over a damaged file replaces it
    CHECK_EQ(save(v.f, kSession2), true);
    CHECK_EQ(load(v.f) == kSession2, 1);
}
}  // namespace

int main() {
    test_record_format();
    test_damage_reads_as_none();
    test_the_boot_mode_decision();
    test_the_comeback_judge();
    test_what_a_vouched_comeback_leaves();
    test_the_follower();
    test_save_and_reload();
    test_save_writes_only_on_change();
    test_a_damaged_file_reads_as_none();
    return test_report("test_rd_intent");
}
