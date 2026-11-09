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
// An ordinary plain cold boot (no record: HID, a phone, a flash) never presents an Rd: boot_mode() hands back Plain.
//
// Bounds on a stale record (the K2 cannot clear it when a charger is simply unplugged: it dies with the marker in place):
//   * only a cold plain boot reads it (reads_marker): a warm reset is the pin mark's business;
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

namespace k2::rdintent {

enum class IntentKind : std::uint8_t { None = 0, Session = 1, Reattach = 2 };

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
    if (r[4] != static_cast<std::uint8_t>(IntentKind::Session) && r[4] != static_cast<std::uint8_t>(IntentKind::Reattach)) return {};
    if (r[5] != 1 && r[5] != 2) return {};
    if (r[6] > static_cast<std::uint8_t>(ctcable::Kind::Saved)) return {};
    const ctcable::Choice c{static_cast<ctcable::Kind>(r[6]), static_cast<std::uint16_t>(r[7] | (r[8] << 8))};
    if (!ctcable::valid(c)) return {};
    if (c.kind != ctcable::Kind::Off && r[5] != 2) return {};
    return {static_cast<IntentKind>(r[4]), r[5], ctcable::canonical(c)};
}

// Only a cold plain boot looks: a key mode pins its own line, a warm reset keeps (or does not keep) the pin mark's pin.
constexpr bool reads_marker(bootmode::Mode keys, bool warm) { return keys == bootmode::Mode::Plain && !warm; }

// The boot mode the Rd decision runs on: a key held decides; otherwise a cold boot with a record is the sink mode of the record's
// pin (so the driver's key-mode path, which keeps the pin from the first write and never calls it inherited, serves it).
constexpr bootmode::Mode boot_mode(bootmode::Mode keys, bool warm, const Intent& in) {
    if (!reads_marker(keys, warm) || in.kind == IntentKind::None) return keys;
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

// The charger test's planned re-attach (the K2 blinks off once so the charger reads the cable at a real attach) is worth its
// blink only when ALL of these hold, and never otherwise (an Apple-like charger never looks at the cable; a bus-powered K2 loses
// nothing and needs no blink): a spoof cable that claims 5 A (the 6 A rows), the Ra side of the plug (the Rd on CC2, so the Ra
// can sit on CC1), a K2 living on the charger's VBUS, and a source that shows the Xiaomi pattern (lists SVID 0x2717).
constexpr bool plan_reattach(ctcable::Choice cable, const ctcable::Facts& facts, std::uint8_t pin, bool self_powered,
                             bool xiaomi_source) {
    if (cable.kind == ctcable::Kind::Off || !ctcable::valid(cable)) return false;
    return facts.max_ma >= 5000 && pin == 2 && self_powered && xiaomi_source;
}

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
    };
    enum class Act : std::uint8_t { Hold, Apply };

    Act update(const Sample& s) {
        Intent want;
        if (s.pinned && !s.host) {
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
