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
constexpr Intent kSessionB2{IntentKind::Session, 2, kB2};
constexpr Intent kSessionS7{IntentKind::Session, 2, kS7};
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
    // Well-formed record, impossible content: an unknown kind, no pin, both pins, a cable on the CC1 pin (the Ra sits on CC1), a
    // clone without its id, a built-in past the table. A session may carry the cable of the run it serves (pin 2 only).
    CHECK_EQ(decode(encode({static_cast<IntentKind>(4), 2, kOff})) == kNone, 1);
    CHECK_EQ(decode(encode({IntentKind::Session, 0, kOff})) == kNone, 1);
    CHECK_EQ(decode(encode({IntentKind::Session, 3, kOff})) == kNone, 1);
    CHECK_EQ(decode(encode(kSessionB2)) == kSessionB2, 1);
    CHECK_EQ(decode(encode({IntentKind::Session, 1, kB2})) == kNone, 1);
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

void test_the_planned_reattach_gate() {
    constexpr Choice kSaved5A{Kind::Saved, 9};
    const ctcable::Facts a5{0x2717, 0x3202, 50000, 5000}, a3{0x04b4, 1, 20000, 3000};
    // Every condition holds: a 6 A spoof cable, the Ra side (CC2 pin), self-powered, the source shows the Xiaomi chain.
    CHECK_EQ(plan_reattach(kB2, a5, 2, true, true), true);
    CHECK_EQ(plan_reattach(kSaved5A, a5, 2, true, true), true);
    // The blink must pay off: each missing condition cancels it (an Apple-like source, a plain PLUS boot, no cable, a 3 A cable, ...).
    CHECK_EQ(plan_reattach(kB2, a5, 2, true, false), false);     // the source does not list SVID 0x2717
    CHECK_EQ(plan_reattach(kB2, a5, 2, false, true), false);     // bus-powered: nothing dies, nothing to come back from
    CHECK_EQ(plan_reattach(kOff, a5, 2, true, true), false);     // no spoof identity chosen
    CHECK_EQ(plan_reattach(kB2, a3, 2, true, true), false);      // a 3 A cable: the 6 A rows are not asked for
    CHECK_EQ(plan_reattach(kB2, a5, 1, true, true), false);      // Rd on CC1: the Ra shares that line
    CHECK_EQ(plan_reattach(kB2, a5, 0, true, true), false);
    CHECK_EQ(plan_reattach(Choice{Kind::Builtin, 99}, a5, 2, true, true), false);   // an invalid choice
}

// Only the Reattach kind marks a boot whose reboot was the planned re-attach: the run journal's resume on that boot is not
// a power loss and must not be counted as one.
void test_only_the_reattach_kind_is_a_planned_reattach() {
    CHECK_EQ(planned_reattach(IntentKind::Reattach), true);
    CHECK_EQ(planned_reattach(IntentKind::Session), false);
    CHECK_EQ(planned_reattach(IntentKind::Rescan), false);
    CHECK_EQ(planned_reattach(IntentKind::None), false);
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

void test_the_follower_carries_a_runs_cable() {
    Follower f;
    CHECK_EQ(f.update({true, 2, true, false, kOff}) == Follower::Act::Apply, 1);
    CHECK_EQ(f.wanted() == kSession2, 1);
    // A charger run starts with a spoof identity: the record says which, once (the comeback arms it before the charger's look).
    CHECK_EQ(f.update({true, 2, true, false, kB2}) == Follower::Act::Apply, 1);
    CHECK_EQ(f.wanted() == kSessionB2, 1);
    CHECK_EQ(f.update({true, 2, true, false, kB2}) == Follower::Act::Hold, 1);
    // The contract drops mid-run (the charger cut VBUS, this is the moment the K2 dies): nothing changes, nothing is written.
    CHECK_EQ(f.update({true, 2, false, false, kB2}) == Follower::Act::Hold, 1);
    CHECK_EQ(f.wanted() == kSessionB2, 1);
    // A saved clone is carried the same way; the run ends: the cable goes, the session stays.
    CHECK_EQ(f.update({true, 2, true, false, kS7}) == Follower::Act::Apply, 1);
    CHECK_EQ(f.wanted() == kSessionS7, 1);
    CHECK_EQ(f.update({true, 2, true, false, kOff}) == Follower::Act::Apply, 1);
    CHECK_EQ(f.wanted() == kSession2, 1);
    // On the CC1 pin the Ra cannot be raised: no cable is recorded, whatever the run chose.
    Follower g;
    CHECK_EQ(g.update({true, 1, true, false, kB2}) == Follower::Act::Apply, 1);
    CHECK_EQ(g.wanted() == kSession1, 1);
    // An impossible choice is not recorded either.
    CHECK_EQ(g.update({true, 2, true, false, Choice{Kind::Builtin, 99}}) == Follower::Act::Apply, 1);
    CHECK_EQ(g.wanted() == kSession2, 1);
}

void test_forget_rewrites_what_the_volume_may_have_lost() {
    // The planned re-attach writes its own record behind the follower's back: forget() makes the next look write the follower's.
    Follower f;
    CHECK_EQ(f.update({true, 2, true, false, kB2}) == Follower::Act::Apply, 1);
    CHECK_EQ(f.update({true, 2, true, false, kB2}) == Follower::Act::Hold, 1);
    f.forget();
    CHECK_EQ(f.update({true, 2, true, false, kB2}) == Follower::Act::Apply, 1);
    CHECK_EQ(f.wanted() == kSessionB2, 1);
    CHECK_EQ(f.update({true, 2, true, false, kB2}) == Follower::Act::Hold, 1);
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
// The Connection page's bridged rescan (kind 3): the K2 lives on the charger's VBUS, the CC cycle kills it, the comeback keeps Rd.
// It carries no spoof identity (a page-armed identity is not in the record), and the older kinds keep their bytes.
void test_the_rescan_kind() {
    constexpr Intent kRescan1{IntentKind::Rescan, 1, kOff};
    constexpr Intent kRescan2{IntentKind::Rescan, 2, kOff};
    const std::array<std::uint8_t, 10> rescan2{'K', '2', 'R', 'I', 3, 2, 0, 0, 0, std::uint8_t(3 ^ 2 ^ 0x5A)};
    CHECK_EQ(int(IntentKind::Rescan), 3);
    CHECK_EQ(encode(kRescan2) == rescan2, 1);
    CHECK_EQ(decode(rescan2) == kRescan2, 1);
    CHECK_EQ(decode(encode(kRescan1)) == kRescan1, 1);
    // no identity rides in it, no unknown kind after it
    CHECK_EQ(decode(encode({IntentKind::Rescan, 2, kB2})) == kNone, 1);
    CHECK_EQ(decode(encode({IntentKind::Rescan, 0, kOff})) == kNone, 1);
    auto bad = encode(kRescan2);
    bad[4] = 4; bad[9] = check_of(4, 2, 0, 0, 0);
    CHECK_EQ(decode(bad) == kNone, 1);
    // a cold plain boot keeps the Rd of the record's pin; a warm reset or a key mode does not take it over
    CHECK_EQ(int(boot_mode(bootmode::Mode::Plain, false, kRescan2)), int(bootmode::Mode::PlusRdCc2));
    CHECK_EQ(int(boot_mode(bootmode::Mode::Plain, false, kRescan1)), int(bootmode::Mode::OkRdCc1));
    CHECK_EQ(int(boot_mode(bootmode::Mode::Plain, true, kRescan2)), int(bootmode::Mode::Plain));
    CHECK_EQ(int(boot_mode(bootmode::Mode::OkRdCc1, false, kRescan2)), int(bootmode::Mode::OkRdCc1));
    // it is judged like any comeback, and a vouched one is consumed by the follower's Session rewrite
    CHECK_EQ(int(judge({100, true, true})), int(Verdict::Vouched));
    CHECK_EQ(int(judge({kNoRpMs, false, false})), int(Verdict::Release));
    Vol v;
    CHECK_EQ(save(v.f, kRescan2), true);
    CHECK_EQ(load(v.f) == kRescan2, 1);
    Follower fo;
    CHECK_EQ(int(fo.update({true, 2, false, false, {}})), int(Follower::Act::Hold));   // not Ready yet: the record stays
    CHECK_EQ(int(fo.update({true, 2, true, false, {}})), int(Follower::Act::Apply));
    CHECK_EQ(fo.wanted() == kSession2, 1);
}
}  // namespace

// Hardware 2026-11-09 (build b00178c): a plain boot on HID power with a record left on the volume came up with Rd on CC2 at 30 ms,
// Requested 12 times over 3 s and collided with the phone behind the K2. The aux rail says who powers the K2 (about 5 V from a
// host, nothing on a charger-powered or comeback boot), and a record never describes a boot that a host powers.
void test_a_host_powered_boot_ignores_the_record() {
    using bootmode::Mode;
    CHECK_EQ(host_powered(5041), true);       // the HID boot of the capture
    CHECK_EQ(host_powered(4600), true);
    CHECK_EQ(host_powered(0), false);         // comeback / charger-powered: the rail is dead
    CHECK_EQ(host_powered(300), false);
    CHECK_EQ(host_powered(kHostAuxMv - 1), false);
    CHECK_EQ(host_powered(kHostAuxMv), true);
    // The record is read, but a host-powered cold boot stays plain whatever it says.
    CHECK_EQ(int(boot_mode(Mode::Plain, false, kSession2, true)), int(Mode::Plain));
    CHECK_EQ(int(boot_mode(Mode::Plain, false, kReattachB2, true)), int(Mode::Plain));
    CHECK_EQ(int(boot_mode(Mode::Plain, false, kSession1, true)), int(Mode::Plain));
    // Without a host the decision is unchanged.
    CHECK_EQ(int(boot_mode(Mode::Plain, false, kSession2, false)), int(Mode::PlusRdCc2));
    CHECK_EQ(int(boot_mode(Mode::Plain, false, kSession2)), int(Mode::PlusRdCc2));
    // A key held decides regardless (the user asked for the Rd).
    CHECK_EQ(int(boot_mode(Mode::PlusRdCc2, false, kNone, true)), int(Mode::PlusRdCc2));
    CHECK_EQ(int(boot_mode(Mode::OkRdCc1, false, kNone, true)), int(Mode::OkRdCc1));
}

// A sink behind the K2 (the Link's inline latch, or another sink's Request on the wire) is the end of a self-powered session: the K2
// presents no Rd for it, so no record may bring the Rd back on the next boot.
void test_a_sink_downstream_scrubs_the_record() {
    Follower f;
    CHECK_EQ(f.update({true, 2, true, false}) == Follower::Act::Apply, 1);
    CHECK_EQ(f.wanted() == kSession2, 1);
    CHECK_EQ(f.update({true, 2, true, false, kOff, true}) == Follower::Act::Apply, 1);
    CHECK_EQ(f.wanted() == kNone, 1);
    CHECK_EQ(f.update({true, 2, true, false, kOff, true}) == Follower::Act::Hold, 1);
    Follower g;   // seen before the contract was ever Ready: the record is scrubbed at once, never written
    CHECK_EQ(g.update({true, 2, false, false, kOff, true}) == Follower::Act::Apply, 1);
    CHECK_EQ(g.wanted() == kNone, 1);
    CHECK_EQ(g.update({true, 2, true, false, kOff, true}) == Follower::Act::Hold, 1);
}

// The SRAM canary written with the marker (k2/core/rd_intent.hpp; the device keeps it in a .noinit section, link.ld): a canary
// that survived this reset vouches for exactly one record, and a torn, garbage or cleared one vouches for nothing.
void test_the_canary() {
    Canary c{};                                   // power-on SRAM reads as nothing vouched
    CHECK_EQ(canary_survived(c), false);
    Canary garbage{kCanaryMagic, 0x12345678u, 0xDEADBEEFu};   // the magic alone is not a canary
    CHECK_EQ(canary_survived(garbage), false);
    Canary random{0xA5A5A5A5u, 0x0BAD0BADu, 0xFEEDFACEu};     // a random power-on pattern
    CHECK_EQ(canary_survived(random), false);
    arm_canary(c, kReattachB2);
    CHECK_EQ(canary_survived(c), true);
    CHECK_EQ(canary_matches(c, kReattachB2), true);
    CHECK_EQ(canary_matches(c, kReattachS7), false);   // another record: not vouched for
    CHECK_EQ(canary_matches(c, kSession2), false);
    CHECK_EQ(canary_matches(c, kNone), false);
    c.check ^= 1;                               // a torn write fails the check
    CHECK_EQ(canary_survived(c), false);
    CHECK_EQ(canary_matches(c, kReattachB2), false);
    arm_canary(c, kSession2);
    CHECK_EQ(canary_matches(c, kSession2), true);
    arm_canary(c, kSession1);                   // re-arm moves the voucher to the new record
    CHECK_EQ(canary_matches(c, kSession1), true);
    CHECK_EQ(canary_matches(c, kSession2), false);
    clear_canary(c);
    CHECK_EQ(canary_survived(c), false);
    CHECK_EQ(canary_matches(c, kSession1), false);
}

// The stale-record decision table, both ways. The canary argument is what the hardware measurement settles (does .noinit SRAM
// survive the comeback's short dark on this board); the table itself is pure and is tested for both outcomes here.
void test_a_cold_boot_trusts_the_record_only_with_a_live_canary() {
    using bootmode::Mode;
    constexpr Intent kRescan2{IntentKind::Rescan, 2, kOff};
    // Canary survived (the comeback: the charger cut VBUS under the self-powered K2 for a short dark and re-applied it):
    // the record still turns a cold plain boot into its sink mode.
    CHECK_EQ(int(boot_mode(Mode::Plain, false, kSession2, false, true)), int(Mode::PlusRdCc2));
    CHECK_EQ(int(boot_mode(Mode::Plain, false, kReattachB2, false, true)), int(Mode::PlusRdCc2));
    CHECK_EQ(int(boot_mode(Mode::Plain, false, kSession1, false, true)), int(Mode::OkRdCc1));
    CHECK_EQ(int(boot_mode(Mode::Plain, false, kRescan2, false, true)), int(Mode::PlusRdCc2));
    // Canary gone (a bare unplug left the marker and the K2 sat dead until SRAM decayed, or a power-on's random pattern):
    // the record is stale, a plain boot stays plain - no Rd for whatever is on the port now.
    CHECK_EQ(int(boot_mode(Mode::Plain, false, kSession2, false, false)), int(Mode::Plain));
    CHECK_EQ(int(boot_mode(Mode::Plain, false, kReattachB2, false, false)), int(Mode::Plain));
    CHECK_EQ(int(boot_mode(Mode::Plain, false, kSession1, false, false)), int(Mode::Plain));
    CHECK_EQ(int(boot_mode(Mode::Plain, false, kRescan2, false, false)), int(Mode::Plain));
    // The other gates are unchanged, whatever the canary says: a host-powered boot and a warm boot ignore the record.
    CHECK_EQ(int(boot_mode(Mode::Plain, false, kSession2, true, true)), int(Mode::Plain));
    CHECK_EQ(int(boot_mode(Mode::Plain, false, kSession2, true, false)), int(Mode::Plain));
    CHECK_EQ(int(boot_mode(Mode::Plain, true, kSession2, false, true)), int(Mode::Plain));
    CHECK_EQ(int(boot_mode(Mode::Plain, true, kSession2, false, false)), int(Mode::Plain));
    // A key held decides, and no record is no comeback, either way.
    CHECK_EQ(int(boot_mode(Mode::PlusRdCc2, false, kSession2, false, false)), int(Mode::PlusRdCc2));
    CHECK_EQ(int(boot_mode(Mode::OkRdCc1, false, kReattachB2, false, false)), int(Mode::OkRdCc1));
    CHECK_EQ(int(boot_mode(Mode::Plain, false, kNone, false, true)), int(Mode::Plain));
    CHECK_EQ(int(boot_mode(Mode::Plain, false, kNone, false, false)), int(Mode::Plain));
    // Without the canary argument the decision is the survived way, so the older tests keep their meaning.
    CHECK_EQ(int(boot_mode(Mode::Plain, false, kSession2)), int(Mode::PlusRdCc2));
}

// The marker and its canary across a simulated power loss: save arms, a short dark keeps the voucher (the comeback trusts the
// record), a long dark loses it (the record is stale and the boot stays plain), a scrub clears both.
void test_the_canary_follows_the_volume_across_a_power_loss() {
    using bootmode::Mode;
    Vol v;
    Canary c{};
    CHECK_EQ(save(v.f, kSession2), true);
    arm_canary(c, kSession2);
    // Short dark (the comeback, SRAM alive): the record on the volume is vouched for.
    CHECK_EQ(canary_matches(c, load(v.f)), true);
    CHECK_EQ(int(boot_mode(Mode::Plain, false, load(v.f), false, canary_matches(c, load(v.f)))), int(Mode::PlusRdCc2));
    // Long dark (a bare unplug, then a wait): SRAM decayed, modelled by a garbage canary.
    const Canary dead{0x11111111u, 0x22222222u, 0x33333333u};
    CHECK_EQ(canary_matches(dead, load(v.f)), false);
    CHECK_EQ(int(boot_mode(Mode::Plain, false, load(v.f), false, canary_matches(dead, load(v.f)))), int(Mode::Plain));
    // The follower scrubs the record on the plain boot: the canary goes with it.
    CHECK_EQ(save(v.f, kNone), true);
    clear_canary(c);
    CHECK_EQ(canary_survived(c), false);
    CHECK_EQ(load(v.f) == kNone, 1);
}

int main() {
    test_a_host_powered_boot_ignores_the_record();
    test_a_sink_downstream_scrubs_the_record();
    test_the_canary();
    test_a_cold_boot_trusts_the_record_only_with_a_live_canary();
    test_the_canary_follows_the_volume_across_a_power_loss();
    test_record_format();
    test_the_rescan_kind();
    test_damage_reads_as_none();
    test_the_boot_mode_decision();
    test_the_comeback_judge();
    test_the_planned_reattach_gate();
    test_only_the_reattach_kind_is_a_planned_reattach();
    test_the_follower();
    test_the_follower_carries_a_runs_cable();
    test_forget_rewrites_what_the_volume_may_have_lost();
    test_save_and_reload();
    test_save_writes_only_on_change();
    test_a_damaged_file_reads_as_none();
    return test_report("test_rd_intent");
}
