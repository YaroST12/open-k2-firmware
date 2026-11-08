// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Charger-test journal bound to the real flash (src/chargertest/charger_journal.cpp). Every call runs from the main loop
// (never from an ISR). A write is a handful of half-word programs, a section file a flash erase or two: the module never
// does more than ONE file per pump() call, and nothing here loops over several.
#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
#include "k2/chargertest/charger_journal.hpp"

namespace k2::charger_journal::hw {

void mount();                                  // names the volume in the log
// Replays the journal into `into` (reset first); true = an unfinished run exists, and the journal then mirrors it (later changes
// are written as changes). A journal in an older layout is dropped here (named in the log); a run whose section does not match
// its header is not resumable (named in the log). The Replay is ~2.8 KB, so the module keeps none: the caller supplies the
// storage for as long as it needs the steps (boot_restore leases the scratch arena for it).
bool load(Replay& into);

// Run start is a polled job: a write holds the main loop (and the PD engine) for 5 to 250 ms, so a plan's worth (57 rows took
// 1.5 to 2.2 s as one call on 2026-11-08 and the source reset the link) is spread over pump() calls, one file each.
// arm_start() only notes the work; working() says whether more is to come; cancel() drops it (a run that ends first).
void arm_start();
bool working();
void cancel();

// A row's result changed / the plan changed shape: only noted (a hash pass, no flash). The section files concerned are
// rewritten by later pump() calls, one per call.
void note(std::span<const charger_test::Step>, std::size_t index);
void note_plan(std::span<const charger_test::Step>);
// Something is still to be written (an arm, a section file, the header that names them). The Runner does not request above
// 5 V while this is true.
bool pending();
// The same for the plan's shape only (an arm, a replan's section files, the header): a result that is still to be written does
// not count. The Runner's phase marker depends on this alone.
bool plan_pending();
// At most ONE file: the arm's next piece, else the first dirty section, else the header.
void pump(std::uint32_t run_id, const charger_test::Options&, const charger_test::Evidence&,
          std::span<const charger_test::Step>);

void phase(Phase, std::span<const charger_test::Step>, std::size_t index);
void resumed(std::uint8_t count);
void meta(const RunMeta&);                     // who started the run: units, unattended, cable identity
void attempt(std::size_t index, std::uint8_t count);  // `index` = a unit; a power loss inside one of its steps was seen `count` times
void finish(std::uint8_t state, std::uint8_t reason);

}  // namespace k2::charger_journal::hw
