// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// E-marker emulation ("Spoof"): answer an SOP' Discover Identity request with a saved record, so
// a source sees a cable with that identity. The frame logic below is pure (host-tested); arm() / disarm() / replies()
// hook it to the FUSB302 receive path through fusb302::set_sop_prime_hook().
//
// What it needs from the chip: Auto-CRC on, so the FUSB302 sends the GoodCRC for the request
// itself (inside the 195 us the bit-banged I2C could never meet), and SOP' receive enabled
// (CONTROL1 ENSOP1, which this driver always sets). A live sink on the line (inline mode) turns
// Auto-CRC off, so arming is refused then. The source only asks a cable that it believes is
// e-marked (Ra on the second CC); see docs/ARCHITECTURE.md.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "k2/pd/emk_quirk.hpp"
#include "k2/pd/emrec.hpp"

namespace k2::emk_spoof {

// ---- pure part ----

inline constexpr std::uint8_t kMsgVendorDefined = 0x0F;
inline constexpr std::size_t kVdoBytes = 20;                          // VDM header + 4 identity VDOs
inline constexpr std::size_t kFrameSize = 4 + 1 + 2 + kVdoBytes + 3;  // 30 bytes of FIFO tokens

// True for a non-extended Vendor_Defined_Message that carries a Structured VDM header with
// SVID 0xFF00, command type REQ (0) and command Discover Identity (1).
constexpr bool is_discover_identity_req(std::uint16_t header, std::span<const std::uint8_t> payload) {
    if ((header & 0x8000) != 0) return false;                         // extended
    if ((header & 0x1F) != kMsgVendorDefined || ((header >> 12) & 7) < 1 || payload.size() < 4) return false;
    const std::uint32_t vdm = payload[0] | (payload[1] << 8) | (payload[2] << 16) | (std::uint32_t(payload[3]) << 24);
    return (vdm & 0xFFFF8000u) == 0xFF008000u && ((vdm >> 6) & 3) == 0 && (vdm & 0x1F) == 1;
}

// The cable's reply header: 5 data objects, cable-plug bit set, the record's spec revision, MessageID.
constexpr std::uint16_t reply_header(const emrec::Record& rec, std::uint8_t msg_id) {
    return static_cast<std::uint16_t>(kMsgVendorDefined | ((rec[0x14] & 3u) << 6) | (1u << 8) |
                                      ((msg_id & 7u) << 9) | (5u << 12));
}

// FIFO token stream for the reply: SOP1 SOP1 SOP3 SOP3, PACKSYM|22, header, the record's five VDOs
// (bytes 0x15..0x28), JAM_CRC, EOP, TXOFF. The caller writes it to the FIFO and sets TX_START.
constexpr std::array<std::uint8_t, kFrameSize> build_reply(const emrec::Record& rec, std::uint8_t msg_id) {
    std::array<std::uint8_t, kFrameSize> f{};
    std::size_t n = 0;
    for (std::uint8_t b : {0x12, 0x12, 0x1B, 0x1B}) f[n++] = b;
    f[n++] = 0x80 | (2 + kVdoBytes);
    const std::uint16_t h = reply_header(rec, msg_id);
    f[n++] = static_cast<std::uint8_t>(h);
    f[n++] = static_cast<std::uint8_t>(h >> 8);
    for (std::size_t i = 0; i < kVdoBytes; i++) f[n++] = rec[0x15 + i];
    f[n++] = 0xFF;   // JAM_CRC
    f[n++] = 0x14;   // EOP
    f[n++] = 0xFE;   // TXOFF
    return f;
}

// True for an SOP' Soft_Reset (control message 13, no data objects, not extended). A source that
// has just become VCONN source sends it to the cable before it talks to it (R3.2 6.1.3.2).
constexpr bool is_soft_reset(std::uint16_t header) {
    return (header & 0x8000) == 0 && (header & 0x1F) == 13 && ((header >> 12) & 7) == 0;
}

// The cable's Accept: control message 3, cable-plug bit, the record's spec revision, MessageID.
// Tokens: SOP1 SOP1 SOP3 SOP3, PACKSYM|2, header, JAM_CRC, EOP, TXOFF.
constexpr std::array<std::uint8_t, 10> build_accept(const emrec::Record& rec, std::uint8_t msg_id) {
    const std::uint16_t h = static_cast<std::uint16_t>(3 | ((rec[0x14] & 3u) << 6) | (1u << 8) | ((msg_id & 7u) << 9));
    return {0x12, 0x12, 0x1B, 0x1B, 0x80 | 2, static_cast<std::uint8_t>(h), static_cast<std::uint8_t>(h >> 8), 0xFF, 0x14, 0xFE};
}

// ---- responder (src/pd/emk_respond.cpp, pure) ----

// FIFO tokens of one reply: sync, PACKSYM, header, objects, JAM_CRC, EOP, TXOFF (at most 6 objects after the header).
inline constexpr std::size_t kMaxFrame = 4 + 1 + 2 + 28 + 3;
struct Frame {
    std::array<std::uint8_t, kMaxFrame> t{};
    std::uint8_t n = 0;
    std::span<const std::uint8_t> tokens() const { return {t.data(), n}; }
};

enum class Match : std::uint8_t {
    None,       // no reply (see Outcome::unmatched)
    Identity,   // the Discover Identity reply from the record
    Row,        // a quirk row answered
    Policy,     // the identity's unmatched policy answered (NAK or Not_Supported)
};

struct Outcome {
    Match how = Match::None;
    std::uint8_t row = 0;          // the row index for Match::Row
    bool counter = true;           // the reply used the cable's MessageID counter (the caller advances it on TX success)
    bool unmatched = false;        // nobody answered a message that should be logged (not a GoodCRC)
    Frame frame;
};

// What a fake cable with identity `rec` and `quirks` answers to a received SOP'/SOP'' message (`sop` is
// emk_quirk::kSopP or kSopPP): quirk rows in order (first match wins), then Discover Identity from the record, then
// the unmatched policy for structured requests. `msg_id` is the cable's MessageID counter.
Outcome respond(const emrec::Record& rec, const emk_quirk::Quirks& quirks, std::uint8_t sop, std::uint16_t header,
                std::span<const std::uint8_t> payload, std::uint8_t msg_id);

// ---- hardware part (src/pd/emk_spoof.cpp) ----

enum class Err : std::uint8_t { Ok, Invalid, Inline };

// A record and quirk table arm() / arm_run() accept (what Err::Invalid means).
inline bool usable(const emrec::Record& rec, const emk_quirk::Quirks& quirks) {
    return emrec::valid(rec) && quirks.count <= emk_quirk::kMaxRows && (quirks.flags & ~emk_quirk::kQFlagsKnown) == 0;
}
// Invalid: not a valid record or quirk table; Inline: a live sink is attached
Err arm(const emrec::Record& rec, const emk_quirk::Quirks& quirks = {});
// The same, for the inline interrogation only (twin_hw::inline_start_cable): works with a device inline, where the answers
// are the run's own window (fusb302 opens it around the charger's cable query). The run disarms it with disarm() when it ends.
Err arm_run(const emrec::Record& rec, const emk_quirk::Quirks& quirks = {});
void disarm();
bool active();
bool matches(const emrec::Record&, const emk_quirk::Quirks& = {});   // actual armed identity, not a page/remote's remembered selection
std::uint32_t generation();         // changes only on explicit arm/disarm
std::uint32_t replies();             // replies sent since arm() (identity, rows and policy)
std::uint32_t requests();            // SOP' Discover Identity requests seen since arm()
std::uint32_t unmatched();           // messages seen since arm() that nothing answered
const emk_quirk::SeenLog& seen();    // the last of them, for the remote

// The armed clone's own record and quirk table, lent to `emarker learn` as the home of the clone it learns (RAM is
// tight; the two are never needed at once: learn is refused while armed). arm() overwrites them, so whoever borrows
// them checks generation() before trusting the content again. Only touch them while !active().
emrec::Record& scratch_record();
emk_quirk::Quirks& scratch_quirks();

}  // namespace k2::emk_spoof
