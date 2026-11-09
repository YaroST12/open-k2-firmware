// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// "Who's attached": the survey one Refresh on the Connection page runs, pure logic (host-tested in tests/test_pd_survey.cpp).
// The page shows what is attached from what the K2 heard passively. Anything it has not been told stays NotAsked until the
// user presses Refresh; then this class owns the ordered plan of the questions, the per-field status and the gating, and
// hands the driver glue one action at a time (core/survey.hpp maps an action to the sender that does it):
//   Caps   re-ask the source's capabilities (rescan: Get_Source_Cap on a live contract)
//   Wire   Discover Identity, SVIDs, Modes, Get_Source_Cap_Extended, Get_Manufacturer_Info: one pd::Reread run
//   Epr    the source's EPR source-cap list (Sink::request_epr_info, needs the EPR bit in PDO 1)
//   Cable  the cable e-marker, checked from what was sniffed (no VCONN read is started from here)
// Rules the plan keeps: one action in flight, a pause between two actions, the whole run bounded (kSurveyTotalMs), and an
// action only goes out when the sink is quiet (Ready: no negotiation in progress) and the line idle. A field the survey
// will not ask is Blocked with the reason (inline sink, charger test, Xiaomi chain, PD 2.0, no EPR bit), never a silent
// gap. An answered field is Got; Not_Supported / Reject / NAK is NotSupported;
// silence, BUSY or a failed transmit is NoAnswer. A started run always ends: fields still open at the bound become NoAnswer.
#pragma once
#include <cstdint>

#include "k2/pd/pd_reread.hpp"

namespace k2::pd {

enum class SvField : std::uint8_t { Caps, Identity, Svids, Modes, CapsExt, MfrInfo, EprList, Cable };
inline constexpr unsigned kSvFields = 8;
enum class SvState : std::uint8_t { NotAsked, Pending, Got, NotSupported, NoAnswer, Blocked };
enum class SvWhy : std::uint8_t { None, Inline, ChargerTest, XiaomiChain, Pd20, NoEpr };
enum class SvAct : std::uint8_t { None, Caps, Wire, Epr, Cable, Abort };
enum class SvOutcome : std::uint8_t { Answered, Refused, Silent };

inline constexpr std::uint32_t kSurveyPaceMs = 220;      // quiet time between two actions
inline constexpr std::uint32_t kSurveyTotalMs = 12000;   // the whole survey, from Refresh
inline constexpr std::uint32_t kSurveyWireMs = 6500;     // the Wire action's own budget inside it (pd::Reread, from its first step)

// What the glue tells the survey when it starts: the state of the world the gating needs.
struct SvCtx {
    bool contract = false;       // the sink holds a contract (the survey only asks a source that is talking)
    bool inline_sink = false;    // passive inline measurement: nothing active, ever
    bool charger_test = false;   // a charger test is running
    bool xiaomi_chain = false;   // the Xiaomi 0x2717 auth chain is in progress
    bool pd20 = false;           // the source speaks PD 2.0: no extended messages, no EPR
    bool epr_capable = false;    // PDO 1 carries the EPR bit
};

struct SvEntry {
    SvState state = SvState::NotAsked;
    SvWhy why = SvWhy::None;
};

// How one Wire field ended, from the finished pd::Reread run: its step answered, refused (notsup_mask) or neither (silence, BUSY,
// a failed transmit, a step that never ran). Fields that have no Reread step are Silent.
SvOutcome wire_outcome(const RereadStatus& rs, SvField f);

class Survey {
public:
    // Plan one run. Without a contract nothing is asked and every field stays NotAsked (the hint stays).
    void start(const SvCtx& ctx, std::uint32_t now);
    void clear();   // forget the whole view (the source went away): every field NotAsked, not running
    // End a run early (a charger test took the line, the source stopped answering): every Pending field becomes NoAnswer, the
    // answers and the Blocked reasons stay, nothing more is handed out.
    void abandon();

    // The next action to do, or None. Pass sink_quiet = the sink is Ready (no message of its own in flight) and
    // line_idle = the CC line is idle. Abort once, when the whole-run bound hits while an action was in flight.
    SvAct poll(std::uint32_t now, bool sink_quiet, bool line_idle);
    // The glue reports one field of the action in flight (Wire sets several); a resolved field is not rewritten.
    void set(SvField f, SvOutcome o);
    // The action in flight is over: its fields still open become NoAnswer, and the pause to the next one starts.
    void finish(std::uint32_t now);

    bool running() const { return running_; }
    bool in_flight() const { return act_ != SvAct::None; }
    SvAct current() const { return act_; }
    bool ran() const { return started_ && !running_ && planned() > 0; }   // a run that reached its end
    const SvEntry& entry(SvField f) const { return e_[unsigned(f)]; }
    // The kReread* steps of the Wire action that are planned (a blocked one is left out).
    std::uint8_t wire_mask() const;

    unsigned planned() const;    // fields asked in this run (everything but NotAsked and Blocked)
    unsigned resolved() const;   // planned fields that are no longer Pending
    unsigned got() const { return count(SvState::Got); }
    unsigned refused() const { return count(SvState::NotSupported); }
    unsigned silent() const { return count(SvState::NoAnswer); }
    unsigned blocked() const { return count(SvState::Blocked); }

private:
    unsigned count(SvState s) const;
    void open_to_no_answer(SvAct a);
    SvAct next_action() const;
    bool open(SvAct a) const;   // does the action have a Pending field
    void settle_running();
    SvEntry e_[kSvFields]{};
    bool started_ = false, running_ = false;
    SvAct act_ = SvAct::None;
    std::uint32_t started_at_ = 0, not_before_ = 0;
};

}  // namespace k2::pd
