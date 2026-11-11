// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Protocol lab command grammar (pure, host-tested): the data-driven table behind the shell's
// accp / qc2 / qc3 / qc3+ / afc / fcp / scp / vooc / ufcs / vivo / dpdm / vbus / pd / cc / lcd families.
// parse() turns the tokens of one line into a Cmd; firmware glue (src/core/shell_hw.cpp) runs it.
//
// Grammar:  <family> <verb> [positional...] [--option value] [--flag] [confirm]
// Positionals are decimal or 0x hex. Options and flags may stand anywhere after the verb.
// Safety (the confirm rule, data-driven by Entry::raise): anything that can raise VBUS above 5 V
// needs `--confirm` or a trailing `confirm`; mV targets are capped at kMaxMv.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace k2::lab {

inline constexpr int kMaxMv = 36000;
inline constexpr std::size_t kMaxBytes = 32;   // `raw <hex>` payload

enum class Id : std::uint8_t {
    AccpPing, AccpRead, AccpWrite, AccpDump,
    Qc2, Qc3Set, Qc3Up, Qc3Down, Qc3pSet, Qc3pUp, Qc3pDown,
    Afc, Fcp, Scp,
    VoocWake, VoocListen, VoocAnswer, VoocSniff,
    UfcsPing, UfcsCaps, UfcsRequest, UfcsRaw,
    UfcsSet, UfcsShow, UfcsReset, UfcsProbe, UfcsResult,
    VivoRaw, VivoRequest,
    Dpdm, CcRp, CcRd, Vbus,
    PdCaps, PdRequest, PdPps, PdAvs, PdEpr, PdVdm, PdGet, PdReread, PdHardReset, PdSoftReset, PdRaw,
    LcdShow, LcdSpi,
    PdStressStart, PdStressStatus, PdStressResult, PdStressStop,
    Count
};

// Every numeric option; 0 means "engine default".
struct Opts {
    std::uint32_t retries = 0, timeout_ms = 0, hs_ms = 0, hold_ms = 0, pulse_us = 0, gap_us = 0,
                  frame_gap_us = 0, watch_ms = 0, sop = 0;
    // pdstress start only (0 = the tool's default): sequence mask, cycle scale in percent, pace, time cap in s, transition cap
    std::uint32_t mask = 0, cycles = 0, pace = 0, max_s = 0, max_trans = 0;
    bool confirm = false, hold = false, release = false, json = false, raw_log = false;
};

struct Cmd {
    Id id{};
    std::array<std::uint32_t, 4> a{};      // numeric positionals (dpdm: dpdm::Drive of D+ and D-; ufcs set: value, field index)
    std::size_t na = 0;
    std::string_view word;                 // word positional (pd get <msg>, pd vdm <what>, ufcs set <field>)
    std::array<std::uint8_t, kMaxBytes> bytes{};   // hex positional
    std::size_t nbytes = 0;
    Opts o;
};

// nullptr when parsed into `out`, else a short reason. `t[0]` is the family word.
const char* parse(const std::string_view* t, std::size_t n, Cmd& out);

// True when the family word belongs to the lab table.
bool is_family(std::string_view w);
// "fam.verb" text of an id, for output lines ("accp.read").
std::string_view name(Id id);

}  // namespace k2::lab
