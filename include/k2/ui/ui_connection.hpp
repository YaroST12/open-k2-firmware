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

constexpr Empty empty_state(Tab t, const Facts &f) {
    const bool direct = f.topo == Topology::DirectReceptacle, via_cable = f.topo == Topology::CableSingleCc;
    switch (t) {
    case Tab::Source:
        // Rp on a line with VBUS down: a charger that waits for a sink (the K2 is passive until a test starts).
        if (direct || via_cable) return {"CHARGER WAITING", "Charger waits for a sink: start a charger test."};
        return {"NO CHARGER", "Use the top or bottom port"};   // one line: the footer already states "No charger"
    case Tab::Sink:
        if (direct) return {"NO DEVICE", "Plug a device into the bottom port."};
        if (via_cable) return {"NO DEVICE", "Charger is on the bottom port: no device side in this setup."};
        return {"NO DEVICE", "Plug a device into the bottom port."};
    case Tab::Cable:
        if (direct) return {"NO CABLE", "Charger is plugged straight in (no cable)."};
        if (via_cable) return {"CABLE NOT READ", "OK -> Read cable (unplug the charger first)."};
        return {"NO CABLE READ", "Plug a cable into the bottom port, then OK -> Read cable."};
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

// ---- status bar -------------------------------------------------------------------------------------
struct StatusIn {
    Tab tab = Tab::Source;
    Facts f;
    std::string_view op;        // what the last action says ("Reading cable...", "Live sink: OK again"), "" = none
    bool spoof_armed = false;   // a saved cable is armed for the next charger test or attach
};

// The bottom bar: an action message wins; then the spoof note when a charger is already attached (the
// identity is presented at the next attach); else the state of the shown tab and the topology word.
template <std::size_t N>
void status_text(const StatusIn &s, StrBuf<N> &out) {
    out.clear();
    if (!s.op.empty()) { out.put(s.op); return; }
    if (s.spoof_armed && s.f.charger) { out.put("Armed, replug to apply"); return; }
    switch (s.tab) {
    case Tab::Source: out.put(s.f.charger ? "Charger" : "No charger"); break;
    case Tab::Cable:  out.put(s.f.cable ? "Cable read" : "No cable"); break;
    case Tab::Sink:   out.put(s.f.inline_sink ? "Device inline" : s.f.sink_seen ? "Sink seen on CC" : "No device"); break;
    }
    const char *w = topology_word(s.f.topo);
    if (w[0]) out.put(", ").put(w);
}

// ---- action pop-up ----------------------------------------------------------------------------------
enum class Act : std::uint8_t { Refresh, ShareCaps, ReadCable, SaveCable, SpoofAs, SavedCables, ShareCable, Identify };
struct Item {
    Act act = Act::Refresh;
    const char *label = "";
    bool enabled = true;
    const char *why = "";       // shown in the status bar when a greyed item is chosen
};
inline constexpr int kMaxItems = 5;

struct ActFacts {
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
        out[std::size_t(n++)] = {Act::ShareCaps, "Share (QR)", a.caps_valid, "No caps yet"};
        break;
    case Tab::Cable:
        out[std::size_t(n++)] = {Act::ReadCable, "Read cable", true, ""};
        out[std::size_t(n++)] = {Act::SaveCable, "Save cable", a.cable_valid, "No cable read"};
        out[std::size_t(n++)] = {Act::SpoofAs, "Spoof as...", !a.inline_sink, kWhyLiveSink};
        out[std::size_t(n++)] = {Act::SavedCables, "Saved cables", true, ""};
        out[std::size_t(n++)] = {Act::ShareCable, "Share (QR)", a.cable_valid, "No cable read"};
        break;
    case Tab::Sink:
        out[std::size_t(n++)] = {Act::Identify, "Identify", a.attached, "No device seen"};
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

}  // namespace k2::conn
