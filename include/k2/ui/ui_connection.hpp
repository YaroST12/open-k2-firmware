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

#include "k2/pd/cable_fact.hpp"
#include "k2/pd/cc_crossed.hpp"
#include "k2/pd/pd_rdseek.hpp"
#include "k2/pd/pd_survey.hpp"
#include "k2/pd/pd_tabs.hpp"
#include "k2/pd/pd_verdict.hpp"
#include "k2/pd/ra_guard.hpp"
#include "k2/ui/text/connection.hpp"
#include "k2/ui/ui_calc.hpp"
#include "k2/ui/ui_tools.hpp"

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
    // What the last read of a cable that gave no identity says (k2/pd/cable_fact.hpp): a chip is there but silent, no chip,
    // or nothing can be said. Only the empty Cable tab reads it.
    cablefact::Fact cable_fact = cablefact::Fact::NotRead;
    Topology topo = Topology::Unknown;
    raguard::WaitKind wait = raguard::WaitKind::Unknown;   // the one waiting classifier (pd::Link::wait_kind, the Rp trace)
    raguard::CrossKind cross = raguard::CrossKind::None;   // crossed CC through the K2 (pd::Link::cc_cross)
    // The idle "source asleep" evidence (pattern slow-rp-wake): the open (no-Rd) reading of the line that reads the
    // standby signature (fusb302::open_cc_mv, -1 = none), VBUS now (-1 = no monitor) and the Wake sleeping port
    // setting. The page fills them from the passive readers; nothing here drives CC.
    std::int32_t open_cc_mv = -1;
    std::int32_t vbus_mv = -1;
    bool wake_port_off = true;
};

// The idle "source asleep" verdict: an open CC line reading the standby signature (pd::standby_signature: >= 2.8 V
// and < 3.15 V, steady - RdSeek notes only a settled pair) with VBUS dark, no charger and no sink inline, and the
// Wake sleeping port off (no idle Rd hold is running). The sleeping source is there but answers only to an Rd:
// START or Refresh wakes it.
constexpr bool source_asleep(const Facts &f) {
    return !f.charger && !f.inline_sink && f.vbus_mv >= 0 && f.vbus_mv < raguard::kVbusAttachedMv &&
           pd::standby_signature(f.open_cc_mv) && f.wake_port_off;
}
namespace tx = text::conn;   // the page copy (include/k2/ui/text/connection.hpp)

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

// Empty state of a tab: the verdict band's word and the one line under it (text::conn, width-checked there).
struct Empty { const char *title; const char *hint; };

// The two ports share one CC network and one rail (docs/K2_PORTS_SPEC.md), so no text names one port as the device's.
// A K2 that is possibly inline (a live line it did not make: VBUS or a sink's Rd on the far side, no contract of its own)
// never says "no device" or "no charger": something is on the line, the K2 just cannot see it talk.
constexpr Empty empty_state(Tab t, const Facts &f) {
    const bool direct = f.topo == Topology::DirectReceptacle, via_cable = f.topo == Topology::CableSingleCc;
    switch (t) {
    case Tab::Source:
        if (f.inline_sink) return {tx::kLineInUse, tx::kLineInUseL};
        // Crossed CC (pd::Link::cc_cross): the two cables' CC wires land on different K2 lines, so neither far end finds a
        // partner. It outranks the waiting words, which would read the loaded Rp as a charger and the toggle as a phone.
        if (f.cross == raguard::CrossKind::Crossed) return {tx::kCrossed, tx::kCrossedL};
        if (f.cross == raguard::CrossKind::Hint) return {tx::kFlip, tx::kFlipL};
        // A sleeping source (pattern slow-rp-wake): an open line in the standby-signature window with VBUS dark
        // and no idle Rd hold. It outranks the waiting words: the standby pull-up also reads as an Rp to the
        // trace, but it never answers - only an Rd (START or Refresh) wakes it.
        if (source_asleep(f)) return {tx::kAsleep, tx::kAsleepL};
        // The one waiting classifier (f.wait, the Rp trace): a steady Rp = a charger waiting for a sink; a toggling
        // Rp = a dual-role port (a phone alone) looking for a partner, not a charger.
        if (f.wait == raguard::WaitKind::ChargerWaiting) return {tx::kWaiting, tx::kWaitingL};
        if (f.wait == raguard::WaitKind::DualRolePort) return {tx::kDualRole, tx::kDualRoleL};
        return {tx::kNoCharger, tx::kNoChargerL};
    case Tab::Sink:
        if (f.inline_sink) return {tx::kDevInUse, tx::kDevInUseL};
        if (direct) return {tx::kNoDevice, tx::kNoDevDirectL};
        if (via_cable) return {tx::kNoDevice, tx::kNoDevViaL};
        return {tx::kNoDevice, tx::kNoDevL};
    case Tab::Cable:
        if (f.inline_sink) return {tx::kCableNotRead, tx::kCableInUseL};
        // A silent chip is not "no e-marker": its pull-down is on the cable, it only did not answer alone (it answers once a
        // source powers it). No pull-down at all means no chip. Both come from the Ra probe of an alone read (pull_down()).
        if (f.cable_fact == cablefact::Fact::ChipSilent)
            return {tx::kChipSilent, tx::kChipSilentL};
        if (f.cable_fact == cablefact::Fact::NoChip)
            return {tx::kNoChip, tx::kNoChipL};
        if (direct) return {tx::kNoCable, tx::kNoCableL};
        if (via_cable) return {tx::kCableNotRead, tx::kCableViaL};
        return {tx::kNoCableRead, tx::kNoCableReadL};
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
    case pd::SvState::NotAsked: return "Not asked";
    case pd::SvState::Pending: return "Asking...";
    case pd::SvState::Got: return "";
    case pd::SvState::NotSupported: return "Not supported";
    case pd::SvState::NoAnswer: return "No answer";
    case pd::SvState::Blocked:
        switch (e.why) {
        case pd::SvWhy::Inline: return "Inline: blocked";
        case pd::SvWhy::ChargerTest: return "Charger test";
        case pd::SvWhy::XiaomiChain: return "Xiaomi chain";
        case pd::SvWhy::Pd20: return "Not in PD 2.0";
        case pd::SvWhy::NoEpr: return "No EPR bit";
        case pd::SvWhy::None: break;
        }
        return "Blocked";
    }
    return "";
}

// A field of the open tab was never asked: the fact, per tab (no key name; the Refresh item reads it).
constexpr const char *unread_text(Tab t) {
    return t == Tab::Source ? tx::kSourceUnread : t == Tab::Cable ? tx::kCableUnread : tx::kSinkUnread;
}
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

// The state of each tab in footer words, lower case (the footer capitalises the first): what an empty tab's footer says
// about the other two.
constexpr const char *source_word(const Facts &f) {
    return f.charger ? "charger" : f.wait == raguard::WaitKind::ChargerWaiting ? "charger waiting"
           : f.wait == raguard::WaitKind::DualRolePort ? "dual-role port" : "no charger";
}
constexpr const char *cable_word(const Facts &f) {
    return f.cable ? "cable read" : f.cable_fact == cablefact::Fact::ChipSilent ? "chip silent"
           : f.cable_fact == cablefact::Fact::NoChip ? "no e-marker" : "no cable read";
}
constexpr const char *sink_word(const Facts &f) {
    return f.inline_sink ? "device inline" : f.sink_seen ? "sink on CC" : "no device";
}

// ---- status bar -------------------------------------------------------------------------------------
struct StatusIn {
    Tab tab = Tab::Source;
    Facts f;
    std::string_view op;        // what the last action says ("Reading cable...", "Live sink: OK again"), "" = none
    bool spoof_armed = false;   // a saved cable is armed for the next charger test or attach
    const pd::Survey *sv = nullptr;   // the Refresh survey (nullptr: none, the old status only)
    std::string_view diag;      // the PD diagnosis verdict of the session (pd::diag::screen::verdict), "" = no PD session
    bool no_pd = false;         // the tab shows the no-PD body: there are no caps to read, so no "not read yet"
};

// The bottom bar: an action message wins; then the spoof note when a charger is already attached (the
// identity is presented at the next attach); then a running survey's progress; then, on the Source tab, the PD diagnosis
// verdict of an open session; then the rest of the survey (the hint while a field of the open tab was never asked, the
// summary after a run); else the state of the shown tab and the topology word.
template <std::size_t N>
void status_text(const StatusIn &s, StrBuf<N> &out) {
    out.clear();
    if (!s.op.empty()) { out.put(s.op); return; }
    if (s.spoof_armed && s.f.charger) { out.put(tx::kArmedBar); return; }
    if (s.sv && s.sv->running()) { out.put("Reading ").put_u32(s.sv->resolved()).put_char('/').put_u32(s.sv->planned()); return; }
    if (s.tab == Tab::Source && !s.diag.empty()) { out.put(s.diag); return; }
    if (s.sv) {
        if (!s.no_pd && !tab_empty(s.tab, s.f) && tab_unasked(s.tab, *s.sv)) { out.put(unread_text(s.tab)); return; }
        if (s.sv->ran() && s.tab != Tab::Sink && !tab_empty(s.tab, s.f)) { read_summary(*s.sv, out); return; }
    }
    // An empty tab's band already names its own state: the footer never repeats it (user, 2026-11-12) and states what the
    // other two tabs hold instead, the nearer one first ("Charger, no device"), the second only while it fits.
    if (tab_empty(s.tab, s.f)) {
        const char *a = s.tab == Tab::Source ? sink_word(s.f) : source_word(s.f);
        const char *b = s.tab == Tab::Cable ? sink_word(s.f) : cable_word(s.f);
        out.put_char(static_cast<char>(a[0] - ('a' - 'A'))).put(a + 1);
        if (std::string_view(a).size() + 2 + std::string_view(b).size() <= kStatusCells) out.put(", ").put(b);
        return;
    }
    out.put(s.tab == Tab::Source ? "Charger" : s.tab == Tab::Cable ? "Cable read" : s.f.inline_sink ? "Device inline" : "Sink seen on CC");
    const char *w = topology_word(s.f.topo);
    if (w[0]) out.put(", ").put(w);
}

// ---- action pop-up ----------------------------------------------------------------------------------
// Bench: a place page's tool (bench::actions of the place, `arg` its index there); the page fills its readiness.
enum class Act : std::uint8_t { Refresh, ShareCaps, ReadCable, SaveCable, SpoofAs, SavedCables, ShareCable, Identify, Details, Bench };
struct Item {
    Act act = Act::Refresh;
    const char *label = "";
    bool enabled = true;
    const char *why = "";       // shown in the status bar when a greyed item is chosen
    int8_t arg = -1;            // Act::Bench: the index into bench::actions(place)
    bool caution = false;       // usable, but the footer names a caution (a Bench tool's readiness)
    const char *tag = "";       // the row's tail word (a Bench tool's readiness tag), "" = none
};
inline constexpr int kMaxItems = 8;

struct ActFacts {
    bool more = false;          // the Source tab shows its second page (Details): the item goes back
    bool inline_sink = false;   // a live sink is inline: active tools that would reach it are refused
    bool attached = false;      // a charger or a device answers on a port
    bool alone = false;         // a device is alone on the K2 (dual-role port, VBUS dark, no caps): active Identify is offered
    bool caps_valid = false;    // Source_Capabilities received
    bool cable_valid = false;   // e-marker data held
    bool pd_none = false;       // the settled verdict is "no PD this attach": a no-PD device cannot be identified
};

// The strip verb of an action (kOkVerbs): a single tool's OK names the action it runs.
constexpr const char *act_verb(Act a, bool more) {
    switch (a) {
    case Act::Refresh: return "Refresh";
    case Act::Details: return more ? "Cancel" : "View";
    case Act::ShareCaps: case Act::ShareCable: return "Share";
    case Act::ReadCable: return "Read";
    case Act::SaveCable: return "Save";
    case Act::SpoofAs: case Act::SavedCables: case Act::Bench: return "Open";
    case Act::Identify: return "Ask";
    }
    return "";
}

inline constexpr const char *kWhyLiveSink = "Live sink: unplug load";
// The no-PD gate's own refusal word (fits the 24-cell pop-up "why"): the same sentence the tab shows.
inline constexpr const char *kWhyNoPd = "No PD from this device";

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
    case Tab::Sink: {
        // No PD this attach: a device that never talks PD cannot be identified - refuse with the same
        // words the tab shows. The alone path (a dual-role phone alone) is PD-capable and stays offered.
        const bool seen = a.attached || a.alone;
        const bool id_ok = seen && !(a.pd_none && !a.alone);
        out[std::size_t(n++)] = {Act::Identify, "Identify", id_ok, !id_ok && seen ? kWhyNoPd : "No device seen"};
        out[std::size_t(n++)] = {Act::Refresh, "Refresh", true, ""};
        break;
    }
    }
    return n;
}

// ---- place pages (Fieldbook CONCEPT 8): Charger = Source, Cable = Cable, Device = Sink --------------------------------
constexpr bench::Place place_of(Tab t) {
    return t == Tab::Source ? bench::Place::Charger : t == Tab::Cable ? bench::Place::Cable : bench::Place::Device;
}

// A place page's items: out[0] is the page's own read (OK while no row is focused: read again, read the e-marker, ask the
// device), out[1..] its rows in order - the cable's own actions first, then the tools of the place (bench::actions, Act::Bench
// with their index; the page fills their readiness), the charger's and the device's read again last. Share is long OK, so it
// has no row; nothing opens a second page.
inline int place_actions(Tab t, const ActFacts &a, std::array<Item, kMaxItems> &out) {
    int n = 0;
    auto put = [&](const Item &it) { if (n < kMaxItems) out[std::size_t(n++)] = it; };
    const Item again{Act::Refresh, "Read again", true, ""};
    const Item read{Act::ReadCable, "Read e-marker", true, ""};
    const bool seen = a.attached || a.alone;
    const bool id_ok = seen && !(a.pd_none && !a.alone);
    const Item ask{Act::Identify, "Ask device", id_ok, !id_ok && seen ? kWhyNoPd : "No device seen"};
    put(t == Tab::Source ? again : t == Tab::Cable ? read : ask);
    if (t == Tab::Cable) {
        put(read);
        put({Act::SaveCable, "Save cable", a.cable_valid, "No cable read"});
        put({Act::SavedCables, "Saved cables", true, ""});
        put({Act::SpoofAs, "Spoof as...", !a.inline_sink, kWhyLiveSink});
    }
    if (t == Tab::Sink) put(ask);
    const bench::Acts b = bench::actions(place_of(t));
    for (int i = 0; i < b.n; i++) put({Act::Bench, b.a[i].name, true, "", static_cast<int8_t>(i)});
    if (t != Tab::Cable) put(again);
    return n;
}

// ---- the no-PD body (ROADMAP 2026-11-10, Row C) --------------------------------------------------------
// With the settled verdict "no PD" the tab drops its placeholder rows and says so in one short message,
// then keeps only the 2-3 rows that are actually measured. The words fit the 240x240 kit (wrapped at 26
// cells, no truncation in any rotation - tests/test_ui_connection.cpp checks the wrap).
// The band word is tx::kNoPd on both tabs; the line says which case it is.
constexpr const char *no_pd_line(Tab t, pd::PdNoneWhy w) {
    return t == Tab::Sink && w == pd::PdNoneWhy::SinkSilentSourceOffered ? tx::kNoPdUnansweredL : tx::kNoPdNeverL;
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
