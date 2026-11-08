// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Firmware glue of `emarker learn`: runs the pure learn plan (k2/pd/emk_learn.hpp) against the cable in the bottom
// port through the FUSB302 cable session, keeps the last result in RAM and saves it into the cable library.
#pragma once
#include <cstdint>

#include "k2/pd/emk_learn.hpp"
#include "k2/pd/emlib.hpp"

namespace k2::emk_learn_hw {

enum class Err : std::uint8_t {
    Ok,         // the plan ran (see last()->outcome: Learned, or Aborted when the link dropped mid-run)
    NoCable,    // the scan found no e-marked cable (or an inline sink refused it): last() holds a NoIdentity result
    Busy,       // a spoof is armed: nothing ran, last() is unchanged
};

// Runs the plan and replaces last(). Blocks the caller for the scan and every exchange (seconds with a sweep); the
// caller must have checked that no PD tool is active. `confirmed`: the inline-scan confirmation, as read_emarker.
Err run(const emk_learn::Options& opt, bool confirmed);

// The last run's result, null before the first run and after a spoof has been armed since (the learned record and
// quirk table share the spoof's buffers: save a result before arming).
const emk_learn::Result* last();

// Saves the last result (identity record + quirk table) into the library. emlib::Err::Invalid when nothing was
// learned (no run yet, or the outcome is not Learned).
emlib::Result save();

}  // namespace k2::emk_learn_hw
