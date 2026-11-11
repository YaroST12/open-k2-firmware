// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// What this boot decided and what the CC pins and the supply did in its first seconds, as data (the pure part) and as
// the compact text the LCD shows as a QR code and the remote command `bootinfo` returns.
//
// Why it exists: a charger-powered K2 with no USB has no log, and the K2LOG lines of the first seconds are gone before any
// host attaches. The decision (PLUS/OK held at reset -> which Rd, whether it moved) must be readable after the fact, from
// a photo of the screen or from one USB query. Collected from the drivers' own APIs by src/core/bootinfo_hw.cpp; formatted here.
//
// Text format "K2B1" (one line, ASCII, at most kQrMax characters; fields in this order, space separated):
//   K2B1 fw=<version> g=<git describe, "-dirty" when the tree was> p=<remote proto number>   (the firmware identity, same
//        strings as PING, always first)
//        t=<uptime ms> vb=<VBUS mV> c1=<CC1 mV> c2=<CC2 mV> pl=<PLUS held now 0|1>
//        m=<plain|ok|plus> k=<key mask hex, bootmode::k*: 1 BACK, 2 MINUS, 4 OK, 8 PLUS> pa=<pin levels hex: 1 PA9, 2 PA13, 4 PB14, 8 PA14;
//        1 = high = released> rf=<reset flags hex> w=<warm reset 0|1> pin=<initial Rd,now,boot,inherited> mk=<pin mark hex|->
//        ra=<Ra on CC1 now 0|1>:<boot decision: -|arm|plain|noid|refused|cable|noread|fail>
//        ri=<the Rd intent marker as this boot read it: - not read (warm or key boot) | 0 no record | <kind s|a|r><pin><canary y|n><host y|n>>
//        sr=<words of the .noinit SRAM probe that survived the reset, 0..32> bp=<backup-register session boots>:<previous record 0|1>
//        mv=<ms>:<from>><to>:<vbus>:<cc1>:<cc2>|- sw=<readback hex>@<ms>,...|- n=<ms><s|c><hex>,...[+<more>]
// Rd pin values are the FUSB302 pull-down bits (1 = CC1, 2 = CC2, 0 = none). `mv` is the one-time move of the single Rd (the
// rule's inputs at that moment, vbus -1 = no power monitor answer). `sw` are SWITCHES0 read back from the chip at ~50 ms,
// ~600 ms and ~3 s. `n` are the first writes after reset: s = SWITCHES0 value written, c = CC switch routing (bit0 PC14, bit1 PC15,
// bit2 PC13); a trailing +N counts the notes that did not fit.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "k2/core/bootmode.hpp"

namespace k2::bootinfo {

inline constexpr std::size_t kQrMax = 240;            // QR payload cap: version 10 at level L holds 271 bytes
inline constexpr unsigned kMaxNotes = 27;             // FUSB302 boot notes (writes) plus the three readbacks

// Pin-level bits of Snapshot::pa (1 = pin high).
inline constexpr std::uint8_t kPaOk = 1, kPaPlus = 2, kPaBack = 4, kPaMinus = 8;

struct Note {
    std::uint16_t ms = 0;
    std::uint8_t kind = 0;   // 0 SWITCHES0 write, 1 CC routing, 2 SWITCHES0 read back from the chip
    std::uint8_t val = 0;
};

struct Move {
    bool moved = false;
    std::uint16_t ms = 0;
    std::uint8_t from = 0, to = 0;       // Rd pin bits before / after
    std::int32_t vbus_mv = -1;           // the rule's inputs; -1 = the power monitor had not answered
    std::uint16_t cc1_mv = 0, cc2_mv = 0;
};

struct Snapshot {
    char fw[32] = {};                    // firmware version, as PING reports it ("1.0.0-435-26b245ac", "-dirty" when dirty; 31 characters fit)
    char git[32] = {};                   // raw build id as PING reports it: `git describe --always --dirty`, so "...-dirty" when dirty
    std::uint8_t proto = 0;              // remote protocol number (PING's proto byte)
    bootmode::Mode mode = bootmode::Mode::Plain;
    std::uint8_t keys = 0;               // bootmode::kBack | kOk | kPlus seen at reset
    std::uint8_t pa = 0;                 // raw pin levels at that moment (kPa*)
    std::uint32_t reset_flags = 0;
    bool warm = false;
    bool mark_read = false;              // the pin mark was consulted (plain boot after a warm reset)
    std::uint16_t mark = 0;
    std::uint8_t pin_initial = 0, pin_now = 0, pin_boot = 0;
    bool inherited = false;
    bool ra_on = false;                  // PC13 high: the Ra on CC1 (a cable identity is armed for the source's discovery)
    std::uint8_t ra_why = 0;             // bootra::Verdict of this boot's decision (0 = not made)
    bool ri_looked = false;              // rdcomeback::BootLook: the marker was read (cold plain boot)
    char ri_kind = 0;                    // 's' session, 'a' reattach, 'r' rescan, 0 = no record
    std::uint8_t ri_pin = 0;
    bool ri_canary = false, ri_host = false;
    std::uint8_t sram_words = 0;         // of rdcomeback::kProbeWords
    std::uint16_t bp_boots = 0;          // k2::session::boots()
    bool bp_prev = false;                // the backup registers held the previous boot's record
    Move move;
    std::uint8_t note_n = 0;             // entries of `notes` in use (kinds 0 and 1 first, then the readbacks)
    std::array<Note, kMaxNotes> notes{};
    std::uint32_t t_ms = 0;              // live values
    std::uint16_t vbus_mv = 0, cc1_mv = 0, cc2_mv = 0;
    bool plus_now = false;
    std::uint16_t samples = 0;           // first-seconds power samples kept for `bootinfo` sample pages
};

// The one-line text (see the format above) into `out`; returns its length (NUL terminated, at most out.size() - 1 and
// never more than kQrMax). Notes that do not fit are replaced by "+N".
std::size_t format(const Snapshot& s, std::span<char> out);

// A parsed field of a K2B1 line, for tests and tools: the value of "key=" up to the next space; empty when absent.
std::string_view field(std::string_view text, std::string_view key);

// One power sample of the first seconds (pure carrier for the remote reply; ms is nominal, index * 50).
struct PowerSample {
    std::uint16_t ms = 0, vbus_mv = 0, cc1_mv = 0, cc2_mv = 0, aux_mv = 0;
    bool plus = false;
};

}  // namespace k2::bootinfo
