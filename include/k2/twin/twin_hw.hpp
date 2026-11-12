// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Device twin on the hardware: the live proof's Env over the FUSB302 driver and the library, and the two ways of learning a
// twin from what the driver holds (a finished Identify of a device alone on the K2; a Request sniffed inline). The logic is
// pure and host-tested (k2/twin/live.hpp, learn.hpp, predict.hpp); this file only wires it to the drivers. One run at a time,
// started by the remote (`twin run --confirm`) or the Device twins page, polled from Ui::tick, stopped by BACK, the `stop`
// command or an ownership change (charger test, inline sink).
#pragma once
#include <cstdint>
#include <span>
#include <string_view>

#include "k2/twin/learn.hpp"
#include "k2/twin/library.hpp"
#include "k2/twin/live.hpp"
#include "k2/twin/predict.hpp"
#include "k2/twin/twin.hpp"

namespace k2::twin_hw {

// What the caller (Ui) knows that the driver does not: the clock, the monitor and who else owns the ports.
struct LiveCtx {
    std::uint32_t now = 0;
    int vbus_fast_mv = -1;          // power::vbus_fast_mv(), -1 unknown
    std::uint32_t cap_mv = 0;       // the monitor ceiling (pd::request_cap_mv)
    bool busy = false;              // a charger test or an FC release owns the ports
    bool inline_link = false;       // a real sink sits behind the K2
    bool owners_busy = false;       // a SuperVOOC / QC trigger or PD Stress owns the link (refuses an inline interrogation)
};

// The charger's power claim (Source_Capabilities_Extended PDP) and the cable the K2 read, for twin::predict.
twin::Facts facts_now();
// Prediction against the charger's list now (pd::active_caps()). False: no list, nothing predicted.
bool predict(const twin::Twin& t, twin::Outcome& out);
// Moves whenever a prediction could change: the charger's list or the facts (power claim, cable) behind it. The Device twins
// page repredicts only when this or the library changes.
std::uint32_t predict_stamp();
// The `[TWIN] predict` log line, one per prediction shown to a user (the remote's and the shell's).
void log_predict(std::uint8_t seq, const twin::Outcome& o);

// ---- live proof ----
// A charger that has not offered a list is woken with the K2's own Rd first (twin::live::may_wake), only after `confirmed`. The Rd
// a finished run presented is released at the run's end, unless `keep_wake` (the Device twins page): then it stays until
// wake_release(), so the charger keeps listing while the page shows the result.
twin::live::Refusal live_start(std::span<const std::uint8_t> seqs, bool confirmed, const LiveCtx& ctx, bool keep_wake = false);
// True when a live proof could start now without a list: a charger waits on the plug (or holds VBUS without listing) and no
// sink or phone is behind the K2. Cheap; the page asks every tick.
bool can_wake(const LiveCtx& ctx);
// Gives back the Rd a finished run left to the caller (leaving the page). Nothing to do when none is held.
void wake_release();
void live_poll(const LiveCtx& ctx);
void live_cancel();                // BACK and `stop`: 5 V asked, the K2's own Sink_Capabilities back, the run over
bool live_running();
const twin::live::Runner& live();  // slots, current, counts, reason

// ---- learning ----
enum class Learn : std::uint8_t {
    Ok,
    NotReady,      // no finished Identify of the partner on the port (run Identify with the device alone first)
    NotInline,     // sniffing needs a real device behind the K2: the K2 itself is the sink here, its Request is not a device's
    NoContract,    // nothing sniffed to learn from: no Request with PS_RDY inside the charger's list
    Store,         // the library said no (see Learned::store.err)
};
constexpr const char* name(Learn l) {
    switch (l) {
        case Learn::Ok: return "ok";
        case Learn::NotReady: return "no identify";
        case Learn::NotInline: return "not inline";
        case Learn::NoContract: return "nothing sniffed";
        case Learn::Store: return "not stored";
    }
    return "?";
}
struct Learned {
    Learn how = Learn::NotReady;
    twin::Result store{};
    twin::Twin twin{};    // what the run learned (the saved record may hold more after the merge)
};
// `name` empty = the auto name. Saves into the library (merging by VID:PID) unless `save` is false (a dry look).
Learned learn_from_identify(std::string_view name, bool save = true);
Learned learn_from_sniff(std::string_view name, bool save = true);

// ---- learning inline: the device asked on its own charger's link (fusb302::interrogate_*, k2/pd/pd_interrogate.hpp) ----
// The K2 resets the link when it has no fresh sniffed contract, asks the device as its source, resets the link again and learns
// what the device answered (twin::from_interrogated). Each reset drops VBUS, so it needs `confirmed`. One run at a time; Ui::tick
// calls inline_poll(), which learns (and saves unless `save` is false) once the run is over.
struct InlineLearn {
    bool pending = false;   // a run inline_start started, not learned from yet
    bool done = false;      // the latest run was learned from: `result`
    bool save = true;
    Learned result{};       // how = NoContract when the run read nothing to learn from
};
pd::IqRefusal inline_start(std::string_view name, bool save, bool confirmed, const LiveCtx& ctx);
void inline_poll();
void inline_cancel();        // BACK / `stop`: the run ends (its final reset still goes out); nothing is learned from it
bool inline_running();       // the run, or its learning, is not over
const InlineLearn& inline_learn();
const pd::IqStatus& inline_status();

}  // namespace k2::twin_hw
