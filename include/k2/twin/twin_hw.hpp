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
};

// The charger's power claim (Source_Capabilities_Extended PDP) and the cable the K2 read, for twin::predict.
twin::Facts facts_now();
// Prediction against the charger's list now (pd::active_caps()). False: no list, nothing predicted.
bool predict(const twin::Twin& t, twin::Outcome& out);
// The `[TWIN] predict` log line, one per prediction shown to a user (the remote's and the shell's).
void log_predict(std::uint8_t seq, const twin::Outcome& o);

// ---- live proof ----
twin::live::Refusal live_start(std::span<const std::uint8_t> seqs, bool confirmed, const LiveCtx& ctx);
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

}  // namespace k2::twin_hw
