// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Host stand-ins for the drivers the real Ui and capture code call (FUSB302, power monitor, USB,
// SysTick), so tests can drive src/ui/ui.cpp and src/ui/capture.cpp unchanged. They share the k2::
// signatures of the real drivers; a signature change breaks this file at compile time.
#include <memory>
#include "ramdisk.hpp"
#include "k2/core/fs.hpp"
#include "ui_stubs.h"
#include "k2/core/bbpersist.hpp"
#include "k2/chargertest/charger_journal_hw.hpp"
#include "k2/fastcharge/fctrig.hpp"
#include "k2/pd/link.hpp"
#include "k2/pd/power_source.hpp"
#include "k2/chargertest/report_card_hw.hpp"
#include "k2/core/scratch.hpp"
#include "k2/core/rd_comeback.hpp"
#include "k2/core/rescan.hpp"
#include "k2/core/survey.hpp"
#include "k2/twin/twin_hw.hpp"
#include "k2/pdstress/journal_hw.hpp"
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include "k2/drivers/systick.hpp"
#include "k2/drivers/usb.hpp"

StubState g_stub;

namespace k2 {

extern const char kGitHash[] = "v1-test-3-gabc1234";

namespace systick {
uint32_t millis() { return g_stub.ms++; }
uint32_t micros() { return g_stub.ms * 1000; }
void delay_ms(uint32_t ms) { g_stub.ms += ms; g_stub.delays_ms += ms; if (g_stub.delay_hook) g_stub.delay_hook(); }
}  // namespace systick

namespace link {
void set_inline_mode(bool on) { g_stub.link_inline = on; }
Reason inline_now() {
    Probe p;
    p.pd_active = g_stub.caps.valid && g_stub.caps.active_cc != 0;
    p.cc_active = g_stub.caps.active_cc != 0;
    p.sticky_inline = g_stub.link_inline;
    p.vbus_known = p.current_known = g_stub.pm_online;
    p.vbus_mv = g_stub.pm.vbus_mv;
    p.current_ma = g_stub.pm.current_ma;
    return inline_reason(p);
}
}  // namespace link

namespace fusb302 {
const pd::SourceCaps& caps() { return g_stub.caps; }
std::uint32_t rx_count() { return g_stub.caps.rx_count; }
const pd::Emarker& emarker() { return g_stub.em; }
const pd::CableLimits& physical_cable() { return g_stub.cable_limits; }
const pd::Revision& source_revision() { return g_stub.source_revision; }
uint8_t device_id() { return 0x92; }
uint8_t active_cc() { return g_stub.cc; }
bool self_powered() { return g_stub.self_powered || g_stub.pin != 0; }
std::uint8_t pin_rd() { return g_stub.pin != 0 ? g_stub.pin : (g_stub.self_powered ? 2 : 0); }
LineTrial line_trial() { return {g_stub.trial_line, g_stub.trial_moves, g_stub.trial_live}; }
bool rd_inherited() { return g_stub.rd_inherited; }
void self_power_release() { g_stub.rd_releases++; g_stub.rd_inherited = false; }
const pd::VdmInfo& vdm() { return g_stub.vdm; }
const pd::VdmInfo& sink_vdm() { return g_stub.sink_vdm; }
const pd::AppleInfo& apple_info() { return g_stub.apple; }
bool apple_armed() { return g_stub.apple.valid && g_stub.apple.why == pd::AppleWhy::Running; }
bool answers_sop() { return true; }
usblink::Link usb_link(std::int32_t, std::int32_t) { return g_stub.ul; }
bool cable_thunderbolt() { return g_stub.cable_tb; }
void rescan() { g_stub.rescans++; }
void set_inline(bool on) { g_stub.inl = on; }
bool inline_mode() { return g_stub.inl; }
void set_wait_kind(k2::raguard::WaitKind kind) { g_stub.wait_kind = kind; }
k2::raguard::WaitKind wait_kind() { return g_stub.wait_kind; }
bool caps_this_attach() { return g_stub.caps_this_attach; }
bool sink_cc_far() { return g_stub.far_cc; }
void new_source() { g_stub.new_sources++; }
void clear_source() { g_stub.clear_sources++; g_stub.caps.num_pdos = 0; }
bool cc_detach() { g_stub.cc_detaches++; return true; }
const pd::Contract& contract() { return g_stub.contract; }
std::uint32_t request_period_ms() { return g_stub.req_period_ms; }
std::uint32_t typec_ma() { return g_stub.typec_ma_val; }
bool pd_traffic() { return g_stub.pd_traffic; }
bool pd_seen_attach() { return g_stub.pd_seen_attach; }
pd::OverLoadFacts over_load_facts() { return g_stub.ol_facts; }
void legacy_only(bool) {}
bool ra_set(bool on) {
    if (on == g_stub.ra_on) return true;
    if (on && (g_stub.ra_refuse || g_stub.inl)) return false;
    g_stub.ra_on = on; (on ? g_stub.ra_ons : g_stub.ra_offs)++;
    g_stub.force_line = on ? 2 : g_stub.force_line;   // like the firmware: the attach scan is forced to CC2, only set_force_line(0) frees it
    return true;
}
bool ra_active() { return g_stub.ra_on; }
emkra::Refuse ra_refusal() { return g_stub.inl ? emkra::Refuse::Inline : g_stub.ra_refuse ? emkra::Refuse::Contract : emkra::Refuse::None; }
void ra_run_own() { if (g_stub.ra_on) g_stub.ra_owned++; }   // a run takes the Ra mode over (it survives the unit detaches)
void set_force_line(std::uint8_t line) { g_stub.force_line = line; }
void set_idle_wake(bool on) { g_stub.idle_wake = on; }
bool idle_wake() { return g_stub.idle_wake; }
void set_run_owner(bool on) { g_stub.run_owner = on; }
bool run_owner() { return g_stub.run_owner; }
void rd_glitch_begin() {
    if (g_stub.ra_ons_at_glitch < 0) { g_stub.ra_ons_at_glitch = g_stub.ra_ons; g_stub.ra_on_at_glitch = g_stub.ra_on; }
    g_stub.glitch_begins++;
}
void rd_glitch_end() { g_stub.glitch_ends++; }
static void set_rd(bool on) {
    if (on != g_stub.rd_run) {
        if (on && g_stub.ra_at_first_rd < 0) g_stub.ra_at_first_rd = g_stub.ra_on ? 1 : 0;
        g_stub.rd_run = on; g_stub.rd_run_changes++;
    }
}
bool rd_for_run(bool on) {
    if (on && g_stub.inl) return false;
    if (!on) ra_set(false);   // the Ra mode goes with the run, as in src/drivers/fusb302.cpp
    set_rd(on);
    return true;
}
void rd_for_run_pause() { set_rd(false); g_stub.rd_pauses++; }   // keeps the Ra mode, like the firmware's pause
// A detach drops the source's output, an attach brings it back with a 5 V contract (model_units, on by default), so a unit
// takes the 300 ms dark hold of the real thing instead of the 10 s bound a source that never goes dark would cost.
void unit_detach() {
    set_rd(false); g_stub.unit_detaches++; g_stub.legacy_attach = false;   // a unit detach keeps the Ra mode of a run
    if (g_stub.model_units) {
        if (g_stub.pm.vbus_mv > 0) g_stub.unit_vbus_mv = g_stub.pm.vbus_mv;
        g_stub.pm.vbus_mv = 0; g_stub.sink = {};
    }
}
void unit_attach(bool legacy) {
    if (!g_stub.inl) set_rd(true);
    g_stub.unit_attaches++; g_stub.legacy_attach = legacy;
    if (g_stub.model_units) {
        g_stub.pm.vbus_mv = g_stub.unit_vbus_mv;
        if (!legacy) { g_stub.sink.state = k2::pd::SinkState::Ready; g_stub.sink.result = k2::pd::SinkResult::Ok; g_stub.sink.mv = 5000; g_stub.sink.pos = 1; }
    }
}
// A self-powered unit boundary: Rd and VBUS untouched; a Pd hold brings the 5 V contract back at once.
void unit_hold(bool legacy) {
    g_stub.unit_holds++; g_stub.legacy_attach = legacy;
    if (g_stub.model_units && !legacy) { g_stub.sink.state = k2::pd::SinkState::Ready; g_stub.sink.result = k2::pd::SinkResult::Ok; g_stub.sink.mv = 5000; g_stub.sink.pos = 1; }
}
bool rd_for_run_on() { return g_stub.rd_run; }
bool reread_own_hold() { return g_stub.reread_hold; }
void reread_take_over() { g_stub.reread_take_overs++; }
std::uint8_t idle_wake_line() { return g_stub.idle_wake_hold; }
std::int32_t open_cc_mv(std::uint8_t line) { return line >= 1 && line <= 2 ? g_stub.open_cc_mv[line - 1] : -1; }
bool rp_on_line() { return false; }
void attach_scan() {}
void service() {
    if (g_stub.rd_run && g_stub.vbus_on_rd) { g_stub.pm.vbus_mv = 5000; g_stub.pm_after_rd = true; }
}
// A Legacy attach that never falls back (pd_chatter): the charger keeps talking PD, so the unit ends as "blocked by PD".
std::uint32_t pd_quiet_ms() { return g_stub.pd_chatter && g_stub.legacy_attach ? 0 : UINT32_MAX; }
pd::TriggerRefusal trigger_refusal() { return g_stub.trig_refusal; }
const pd::SinkCapsSeen& sink_caps() { return g_stub.sink_seen; }
void feed(uint8_t, uint16_t, std::span<const uint8_t>) {}
pd::Err sink_request(const pd::Choice& c, bool confirmed, bool over_load) {
    g_stub.requests++;
    g_stub.last_choice = c;
    g_stub.last_confirmed = confirmed;
    g_stub.last_over_load = over_load;
    if (g_stub.vbus_ceiling_mv > 0 && g_stub.request_err == pd::Err::Ok && c.pos >= 1 && c.pos <= g_stub.caps.num_pdos) {
        const std::int32_t want = c.mv ? c.mv : g_stub.caps.pdos[c.pos - 1].max_mv;
        g_stub.pm.vbus_mv = g_stub.pm_fast_mv = std::min(want, g_stub.vbus_ceiling_mv);
        g_stub.pm_fast_set = true;
        g_stub.sink.state = pd::SinkState::Ready; g_stub.sink.result = pd::SinkResult::Ok;   // the contract is the one asked for
        g_stub.sink.pos = c.pos; g_stub.sink.mv = static_cast<std::uint16_t>(want); g_stub.sink.held = true;
    }
    return g_stub.request_err;
}
void sink_release() {
    g_stub.releases++;
    if (g_stub.vbus_ceiling_mv > 0) {
        g_stub.pm.vbus_mv = g_stub.pm_fast_mv = 5000; g_stub.pm_fast_set = true;
        g_stub.sink.pos = 1; g_stub.sink.mv = 5000; g_stub.sink.held = false;
    }
}
bool sink_hard_reset(bool) { g_stub.hard_resets++; return true; }
bool sink_soft_reset() { g_stub.ps_soft_resets++; return g_stub.ps_soft_reset_ok; }
void sink_set_cap(std::uint32_t mv) { g_stub.sink_cap = mv; }
void sink_vbus(std::int32_t mv) { g_stub.sink_vbus = mv; }
const pd::SinkStatus& sink_status() { return g_stub.sink; }
const pd::XiaomiTrace& xiaomi_trace() { return g_stub.xm; }
pd::Err epr_info_request(bool force) { g_stub.epr_info_asks++; g_stub.epr_info_forced = force; return g_stub.epr_info_err; }
const pd::SourceCaps& epr_info_list() { return g_stub.epr_info_caps; }
bool get_source_caps(bool confirmed) {
    if (g_stub.inl) return false;   // the real driver refuses inline, confirmed or not
    g_stub.getcaps++; g_stub.last_confirmed = confirmed; return true;
}
pd::Err refresh_source_caps() { g_stub.refresh_asks++; return g_stub.refresh_err; }
std::uint32_t caps_count() { return g_stub.caps.rx_count; }
bool legacy_only_active() { return g_stub.legacy_active; }
bool identify_start(bool confirmed) {
    if (g_stub.inl) return false;   // the real driver refuses inline even with a confirm
    g_stub.identifies++; g_stub.last_confirmed = confirmed; return g_stub.identify_ok;
}
void identify_abort() {}
bool probe_start() { g_stub.probes++; return g_stub.probe_ok; }
void probe_abort() {}
const pd::ProbeStatus& probe_status() { return g_stub.probe; }
const pd::ExtInfo& ext() { return g_stub.ext; }
const pd::IdStatus& identify_status() { return g_stub.ident; }
bool identify_current() { return g_stub.ident_current; }
bool read_emarker(bool confirmed, pd::EmarkerMode mode) {
    g_stub.scans++;
    g_stub.scan_confirmed = confirmed;
    g_stub.last_scan_mode = mode;
    if (g_stub.on_scan) g_stub.on_scan();
    return g_stub.scan_result;
}
std::uint32_t cable_seq() { return g_stub.cable_seq; }
std::uint32_t session() { return g_stub.session; }
void sink_cc(std::int32_t, std::int32_t) {}
}  // namespace fusb302

namespace rdcomeback {
bool pending() { return false; }
const rdintent::Intent& found() { return g_stub.rdc_found; }
bool planned_reattach() { return rdintent::planned_reattach(g_stub.rdc_found.kind); }
void set_run_cable(ctcable::Choice cable) { g_stub.rdc_run_cable = cable; g_stub.rdc_run_sets++; }
void drop_identity() { g_stub.rdc_drops++; }
bool begin_reattach(ctcable::Choice cable) {
    g_stub.rdc_begins++;
    g_stub.rdc_begin_cable = cable;
    return g_stub.rdc_begin_ok;
}
void end_reattach() { g_stub.rdc_ends++; }
bool begin_rescan() { g_stub.rdc_rescan_begins++; return g_stub.rdc_rescan_ok; }
bool rescan_landing() { return g_stub.rdc_rescan_landing; }
}  // namespace rdcomeback

namespace rescan {
// The page's Refresh goes through here; the stub answers what the test set and counts the asks (the real one decides on the
// sink, the link and the marker: tests/test_rescan_bridge.cpp).
Result request() {
    g_stub.rescan_requests++;
    if (g_stub.rescan_result == Result::Passive || g_stub.rescan_result == Result::Cycled) g_stub.rescans++;
    return g_stub.rescan_result;
}
void poll() {}
bool active() { return false; }
}  // namespace rescan

namespace survey {
// The page's Refresh starts the survey and the Connection page reads its table; the real runner is core/survey.cpp.
void on_refresh(rescan::Result r, bool) { g_stub.survey_refreshes++; g_stub.survey_last = r; }
void poll(bool) {}
const pd::Survey& state() { return g_stub.survey; }
bool running() { return g_stub.survey.running(); }
}  // namespace survey

namespace bbpersist {
void hold_run() { g_stub.bb_holds++; }
void make_room() { g_stub.bb_make_rooms++; }
Status status() {
    Status s;
    s.ready = true;
    s.free_bytes = 64 * 1024;
    return s;
}
}  // namespace bbpersist

namespace storage {
emlib::Result lib_save(const emrec::Record& rec, const emk_quirk::Quirks& quirks) {
    g_stub.lib_saves++;
    if (g_stub.lib_err != emlib::Err::Ok) return {g_stub.lib_err, 0, false};
    for (const emlib::Entry& e : g_stub.lib)
        if (e.rec == rec) return {emlib::Err::Ok, e.seq, true};
    emlib::Entry e;
    e.seq = static_cast<std::uint16_t>(g_stub.lib.empty() ? 1 : g_stub.lib.back().seq + 1);
    e.rec = rec;
    if (!quirks.empty()) g_stub.lib_quirks.emplace_back(e.seq, quirks);
    const char nm[] = "Test-USB2-60W";
    for (std::size_t i = 0; i < sizeof nm; i++) e.name[i] = nm[i];
    g_stub.lib.push_back(e);
    return {emlib::Err::Ok, e.seq, false};
}
emlib::Result lib_save_current() {
    const pd::Emarker& em = g_stub.em;
    if (!em.valid || em.num_vdos < 5 || (em.spec_rev != 1 && em.spec_rev != 2))
        return {emlib::Err::Invalid, 0, false};
    return lib_save(emrec::make({em.raw_vdos.data(), 5}, em.spec_rev), {});
}
unsigned lib_list(std::span<std::uint16_t> out) {
    unsigned n = 0;
    for (const emlib::Entry& e : g_stub.lib) if (n < out.size()) out[n++] = e.seq;
    return n;
}
bool lib_get(std::uint16_t seq, emlib::Entry& out) {
    for (const emlib::Entry& e : g_stub.lib) if (e.seq == seq) { out = e; return true; }
    return false;
}
bool lib_get(std::uint16_t seq, emlib::Entry& out, emk_quirk::Quirks& quirks) {
    quirks = {};
    for (const auto& p : g_stub.lib_quirks) if (p.first == seq) quirks = p.second;
    return lib_get(seq, out);
}
emlib::Err lib_remove(std::uint16_t seq) {
    g_stub.lib_removes++;
    if (g_stub.lib_err != emlib::Err::Ok) return g_stub.lib_err;
    for (std::size_t i = 0; i < g_stub.lib.size(); i++)
        if (g_stub.lib[i].seq == seq) { g_stub.lib.erase(g_stub.lib.begin() + static_cast<long>(i)); return emlib::Err::Ok; }
    return emlib::Err::NotFound;
}
emlib::Err lib_rename(std::uint16_t, std::string_view) { return emlib::Err::Ok; }
uint32_t generation() { return static_cast<uint32_t>(g_stub.lib_saves + g_stub.lib_removes + g_stub.twin_gen); }

// The device twin library: g_stub.twins, numbered like the real one (numbers are kept when another twin goes).
unsigned twin_list(std::span<std::uint8_t> out) {
    unsigned n = 0;
    for (const auto& p : g_stub.twins) if (n < out.size()) out[n++] = p.first;
    return n;
}
bool twin_get(std::uint8_t seq, twin::Twin& out) {
    for (const auto& p : g_stub.twins) if (p.first == seq) { out = p.second; return true; }
    return false;
}
twin::Result twin_save(const twin::Twin& in) {
    g_stub.twin_gen++;
    const std::uint8_t seq = static_cast<std::uint8_t>(g_stub.twins.empty() ? 1 : g_stub.twins.back().first + 1);
    g_stub.twins.emplace_back(seq, in);
    twin::Result r;
    r.seq = seq;
    return r;
}
twin::Err twin_remove(std::uint8_t seq) {
    for (std::size_t i = 0; i < g_stub.twins.size(); i++)
        if (g_stub.twins[i].first == seq) { g_stub.twins.erase(g_stub.twins.begin() + static_cast<long>(i)); g_stub.twin_gen++; return twin::Err::Ok; }
    return twin::Err::NotFound;
}
twin::Err twin_rename(std::uint8_t, std::string_view) { return twin::Err::Ok; }
}  // namespace storage

}  // namespace k2

// The two twin test hooks are global, like stub_cards_reset.
std::uint8_t stub_twin_add(const char *name, std::initializer_list<std::uint32_t> sink_pdos, std::uint16_t vid, std::uint16_t pid) {
    k2::twin::Twin t;
    for (std::size_t i = 0; name[i] && i < k2::twin::kNameMax; i++) t.name[i] = name[i];
    for (std::uint32_t w : sink_pdos) if (t.num_pdos < t.pdos.size()) t.pdos[t.num_pdos++] = w;
    t.vid = vid;
    t.pid = pid;
    const std::uint8_t seq = static_cast<std::uint8_t>(g_stub.twins.empty() ? 1 : g_stub.twins.back().first + 1);
    g_stub.twins.emplace_back(seq, t);
    g_stub.twin_gen++;
    return seq;
}

namespace k2 {
namespace twin_hw {
namespace {
// The live proof, simulated: the REAL twin::live::Runner over a charger that answers with g_stub.caps (grants the Request, or
// rejects it when g_stub.twin_reject), so the page sees genuine slots and phases. Events fire on the caller's clock.
enum class Ev : std::uint8_t { None, Dark, Attach, Reply, Release };
struct StubTwinEnv final : twin::live::Env {
    twin::live::Runner run;
    pd::SinkStatus sink;
    int vbus = 5000;
    std::uint32_t seq = 1, now = 0, ev_at = 0;
    Ev ev = Ev::None;
    pd::Choice last{};

    StubTwinEnv() { sink.state = pd::SinkState::Ready; sink.pos = 1; }
    bool load(std::uint8_t n, twin::Twin& out) override { return storage::twin_get(n, out); }
    void impersonate(const twin::Twin*) override {}
    void detach() override { sink.state = pd::SinkState::Idle; ev = Ev::Dark; ev_at = now + 100; }
    void attach() override { ev = Ev::Attach; ev_at = now + 200; }
    void hold() override { ev = Ev::Attach; ev_at = now + 200; }
    pd::Err request(pd::Choice c) override {
        last = c;
        sink.state = pd::SinkState::Requesting;
        sink.result = pd::SinkResult::None;
        ev = Ev::Reply; ev_at = now + 100;
        return pd::Err::Ok;
    }
    void release() override { sink.state = pd::SinkState::Transition; ev = Ev::Release; ev_at = now + 100; }
    void record(std::uint8_t, const twin::live::Slot&) override {}

    void fire(std::uint32_t t) {
        now = t;
        if (ev == Ev::None || now < ev_at) return;
        const Ev e = ev;
        ev = Ev::None;
        switch (e) {
        case Ev::Dark: vbus = 0; break;
        case Ev::Attach: seq++; sink.state = pd::SinkState::Ready; sink.pos = 1; sink.result = pd::SinkResult::None; vbus = 5000; break;
        case Ev::Reply:
            sink.state = pd::SinkState::Ready;
            if (g_stub.twin_reject) { sink.pos = 1; sink.result = pd::SinkResult::Rejected; break; }
            sink.pos = last.pos;
            vbus = last.mv != 0 ? int(last.mv) : int(g_stub.caps.pdos[last.pos - 1].max_mv);
            break;
        case Ev::Release: sink.state = pd::SinkState::Ready; sink.pos = 1; sink.result = pd::SinkResult::None; vbus = 5000; break;
        case Ev::None: break;
        }
    }
    twin::live::Input input(const LiveCtx& c) const {
        twin::live::Input in;
        in.now = c.now;
        in.vbus_mv = vbus;
        in.caps = &g_stub.caps;
        in.caps_seq = seq;
        in.sink = sink;
        in.cap_mv = 36000;
        in.busy = c.busy;
        in.inline_link = c.inline_link;
        return in;
    }
};
StubTwinEnv g_sim;
}  // namespace

bool predict(const twin::Twin& t, twin::Outcome& out) {
    if (!g_stub.caps.valid) return false;
    out = twin::predict(t, g_stub.caps, twin::Facts{});
    return true;
}
std::uint32_t predict_stamp() {
    std::uint32_t h = g_stub.caps.valid ? 2166136261u : 0u;
    for (std::size_t i = 0; g_stub.caps.valid && i < g_stub.caps.num_pdos; i++) h = (h ^ g_stub.caps.pdos[i].raw) * 16777619u;
    return h + g_stub.twin_stamp;
}
twin::live::Refusal live_start(std::span<const std::uint8_t> seqs, bool confirmed, const LiveCtx& ctx) {
    if (g_stub.twin_refusal != twin::live::Refusal::None) return g_stub.twin_refusal;
    g_sim.now = ctx.now;
    const twin::live::Refusal r = g_sim.run.start(g_sim, seqs, g_sim.input(ctx), confirmed);
    if (r == twin::live::Refusal::None) {
        g_stub.twin_starts++;
        g_stub.twin_start_seqs.assign(seqs.begin(), seqs.end());
        g_stub.twin_start_confirmed = confirmed;
    }
    return r;
}
bool live_running() { return g_sim.run.running(); }
void live_poll(const LiveCtx& ctx) {
    if (!g_sim.run.running()) return;
    g_sim.fire(ctx.now);
    g_sim.run.poll(g_sim.input(ctx));
}
void live_cancel() {
    if (!g_sim.run.running()) return;
    g_stub.twin_cancels++;
    g_sim.run.cancel();
}
const twin::live::Runner& live() { return g_sim.run; }
Learned learn_from_identify(std::string_view name, bool save) {
    g_stub.twin_learns++;
    Learned r = g_stub.twin_learned;
    if (r.how != Learn::Ok || !save) return r;
    if (!name.empty()) { r.twin.name = {}; for (std::size_t i = 0; i < name.size() && i < twin::kNameMax; i++) r.twin.name[i] = name[i]; }
    r.store = storage::twin_save(r.twin);
    return r;
}
}  // namespace twin_hw
}  // namespace k2

void stub_twin_reset() {
    k2::twin_hw::g_sim = k2::twin_hw::StubTwinEnv();
    g_stub.twins.clear();
}

namespace k2 {

namespace emk_spoof {
Err arm(const emrec::Record& rec, const emk_quirk::Quirks& quirks) {
    g_stub.spoof_arms++;
    if (!emrec::valid(rec)) return Err::Invalid;
    if (g_stub.inl) return Err::Inline;
    if (g_stub.spoof_err == Err::Ok) {
        g_stub.spoof_on = true; g_stub.spoof_record = rec; g_stub.spoof_quirks = quirks; ++g_stub.spoof_generation;
    }
    return g_stub.spoof_err;
}
void disarm() {
    g_stub.spoof_disarms++;
    if (g_stub.spoof_on) ++g_stub.spoof_generation;
    g_stub.spoof_on = false;
}
bool active() { return g_stub.spoof_on; }
bool matches(const emrec::Record& rec, const emk_quirk::Quirks& quirks) {
    return g_stub.spoof_on && rec == g_stub.spoof_record && quirks == g_stub.spoof_quirks;
}
std::uint32_t generation() { return g_stub.spoof_generation; }
std::uint32_t replies() { return 0; }
std::uint32_t requests() { return g_stub.spoof_requests; }
std::uint32_t unmatched() { return 0; }
const emk_quirk::SeenLog& seen() { static const emk_quirk::SeenLog none; return none; }
}  // namespace emk_spoof

namespace power {
Chip chip() { return g_stub.power_chip; }
std::int32_t max_vbus_mv() { return g_stub.max_vbus_mv; }
std::optional<Metrics> read() {
    if (!g_stub.pm_online) return std::nullopt;
    return g_stub.pm;
}
std::optional<Metrics> read_display() {   // no filter in the stub: a test sets pm_display itself, else display = pm
    if (!g_stub.pm_online) return std::nullopt;
    return g_stub.pm_display_set ? g_stub.pm_display : g_stub.pm;
}
std::optional<Metrics> read_raw() { return read(); }
void set_smoothing(smoothing::Preset p) { g_stub.smoothing_applied = p; }
smoothing::Preset smoothing_preset() { return g_stub.smoothing_applied; }
std::optional<std::int32_t> vbus_fast_mv() {
    if (!g_stub.pm_online) return std::nullopt;
    return g_stub.pm_fast_set ? g_stub.pm_fast_mv : g_stub.pm.vbus_mv;
}
std::optional<Metrics> sample_now() {   // a direct read: the un-averaged VBUS
    auto m = read();
    if (m && g_stub.pm_fast_set) m->vbus_mv = g_stub.pm_fast_mv;
    return m;
}
bool fast_vbus_mv(std::int32_t& mv) {   // one VBUS register read
    const auto m = sample_now();
    if (!m) return false;
    mv = m->vbus_mv;
    return true;
}
void restart_average() {}
}  // namespace power

namespace transcap {
ArmResult arm(std::uint8_t step, std::uint16_t arg) {
    if (g_stub.tc_arm_result != ArmResult::Ok) return g_stub.tc_arm_result;
    g_stub.tc_arms++; g_stub.tc_arm_pos = step; g_stub.tc_arm_mv = arg;
    g_stub.tc_state = g_stub.tc_freeze_on_arm ? State::Frozen : State::Armed;
    return ArmResult::Ok;
}
void disarm() { g_stub.tc_disarms++; g_stub.tc_state = State::Off; }
void disarm_if_armed() { if (g_stub.tc_state == State::Armed) disarm(); }
bool trigger(Reason r) {
    if (g_stub.tc_state != State::Armed) return false;
    g_stub.tc_triggers++; g_stub.tc_reason = r; g_stub.tc_state = State::Post;
    return true;
}
bool triggered() { return g_stub.tc_state == State::Post || g_stub.tc_state == State::Frozen; }
void keep(std::uint32_t run_id, std::uint16_t step) { g_stub.tc_keeps++; g_stub.tc_keep_run = run_id; g_stub.tc_keep_step = step; }
Status status() { Status s; s.state = g_stub.tc_state; return s; }
bool find_stored(std::uint32_t run_id, std::uint16_t step, Meta& m) {
    if (!g_stub.tc_has_meta || g_stub.tc_meta.run_id != run_id ||
        (g_stub.tc_meta.step != step && g_stub.tc_meta.step != 0xFFFF)) return false;   // 0xFFFF: any step
    m = g_stub.tc_meta;
    return true;
}
}  // namespace transcap

namespace adc {
std::optional<Reading> read() { return g_stub.adc; }
}  // namespace adc

namespace fastcharge {
Protocol read() { return g_stub.fc; }
Protocol label() { return report_label(g_stub.fc, g_stub.vooc.seen); }
vooc::Live vooc_live() { return g_stub.vooc; }
}  // namespace fastcharge

namespace power_source {
bool decided() { return g_stub.supply_decided; }
bool bus_powered() { return g_stub.bus_powered; }
}  // namespace power_source

namespace usb {
Stats stats;
StatsSnapshot stats_snapshot() { return {stats.resets, stats.isrs, stats.log_sent, stats.log_dropped, stats.poll_gap_max}; }
bool configured() { return true; }
void poll() { g_stub.polls++; }
void log(std::string_view) {}
void log_fmt(const char* fmt, ...) {
    // K2LOG on the host formats the text directly: keep it, so tests can assert on log lines (capped, they look at the tail).
    char buf[256];
    std::va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    if (g_stub.log_text.size() > 64 * 1024) g_stub.log_text.clear();
    g_stub.log_text.append(buf, std::min<std::size_t>(static_cast<std::size_t>(n), sizeof buf - 1));
    if (g_stub.log_fmts.size() > 64 * 1024) g_stub.log_fmts.clear();
    g_stub.log_fmts.append(fmt); g_stub.log_fmts.push_back('\n');
}
std::size_t log_bin(std::span<std::uint8_t> out, std::uint32_t& base_ms, bool& older) {
    base_ms = g_stub.bin_base;
    older = g_stub.bin_older;
    const std::size_t n = std::min(out.size(), g_stub.bin_log.size());
    std::copy_n(g_stub.bin_log.begin(), n, out.begin());
    return n;
}
std::uint8_t tx_free() { return 16; }
// Like the firmware, every report handed to the IN endpoint moves the TX counter the Diag page shows.
void send_report(const std::uint8_t* r) {
    g_stub.reports.emplace_back(r, r + 64);
    stats.log_sent = stats.log_sent + 1;
    if (g_stub.on_report) g_stub.on_report();
}
}  // namespace usb

}  // namespace k2

namespace k2::pdstress::journal_hw {
// Host stand-in for the PD Stress journal file: RAM, with a switch that makes the write fail.
namespace {
class RamStore final : public journal::Store {
public:
    bool read(std::span<std::uint8_t> out, std::size_t& n) override {
        n = 0;
        if (!g_stub.ps_file_present || g_stub.ps_file.size() > out.size()) return false;
        std::copy(g_stub.ps_file.begin(), g_stub.ps_file.end(), out.begin());
        n = g_stub.ps_file.size();
        return true;
    }
    bool write(std::span<const std::uint8_t> in) override {
        if (g_stub.ps_write_fails) return false;
        g_stub.ps_file.assign(in.begin(), in.end());
        g_stub.ps_file_present = true;
        g_stub.ps_writes++;
        return true;
    }
    void remove() override { g_stub.ps_file.clear(); g_stub.ps_file_present = false; g_stub.ps_removes++; }
};
RamStore g_ram;
}  // namespace
journal::Store& store() { return g_ram; }
}  // namespace k2::pdstress::journal_hw

namespace k2::fctrig {
// Host stand-in for the trigger driver: idle, never running, records what the UI asked for.
static Status g_status{fc::State::Idle, fc::Proto::Qc2, fc::Fail::None, 0, 0};
Fail start(Proto proto, int mv, bool confirmed, bool allow_inline, int, int) {
    if (g_stub.fc_start_hook) g_stub.fc_start_hook(int(proto));
    g_stub.fc_starts++;
    g_stub.fc_start_mv = mv; g_stub.fc_confirmed = confirmed;
    // Only a protocol the engine keeps the page out of (Engine::blocks_paint) opens a window; the QC handshake is served by the hook.
    if (!g_stub.fc_start_fail && g_stub.fc_open_on_start && fc::Engine::blocks_paint(proto)) g_stub.fc_critical = true;
    if (!g_stub.fc_start_fail) {
        g_stub.fc_proto = int(proto); g_stub.fc_target_mv = mv;
        g_stub.fc_state = int(g_stub.fc_answers ? State::Active : State::Negotiating);
    }
    if (allow_inline) g_stub.fc_inline_ok++;
    return static_cast<Fail>(g_stub.fc_start_fail);
}
Fail set_target(int mv, bool confirmed, int) {
    g_stub.fc_retargets++;
    g_stub.fc_retarget_mv = mv;
    g_stub.fc_confirmed = confirmed;
    if (!g_stub.fc_retarget_fail) g_stub.fc_target_mv = mv;
    return static_cast<Fail>(g_stub.fc_retarget_fail);
}
void stop() {
    g_stub.fc_stops++;
    g_stub.fc_state = int(State::Idle); g_stub.fc_scanning = false;
    if (g_stub.reader_scanner) g_stub.reader_scanner->abort();
}
void poll() {}
bool paint_critical() { return g_stub.fc_critical; }
const ufcs::Tuner& ufcs_tuner() { return g_stub.ufcs_tuner; }
TuneSet ufcs_set(ufcs::Field f, std::uint32_t v) {
    if (g_stub.ufcs_probe_run) return TuneSet::Busy;
    return ufcs::set(g_stub.ufcs_tuner, f, v) ? TuneSet::Ok : TuneSet::Range;
}
bool ufcs_tuner_reset() {
    if (g_stub.ufcs_probe_run) return false;
    g_stub.ufcs_tuner = ufcs::kDefaultTuner;
    return true;
}
ufcs::ProbeStart ufcs_probe_start() {
    g_stub.ufcs_probe_starts++;
    if (g_stub.ufcs_probe_refuse) return static_cast<ufcs::ProbeStart>(g_stub.ufcs_probe_refuse);
    g_stub.ufcs_probe_run = true;
    return ufcs::ProbeStart::Ok;
}
bool ufcs_probe_running() { return g_stub.ufcs_probe_run; }
bool ufcs_probe_done() { return !g_stub.ufcs_probe_run && !g_stub.ufcs_blob.empty(); }
void ufcs_probe_stop() { g_stub.ufcs_probe_stops++; g_stub.ufcs_probe_run = false; }
std::span<const std::uint8_t> ufcs_probe_blob() { return g_stub.ufcs_blob; }
const ufcs::ProbeResult& ufcs_probe_result() { return g_stub.ufcs_result; }
Fail scan_start(bool confirmed, bool allow_inline) {
    g_stub.fc_scans += confirmed ? 1 : 0;
    if (allow_inline) g_stub.fc_inline_ok++;
    if (g_stub.reader_scanner) return g_stub.reader_scanner->start(confirmed, allow_inline);
    return confirmed ? Fail::None : Fail::NeedConfirm;
}
bool scan_running() { return g_stub.reader_scanner ? g_stub.reader_scanner->running() : g_stub.fc_scanning; }
bool scan_done() { return g_stub.reader_scanner && g_stub.reader_scanner->done(); }
std::size_t scan_step() { return g_stub.reader_scanner ? g_stub.reader_scanner->step() : static_cast<std::size_t>(g_stub.fc_scan_step); }
Fail scan_fail() { return g_stub.reader_scanner ? g_stub.reader_scanner->fail() : Fail::None; }
std::span<const fc::ScanResult> scan_results() { return g_stub.reader_scanner ? g_stub.reader_scanner->results() : std::span<const fc::ScanResult>(g_stub.fc_scan_res); }
Status status() {
    g_status.state = static_cast<State>(g_stub.fc_state);
    g_status.proto = static_cast<Proto>(g_stub.fc_proto);
    g_status.target_mv = g_stub.fc_target_mv;
    return g_status;
}
const fc::Caps& caps() { return g_stub.fc_caps; }
}  // namespace k2::fctrig

namespace k2::usbcfg {
void init() { current = g_stub.usb_stored; }
Setting stored() { return g_stub.usb_stored; }
bool set(Setting s) {
    g_stub.usb_sets++;
    if (g_stub.usb_set_ok) g_stub.usb_stored = s;
    return g_stub.usb_set_ok;
}
}  // namespace k2::usbcfg

namespace k2::uiprefs {
Prefs stored() { return g_stub.prefs; }
bool set(Prefs p) { g_stub.prefs_sets++; g_stub.prefs = p; return true; }
}  // namespace k2::uiprefs

namespace k2::cablepref {
ctcable::Choice stored() { return g_stub.cable_pref; }
bool set(ctcable::Choice c) {
    g_stub.cable_pref_sets++;
    if (g_stub.cable_pref_ok) g_stub.cable_pref = c;
    return g_stub.cable_pref_ok;
}
}  // namespace k2::cablepref

// Charger journal: host UI tests keep it in memory-free no-op form (the journal itself has its own tests).
namespace k2::charger_journal::hw {
void mount() {}
bool load(Replay& into) {
    g_stub.journal_load_owner = scratch::owner();
    g_stub.journal_load_in_arena = scratch::held() && static_cast<void*>(&into) == static_cast<void*>(scratch::arena());
    if (g_stub.journal_load) into = g_stub.journal;
    return g_stub.journal_load;
}
static void note_write() {
    g_stub.journal_writes++;
    if (g_stub.journal_quiet && g_stub.journal_quiet()) g_stub.journal_writes_quiet++;
}
// The polled jobs: a stand-in with the same shape as the real one (one write per pump, working() until the last).
void arm_start() {
    g_stub.journal_arms++;
    g_stub.journal_job = true; g_stub.journal_job_arm = true; g_stub.journal_arm_open = true;
    g_stub.journal_job_left = g_stub.arm_pumps ? g_stub.arm_pumps : -1;   // -1: sized by the plan at the first pump
    g_stub.journal_log += 'A';
}
bool working() { return g_stub.journal_job; }
bool pending() { return g_stub.journal_job || g_stub.journal_plan_dirty || !g_stub.journal_noted.empty(); }
bool plan_pending() { return g_stub.journal_job || g_stub.journal_plan_dirty; }
void cancel() {
    const bool had = g_stub.journal_job || g_stub.journal_plan_dirty;
    g_stub.journal_noted.clear(); g_stub.journal_plan_dirty = false;
    if (!had) return;
    g_stub.journal_job = false; g_stub.journal_cancels++;
    g_stub.journal_arm_open = false;
}
// A row's result / the plan's shape is only noted (no write), as in the real journal; pump() writes one file.
void note(std::span<const charger_test::Step>, std::size_t index) {
    for (const auto i : g_stub.journal_noted) if (i == index) return;
    g_stub.journal_noted.push_back(index);
}
void note_plan(std::span<const charger_test::Step>) {
    if (g_stub.journal_job && g_stub.journal_job_arm) return;   // the arm writes the plan as it stands when its last file goes in
    if (!g_stub.journal_plan_dirty) { g_stub.journal_replan_starts++; g_stub.journal_replan_left = g_stub.replan_pumps; }
    g_stub.journal_plan_dirty = true;
}
static void publish(std::span<const charger_test::Step> steps) {
    for (const auto index : g_stub.journal_noted) {
        if (index >= steps.size()) continue;
        if (g_stub.journal_verdicts.size() <= index) g_stub.journal_verdicts.resize(index + 1, 0xFF);
        g_stub.journal_verdicts[index] = static_cast<std::uint8_t>(steps[index].verdict);
        if (steps[index].verdict != charger_test::Verdict::Skip) continue;
        g_stub.journal_skips++;
        if (g_stub.journal_finishes == 0) { g_stub.journal_skips_before_finish++; g_stub.journal_skip_rows_before_finish.push_back(index); }
    }
    g_stub.journal_noted.clear();
}
void pump(std::uint32_t, const charger_test::Options&, const charger_test::Evidence&, std::span<const charger_test::Step> steps) {
    if (!pending()) return;
    note_write(); g_stub.journal_pumps++;
    if (g_stub.journal_job) {
        if (g_stub.journal_job_left < 0) g_stub.journal_job_left = int(steps.size()) + 1;
        g_stub.journal_log += 'P';
        if (--g_stub.journal_job_left > 0) return;
        g_stub.journal_job = false; g_stub.journal_log += 'H';
        publish(steps);   // the files were written from the rows as they stood at the last piece
        return;
    }
    if (g_stub.journal_plan_dirty) {
        g_stub.journal_log += 'P';
        if (--g_stub.journal_replan_left > 0) return;
        g_stub.journal_plan_dirty = false; g_stub.journal_replans++; g_stub.journal_log += 'R';
        return;
    }
    g_stub.journal_log += 'S';
    if (g_stub.journal_arm_open) g_stub.journal_writes_in_arm++;
    publish(steps);   // one section file carries every row noted in it
}
void phase(Phase, std::span<const charger_test::Step>, std::size_t) {
    g_stub.journal_phase_writes++; g_stub.journal_log += 'F';
    if (g_stub.journal_arm_open) g_stub.journal_writes_in_arm++;
}
void resumed(std::uint8_t) { g_stub.journal_resumes++; }
void meta(const RunMeta& m) {
    g_stub.journal_metas++; g_stub.journal_meta = m; g_stub.journal_log += 'M';
    if (g_stub.journal_job) g_stub.journal_writes_in_arm++;   // the meta follows the header, never the rows
    g_stub.journal_arm_open = false;
}
void attempt(std::size_t, std::uint8_t) { g_stub.journal_attempts++; }
void finish(std::uint8_t, std::uint8_t) { g_stub.journal_finishes++; g_stub.journal_log += 'N'; g_stub.journal_arm_open = false; }
}  // namespace k2::charger_journal::hw

// Report cards: a real Store over littlefs on a RAM disk (power-cut behaviour is tested in tests/test_report_card_store.cpp).
namespace {
struct CardFs {
    k2test::RamDisk disk{96};
    k2::fs::Fs fs{disk, 96};
    k2::report_card::Store store{fs};
    CardFs() { bool fm = false; fs.mount_or_format(fm); }
};
std::unique_ptr<CardFs> g_cf = std::make_unique<CardFs>();
std::uint32_t g_card_gen = 0;
}  // namespace
void stub_cards_reset() { g_cf = std::make_unique<CardFs>(); ++g_card_gen; }
namespace k2::report_card::hw {
std::size_t list(std::span<Card> out) { return g_cf->store.list(out); }
std::size_t numbers(std::span<std::uint16_t> out) { return g_cf->store.numbers(out); }
std::uint32_t generation() { return g_card_gen; }
bool find(std::uint32_t fp, Card& c) { return g_cf->store.find(fp, c); }
bool get(std::uint16_t no, Card& c) { return g_cf->store.get(no, c); }
Err put(Card& c) { const Err e = g_cf->store.put(c); if (e == Err::Ok) ++g_card_gen; return e; }
Err put(Card& c, Card& prev, bool& had) { const Err e = g_cf->store.put(c, prev, had); if (e == Err::Ok) ++g_card_gen; return e; }
Err rename(std::uint16_t no, std::string_view n) { const Err e = g_cf->store.rename(no, n); if (e == Err::Ok) ++g_card_gen; return e; }
Err remove(std::uint16_t no) { const Err e = g_cf->store.remove(no); if (e == Err::Ok) ++g_card_gen; return e; }
Context context() { return {.fw = 0x0FF2C610, .boots = 4, .uptime_s = g_stub.ms / 1000}; }
}  // namespace k2::report_card::hw
