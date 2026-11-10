// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Connection (Tools): one screen for the three parties on the two ports, Source / Cable / Sink, pure logic
// (host-tested in tests/test_ui_connection.cpp). No drawing, no hardware: the page fills Facts from what
// the passive readers already know and these functions decide what to say.
//
// Port-aware empty states use ONLY detection that already exists and never touch a live link:
//   - raguard::classify over the CC levels the ADC already averages (Rp on both lines = a receptacle
//     charger straight on the top plug, Rp on one line = a cable in between). It is valid only while the
//     K2 is passive and VBUS is down (a charger waiting for a sink); anything else is Unknown.
//   - the inline-sink detector (pd::Link::inline_mode) and the e-marker data held in the driver.
// Which side the ADC taps is not hardware-proven (docs/K2_PORTS_SPEC.md), so the words below name what the
// lines show ("top plug", "via cable"), and the Unknown topology gets a generic hint that names no port.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "k2/pd/cc_crossed.hpp"
#include "k2/pd/pd_survey.hpp"
#include "k2/pd/pd_tabs.hpp"
#include "k2/pd/ra_guard.hpp"
#include "k2/ui/ui_calc.hpp"

namespace k2::conn {

using Tab = pdtabs::Tab;
using Topology = raguard::Topology;
inline constexpr int kTabs = pdtabs::kTabs;

constexpr const char *tab_name(Tab t) {
    return t == Tab::Source ? "Source" : t == Tab::Cable ? "Cable" : "Sink";
}

// What the passive readers say right now.
struct Facts {
    bool inline_sink = false;   // a live sink is attached on the far port (pd::Link::inline_mode)
    bool sink_seen = false;     // a sink's Request / Sink_Capabilities was decoded on CC (roles follow a power-role swap)
    bool charger = false;       // a source is attached or its capabilities were seen
    bool cable = false;         // e-marker data is held (read by VCONN or sniffed from the charger's own query)
    Topology topo = Topology::Unknown;
    raguard::WaitKind wait = raguard::WaitKind::Unknown;   // the one waiting classifier (pd::Link::wait_kind, the Rp trace)
    raguard::CrossKind cross = raguard::CrossKind::None;   // crossed CC through the K2 (pd::Link::cc_cross)
};

// The tab to open on: Sink when a device is inline or was seen on CC, else Source when a charger is seen,
// else Cable when a cable is present, else Source (empty).
constexpr Tab default_tab(const Facts &f) {
    if (f.inline_sink || f.sink_seen) return Tab::Sink;
    if (f.charger) return Tab::Source;
    if (f.cable) return Tab::Cable;
    return Tab::Source;
}

constexpr bool tab_empty(Tab t, const Facts &f) {
    return t == Tab::Source ? !f.charger : t == Tab::Cable ? !f.cable : !(f.inline_sink || f.sink_seen);
}

// "top plug" / "via cable" when the lines say so, "" otherwise (status bar).
constexpr const char *topology_word(Topology t) {
    return t == Topology::DirectReceptacle ? "top plug" : t == Topology::CableSingleCc ? "via cable" : "";
}

// Empty state of a tab: a large message and one hint (wrapped by the page).
struct Empty { const char *title; const char *hint; };

// The two ports share one CC network and one rail (docs/K2_PORTS_SPEC.md), so no text names one port as the device's.
// A K2 that is possibly inline (a live line it did not make: VBUS or a sink's Rd on the far side, no contract of its own)
// never says "no device" or "no charger": something is on the line, the K2 just cannot see it talk.
constexpr Empty empty_state(Tab t, const Facts &f) {
    const bool direct = f.topo == Topology::DirectReceptacle, via_cable = f.topo == Topology::CableSingleCc;
    switch (t) {
    case Tab::Source:
        if (f.inline_sink) return {"LINE IN USE", "VBUS is live, no source PD seen: a device may be in use."};
        // Crossed CC (pd::Link::cc_cross): the two cables' CC wires land on different K2 lines, so neither far end finds a
        // partner. It outranks the waiting words, which would read the loaded Rp as a charger and the toggle as a phone.
        if (f.cross == raguard::CrossKind::Crossed) return {"CC CROSSED", "Flip one of the two cables (the CC wires meet no partner)."};
        if (f.cross == raguard::CrossKind::Hint) return {"FLIP A CABLE?", "A charger waits, nothing answers: try flipping a cable."};
        // The one waiting classifier (f.wait, the Rp trace): a steady Rp = a charger waiting for a sink; a toggling
        // Rp = a dual-role port (a phone alone) looking for a partner, not a charger.
        if (f.wait == raguard::WaitKind::ChargerWaiting) return {"CHARGER WAITING", "Charger waits for a sink: start a charger test."};
        if (f.wait == raguard::WaitKind::DualRolePort) return {"DUAL-ROLE PORT", "A phone? It toggles Rp looking for a partner; START makes it source 5 V."};
        return {"NO CHARGER", "Use either port"};   // one line: the footer already states "No charger"
    case Tab::Sink:
        if (f.inline_sink) return {"DEVICE IN USE", "Something is on the line behind the K2: nothing is asked of it."};
        if (direct) return {"NO DEVICE", "Plug a device into the other port."};
        if (via_cable) return {"NO DEVICE", "Charger is on one port: no device side in this setup."};
        return {"NO DEVICE", "Plug a device into either port."};
    case Tab::Cable:
        if (f.inline_sink) return {"CABLE NOT READ", "A device may be in use: OK -> Read cable asks first."};
        if (direct) return {"NO CABLE", "Charger is plugged straight in (no cable)."};
        if (via_cable) return {"CABLE NOT READ", "OK -> Read cable (unplug the charger first)."};
        return {"NO CABLE READ", "Plug a cable into either port, then OK -> Read cable."};
    }
    return {"", ""};
}

// Greedy word wrap into at most lines.size() lines of `width` characters; a word longer than the width is
// cut. Returns the number of lines used.
template <std::size_t N>
int wrap(std::string_view text, std::size_t width, std::array<std::string_view, N> &lines) {
    int n = 0;
    std::size_t pos = 0;
    while (pos < text.size() && n < int(N)) {
        while (pos < text.size() && text[pos] == ' ') pos++;
        if (pos >= text.size()) break;
        std::size_t end = pos;
        std::size_t last_space = std::string_view::npos;
        while (end < text.size() && end - pos < width) {
            if (text[end] == ' ') last_space = end;
            end++;
        }
        if (end < text.size() && text[end] != ' ' && last_space != std::string_view::npos) end = last_space;
        std::size_t stop = end;
        while (stop > pos && text[stop - 1] == ' ') stop--;
        lines[std::size_t(n++)] = text.substr(pos, stop - pos);
        pos = end;
    }
    return n;
}

// ---- survey (what Refresh reads, ui side) ------------------------------------------------------------
// The survey fields a tab shows: the Source tab everything the wire answers, the Cable tab the cable, the Sink tab none (its side
// is passive; Identify is refused inline and shows the sniffed identity instead).
constexpr bool tab_shows(Tab t, pd::SvField f) {
    return t == Tab::Source ? f != pd::SvField::Cable : t == Tab::Cable ? f == pd::SvField::Cable : false;
}

// True while a field the tab shows has not been asked yet (the status bar then says how to ask).
inline bool tab_unasked(Tab t, const pd::Survey &s) {
    for (unsigned i = 0; i < pd::kSvFields; i++) {
        const pd::SvField f = pd::SvField(i);
        if (tab_shows(t, f) && s.entry(f).state == pd::SvState::NotAsked) return true;
    }
    return false;
}

// One word for a field that has no value: why the row is empty (<= 19 cells, the value column). Got has no word.
constexpr const char *sv_word(const pd::SvEntry &e) {
    switch (e.state) {
    case pd::SvState::NotAsked: return "not asked";
    case pd::SvState::Pending: return "asking...";
    case pd::SvState::Got: return "";
    case pd::SvState::NotSupported: return "not supported";
    case pd::SvState::NoAnswer: return "no answer";
    case pd::SvState::Blocked:
        switch (e.why) {
        case pd::SvWhy::Inline: return "inline: blocked";
        case pd::SvWhy::ChargerTest: return "charger test";
        case pd::SvWhy::XiaomiChain: return "Xiaomi chain";
        case pd::SvWhy::Pd20: return "not in PD 2.0";
        case pd::SvWhy::NoEpr: return "no EPR bit";
        case pd::SvWhy::None: break;
        }
        return "blocked";
    }
    return "";
}

inline constexpr const char *kRefreshHint = "OK > Refresh to read all";   // one message, the same on every tab
inline constexpr std::size_t kStatusCells = 27;                            // the footer line (static_asserted against it by the page)

// "Read: 5 got, 2 unsupported": the footer holds 27 cells, "2 not supported" would make the line 28, so the short word. A
// ", 1 no answer" follows only while it still fits.
template <std::size_t N>
void read_summary(const pd::Survey &s, StrBuf<N> &out) {
    out.put("Read: ").put_u32(s.got()).put(" got");
    if (s.refused()) out.put(", ").put_u32(s.refused()).put(" unsupported");
    if (s.silent()) {
        StrBuf<24> t;
        t.put(", ").put_u32(s.silent()).put(" no answer");
        if (out.view().size() + t.view().size() <= kStatusCells) out.put(t.view());
    }
}

// ---- status bar -------------------------------------------------------------------------------------
struct StatusIn {
    Tab tab = Tab::Source;
    Facts f;
    std::string_view op;        // what the last action says ("Reading cable...", "Live sink: OK again"), "" = none
    bool spoof_armed = false;   // a saved cable is armed for the next charger test or attach
    const pd::Survey *sv = nullptr;   // the Refresh survey (nullptr: none, the old status only)
};

// The bottom bar: an action message wins; then the spoof note when a charger is already attached (the
// identity is presented at the next attach); then the survey (progress while it runs, the hint while a field of the open
// tab was never asked, the summary after a run); else the state of the shown tab and the topology word.
template <std::size_t N>
void status_text(const StatusIn &s, StrBuf<N> &out) {
    out.clear();
    if (!s.op.empty()) { out.put(s.op); return; }
    if (s.spoof_armed && s.f.charger) { out.put("Armed, replug to apply"); return; }
    if (s.sv) {
        if (s.sv->running()) { out.put("Reading ").put_u32(s.sv->resolved()).put_char('/').put_u32(s.sv->planned()); return; }
        if (!tab_empty(s.tab, s.f) && tab_unasked(s.tab, *s.sv)) { out.put(kRefreshHint); return; }
        if (s.sv->ran() && s.tab != Tab::Sink && !tab_empty(s.tab, s.f)) { read_summary(*s.sv, out); return; }
    }
    switch (s.tab) {
    case Tab::Source:
        // The same waiting classifier as the tab (f.wait): the bar never disagrees with it. The crossed-CC words fill the whole
        // bar (27 cells), so no topology word follows them.
        if (!s.f.charger && !s.f.inline_sink && s.f.cross == raguard::CrossKind::Crossed) { out.put("CC CROSSED - flip one cable"); return; }
        if (!s.f.charger && !s.f.inline_sink && s.f.cross == raguard::CrossKind::Hint) { out.put("Flip a cable?"); return; }
        if (s.f.charger) out.put("Charger");
        else if (s.f.wait == raguard::WaitKind::ChargerWaiting) out.put("Charger waiting");
        else if (s.f.wait == raguard::WaitKind::DualRolePort) out.put("Dual-role port");
        else out.put("No charger");
        break;
    case Tab::Cable:  out.put(s.f.cable ? "Cable read" : "No cable"); break;
    case Tab::Sink:   out.put(s.f.inline_sink ? "Device inline" : s.f.sink_seen ? "Sink seen on CC" : "No device"); break;
    }
    const char *w = topology_word(s.f.topo);
    if (w[0]) out.put(", ").put(w);
}

// ---- action pop-up ----------------------------------------------------------------------------------
enum class Act : std::uint8_t { Refresh, ShareCaps, ReadCable, SaveCable, SpoofAs, SavedCables, ShareCable, Identify, Details };
struct Item {
    Act act = Act::Refresh;
    const char *label = "";
    bool enabled = true;
    const char *why = "";       // shown in the status bar when a greyed item is chosen
};
inline constexpr int kMaxItems = 6;

struct ActFacts {
    bool more = false;          // the Source tab shows its second page (Details): the item goes back
    bool inline_sink = false;   // a live sink is inline: active tools that would reach it are refused
    bool attached = false;      // a charger or a device answers on a port
    bool caps_valid = false;    // Source_Capabilities received
    bool cable_valid = false;   // e-marker data held
};

inline constexpr const char *kWhyLiveSink = "Live sink: unplug load";

constexpr int actions(Tab t, const ActFacts &a, std::array<Item, kMaxItems> &out) {
    int n = 0;
    switch (t) {
    case Tab::Source:
        out[std::size_t(n++)] = {Act::Refresh, "Refresh", true, ""};
        out[std::size_t(n++)] = {Act::Details, a.more ? "Main page" : "Details", a.attached, "No charger"};
        out[std::size_t(n++)] = {Act::ShareCaps, "Share (QR)", a.caps_valid, "No caps yet"};
        break;
    case Tab::Cable:   // Refresh last: the cable's own items keep their place
        out[std::size_t(n++)] = {Act::ReadCable, "Read cable", true, ""};
        out[std::size_t(n++)] = {Act::SaveCable, "Save cable", a.cable_valid, "No cable read"};
        out[std::size_t(n++)] = {Act::SpoofAs, "Spoof as...", !a.inline_sink, kWhyLiveSink};
        out[std::size_t(n++)] = {Act::SavedCables, "Saved cables", true, ""};
        out[std::size_t(n++)] = {Act::ShareCable, "Share (QR)", a.cable_valid, "No cable read"};
        out[std::size_t(n++)] = {Act::Refresh, "Refresh", true, ""};
        break;
    case Tab::Sink:
        out[std::size_t(n++)] = {Act::Identify, "Identify", a.attached, "No device seen"};
        out[std::size_t(n++)] = {Act::Refresh, "Refresh", true, ""};
        break;
    }
    return n;
}

// ---- protocol line ----------------------------------------------------------------------------------
// A vendor SVID names the vendor PROTOCOL the partner speaks (never the chip): "Xiaomi PD", "OPPO PD".
// nullptr for every other SVID (standard ones, DisplayPort, Thunderbolt and unknown values are not protocols).
constexpr const char *protocol_name(std::uint16_t svid) {
    switch (svid) {
    case 0x2717: return "Xiaomi PD";
    case 0x22D9: return "OPPO PD";
    case 0x12D1: return "Huawei PD";
    case 0x2D95: return "Vivo PD";
    case 0x2E87: return "Cuktech PD";
    case 0x5FC9: return "UFCS";
    default:     return nullptr;
    }
}

// "Xiaomi PD, OPPO PD": the names only, each once, no hex. False (out left empty) when no SVID is a protocol.
template <std::size_t N>
bool protocol_text(std::span<const std::uint16_t> svids, StrBuf<N> &out) {
    out.clear();
    const char *seen[8] = {};
    int n = 0;
    for (const std::uint16_t sv : svids) {
        const char *nm = protocol_name(sv);
        if (!nm || n >= 8) continue;
        bool dup = false;
        for (int i = 0; i < n; i++) dup = dup || seen[i] == nm;
        if (dup) continue;
        seen[n++] = nm;
        if (out.view().size() + (n > 1 ? 2 : 0) + std::string_view(nm).size() > N - 1) break;
        if (n > 1) out.put(", ");
        out.put(nm);
    }
    return !out.view().empty();
}

// The ID row's value on the Sink tab: "VID:PID" always; the vendor name only when this session's
// Identify collected the answer (`asked`), and a "sniff " prefix when the identity was only heard
// on the wire - an inline K2 never asks, so the phone's answer to the charger's own Discover
// Identity is shown as sniffed. `val_cells` is the value column's width; the mark and the name are
// kept whole or left out, never cut.
template <std::size_t N>
void id_value(StrBuf<N> &out, int val_cells, bool asked, std::uint16_t vid, std::uint16_t pid, const char *vendor) {
    out.clear();
    if (!asked && out.view().size() + 6 + 9 <= static_cast<std::size_t>(val_cells)) out.put("sniff ");
    out.put_hex(vid, 4).put_char(':').put_hex(pid, 4);
    if (asked && vendor && vendor[0] &&
        out.view().size() + 1 + std::string_view(vendor).size() <= static_cast<std::size_t>(val_cells)) {
        out.put_char(' ');
        out.put(vendor);
    }
}

}  // namespace k2::conn
