// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The cable identity a charger-test run presents ("Cable identity" dialog at START, Cmd::ChargerCable): pure values,
// shared by the page, the remote protocol and the host tests.
//
// Off          no spoof and no Ra: today's behaviour, a spoof the user armed elsewhere stays as it is.
// Builtin      a built-in passive cable, `seq` = its emrec::builtin_cable() id (1, 2 or 4, see kDialogIds), answered on SOP'.
//              Seq 0 is the older spelling of the default built-in (kBuiltinKind), seq 3 the retired "OnePlus 100W (mag)"
//              (now id 4); canonical() maps both.
// Saved        a clone from the e-marker library (`seq` = its library id).
// Any identity but Off also turns the Ra mode on for the run (PC13 high = an Ra on CC1, Rd on CC2 alone, k2/pd/emk_ra.hpp).
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

#include "k2/pd/emrec.hpp"

namespace k2::ctcable {

enum class Kind : uint8_t { Off = 0, Builtin = 1, Saved = 2 };

constexpr unsigned kBuiltinKind = 1;                       // the default built-in: the hardware-proven EPR 50 V / 5 A cable
// The dialog / Spoof list shows the built-in ids 1, 2 and 4 (id 0, generic, is remote only; id 3 is retired, see emrec.hpp).
constexpr unsigned kDialogBuiltins = 3;
constexpr unsigned kDialogIds[kDialogBuiltins] = {1, 2, emrec::kOnePlusId};
static_assert(kDialogBuiltins + 2 == emrec::kBuiltinCount);
constexpr unsigned dialog_id(unsigned row) { return kDialogIds[row < kDialogBuiltins ? row : 0]; }   // built-in id of dialog row `row` (0-based)
constexpr int dialog_row(unsigned id) {   // 0-based dialog row of built-in `id` (a retired id reads as its successor); not in the list: row 0
    id = emrec::canonical_builtin_id(id);
    for (unsigned r = 0; r < kDialogBuiltins; ++r) if (kDialogIds[r] == id) return static_cast<int>(r);
    return 0;
}
constexpr size_t kNameLen = 24;                            // name field of the remote reply (NUL padded)

struct Choice {
    Kind kind = Kind::Off;
    uint16_t seq = 0;   // library id, Saved only
    constexpr bool operator==(const Choice&) const = default;
};

constexpr bool valid(Choice c) {
    if (c.kind == Kind::Builtin) return c.seq < emrec::kBuiltinCount;
    return c.kind == Kind::Off ? c.seq == 0 : c.kind == Kind::Saved && c.seq != 0;
}
constexpr Choice canonical(Choice c) {
    if (c.kind == Kind::Builtin && c.seq == 0) c.seq = kBuiltinKind;
    if (c.kind == Kind::Builtin && c.seq == emrec::kRetiredMagId) c.seq = emrec::kOnePlusId;   // stored / remote 3 = OnePlus 100W
    return c;
}

// What the report card records of the identity a run presented: the e-marker's VID / PID (ID header VDO bits 15:0,
// product VDO bits 31:16) and the limits its cable VDO claims (current bits 6:5: 1 = 3 A, 2 = 5 A; voltage bits 10:9:
// 20 / 30 / 40 / 50 V; an unknown code counts as the 3 A / 20 V default).
struct Facts {
    uint16_t vid = 0, pid = 0, max_mv = 20000, max_ma = 3000;
};
inline Facts facts(const emrec::Record& rec) {
    Facts f;
    f.vid = static_cast<uint16_t>(emrec::vdo(rec, 1) & 0xFFFF);
    f.pid = static_cast<uint16_t>(emrec::vdo(rec, 3) >> 16);
    const uint32_t cable = emrec::vdo(rec, 4);
    f.max_mv = static_cast<uint16_t>(20000 + 10000 * ((cable >> 9) & 3));
    f.max_ma = ((cable >> 5) & 3) == 2 ? 5000 : 3000;
    return f;
}

// Cmd::ChargerCable reply: the choice the next START dialog starts on, the identity of the current / last run, and that
// identity's name (empty for Off).
struct Info {
    Choice choice, run;
    std::array<char, kNameLen> run_name{};
};

}  // namespace k2::ctcable
