// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// On-device storage actions: save the decoded e-marker into the library, save the RAM log, and
// list what is stored. The native e-marker pages (0x080E3000, 0x080E3800) are only ever read.
// Wraps k2::store over the real flash; every call that writes is an explicit
// request (remote `store ...`), nothing here runs on its own. The drive (msc.cpp) polls
// generation() and rebuilds its volume when it changed.
#pragma once
#include <cstdint>
#include <span>
#include <string_view>

#include "k2/pd/emlib.hpp"
#include "k2/pd/emrec.hpp"
#include "k2/pd/store.hpp"

namespace k2::storage {

struct Result {
    store::Err err = store::Err::Ok;
    unsigned slot = 0;       // library number written by save_emarker (low 8 bits)
};

// Why a save_emarker() found nothing to save.
inline constexpr store::Err kNoEmarker = store::Err::Empty;

store::Info info();
store::Brief brief();   // the flags only, no decoded tables: prefer it to info() when only flags are read (a full Info is ~1.5 KB of stack)
std::span<const char> saved_log();   // the flash snapshot in place; empty unless Drive owns the overlay (then it is the SAVEDLOG.TXT mirror)
std::size_t saved_log_len();         // the snapshot length in every mode (0 when none); cheap, no buffer touched
Result save_emarker();            // the cable held by fusb302 into the library (lib_save_current); Err::Empty when none
Result save_log();                // RAM log ring -> flash snapshot
// The e-marker library (k2/pd/emlib.hpp): saved cables, one entry each, numbered 1..9999.
// idempotent: the same cable (and quirks) answers existed = true; quirks are the clone's extra e-marker replies
emlib::Result lib_save(const emrec::Record& rec, const emk_quirk::Quirks& quirks = {});
emlib::Result lib_save_current();                   // the cable identity held by fusb302; Err::Invalid when none
unsigned lib_list(std::span<std::uint16_t> out);    // numbers in ascending order
bool lib_get(std::uint16_t seq, emlib::Entry& out);
bool lib_get(std::uint16_t seq, emlib::Entry& out, emk_quirk::Quirks& quirks);   // + the quirk table (empty when none)
emlib::Err lib_remove(std::uint16_t seq);
emlib::Err lib_rename(std::uint16_t seq, std::string_view name);   // appends a new version of the entry

std::uint8_t bank();              // flash register set used by the last store write (2, 1, 0 = none yet)

// Bumped after every successful write or erase.
std::uint32_t generation();

}  // namespace k2::storage
