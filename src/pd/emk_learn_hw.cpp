// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#include "k2/pd/emk_learn_hw.hpp"

#include "k2/pd/emk_spoof.hpp"
#include "k2/drivers/fusb302.hpp"
#include "k2/core/log.hpp"
#include "k2/core/storage.hpp"

namespace k2::emk_learn_hw {
namespace {

// A cable answers a request within tVDMSenderResponse (up to 30 ms); a silent probe costs exactly this.
constexpr std::uint32_t kReplyTimeoutMs = 32;

// The learned record and quirk table live in the spoof's buffers (see emk_spoof::scratch_*); the result itself is
// only the packed transcript, ~300 bytes.
emk_learn::Result g_result{emk_spoof::scratch_record(), emk_spoof::scratch_quirks()};
bool g_have = false;
std::uint32_t g_spoof_generation = 0;   // emk_spoof::generation() when the result was made

struct Job {
    const emk_learn::Options* opt;
    bool done = false;
};

bool exchange(void*, const emk_learn::Request& req, emk_learn::Reply& rep) {
    return fusb302::cable_exchange(req, rep, kReplyTimeoutMs);
}

// How the scan ran and how many variants the chip acknowledged (a result of "nothing found" carries them too).
void note_read() {
    const emk_attached::ReadInfo ri = fusb302::last_cable_read();
    g_result.read_mode = static_cast<std::uint8_t>(ri.mode);
    g_result.read_acks = ri.acks;
    g_result.pull_down = static_cast<std::uint8_t>(fusb302::pull_down());
}

void body(void* ctx) {
    auto& job = *static_cast<Job*>(ctx);
    g_result.answered = fusb302::cable_answered();   // the scan that opened this session found the cable
    note_read();
    emk_learn::learn(&exchange, nullptr, *job.opt, g_result);
    job.done = true;
}

}  // namespace

Err run(const emk_learn::Options& opt, bool confirmed) {
    if (emk_spoof::active()) return Err::Busy;   // the clone's buffers are the armed spoof's
    // Reset in place; learn() resets the same things when the plan runs (this covers "no cable found").
    g_result.outcome = emk_learn::Outcome::NoIdentity;
    g_result.rec = {};
    g_result.quirks = {};
    g_result.answered = {};
    g_result.read_mode = g_result.read_acks = g_result.pull_down = 0;
    g_result.svids = g_result.probes = g_result.silent = 0;
    g_result.truncated = false;
    g_result.clear_log();
    g_have = true;
    g_spoof_generation = emk_spoof::generation();
    Job job{&opt};
    K2LOG("[EMK] learn start seeds=%u sweep=%u\n", static_cast<unsigned>(opt.nseeds), static_cast<unsigned>(opt.sweep));
    fusb302::cable_session(confirmed, &body, &job);
    if (!job.done) {
        note_read();
        if (fusb302::last_cable_read().acked_no_answer(false)) g_result.outcome = emk_learn::Outcome::AckedNoAnswer;
        K2LOG("[EMK] learn: no e-marked cable (nothing found or the scan was refused)\n");
        return Err::NoCable;
    }
    K2LOG("[EMK] learn done outcome=%u svids=%u probes=%u silent=%u rows=%u unmatched=%u\n",
          static_cast<unsigned>(g_result.outcome), static_cast<unsigned>(g_result.svids), static_cast<unsigned>(g_result.probes),
          static_cast<unsigned>(g_result.silent), static_cast<unsigned>(g_result.quirks.count),
          static_cast<unsigned>(g_result.quirks.unmatched));
    for (unsigned i = 0; i < g_result.nlog; i++) {
        const auto x = g_result.entry(i);
        K2LOG("[EMK] learn #%u step=%u req0=0x%08X got=%u hdr=0x%04X rep0=0x%08X rep1=0x%08X rep2=0x%08X\n", i,
              static_cast<unsigned>(x.step), static_cast<unsigned>(x.req.w[0]), static_cast<unsigned>(x.rep.got),
              static_cast<unsigned>(x.rep.header), static_cast<unsigned>(x.rep.w[0]), static_cast<unsigned>(x.rep.w[1]),
              static_cast<unsigned>(x.rep.w[2]));
    }
    return Err::Ok;
}

// Arming a spoof afterwards overwrote the borrowed record and table: the result is gone.
const emk_learn::Result* last() { return g_have && g_spoof_generation == emk_spoof::generation() ? &g_result : nullptr; }

emlib::Result save() {
    if (last() == nullptr || g_result.outcome != emk_learn::Outcome::Learned) {
        emlib::Result r;
        r.err = emlib::Err::Invalid;
        return r;
    }
    return storage::lib_save(g_result.rec, g_result.quirks);
}

}  // namespace k2::emk_learn_hw
