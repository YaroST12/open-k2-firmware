// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Connection (pure model): the smart default tab, the port-aware empty states of every tab x topology, the
// status bar text, the action pop-up with its greyed Spoof, the wrap helper and the protocol line.
#include <array>
#include <cstring>
#include <string>
#include "test_util.h"
#include "k2/pd/pd_link.hpp"
#include "k2/ui/ui_connection.hpp"

using namespace k2;
using namespace k2::conn;

static Facts facts(bool inl, bool charger, bool cable, Topology t = Topology::Unknown, bool seen = false,
                   raguard::WaitKind w = raguard::WaitKind::Unknown) {
    Facts f;
    f.inline_sink = inl; f.sink_seen = seen; f.charger = charger; f.cable = cable; f.topo = t; f.wait = w;
    return f;
}

static void test_default_tab() {
    CHECK_EQ(int(default_tab(facts(true, true, true))), int(Tab::Sink));      // a device inline wins over everything
    CHECK_EQ(int(default_tab(facts(true, false, false))), int(Tab::Sink));
    CHECK_EQ(int(default_tab(facts(false, true, true, Topology::Unknown, true))), int(Tab::Sink));  // seen on CC (e.g. after a role swap)
    CHECK_EQ(int(default_tab(facts(false, true, true))), int(Tab::Source));   // a charger seen
    CHECK_EQ(int(default_tab(facts(false, true, false))), int(Tab::Source));
    CHECK_EQ(int(default_tab(facts(false, false, true))), int(Tab::Cable));   // only a cable
    CHECK_EQ(int(default_tab(facts(false, false, false))), int(Tab::Source)); // nothing: Source, empty
}

static void test_tab_empty() {
    CHECK_EQ(tab_empty(Tab::Source, facts(false, false, false)), true);
    CHECK_EQ(tab_empty(Tab::Source, facts(false, true, false)), false);
    CHECK_EQ(tab_empty(Tab::Cable, facts(false, true, false)), true);
    CHECK_EQ(tab_empty(Tab::Cable, facts(false, false, true)), false);
    CHECK_EQ(tab_empty(Tab::Sink, facts(false, true, true)), true);
    CHECK_EQ(tab_empty(Tab::Sink, facts(true, false, false)), false);
    CHECK_EQ(tab_empty(Tab::Sink, facts(false, true, true, Topology::Unknown, true)), false);  // a sink was seen on CC
    CHECK_EQ(tab_empty(Tab::Sink, facts(false, false, false, Topology::Unknown, true)), false);
}

static void test_topology_word() {
    CHECK_STR(topology_word(Topology::DirectReceptacle), "top plug");
    CHECK_STR(topology_word(Topology::CableSingleCc), "via cable");
    CHECK_STR(topology_word(Topology::NoSource), "");
    CHECK_STR(topology_word(Topology::Unknown), "");
}

// Every tab x topology: the spec's hint table, word for word.
static void test_empty_states() {
    const auto e = [](Tab t, Topology o) { return empty_state(t, facts(false, false, false, o)); };
    // Source
    CHECK_STR(e(Tab::Source, Topology::NoSource).title, "NO CHARGER");
    CHECK_STR(e(Tab::Source, Topology::NoSource).hint, "Use either port");   // one short line: the footer already says "No charger"
    CHECK_STR(e(Tab::Source, Topology::Unknown).title, "NO CHARGER");
    CHECK_STR(e(Tab::Source, Topology::Unknown).hint, "Use either port");
    {   // the empty Source tab is one line of 26 cells (the page paints the hint at 8 px advance in 224 - 16 px)
        std::array<std::string_view, 4> ln{};
        CHECK_EQ(wrap(e(Tab::Source, Topology::NoSource).hint, 26, ln), 1);
    }
    // The topology word alone no longer says "waiting": the classifier (Facts::wait) decides, see
    // test_source_tab_reads_the_waiting_classifier - with no verdict yet (Unknown) the tab says NO CHARGER.
    CHECK_STR(e(Tab::Source, Topology::DirectReceptacle).title, "NO CHARGER");
    CHECK_STR(e(Tab::Source, Topology::CableSingleCc).title, "NO CHARGER");
    // Sink
    CHECK_STR(e(Tab::Sink, Topology::DirectReceptacle).title, "NO DEVICE");
    // The two ports share one CC network and one rail (docs/K2_PORTS_SPEC.md): no copy names a port as "the" device port.
    CHECK_STR(e(Tab::Sink, Topology::DirectReceptacle).hint, "Plug a device into the other port.");
    CHECK_STR(e(Tab::Sink, Topology::CableSingleCc).title, "NO DEVICE");
    CHECK_STR(e(Tab::Sink, Topology::CableSingleCc).hint, "Charger is on one port: no device side in this setup.");
    CHECK_STR(e(Tab::Sink, Topology::NoSource).hint, "Plug a device into either port.");
    CHECK_STR(e(Tab::Sink, Topology::Unknown).title, "NO DEVICE");
    CHECK_STR(e(Tab::Sink, Topology::Unknown).hint, "Plug a device into either port.");
    // Cable
    CHECK_STR(e(Tab::Cable, Topology::DirectReceptacle).title, "NO CABLE");
    CHECK_STR(e(Tab::Cable, Topology::DirectReceptacle).hint, "Charger is plugged straight in (no cable).");
    CHECK_STR(e(Tab::Cable, Topology::CableSingleCc).title, "CABLE NOT READ");
    CHECK_STR(e(Tab::Cable, Topology::CableSingleCc).hint, "OK -> Read cable (unplug the charger first).");
    CHECK_STR(e(Tab::Cable, Topology::Unknown).title, "NO CABLE READ");
    CHECK_STR(e(Tab::Cable, Topology::Unknown).hint, "Plug a cable into either port, then OK -> Read cable.");
    CHECK_STR(e(Tab::Cable, Topology::NoSource).title, "NO CABLE READ");
    // Titles fit the big font (12 px per character) in 224 px; every hint wraps into at most 4 lines of 27.
    for (int t = 0; t < kTabs; t++) {
        for (int o = 0; o < 4; o++) {
            const Empty em = e(static_cast<Tab>(t), static_cast<Topology>(o));
            CHECK_EQ(std::strlen(em.title) <= 18, 1);
            std::array<std::string_view, 4> ln{};
            const int n = wrap(em.hint, 27, ln);
            CHECK_EQ(n >= 1 && n <= 4, 1);
            std::string joined;
            for (int i = 0; i < n; i++) { CHECK_EQ(ln[std::size_t(i)].size() <= 27, 1); joined += (i ? " " : ""); joined += std::string(ln[std::size_t(i)]); }
            CHECK_STR(joined.c_str(), em.hint);   // nothing lost in the wrap
        }
    }
}

// A K2 that sees a live line it did not make (VBUS, or a sink's Rd on the far side) says "in use", never "no device",
// and never names a port: a dock -> K2 -> laptop chain with no PD traffic of its own showed NO DEVICE on the Sink tab.
static void test_empty_states_when_possibly_inline() {
    const auto e = [](Tab t, Topology o) { return empty_state(t, facts(true, false, false, o)); };
    for (int t = 0; t < kTabs; t++) {
        for (int o = 0; o < 4; o++) {
            const Empty em = e(static_cast<Tab>(t), static_cast<Topology>(o));
            CHECK_EQ(std::strstr(em.title, "NO DEVICE") == nullptr, 1);
            CHECK_EQ(std::strstr(em.title, "NO CHARGER") == nullptr, 1);
            CHECK_EQ(std::strstr(em.hint, "bottom") == nullptr && std::strstr(em.hint, "top port") == nullptr, 1);
            CHECK_EQ(std::strlen(em.title) <= 18, 1);
            std::array<std::string_view, 4> ln{};
            const int n = wrap(em.hint, 27, ln);
            CHECK_EQ(n >= 1 && n <= 4, 1);
            std::size_t kept = 0;
            for (int i = 0; i < n; i++) kept += ln[std::size_t(i)].size();
            CHECK_EQ(kept + std::size_t(n - 1) >= std::strlen(em.hint), 1);   // nothing lost in the wrap
        }
    }
    CHECK_STR(e(Tab::Source, Topology::Unknown).title, "LINE IN USE");
    CHECK_STR(e(Tab::Sink, Topology::Unknown).title, "DEVICE IN USE");
    CHECK_STR(e(Tab::Cable, Topology::Unknown).title, "CABLE NOT READ");
    // The other empty states do not name a port either.
    const auto n = [](Tab t, Topology o) { return empty_state(t, facts(false, false, false, o)); };
    for (int t = 0; t < kTabs; t++)
        for (int o = 0; o < 4; o++) CHECK_EQ(std::strstr(n(static_cast<Tab>(t), static_cast<Topology>(o)).hint, "bottom") == nullptr, 1);
}

static void test_wrap() {
    std::array<std::string_view, 3> l{};
    CHECK_EQ(wrap("short", 27, l), 1);
    CHECK_STR(std::string(l[0]).c_str(), "short");
    CHECK_EQ(wrap("aaa bbb ccc", 7, l), 2);
    CHECK_STR(std::string(l[0]).c_str(), "aaa bbb");
    CHECK_STR(std::string(l[1]).c_str(), "ccc");
    CHECK_EQ(wrap("abcdefghij", 4, l), 3);          // a word longer than the width is cut
    CHECK_STR(std::string(l[0]).c_str(), "abcd");
    CHECK_STR(std::string(l[2]).c_str(), "ij");
    CHECK_EQ(wrap("", 10, l), 0);
    std::array<std::string_view, 1> one{};
    CHECK_EQ(wrap("one two three", 3, one), 1);     // capped by the line count
}

// The Source tab reads the one waiting classifier (Facts::wait, the Rp trace): a steady Rp = a charger waiting
// for a sink, a toggling Rp (a dual-role port, a phone alone) = not a charger. Today the tab keys on the topology
// alone, so a phone alone reads "CHARGER WAITING" (connection matrix H13).
static void test_source_tab_reads_the_waiting_classifier() {
    CHECK_STR(empty_state(Tab::Source, facts(false, false, false, Topology::Unknown, false, raguard::WaitKind::ChargerWaiting)).title,
              "CHARGER WAITING");   // RED until the fix: the topology alone says "NO CHARGER"
    CHECK_STR(empty_state(Tab::Source, facts(false, false, false, Topology::Unknown, false, raguard::WaitKind::ChargerWaiting)).hint,
              "Charger waits for a sink: start a charger test.");
    const Empty ph = empty_state(Tab::Source, facts(false, false, false, Topology::CableSingleCc, false, raguard::WaitKind::DualRolePort));
    CHECK_STR(ph.title, "DUAL-ROLE PORT");   // RED until the fix: a toggling phone still says "CHARGER WAITING"/"NO CHARGER"
    CHECK_EQ(std::strlen(ph.title) <= 18, 1);
    {   // the hint wraps into at most 4 lines of 27 cells
        std::array<std::string_view, 4> ln{};
        const int n = wrap(ph.hint, 27, ln);
        CHECK_EQ(n >= 1 && n <= 4, 1);
    }
    CHECK_STR(empty_state(Tab::Source, facts(false, false, false, Topology::CableSingleCc, false, raguard::WaitKind::NoSource)).title,
              "NO CHARGER");
    CHECK_STR(empty_state(Tab::Source, facts(false, false, false, Topology::Unknown, false, raguard::WaitKind::Unknown)).title,
              "NO CHARGER");
}

// The bottom bar reads the same classifier: on a real waiting charger it must not say "No charger" while the tab
// says "CHARGER WAITING" (and k2_remote state.waiting must agree too).
static void test_bar_reads_the_waiting_classifier() {
    StrBuf<40> s;
    StatusIn in;
    in.tab = Tab::Source;
    in.f = facts(false, false, false, Topology::CableSingleCc, false, raguard::WaitKind::ChargerWaiting);
    status_text(in, s); CHECK_STR(s.c_str(), "Charger waiting, via cable");   // RED until the fix: "No charger, via cable"
    CHECK_EQ(s.view().size() <= kStatusCells, 1);
    in.f = facts(false, false, false, Topology::DirectReceptacle, false, raguard::WaitKind::ChargerWaiting);
    status_text(in, s); CHECK_STR(s.c_str(), "Charger waiting, top plug");    // RED until the fix: "No charger, top plug"
    in.f = facts(false, false, false, Topology::CableSingleCc, false, raguard::WaitKind::DualRolePort);
    status_text(in, s); CHECK_STR(s.c_str(), "Dual-role port, via cable");    // RED until the fix
    CHECK_EQ(s.view().size() <= kStatusCells, 1);
    in.f = facts(false, false, false, Topology::CableSingleCc, false, raguard::WaitKind::NoSource);
    status_text(in, s); CHECK_STR(s.c_str(), "No charger, via cable");
    in.f = facts(false, true, false, Topology::Unknown, false, raguard::WaitKind::ChargerWaiting);
    status_text(in, s); CHECK_STR(s.c_str(), "Charger");   // a charger already attached wins
}

// One classifier, one result: the Source tab, the bottom bar and the remote state field (kLinkWaiting, fed from
// link.charger_waiting()) all read pd::Link::wait_kind(). A steady Rp trace (a charger waiting behind a cable) and
// a toggling one (a phone alone) through the real Link input.
static void test_the_three_readers_agree() {
    auto feed = [](pd::Link& l, std::uint32_t t, std::int32_t cc1, std::int32_t cc2) {
        pd::LinkInput in;
        in.now_ms = t; in.vbus_known = true; in.vbus_mv = 0; in.cc = 0; in.caps_rx = 0;
        in.cc1_mv = cc1; in.cc2_mv = cc2; in.passive = true;
        l.update(in);
    };
    pd::Link charger;
    for (std::uint32_t t = 0; t <= 1000; t += 20) feed(charger, t, 0, 2990);   // Rp on CC2 only, steady (a sleeping port behind a cable)
    CHECK_EQ(int(charger.wait_kind()), int(raguard::WaitKind::ChargerWaiting));
    CHECK_EQ(charger.charger_waiting(), true);   // what remote.cpp puts in kLinkWaiting -> state.waiting
    Facts f = facts(false, false, false, Topology::CableSingleCc, false, charger.wait_kind());
    CHECK_STR(empty_state(Tab::Source, f).title, "CHARGER WAITING");
    StrBuf<40> s;
    StatusIn in;
    in.tab = Tab::Source; in.f = f;
    status_text(in, s); CHECK_STR(s.c_str(), "Charger waiting, via cable");

    pd::Link phone;
    for (std::uint32_t t = 0; t <= 1000; t += 20) feed(phone, t, 0, (t / 100) % 2 == 0 ? 3300 : 0);   // Rp toggling every 100 ms
    CHECK_EQ(int(phone.wait_kind()), int(raguard::WaitKind::DualRolePort));
    CHECK_EQ(phone.charger_waiting(), false);   // state.waiting stays False: not a charger
    f = facts(false, false, false, Topology::CableSingleCc, false, phone.wait_kind());
    CHECK_STR(empty_state(Tab::Source, f).title, "DUAL-ROLE PORT");
    in.f = f;
    status_text(in, s); CHECK_STR(s.c_str(), "Dual-role port, via cable");

    pd::Link none;
    for (std::uint32_t t = 0; t <= 1000; t += 20) feed(none, t, 0, 1707);   // idle CC2 only
    CHECK_EQ(int(none.wait_kind()), int(raguard::WaitKind::NoSource));
    CHECK_EQ(none.charger_waiting(), false);
    f = facts(false, false, false, Topology::NoSource, false, none.wait_kind());
    CHECK_STR(empty_state(Tab::Source, f).title, "NO CHARGER");
    in.f = f;
    status_text(in, s); CHECK_STR(s.c_str(), "No charger");

    // Not passive (the K2 holds the lines itself, e.g. a pinned boot): the classifier is void, the trace dropped.
    pd::Link active;
    for (std::uint32_t t = 0; t <= 1000; t += 20) {
        pd::LinkInput in2;
        in2.now_ms = t; in2.vbus_known = true; in2.vbus_mv = 0; in2.cc = 0; in2.caps_rx = 0;
        in2.cc1_mv = 0; in2.cc2_mv = 3300; in2.passive = false;
        active.update(in2);
    }
    CHECK_EQ(int(active.wait_kind()), int(raguard::WaitKind::Unknown));
    CHECK_EQ(active.charger_waiting(), false);
    // VBUS up (a source already feeding something): also void.
    pd::Link live;
    for (std::uint32_t t = 0; t <= 1000; t += 20) {
        pd::LinkInput in2;
        in2.now_ms = t; in2.vbus_known = true; in2.vbus_mv = 5000; in2.cc = 1; in2.caps_rx = 1;
        in2.cc1_mv = 3300; in2.cc2_mv = 3300; in2.passive = true;
        live.update(in2);
    }
    CHECK_EQ(int(live.wait_kind()), int(raguard::WaitKind::Unknown));
}

static void test_status_text() {
    StrBuf<40> s;
    StatusIn in;
    in.tab = Tab::Source; in.f = facts(false, false, false);
    status_text(in, s); CHECK_STR(s.c_str(), "No charger");
    in.f = facts(false, false, false, Topology::DirectReceptacle);
    status_text(in, s); CHECK_STR(s.c_str(), "No charger, top plug");     // the topology word is in the bar
    in.f = facts(false, false, false, Topology::CableSingleCc);
    status_text(in, s); CHECK_STR(s.c_str(), "No charger, via cable");
    in.f = facts(false, true, false);
    status_text(in, s); CHECK_STR(s.c_str(), "Charger");
    in.tab = Tab::Cable;
    status_text(in, s); CHECK_STR(s.c_str(), "No cable");
    in.f = facts(false, true, true);
    status_text(in, s); CHECK_STR(s.c_str(), "Cable read");
    in.tab = Tab::Sink; in.f = facts(true, true, true);
    status_text(in, s); CHECK_STR(s.c_str(), "Device inline");
    in.f = facts(false, true, true, Topology::Unknown, true);   // seen on CC but not inline any more
    status_text(in, s); CHECK_STR(s.c_str(), "Sink seen on CC");
    in.f = facts(false, false, false);
    status_text(in, s); CHECK_STR(s.c_str(), "No device");
    // An action message wins; the spoof note shows only with a charger already attached.
    in.op = "Reading cable...";
    status_text(in, s); CHECK_STR(s.c_str(), "Reading cable...");
    in.op = ""; in.spoof_armed = true; in.tab = Tab::Cable; in.f = facts(false, true, true);
    status_text(in, s); CHECK_STR(s.c_str(), "Armed, replug to apply");
    in.f = facts(false, false, true);
    status_text(in, s); CHECK_STR(s.c_str(), "Cable read");               // nothing attached: the identity applies at the next attach
    // Fits the footer (the longest state + topology word).
    in.spoof_armed = false; in.tab = Tab::Source; in.f = facts(false, false, false, Topology::CableSingleCc);
    status_text(in, s); CHECK_EQ(s.view().size() <= 24, 1);
}

// A survey driven to its end with every action answered as `o` (the actions the plan hands out are answered at once).
static void finish_survey(pd::Survey &s, const pd::SvCtx &c, pd::SvOutcome o) {
    std::uint32_t now = 1000;
    s.start(c, now);
    for (int guard = 0; guard < 400 && s.running(); guard++) {
        const pd::SvAct a = s.poll(now, true, true);
        now += 10;
        if (a == pd::SvAct::None) continue;
        switch (a) {
        case pd::SvAct::Caps: s.set(pd::SvField::Caps, o); break;
        case pd::SvAct::Wire:
            for (pd::SvField f : {pd::SvField::Identity, pd::SvField::Svids, pd::SvField::Modes, pd::SvField::CapsExt, pd::SvField::MfrInfo}) s.set(f, o);
            break;
        case pd::SvAct::Epr: s.set(pd::SvField::EprList, o); break;
        case pd::SvAct::Cable: s.set(pd::SvField::Cable, o); break;
        default: break;
        }
        s.finish(now);
    }
}

static const pd::SvCtx kLive{.contract = true, .inline_sink = false, .charger_test = false, .xiaomi_chain = false, .pd20 = false, .epr_capable = true};

// The hint, the progress and the summary of a Refresh in the status bar; the strings fit the 27-cell footer.
static void test_status_survey() {
    StrBuf<40> s;
    StatusIn in;
    in.tab = Tab::Source; in.f = facts(false, true, true);
    pd::Survey sv;
    in.sv = &sv;
    status_text(in, s); CHECK_STR(s.c_str(), "OK > Refresh to read all");   // nothing asked yet, same words on every tab
    in.tab = Tab::Cable;
    status_text(in, s); CHECK_STR(s.c_str(), "OK > Refresh to read all");
    in.tab = Tab::Sink; in.f = facts(true, true, true);
    status_text(in, s); CHECK_STR(s.c_str(), "Device inline");              // the sink side is passive: nothing for Refresh to read
    in.tab = Tab::Source; in.f = facts(false, false, false);
    status_text(in, s); CHECK_STR(s.c_str(), "No charger");                 // an empty tab has no fields to ask
    in.f = facts(false, true, true);

    sv.start(kLive, 1000);                                                  // running: progress, resolved / planned
    CHECK_EQ(int(sv.planned()), 8);
    status_text(in, s); CHECK_STR(s.c_str(), "Reading 0/8");
    sv.poll(1000, true, true);
    sv.set(pd::SvField::Caps, pd::SvOutcome::Answered);
    sv.finish(1010);
    status_text(in, s); CHECK_STR(s.c_str(), "Reading 1/8");
    in.op = "Rescanning...";
    status_text(in, s); CHECK_STR(s.c_str(), "Rescanning...");              // an action message still wins
    in.op = "";

    pd::Survey ok;
    in.sv = &ok;
    finish_survey(ok, kLive, pd::SvOutcome::Answered);
    status_text(in, s); CHECK_STR(s.c_str(), "Read: 8 got");
    pd::Survey mix;
    in.sv = &mix;
    finish_survey(mix, kLive, pd::SvOutcome::Refused);
    status_text(in, s); CHECK_STR(s.c_str(), "Read: 0 got, 8 unsupported");
    pd::Survey quiet;
    in.sv = &quiet;
    finish_survey(quiet, kLive, pd::SvOutcome::Silent);
    status_text(in, s); CHECK_STR(s.c_str(), "Read: 0 got, 8 no answer");
    CHECK_EQ(s.view().size() <= 27, 1);

    pd::Survey blocked;                                                     // a blocked sink: reasons in the rows, no hint, no summary
    in.sv = &blocked;
    pd::SvCtx c = kLive; c.inline_sink = true;
    blocked.start(c, 1000);
    status_text(in, s); CHECK_STR(s.c_str(), "Charger");
    in.sv = nullptr;
    status_text(in, s); CHECK_STR(s.c_str(), "Charger");
}

// A row says why it is empty: one word per status, each fits the 19-cell value column.
static void test_survey_words() {
    using pd::SvState; using pd::SvWhy;
    CHECK_STR(sv_word({SvState::NotAsked, SvWhy::None}), "not asked");
    CHECK_STR(sv_word({SvState::Pending, SvWhy::None}), "asking...");
    CHECK_STR(sv_word({SvState::Got, SvWhy::None}), "");
    CHECK_STR(sv_word({SvState::NotSupported, SvWhy::None}), "not supported");
    CHECK_STR(sv_word({SvState::NoAnswer, SvWhy::None}), "no answer");
    CHECK_STR(sv_word({SvState::Blocked, SvWhy::Inline}), "inline: blocked");
    CHECK_STR(sv_word({SvState::Blocked, SvWhy::ChargerTest}), "charger test");
    CHECK_STR(sv_word({SvState::Blocked, SvWhy::XiaomiChain}), "Xiaomi chain");
    CHECK_STR(sv_word({SvState::Blocked, SvWhy::Pd20}), "not in PD 2.0");
    CHECK_STR(sv_word({SvState::Blocked, SvWhy::NoEpr}), "no EPR bit");
    for (unsigned st = 0; st <= unsigned(SvState::Blocked); st++)
        for (unsigned w = 0; w <= unsigned(SvWhy::NoEpr); w++)
            CHECK_EQ(std::strlen(sv_word({SvState(st), SvWhy(w)})) <= 19, 1);
    // Which fields a tab shows: Source everything the wire answers, Cable its own, Sink none.
    CHECK_EQ(tab_shows(Tab::Source, pd::SvField::MfrInfo), true);
    CHECK_EQ(tab_shows(Tab::Source, pd::SvField::Cable), false);
    CHECK_EQ(tab_shows(Tab::Cable, pd::SvField::Cable), true);
    CHECK_EQ(tab_shows(Tab::Cable, pd::SvField::Identity), false);
    CHECK_EQ(tab_shows(Tab::Sink, pd::SvField::Identity), false);
}

static void test_actions() {
    std::array<Item, kMaxItems> it{};
    ActFacts a;
    a.attached = true; a.caps_valid = true; a.cable_valid = true;
    int n = actions(Tab::Source, a, it);
    CHECK_EQ(n, 3);                                                         // Refresh, Details, Share (QR)
    CHECK_STR(it[0].label, "Refresh");
    CHECK_EQ(int(it[1].act), int(Act::Details));
    CHECK_STR(it[1].label, "Details");
    CHECK_EQ(it[1].enabled, true);
    CHECK_STR(it[2].label, "Share (QR)");
    CHECK_EQ(it[2].enabled, true);
    a.more = true;                                                          // on the second page the same item goes back
    actions(Tab::Source, a, it);
    CHECK_STR(it[1].label, "Main page");
    a.more = false;
    a.attached = false;
    actions(Tab::Source, a, it);
    CHECK_EQ(it[1].enabled, false);                                         // no source: no details
    CHECK_STR(it[1].why, "No charger");
    CHECK_EQ(it[0].enabled, true);
    a.attached = true;
    a.caps_valid = false;
    actions(Tab::Source, a, it);
    CHECK_EQ(it[2].enabled, false);                                         // nothing to share yet

    a.caps_valid = true;
    n = actions(Tab::Cable, a, it);
    CHECK_EQ(n, 6);                                                         // the existing items keep their place, Refresh is last
    CHECK_STR(it[0].label, "Read cable");
    CHECK_STR(it[1].label, "Save cable");
    CHECK_EQ(it[1].enabled, true);
    CHECK_STR(it[2].label, "Spoof as...");
    CHECK_EQ(it[2].enabled, true);
    CHECK_STR(it[3].label, "Saved cables");
    CHECK_STR(it[4].label, "Share (QR)");
    CHECK_EQ(it[4].enabled, true);
    CHECK_EQ(int(it[5].act), int(Act::Refresh));
    CHECK_EQ(it[5].enabled, true);
    a.inline_sink = true;                                                    // a live sink: Spoof is greyed with the reason
    actions(Tab::Cable, a, it);
    CHECK_EQ(it[2].enabled, false);
    CHECK_STR(it[2].why, "Live sink: unplug load");
    CHECK_EQ(it[0].enabled, true);                                           // Read cable stays (inline the driver refuses it, no confirm)
    CHECK_EQ(it[5].enabled, true);                                           // Refresh stays: the survey blocks its own fields
    a.cable_valid = false;
    actions(Tab::Cable, a, it);
    CHECK_EQ(it[1].enabled, false);                                          // nothing to save / share without a read
    CHECK_EQ(it[4].enabled, false);

    n = actions(Tab::Sink, a, it);
    CHECK_EQ(n, 2);
    CHECK_STR(it[0].label, "Identify");
    CHECK_EQ(it[0].enabled, true);
    CHECK_EQ(int(it[1].act), int(Act::Refresh));
    a.attached = false;
    actions(Tab::Sink, a, it);
    CHECK_EQ(it[0].enabled, false);
    CHECK_STR(it[0].why, "No device seen");
    CHECK_EQ(it[1].enabled, true);
    for (int t = 0; t < kTabs; t++) {                                         // every label fits the pop-up row (title at x 10, <= 24 chars)
        ActFacts all;
        all.attached = all.caps_valid = all.cable_valid = true;
        const int c = actions(static_cast<Tab>(t), all, it);
        for (int i = 0; i < c; i++) {
            CHECK_EQ(std::strlen(it[std::size_t(i)].label) <= 24, 1);
            CHECK_EQ(std::strlen(it[std::size_t(i)].why) <= 24, 1);
        }
    }
}

static void test_protocol() {
    CHECK_STR(protocol_name(0x2717), "Xiaomi PD");
    CHECK_STR(protocol_name(0x22D9), "OPPO PD");
    CHECK_EQ(protocol_name(0xFF01) == nullptr, true);                        // DisplayPort is not a protocol line
    CHECK_EQ(protocol_name(0x8087) == nullptr, true);                        // neither is Thunderbolt
    CHECK_EQ(protocol_name(0x1234) == nullptr, true);
    StrBuf<40> s;
    const std::uint16_t none[] = {0xFF00, 0xFF01, 0x8087};
    CHECK_EQ(protocol_text(none, s), false);
    CHECK_STR(s.c_str(), "");
    const std::uint16_t some[] = {0xFF01, 0x2717, 0x22D9, 0x2717};
    CHECK_EQ(protocol_text(some, s), true);
    CHECK_STR(s.c_str(), "Xiaomi PD, OPPO PD");                               // names only, once each, no hex
    CHECK_EQ(std::string(s.c_str()).find("0x") == std::string::npos, true);
}

// The Sink tab's ID row: a sniffed identity is marked "sniff VID:PID" (an inline K2 never asks),
// an asked one shows the vendor name when it fits the value column whole.
static void test_id_value() {
    StrBuf<28> s;
    conn::id_value(s, 19, false, 0x05C6, 0x209E, "OnePlus");
    CHECK_STR(s.c_str(), "sniff 05C6:209E");                                  // the vendor is dropped for the mark
    conn::id_value(s, 19, true, 0x05AC, 0x1A2B, "Apple");
    CHECK_STR(s.c_str(), "05AC:1A2B Apple");
    conn::id_value(s, 19, true, 0x05AC, 0x1A2B, nullptr);
    CHECK_STR(s.c_str(), "05AC:1A2B");
    conn::id_value(s, 19, true, 0x18D1, 0x4EE1, "VeryLongVendorName");        // kept whole or left out
    CHECK_STR(s.c_str(), "18D1:4EE1");
    conn::id_value(s, 19, false, 0x05C6, 0x209E, nullptr);
    CHECK_STR(s.c_str(), "sniff 05C6:209E");
    conn::id_value(s, 14, false, 0x05C6, 0x209E, nullptr);                     // a narrow column: no room for the mark
    CHECK_STR(s.c_str(), "05C6:209E");
}

int main() {
    test_default_tab();
    test_tab_empty();
    test_topology_word();
    test_empty_states();
    test_empty_states_when_possibly_inline();
    test_source_tab_reads_the_waiting_classifier();
    test_bar_reads_the_waiting_classifier();
    test_the_three_readers_agree();
    test_wrap();
    test_status_text();
    test_status_survey();
    test_survey_words();
    test_actions();
    test_protocol();
    test_id_value();
    return test_report("test_ui_connection");
}
