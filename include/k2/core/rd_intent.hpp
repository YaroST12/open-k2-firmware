// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The Rd intent marker: what a COLD plain boot needs to know to keep an Rd it would otherwise never present (pure rules, host-tested in
// tests/test_rd_intent.cpp; the store is one small littlefs file).
//
// Why it exists. A K2 that lives on the measured charger's VBUS dies whenever that charger cuts VBUS: a charger's Hard Reset, and
// the VBUS dark of a planned re-attach (the charger reads the cable only at a real attach, and only when an Ra is on CC1 at the
// Rd edge: docs/CHARGER_QUIRKS.md `cable-look-at-attach`). The K2 comes back COLD (a power-on, no backup-register state: the pin
// mark of k2/core/bootmode.hpp survives warm resets only), and a cold plain boot presents no Rd, so the charger sees nothing to
// serve and the K2 stays dead. A flash record outlives the dark. The comeback keeps its Rd from the first write only when the
// record says it was meant to:
//   Session   a self-powered sink was running (its pin): the charger's own Hard Reset brings the K2 back as the same sink.
//   Reattach  the charger test planned this re-attach: the pin, and the cable identity to answer the charger's first look with.
//   Rescan    the Connection page's Refresh cycled CC on a self-powered K2: the pin only, no identity (core/rescan_bridge.hpp).
// An ordinary plain cold boot (no record: HID, a phone, a flash) never presents an Rd: boot_mode() hands back Plain.
//
// Bounds on a stale record (the K2 cannot clear it when a charger is simply unplugged: it dies with the marker in place):
//   * only a cold plain boot reads it (reads_marker): a warm reset is the pin mark's business;
//   * the backup domain (rd_comeback asks session::backup_alive): a cold plain boot trusts the record only when the session
//     record the last boot wrote in the backup registers survived this reset (boot_mode's canary_ok). A marker left by a run
//     the K2 did not come back from (a bare unplug and a wait: the backup registers lost power) is stale. A .noinit SRAM
//     canary did this until 1.0.0-524; SRAM never survives a dark on this board (0 of 32 words after 1 s, 2026-11-11);
//   * the comeback is judged in the first 1.5 s (judge): capabilities seen = vouched; no Rp on any line after kNoRpMs, or no
//     capabilities after kGiveUpMs = released (Rd, Ra, spoof dropped, the record removed). One boot at most carries a stale one;
//   * a vouched Reattach is consumed by the boot that served it: the follower rewrites it as the Session that boot now is, as
//     soon as its contract is Ready (a boot that never gets there leaves the Reattach, which the next comeback judges again);
//   * the follower rewrites it only on a state change, and scrubs it on the first look of any boot that does not pin.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

#include "k2/chargertest/ct_cable.hpp"
#include "k2/core/bootmode.hpp"
#include "k2/core/fs.hpp"
#include "k2/pd/pd.hpp"

namespace k2::rdintent {

// Rescan: the Connection page's Refresh had to cycle CC on a self-powered K2 (the source did not answer a Get_Source_Cap). The cycle
// drops VBUS, the K2 dies and comes back cold. The comeback keeps its Rd (either pin) and lands on the page again. It carries no
// identity, and it is never a charger-test resume (that is the run journal's business, not the marker's).
enum class IntentKind : std::uint8_t { None = 0, Session = 1, Reattach = 2, Rescan = 3 };

struct Intent {
    IntentKind kind = IntentKind::None;
    std::uint8_t pin = 0;            // 1 = Rd on CC1, 2 = Rd on CC2 (SWITCHES0 bit0 / bit1), never both
    ctcable::Choice cable{};         // Reattach only: the identity armed before the Rd edge (Off = keep Rd only)
    constexpr bool operator==(const Intent&) const = default;
};

inline constexpr const char* kPath = "/rd.rec";
inline constexpr std::array<std::uint8_t, 4> kMagic{'K', '2', 'R', 'I'};
inline constexpr std::uint8_t kCheckSalt = 0x5A;
inline constexpr std::size_t kSize = 10;   // "K2RI", kind, pin, cable kind, seq low, seq high, check

constexpr std::uint8_t check_of(std::uint8_t kind, std::uint8_t pin, std::uint8_t ck, std::uint8_t lo, std::uint8_t hi) {
    return static_cast<std::uint8_t>(kind ^ pin ^ ck ^ lo ^ hi ^ kCheckSalt);
}

constexpr std::array<std::uint8_t, kSize> encode(Intent in) {
    in.cable = ctcable::canonical(in.cable);
    const std::uint8_t kind = static_cast<std::uint8_t>(in.kind), ck = static_cast<std::uint8_t>(in.cable.kind);
    const std::uint8_t lo = static_cast<std::uint8_t>(in.cable.seq & 0xFF), hi = static_cast<std::uint8_t>(in.cable.seq >> 8);
    return {kMagic[0], kMagic[1], kMagic[2], kMagic[3], kind, in.pin, ck, lo, hi, check_of(kind, in.pin, ck, lo, hi)};
}

// A missing, damaged or impossible record is no intent. Impossible: an unknown kind, a pin that is not exactly one line, a cable
// on the CC1 pin (the Ra sits on CC1: the Rd and the Ra would share the line), an invalid cable.
constexpr Intent decode(const std::array<std::uint8_t, kSize>& r) {
    for (std::size_t i = 0; i < kMagic.size(); i++) if (r[i] != kMagic[i]) return {};
    if (check_of(r[4], r[5], r[6], r[7], r[8]) != r[9]) return {};
    if (r[4] < static_cast<std::uint8_t>(IntentKind::Session) || r[4] > static_cast<std::uint8_t>(IntentKind::Rescan)) return {};
    if (r[5] != 1 && r[5] != 2) return {};
    if (r[6] > static_cast<std::uint8_t>(ctcable::Kind::Saved)) return {};
    const ctcable::Choice c{static_cast<ctcable::Kind>(r[6]), static_cast<std::uint16_t>(r[7] | (r[8] << 8))};
    if (!ctcable::valid(c)) return {};
    if (c.kind != ctcable::Kind::Off && r[5] != 2) return {};
    if (c.kind != ctcable::Kind::Off && r[4] == static_cast<std::uint8_t>(IntentKind::Rescan)) return {};   // a rescan carries no identity
    return {static_cast<IntentKind>(r[4]), r[5], ctcable::canonical(c)};
}

// Only a cold plain boot looks: a key mode pins its own line, a warm reset keeps (or does not keep) the pin mark's pin.
constexpr bool reads_marker(bootmode::Mode keys, bool warm) { return keys == bootmode::Mode::Plain && !warm; }

// The boot mode the Rd decision runs on: a key held decides; otherwise a cold boot with a record is the sink mode of the record's
// pin (so the driver's key-mode path, which keeps the pin from the first write and never calls it inherited, serves it).
//
// A K2 that a USB host powers (the aux rail near 5 V) has no charger to come back from: the record is a leftover (a failed scrub, a
// reflash, a stale session) and a plain boot there stays passive. Hardware 2026-11-09: a hub-powered, HID-attached K2 with a leftover
// record put Rd on CC2 from its first write, Requested 12 times and collided with the phone behind it. A comeback boot is powered
// by the charger through its own Rd, so its aux rail is dead; the rail is sampled once, early, and only when a record exists.
inline constexpr int kHostAuxMv = 3500;
constexpr bool host_powered(int aux_mv) { return aux_mv >= kHostAuxMv; }

// The boot mode the Rd decision runs on: a key held decides; otherwise a cold boot with a record is the sink mode of the record's
// pin (so the driver's key-mode path, which keeps the pin from the first write and never calls it inherited, serves it). A record
// is trusted only when the backup registers survived this reset (canary_ok, session::backup_alive in rd_comeback): a host never
// comes back from a charger, and neither does a boot after a dark too long for the backup domain.
constexpr bootmode::Mode boot_mode(bootmode::Mode keys, bool warm, const Intent& in, bool host = false, bool canary_ok = true) {
    if (!reads_marker(keys, warm) || in.kind == IntentKind::None || host || !canary_ok) return keys;
    return in.pin == 1 ? bootmode::Mode::OkRdCc1 : in.pin == 2 ? bootmode::Mode::PlusRdCc2 : keys;
}

// The comeback's verdict, asked until it is no longer Wait. ms = since the comeback began; rp_seen = a source's Rp is on a line (the
// driver found the attach); caps_seen = Source_Capabilities arrived (a source that serves us exists).
inline constexpr std::uint32_t kNoRpMs = 500;     // no Rp anywhere by now: nothing to keep the Rd for
inline constexpr std::uint32_t kGiveUpMs = 1500;  // an Rp that never speaks
struct Watch {
    std::uint32_t ms = 0;
    bool rp_seen = false;
    bool caps_seen = false;
};
enum class Verdict : std::uint8_t { Wait, Vouched, Release };
constexpr Verdict judge(const Watch& w) {
    if (w.caps_seen) return Verdict::Vouched;
    if (w.ms >= kGiveUpMs) return Verdict::Release;
    if (w.ms >= kNoRpMs && !w.rp_seen) return Verdict::Release;
    return Verdict::Wait;
}

// Is the source's offer held down by the cable it read? Behaviour, never identity: a charger that claims more power than it
// offers, while no level exceeds 3 A (the plain 3 A cable ceiling), has priced the cable in; it reads the cable again at a real
// attach (docs/CHARGER_QUIRKS.md `cable-look-at-attach`) and a spoofed 5 A identity then lifts the offer. A charger that is just a
// smaller charger (a 30 W brick offering 30 W of 30 W) has nothing to lift.
//
// The claim is the Source_Caps_Ext PDP in whole watts (0 = not asked or not answered). The best offer is the largest whole-watt
// product over the list: fixed V x I, a PPS or AVS range at its top voltage and current. The margin keeps a rounded label from
// firing it: a 65 W charger whose best level is 60 W is not held down, a 120 W charger offering 60 W, and a 100 W one offering
// 63 W, are.
inline constexpr std::uint32_t kClaimMarginW = 10;
inline constexpr std::uint16_t kCableCeilingMa = 3000;

constexpr std::uint32_t pdo_power_w(const pd::Pdo& p) {
    return static_cast<std::uint32_t>(static_cast<std::uint64_t>(p.max_mv) * p.max_ma / 1000000u);
}
// The best whole-watt power any listed PDO offers (0 for no list).
constexpr std::uint32_t best_power_w(const pd::SourceCaps& caps) {
    std::uint32_t best = 0;
    for (std::size_t i = 0; i < caps.pdos.size() && i < caps.num_pdos; ++i) {
        const pd::Pdo& p = caps.pdos[i];
        if (p.type == pd::PdoType::Unknown) continue;
        const std::uint32_t w = pdo_power_w(p);
        if (w > best) best = w;
    }
    return best;
}
// True when any listed PDO offers more than the 3 A cable ceiling (the source's own limit is higher: nothing is held down).
constexpr bool offers_above_ceiling(const pd::SourceCaps& caps) {
    for (std::size_t i = 0; i < caps.pdos.size() && i < caps.num_pdos; ++i)
        if (caps.pdos[i].type != pd::PdoType::Unknown && caps.pdos[i].max_ma > kCableCeilingMa) return true;
    return false;
}
// With the source's own claim in hand: nothing above 3 A, and the claim at least kClaimMarginW above the best offer.
constexpr bool cable_limited(const pd::SourceCaps& caps, std::uint32_t claim_w) {
    if (!caps.valid || caps.num_pdos == 0 || claim_w == 0) return false;
    const std::uint32_t best = best_power_w(caps);
    return !offers_above_ceiling(caps) && best != 0 && claim_w >= best + kClaimMarginW;
}
// Without a claim (the source never answered Get_Source_Cap_Extended): the offer's own shape. The top fixed level is 20 V at
// exactly 3.00 A and nothing offers more than 3 A, the signature of a charger of 60 W or more held at the cable ceiling.
constexpr bool cable_limited_no_claim(const pd::SourceCaps& caps) {
    if (!caps.valid || caps.num_pdos == 0 || offers_above_ceiling(caps)) return false;
    const pd::Pdo* top = nullptr;
    for (std::size_t i = 0; i < caps.pdos.size() && i < caps.num_pdos; ++i) {
        const pd::Pdo& p = caps.pdos[i];
        if (p.type == pd::PdoType::Fixed && (top == nullptr || p.max_mv > top->max_mv)) top = &p;
    }
    return top != nullptr && top->max_mv == 20000 && top->max_ma == kCableCeilingMa;
}
// The planning rule: a known claim decides alone, the fallback only speaks when no claim is in hand.
constexpr bool offer_held_by_cable(const pd::SourceCaps& caps, std::uint32_t claim_w) {
    return claim_w != 0 ? cable_limited(caps, claim_w) : cable_limited_no_claim(caps);
}

// The charger test's planned re-attach (the K2 blinks off once so the charger reads the cable at a real attach) is worth its
// blink only when ALL of these hold, and never otherwise (an Apple-like charger never looks at the cable; a bus-powered K2 loses
// nothing and needs no blink): a spoof cable that claims 5 A (the 6 A rows), the Ra side of the plug (the Rd on CC2, so the Ra
// can sit on CC1), a K2 living on the charger's VBUS, and a source whose offer the cable holds down (offer_held_by_cable).
constexpr bool plan_reattach(ctcable::Choice cable, const ctcable::Facts& facts, std::uint8_t pin, bool self_powered,
                             bool offer_held) {
    if (cable.kind == ctcable::Kind::Off || !ctcable::valid(cable)) return false;
    return facts.max_ma >= 5000 && pin == 2 && self_powered && offer_held;
}

// The marker kind that says the reboot this boot came back from was the charger test's planned re-attach: a run resumed on
// such a boot is that re-attach, not a power loss, and the run journal does not count it as a resume.
constexpr bool planned_reattach(IntentKind k) { return k == IntentKind::Reattach; }

// Keeps the record in step with the boot's own state, in RAM, so the main loop pays nothing per pass and flash is touched on a
// change only. Self-powered here means a pinned Rd (the boot pin, stable across a run's own detach) and no host: a K2 with a USB
// host lives on that host and has nothing to come back from. A pinned K2 waits for its contract before it says anything; once it
// has, a drop of the contract changes nothing (the record is already on the volume when the charger cuts VBUS). The cable of a
// running charger test rides in the session record, so the comeback answers the charger's first look with that identity.
class Follower {
public:
    struct Sample {
        bool pinned = false;
        std::uint8_t pin = 0;
        bool ready = false;     // the sink contract (self-hold or a request) is Ready
        bool host = false;      // a USB host is configured
        ctcable::Choice cable{};   // the spoof identity of the run in progress (Off = none)
        bool sink_downstream = false;   // a sink sits behind the K2 (inline latch, or another sink's Request): not a session
    };
    enum class Act : std::uint8_t { Hold, Apply };

    Act update(const Sample& s) {
        Intent want;
        if (s.pinned && !s.host && !s.sink_downstream) {
            if (!s.ready) return Act::Hold;
            want = {IntentKind::Session, s.pin, {}};
            if (s.pin == 2 && s.cable.kind != ctcable::Kind::Off && ctcable::valid(s.cable)) want.cable = ctcable::canonical(s.cable);
        }
        if (primed_ && want == last_) return Act::Hold;
        primed_ = true;
        last_ = want;
        return Act::Apply;
    }
    const Intent& wanted() const { return last_; }
    // Something else wrote the record (the planned re-attach): the next look writes the follower's state again, change or not.
    void forget() { primed_ = false; }

private:
    bool primed_ = false;
    Intent last_{};
};

inline Intent load(fs::Fs& f) {
    std::array<std::uint8_t, kSize> r{};
    std::size_t n = 0;
    if (f.read_file(kPath, r, n) != fs::Err::Ok || n != r.size()) return {};
    return decode(r);
}

// Writes only on a change (a repeated save reads and returns); None removes the file. True when the volume holds `in` afterwards.
inline bool save(fs::Fs& f, const Intent& in) {
    if (in.kind == IntentKind::None) return !f.exists(kPath) || f.remove(kPath) == fs::Err::Ok;
    const auto r = encode(in);
    std::array<std::uint8_t, kSize> cur{};
    std::size_t n = 0;
    if (f.read_file(kPath, cur, n) == fs::Err::Ok && n == cur.size() && cur == r) return true;
    return f.write_file(kPath, r) == fs::Err::Ok;
}

}  // namespace k2::rdintent
