// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Connection page (Tools): the three parties on the two ports as tabs, Source / Cable / Sink, one list of
// compact rows per tab, a status bar at the bottom and a small action pop-up on OK (the pure model is
// ui_connection.hpp). It extends the look of the old PD page: tab row on top, hairline rows (12 px dim key
// at x 4, 16 px value at x 64), nothing scrolls.
//
// Passive: it only reads what the PD engine and the ADC already know. The topology word in the status bar
// and the port-aware empty states come from raguard::classify over the CC levels the ADC already averages;
// reading them never starts an active CC operation. Active work (Refresh, Read cable, Identify) happens only
// from the pop-up, through Ui::run_action and its inline confirm.
#include "k2/core/log.hpp"
#include "k2/core/survey.hpp"
#include "k2/pd/emk_variant.hpp"
#include "k2/pd/emrec.hpp"
#include "k2/pd/diag_live.hpp"
#include "k2/pd/diag_screen.hpp"
#include "k2/drivers/fusb302.hpp"
#include "k2/drivers/power_calc.hpp"
#include "k2/pd/pd_source.hpp"
#include "k2/pd/pd_tabs.hpp"
#include "k2/pd/pd_vdm.hpp"
#include "k2/ui/qrshare_records.hpp"
#include "k2/core/storage.hpp"
#include "k2/pd/usb_link.hpp"
#include "k2/ui/text.hpp"
#include "k2/ui/ui.hpp"
#include "k2/ui/ui_pages.hpp"
#include "k2/ui/ui_style.hpp"
#include "k2/ui/ui_widgets.hpp"

namespace k2 {

namespace {

// The sink's wants list of the current attach: its Sink_Capabilities sniffed when the source asked for them
// (cleared by the driver on every attach), or the ones Identify collected in this attach session.
WantsList wants_now() {
    return pick_wants(fusb302::sink_caps(), fusb302::identify_current(), fusb302::identify_status());
}
std::span<const uint32_t> sniffed_sink_caps() { return wants_now().pdos; }
// The mismatch bit is part of the RDO we sniffed; unknown while no Request was seen.
Tri sniffed_mismatch() {
    const pd::Contract &c = fusb302::contract();
    if (!c.valid || pd::is_fake()) return Tri::Unknown;
    return (c.flags & pd::kRdoFMismatch) ? Tri::Yes : Tri::No;
}

uint32_t fnv(uint32_t h, uint32_t v) {
    for (int i = 0; i < 4; i++) { h ^= (v >> (8 * i)) & 0xFF; h *= 16777619u; }
    return h;
}

// `until` is a deadline in millis(); 0 = none. Wrap-safe.
bool before(uint32_t now, uint32_t until) { return until != 0 && int32_t(until - now) > 0; }

// The offers of the current source, append-only (ui_match.hpp). It starts over on every new attach and
// on every debug fake list, and a detach empties it: the page never shows a charger that is gone.
OfferBook g_book;

const OfferBook &sync_book(const pd::Link &link) {
    static bool was_attached = false, was_fake = false;
    static uint32_t was_session = 0;
    const bool attached = link.attached();
    const uint32_t session = fusb302::session();
    // A new attach, a detach, a new attach session (even when the detach went unseen) or the fake list
    // coming or going starts over: nothing of the previous source may render, not even as GONE rows.
    if (attached != was_attached || pd::is_fake() || was_fake || session != was_session) g_book.clear();
    was_attached = attached;
    was_fake = pd::is_fake();
    was_session = session;
    if (attached && link.waiting()) g_book.clear();   // the new source has not sent caps yet
    else g_book.merge(pd::active_caps());
    return g_book;
}

}  // namespace

uint32_t g_match_live_calls = 0;
uint32_t g_conn_body_passes = 0;

MatchResult match_live(const pd::Link &link) {
    g_match_live_calls++;
    static const pd::Contract kNoContract{};
    const OfferBook &b = sync_book(link);
    const bool none = pd::is_fake() || (link.attached() && link.waiting());
    MatchResult r = match_compute({b.caps, none ? kNoContract : fusb302::contract(), sniffed_mismatch(), sniffed_sink_caps(), b.offered});
    // An attached source that sent no PD caps is a plain Type-C source: its Rp is on the active line
    // (attach latched the comparator), shown with its advertised current when it is decided.
    if (!r.caps_seen && !pd::is_fake() && link.attached()) {
        r.typec_ma = fusb302::typec_ma();
        r.typec_rp = true;
    }
    return r;
}

// ---------------------------------------------------------------------------------------
// Page lifecycle and events
// ---------------------------------------------------------------------------------------

void ConnectionPage::enter() {
    guard_.clear();
    guard_primed_ = false;
    popup_ = false;
    popup_drawn_ = false;
    more_ = false;
    op_ = StrBuf<24>();
    op_until_ = 0;
    id_asked_ = false;
    sig_valid_ = false;
    act_sel_ = -1;
    first_ = 0;
    id_sig_ = thumb_sig_ = 0;
    sense(ctx_.now);                   // facts only; the topology window starts with this one sample
    if (locked_) {                     // a place keeps its tab and comes back on the row it was left on (update clamps it)
        act_sel_ = ctx_.keep.place_sel[static_cast<int>(conn::place_of(tab_))];
        return;
    }
    tab_ = conn::default_tab(facts_);   // Sink if a device is inline or was seen on CC, else Source if a charger is seen, else Cable if a cable is held
    defaulted_ = true;
}

// A place page (Charger, Cable, Device): this page locked to one tab, no tab strip, the place's name and icon in the masthead.
// OK with no row focused runs the place's own read; its actions are rows under the fact table.
void ConnectionPage::lock(conn::Tab t) {
    locked_ = true;
    tab_ = t;
    defaulted_ = false;
    more_ = false;
    act_sel_ = -1;
    first_ = 0;
    const bench::Place p = conn::place_of(t);
    title_ = bench::place_name(p);
    icon_ = place_icon(p);
    bar_icon_ = true;
    sig_valid_ = false;
}

// Up / Dn on a place page. With a row focused they move the focus (Up from the first row lets go of it: OK then reads again);
// with none they scroll the fact table a window at a time, and Dn takes the first row once it is on screen.
void ConnectionPage::place_key(bool down) {
    const int rows = n_items_ - 1;
    constexpr int step = kPlaceRows - 1;   // a window less one row: the last row seen stays on screen
    if (down) {
        if (act_sel_ >= 0) { if (act_sel_ + 1 < rows) act_sel_++; }
        else if (rows > 0 && res_n_ < first_ + kPlaceRows) act_sel_ = 0;
        else first_ += step;   // place_first clamps it at the next paint
    } else {
        if (act_sel_ >= 0) act_sel_--;
        else first_ = first_ > step ? first_ - step : 0;
    }
    if (act_sel_ >= 0) ctx_.keep.place_sel[static_cast<int>(conn::place_of(tab_))] = static_cast<uint8_t>(act_sel_);
}

void ConnectionPage::leave() {
    popup_ = false;
    popup_drawn_ = false;
    op_ = StrBuf<24>();
    op_until_ = 0;
    id_asked_ = false;
    ctx_.em_listen_until = 0;    // nor does a listen window or its "heard nothing" verdict
    ctx_.em_quiet = false;
}

void ConnectionPage::draw_static() {
    title_bar();
    sig_valid_ = false;
    popup_drawn_ = false;   // the pop-up re-binds and repaints the shared RowList
    id_sig_ = thumb_sig_ = 0;
    status_.invalidate();
}

void ConnectionPage::invalidate() {
    sig_valid_ = false;
    popup_drawn_ = false;
    id_sig_ = thumb_sig_ = 0;
    status_.invalidate();
}

void ConnectionPage::open_tab(conn::Tab t) {
    tab_ = t;
    defaulted_ = false;
    more_ = false;
    if (popup_) close_popup();
    act_sel_ = -1;
    sig_valid_ = false;
}

void ConnectionPage::close_popup() {
    popup_ = false;
    popup_drawn_ = false;
    sig_valid_ = false;
}

PanelLabels ConnectionPage::labels() const {
    PanelLabels l = base_labels();
    if (popup_) {
        l.b[static_cast<int>(Button::Ok)] = {"Select", "", "", false};
        l.b[static_cast<int>(Button::Plus)] = {"Dn", "", "", false};
        l.b[static_cast<int>(Button::Minus)] = {"Up", "", "", false};
        l.b[static_cast<int>(Button::Back)] = {"Back", "", "", false};
        return l;
    }
    const bool share = (tab_ == conn::Tab::Source && pd::active_caps().valid) || (tab_ == conn::Tab::Cable && fusb302::emarker().valid);
    if (locked_) {
        const int i = act_sel_ < 0 ? 0 : 1 + act_sel_;
        const char *verb = i < n_items_ ? conn::act_verb(items_[static_cast<std::size_t>(i)].act, more_) : "";
        l.b[static_cast<int>(Button::Ok)] = {verb, share ? "Share" : "", "", false};
        l.b[static_cast<int>(Button::Plus)] = {"Dn", "", "", false};    // a place page always scrolls or moves the focus
        l.b[static_cast<int>(Button::Minus)] = {"Up", "", "", false};
        return l;
    }
    l.b[static_cast<int>(Button::Ok)] = {"Actions", share ? "Share" : "", "", false};
    l.b[static_cast<int>(Button::Plus)] = {"Tab+", "", "", false};
    l.b[static_cast<int>(Button::Minus)] = {"Tab-", "", "", false};
    return l;
}

bool ConnectionPage::on_event(Event e) {
    const bool shrt = e.press == Press::Short;
    if (popup_) {
        if (!shrt) return false;
        switch (e.button) {
        case Button::Back: close_popup(); break;
        case Button::Plus: if (popup_sel_ + 1 < n_items_) popup_sel_++; break;
        case Button::Minus: if (popup_sel_ > 0) popup_sel_--; break;
        case Button::Ok:
            if (popup_sel_ >= 0 && popup_sel_ < n_items_) activate(items_[static_cast<std::size_t>(popup_sel_)], ctx_.now);
            break;
        default: return false;
        }
        update(ctx_.now);
        return true;
    }
    if (locked_ && shrt && e.button != Button::Back) {
        switch (e.button) {
        case Button::Plus: place_key(true); break;
        case Button::Minus: place_key(false); break;
        case Button::Ok: {
            const int i = act_sel_ < 0 ? 0 : 1 + act_sel_;
            if (i >= n_items_) break;
            const conn::Item &it = items_[static_cast<std::size_t>(i)];
            if (it.act == conn::Act::Bench) {   // a tool or a page of the place: it may replace this page, so nothing here is touched after
                const bench::Acts b = bench::actions(conn::place_of(tab_));
                if (it.arg >= 0 && it.arg < b.n) open_bench_act(*ctx_.ui, b.a[it.arg], ctx_.now);   // a blocked tool opens and says why
                return true;
            }
            if (it.enabled) activate(it, ctx_.now);
            else set_op(it.why, ctx_.now);
            break;
        }
        default: return false;
        }
        sig_valid_ = false;
        update(ctx_.now);
        return true;
    }
    if (shrt && (e.button == Button::Plus || e.button == Button::Minus)) {
        tab_ = e.button == Button::Plus ? pdtabs::next(tab_) : pdtabs::prev(tab_);
        defaulted_ = false;
        more_ = false;
        sig_valid_ = false;
        update(ctx_.now);
        return true;
    }
    if (shrt && e.button == Button::Ok) {
        build_items();
        if (n_items_ == 0) return true;
        popup_ = true;
        popup_drawn_ = false;
        popup_sel_ = 0;
        sig_valid_ = false;
        update(ctx_.now);
        return true;
    }
    if (e.press == Press::Long && e.button == Button::Ok) {   // Share (QR): the shown tab's record
        if (tab_ == conn::Tab::Source) { share_caps(ctx_.now); return true; }
        if (tab_ == conn::Tab::Cable) { share_cable(ctx_.now); return true; }
        return false;
    }
    return back_event(e);
}

void ConnectionPage::set_op(const char *text, uint32_t now_ms, uint32_t ms) {
    op_ = StrBuf<24>();
    op_.put(text);
    op_until_ = now_ms + ms;
    if (op_until_ == 0) op_until_ = 1;
}

void ConnectionPage::share_caps(uint32_t now_ms) {
    const pd::SourceCaps &caps = pd::active_caps();
    if (!caps.valid) { set_op("No caps yet", now_ms); return; }
    const std::size_t len = qrshare::records::pdcaps(caps, ctx_.ui->share_buffer());
    ctx_.ui->open_share(qrshare::Type::PdCaps, qrshare::kSchemaPdCaps, "Source caps", len, now_ms);
}

// The live cable as a share record (no quirk table: that belongs to a saved clone).
void ConnectionPage::share_cable(uint32_t now_ms) {
    const pd::Emarker &em = fusb302::emarker();
    if (!em.valid || em.num_vdos < 5 || (em.spec_rev != 1 && em.spec_rev != 2)) { set_op("No cable read", now_ms); return; }
    static const emk_quirk::Quirks kNoQuirks{};
    const emrec::Record rec = emrec::make({em.raw_vdos.data(), 5}, em.spec_rev, "Live cable");
    const std::size_t len = qrshare::records::emarker(rec, "Live cable", kNoQuirks, ctx_.ui->share_buffer());
    ctx_.ui->open_share(qrshare::Type::Emarker, qrshare::kSchemaEmarker, "Live cable", len, now_ms);
}

void ConnectionPage::build_items() {
    conn::ActFacts a;
    const pd::Link &link = ctx_.ui->link();
    a.inline_sink = link.inline_mode() || ctx_.inline_mode;
    a.attached = facts_.charger || facts_.inline_sink;
    // A device alone on the K2 (a dual-role phone): offer the active Identify there too (the driver presents Rd)
    a.alone = pd::identify_alone(a.inline_sink, ctx_.pm_online, ctx_.pm_online ? ctx_.vbus_fast_mv : -1,
                                 fusb302::caps_this_attach(), link.wait_kind());
    a.caps_valid = pd::active_caps().valid;
    a.cable_valid = fusb302::emarker().valid;
    a.more = more_;
    // The no-PD gate (ROADMAP 2026-11-10, Row C): with the settled verdict "no PD" a device on the line
    // cannot be identified. The alone path (a dual-role phone) is PD-capable and stays offered.
    a.pd_none = verdict_ == pd::PdVerdict::None && (facts_.inline_sink || facts_.sink_seen) && !a.alone;
    if (locked_) {
        n_items_ = conn::place_actions(tab_, a, items_);
        // The place's tools carry the Bench's readiness: a blocked one is dimmed with its tag and the fact line says why.
        const SetupState ss = setup_state(ctx_);
        const bench::Acts b = bench::actions(conn::place_of(tab_));
        for (int i = 0; i < n_items_; i++) {
            conn::Item &it = items_[static_cast<std::size_t>(i)];
            if (it.act != conn::Act::Bench || it.arg < 0 || it.arg >= b.n || b.a[it.arg].kind != bench::ActKind::Tool) continue;
            const Readiness rd = tool_readiness(b.a[it.arg].arg, ss, ctx_.pm_online);
            it.enabled = rd.level != ToolLevel::Blocked;
            it.caution = rd.level == ToolLevel::Caution;
            it.why = rd.reason;
            it.tag = rd.level == ToolLevel::Ready ? "" : rd.tag;
        }
        return;
    }
    n_items_= conn::actions(tab_, a, items_);
    if (popup_sel_ >= n_items_) popup_sel_ = n_items_ - 1;
    if (popup_sel_ < 0) popup_sel_ = 0;
}

void ConnectionPage::activate(const conn::Item &it, uint32_t now_ms) {
    if (!it.enabled) return;   // the status bar names the reason while the greyed item is selected
    switch (it.act) {
    case conn::Act::Refresh:
        if (ctx_.ui->run_action(Action::PdRescan, now_ms)) set_op("Rescanning...", now_ms, 1500);
        else set_op("Busy, retry", now_ms, 1500);   // a negotiation in flight: the refresh waits for it
        break;
    case conn::Act::Details:
        more_ = !more_;
        break;   // closes the pop-up and repaints the body (the signature carries more_)
    case conn::Act::ShareCaps:
        close_popup();
        share_caps(now_ms);
        return;
    case conn::Act::ReadCable:
        // Inline the Vconn read is refused by the driver (an inline K2 never transmits); the sniff
        // method stays available and touches nothing.
        ctx_.ui->run_action(Action::EmarkerScan, now_ms);
        break;
    case conn::Act::SaveCable: {
        const emlib::Result r = storage::lib_save_current();
        set_op(r.err == emlib::Err::Ok ? (r.existed ? "Already saved" : "Cable saved")
               : r.err == emlib::Err::Exists ? "Already saved"
               : r.err == emlib::Err::Full ? "Library full"
               : r.err == emlib::Err::Invalid ? "No cable read" : "Flash error", now_ms);
        break;
    }
    case conn::Act::SpoofAs:
        close_popup();
        if (!ctx_.ui->push_menu(Menu::Spoof, now_ms)) set_op("Test running", now_ms);
        return;
    case conn::Act::SavedCables:
        close_popup();
        if (!ctx_.ui->push_menu(Menu::Emarker, now_ms)) set_op("Test running", now_ms);
        return;
    case conn::Act::ShareCable:
        close_popup();
        share_cable(now_ms);
        return;
    case conn::Act::Identify: {
        // Inline this is refused outright (no confirm): the driver never transmits inline.
        const bool started = ctx_.ui->run_action(Action::Identify, now_ms);
        id_asked_ = started;
        if (!started) set_op(ctx_.id_no_device ? "No device to ask" : "Identify refused", now_ms);
        break;
    }
    case conn::Act::Bench: break;   // on_event opens it (it may leave this page)
    }
    close_popup();
}

// ---------------------------------------------------------------------------------------
// Sensing: facts and the passive topology
// ---------------------------------------------------------------------------------------

void conn::sense_passive(Facts &f, const UiContext &ctx, const pd::Link &link, const raguard::PeakWindow &guard) {
    // The same passive recipe the charger test's Ra guard uses: valid only while the K2 holds nothing on the
    // lines and VBUS is down (a charger waiting for a sink). Anything else is Unknown. Nothing here drives CC.
    raguard::Reading r;
    r.cc1_mv = guard.cc1();
    r.cc2_mv = guard.cc2();
    r.vbus_mv = ctx.pm_online ? ctx.vbus_fast_mv : -1;
    r.passive = !fusb302::rd_for_run_on() && !fusb302::ra_active() && !fusb302::self_powered() && !ctx.inline_mode && !link.inline_mode();
    f.topo = raguard::classify(r);
    f.wait = link.wait_kind();   // the one waiting classifier: the tab, the bar and the remote state read this
    f.cross = link.cc_cross();   // crossed CC through the K2: the same single verdict for the tab, the bar, Home and the remote
    f.charger = link.attached();
    f.cable = fusb302::emarker().valid;
    f.cable_fact = cablefact::classify(f.cable, fusb302::last_cable_read(), fusb302::pull_down());   // a silent chip is not "no e-marker"
    f.inline_sink = link.inline_mode();
    // A decoded Request or Sink_Capabilities was received from a sink (the engine only sees received
    // frames, so the K2's own trigger requests never set these): the sink stays seen after a power-role
    // swap and after the inline latch expires.
    f.sink_seen = fusb302::contract().valid || fusb302::sink_caps().valid;
    // The idle "source asleep" evidence: the settled open reading of the line that reads the standby signature
    // (the CC2 bias net at 1.69 V and a waiting charger's 3.3 V open Rp are outside the window), VBUS now, and
    // the Wake sleeping port setting (off = no idle Rd hold is running).
    const std::int32_t o1 = fusb302::open_cc_mv(1), o2 = fusb302::open_cc_mv(2);
    f.open_cc_mv = pd::standby_signature(o1) ? o1 : pd::standby_signature(o2) ? o2 : -1;
    f.vbus_mv = ctx.pm_online ? ctx.vbus_fast_mv : -1;
    f.wake_port_off = !fusb302::idle_wake();
}

// No page on screen, so no peak window: one window holding the latest CC levels (the page's window holds up to 8).
// The verdicts that read the lines can differ from the page's for one sag; everything else is identical.
conn::Facts Ui::passive_facts() const {
    raguard::PeakWindow one;
    one.add(ctx_.analog ? ctx_.analog->cc1_mv : -1, ctx_.analog ? ctx_.analog->cc2_mv : -1);
    conn::Facts f;
    conn::sense_passive(f, ctx_, link_, one);
    return f;
}

void ConnectionPage::sense(uint32_t now_ms) {
    const pd::Link &link = ctx_.ui->link();
    if (!guard_primed_ || uint32_t(now_ms - guard_t_) >= raguard::kSampleMs) {
        guard_.add(ctx_.analog ? ctx_.analog->cc1_mv : -1, ctx_.analog ? ctx_.analog->cc2_mv : -1);
        guard_t_ = now_ms;
        guard_primed_ = true;
    }
    conn::sense_passive(facts_, ctx_, link, guard_);
    // The one "PD present" verdict of this attach, cached for build_items / paint_body / the tests.
    verdict_ = link.pd_verdict();
    none_why_ = link.pd_none_why();
}

// ---------------------------------------------------------------------------------------
// Rows
// ---------------------------------------------------------------------------------------

namespace {

// Layout: a tab strip under the top bar (three cells of the bar's rule pitch, the active one underlined in kFocus), then
// ONE edge-to-edge hairline list, 20 px pitch, 12 px dim key at x 6, 16 px value at x 64. The list ends at the footer
// (kFooterY 222): nine rows fit y 42..222.
constexpr int16_t kTabY = kit::kBodyY, kTabH = 18, kRowY0 = kTabY + kTabH, kRowPitch = 20, kValX = 64, kKeyX = 6;
constexpr int kMaxList = 9;
constexpr int kValCells = (224 - kValX - 4) / 8;   // 19 cells of S16 in the value column
static_assert(kRowY0 + kMaxList * kRowPitch <= kFooterY, "the nine rows end at the footer");
// An empty or no-PD tab (UI_SYSTEM 5.8, 7.13): a 36 px verdict band (kSurface, a dash, the word in S24) under the tab strip,
// ONE line of S16 under it, then (no-PD only) the measured rows from kNoPdRowY0.
constexpr int16_t kBandH = 36, kLineDy = kBandH + 6, kListDy = kLineDy + 16 + 8;   // offsets from the body top
constexpr int kVisRows = kMaxList;   // row slots on screen (a single tool scrolls its result + action rows through them)
static_assert(kRowY0 + kListDy + 3 * kRowPitch <= kFooterY, "three measured rows end at the footer");
static_assert(conn::kStatusCells <= static_cast<std::size_t>(kFooterCells), "the survey texts fit the footer line");
// The Cable tab's Answered row: every orientation and table row fits the value column, and the 12 px key fits the
// 58 px before it (6 px per cell).
constexpr bool answered_rows_fit() {
    for (std::uint8_t o = 1; o <= 2; o++)
        for (std::size_t i = 0; i < emk_variant::kAttempts; i++)
            if (emk_variant::short_text(emk_variant::answered_from(o, i)).view().size() > static_cast<std::size_t>(kValCells)) return false;
    return std::string_view("Answered").size() * 6 <= static_cast<std::size_t>(kValX - kKeyX);
}
static_assert(answered_rows_fit(), "the Answered row's key and value fit their columns");

// A value row. `cut` > 0 splits the value there: the head in `c`, the tail (the current) in kAmp; so a "20/28V 5A" line
// reads in unit colours. 0 = one colour.
// The rows are built in order and only the window is kept: `skip` rows are counted and dropped, the next kCap stored, the rest
// counted. A place page builds twice - once to count (skip = kCountOnly), once for the window it then shows.
struct Rows {
    struct R {
        const char *key = ""; StrBuf<28> v; uint16_t c = kText; uint8_t cut = 0;
        int8_t act = -1;       // an action row (a place page's extras): its index, -1 = a result row
        bool focus = false, off = false;
        bool drill = false;    // an action row that opens a page: the chevron when it has no tail
        bool perf = false;     // the first action row under fact rows: the dashed perforation on its top line
        const char *tail = ""; // an action row's tail word (a tool's readiness tag)
    };
    static constexpr int kCap = kMaxList;
    static constexpr int kCountOnly = 1 << 14;
    R r[kCap];
    R spill;   // rows outside the window land here and are dropped
    R *open = &spill;
    int n = 0;       // rows stored (the window)
    int total = 0;   // rows built
    int skip = 0;    // rows before the window
    void reset(int from) { n = 0; total = 0; skip = from; open = &spill; }
    StrBuf<28> &add(const char *key, uint16_t c = kText) {
        R &x = push();
        x.key = key; x.c = c;
        open = &x;
        return x.v;
    }
    R &push() {
        const bool in = total++ >= skip && n < kCap;
        R &x = in ? r[n++] : spill;
        x = R{};
        open = &x;
        return x;
    }
    void cut_here() { open->cut = static_cast<uint8_t>(open->v.view().size()); }   // the tail of the open row is a current
    void colour(uint16_t c) { open->c = c; }                                       // recolours the open row
};

struct TabData {
    const MatchResult *r;
    const pd::SourceCaps *caps;
    std::span<const uint32_t> sink;
    usblink::Link ul;
    bool waiting, k2_sink, k2_sink_ok;
    bool in_use = false;    // possibly inline: the line is live and a device behind the K2 may be using it
    fastcharge::Protocol fc;
    int32_t vbus_mv = -1;   // the fast VBUS reading in 100 mV steps (what the Type-C row shows), -1 unknown
    uint32_t vooc = 0;      // the Sink tab's raw VOOC note (ConnectionPage::vooc_), 0 = no live session
};

// The Type-C row's VBUS: the un-averaged reading, 100 mV steps (the row's resolution), -1 when the
// monitor is offline or VBUS is down.
int32_t typec_vbus_mv(bool pm_online, int32_t vbus_fast_mv) {
    if (!pm_online || vbus_fast_mv < 1000) return -1;
    return (vbus_fast_mv / 100) * 100;
}

uint16_t conf_colour(const usblink::Link &l) {
    return l.conf == usblink::Confidence::Negotiated ? kOk : l.conf == usblink::Confidence::Advertised ? kCaution : kFaint;
}

void put_id(StrBuf<28> &v, uint16_t vid, uint16_t pid, const char *vendor) {
    v.put_hex(vid, 4).put_char(':').put_hex(pid, 4);
    // The ids always show; the vendor name only when the whole name fits the value column (never a cut name).
    if (vendor && vendor[0] && v.view().size() + 1 + std::string_view(vendor).size() <= static_cast<std::size_t>(kValCells))
        v.put_char(' ').put(vendor);
}

// Appends `tag` only when it fits the value column whole.
void put_if_fits(StrBuf<28> &v, std::string_view tag) {
    if (v.view().size() + tag.size() <= static_cast<std::size_t>(kValCells)) v.put(tag);
}

// "PDO3 9V 3A", "PPS6 8.88V 2.05A", "AVS8 20V 5A", "BAT5 15V 45W": which offer the sink asked for and what it asked.
// The type leads, so PPS and AVS need no suffix; the longest case "PPS11 21.12V 3.25A" is 18 cells.
void put_request(StrBuf<28> &v, const MatchResult &r, const pd::SourceCaps &caps) {
    const pd::Pdo *p = r.want_pos > 0 && r.want_pos <= caps.num_pdos ? &caps.pdos[r.want_pos - 1] : nullptr;
    const pd::PdoType ty = p ? p->type : pd::PdoType::Fixed;
    v.put(ty == pd::PdoType::Pps ? "PPS" : ty == pd::PdoType::Avs ? "AVS" : ty == pd::PdoType::Battery ? "BAT" : "PDO");
    v.put_u32(uint32_t(r.want_pos)).put_char(' ');
    put_v(v, r.want_mv);
    v.put("V ");
    if (ty == pd::PdoType::Battery) v.put_u32(r.want_mw / 1000).put_char('W');
    else put_a(v, r.want_ma);
}

// USB / Alt rows of the live link: the data speed always, then the alt mode (entered green, advertised
// amber). With no alt mode at all the Alt row would only show "-", so it is dropped and the freed row
// lets the Proto row survive the 9-row cap.
void link_rows(Rows &R, const TabData &d) {
    std::array<char, usblink::kUiMaxChars + 1> ub{}, ab{};
    usblink::format_speed(d.ul, ub);
    R.add("USB", conf_colour(d.ul)).put(ub.data());
    const usblink::AltState as = usblink::format_alt(d.ul, ab);
    if (as == usblink::AltState::None) return;
    R.add("Alt", as == usblink::AltState::Entered ? kOk : kCaution).put(ab.data());
}

// "Proto Xiaomi PD": the vendor PROTOCOL one side lists by SVID (names only; the row is left out when there is none).
// `vdm` is that side's own answers: the source's (fusb302::vdm) on the Source tab, the sink's (sink_vdm) on the Sink tab.
void proto_row(Rows &R, const pd::VdmInfo &vdm) {
    StrBuf<kValCells + 1> t;   // protocol_text keeps whole names within what it is given
    if (conn::protocol_text(std::span<const uint16_t>(vdm.svids.data(), vdm.num_svids), t)) R.add("Proto", kEnergy).put(t.view());
}

// The fixed PDOs, one row per current group and per line of 19 cells ("5/9/15V 3A", "20/28/36/48V 5A"). Every line
// ends with its own current, so no line needs the cut and none reads as another group's.
void fixed_rows(Rows &R, const pd::SourceCaps &caps, int rows) {
    StrBuf<28> head;   // the voltages of the open line, "5/9/15"
    uint16_t cur_ma = 0;
    auto flush = [&] {
        if (head.view().empty()) return;
        StrBuf<28> &v = R.add("Fixed", kVolt);
        v.put(head.view()).put_char('V');
        R.cut_here();
        v.put_char(' ');
        put_a(v, cur_ma);
        head.clear();
    };
    for (int i = 0; i < rows; i++) {
        const pd::Pdo &p = caps.pdos[i];
        if (p.type != pd::PdoType::Fixed) continue;
        StrBuf<12> vs;
        put_v(vs, p.max_mv);
        StrBuf<12> as;
        put_a(as, p.max_ma);
        const std::size_t joined = head.view().size() + (head.view().empty() ? 0 : 1) + vs.view().size();
        if (!head.view().empty() && (p.max_ma != cur_ma || joined + 2 + as.view().size() > kValCells)) flush();
        if (!head.view().empty()) head.put_char('/');
        head.put(vs.view());
        cur_ma = p.max_ma;
    }
    flush();
}

// "3.3-11V 5A": the range in kVolt, the current in kAmp.
void range_row(Rows &R, const char *key, const pd::Pdo &p) {
    StrBuf<28> &v = R.add(key, kVolt);
    if (p.min_mv != p.max_mv) { put_v(v, p.min_mv); v.put_char('-'); }
    put_v(v, p.max_mv);
    v.put_char('V');
    R.cut_here();
    v.put_char(' ');
    put_a(v, p.max_ma);
}

// Only one tab builder runs per paint, but inlined all three their locals add up in paint_body()'s frame,
// which already holds the Rows (stack budget, tools/stack_depth.py).
[[gnu::noinline]] void source_rows(Rows &R, const TabData &d) {
    const MatchResult &r = *d.r;
    const pd::Identity &id = fusb302::vdm().identity;
    if (d.waiting) {
        R.add("PD", kFaint).put("Waiting for caps...");
        return;
    }
    if (id.valid) put_id(R.add("ID"), id.vid, id.pid, id.vendor);
    else {   // the row says why it is empty: not asked yet (Refresh reads it), asking, no answer, not supported...
        const char *w = conn::sv_word(survey::state().entry(pd::SvField::Identity));
        R.add("ID", kFaint).put(w[0] ? w : "Unknown");
    }
    const pd::SourceCaps &caps = *d.caps;
    StrBuf<28> &pdv = R.add("PD", r.caps_seen ? kEnergy : kFaint);
    const bool traffic = fusb302::pd_traffic();
    if (r.caps_seen) {
        pdv.put(caps.spec_rev == 1 || caps.spec_rev == 2 ? pd::wire_revision_name(caps.spec_rev) : "No PD yet");
        if (caps.epr) pdv.put(" EPR");
        pdv.put_char(' ').put_u32(r.src_max_mw / 1000).put_char('W');
        fixed_rows(R, caps, r.rows);
        for (int i = 0; i < r.rows; i++) {
            const pd::Pdo &p = caps.pdos[i];
            if (p.type != pd::PdoType::Pps && p.type != pd::PdoType::Avs) continue;
            range_row(R, p.type == pd::PdoType::Pps ? "PPS" : "AVS", p);
        }
        if (r.request_seen) put_request(R.add("Used", kOk), r, caps);
    } else if (!traffic && (r.typec_ma || r.typec_rp)) {
        // A Type-C Rp advertisement exists (the source is attached): show it with the measured VBUS
        // and the advertised current, a default-USB Rp included.
        pdv.put(d.in_use ? "None, in use" : "None");
        StrBuf<28> &v = R.add("Type-C", kText);
        typec_row_text(d.vbus_mv, r.typec_ma, v);
    } else {
        pdv.put(traffic ? "Active, no caps" : d.in_use ? "None, in use" : "No caps yet");
    }
    if (!r.caps_seen && legacy_row_protocol(d.fc)) {   // legacy fast-charge protocols the K2 detected
        StrBuf<6> fcn;
        fc_pill_text(d.fc, fcn);
        if (!fcn.view().empty() && d.fc != fastcharge::Protocol::Pending) R.add("Legacy", kEnergy).put(fcn.view());
    }
    link_rows(R, d);
    proto_row(R, fusb302::vdm());
    // Lab-only Apple readout (armed by `k2_remote apple-read`): the product and fw/hw of this attach.
    // The readout resets on detach, so the rows disappear with it.
    const pd::AppleInfo &ap = fusb302::apple_info();
    if (ap.valid) {
        R.add("Apple", kEnergy).put(ap.product[0] ? std::string_view(ap.product.data()) : std::string_view(ap.maker.data()));
        StrBuf<28> &fw = R.add("fw/hw", kText);
        fw.put("fw ").put(std::string_view(ap.fw.data()));
        if (ap.hw[0]) fw.put(" hw ").put(std::string_view(ap.hw.data()));
    }
}

[[gnu::noinline]] void cable_rows(Rows &R, const TabData &) {
    const pd::Emarker &em = fusb302::emarker();
    put_id(R.add("ID"), em.vid, em.pid, em.vendor_str);
    StrBuf<28> &sp = R.add("Speed", kText);
    char sb[24];
    pdtabs::speed_short(sb, em.speed_str ? em.speed_str : "?");
    sp.put(sb);
    // The lane and Thunderbolt / USB4 tags follow while they fit; the speed itself is always whole.
    if (em.is_active && em.vdo2_valid && em.usb_lanes == 2) put_if_fits(sp, " x2");
    if (em.vid == 0x8087 || fusb302::cable_thunderbolt()) { if (sp.view().substr(0, 2) != "TB") put_if_fits(sp, " TB"); }
    else if (em.usb4_supported && sp.view().substr(0, 4) != "USB4") put_if_fits(sp, " USB4");
    put_a(R.add("Current", kAmp), uint32_t(em.max_current_a) * 1000);
    StrBuf<28> &volt = R.add("Voltage", kVolt);
    volt.put_u32(em.max_voltage_v).put_char('V').put_char(' ').put_u32(em.max_power_w).put_char('W');
    R.add("EPR", em.epr_capable ? kOk : kDim).put(em.epr_capable ? "yes" : "no");
    StrBuf<28> &ty = R.add("Type", kText);
    ty.put(em.is_active ? "Active" : "Passive");
    ty.put(em.type_c_plug ? ", Type-C" : ", captive");
    if (em.vconn_required) R.add("VCONN", kText).put("required");
    R.add("Length", kText).put(em.latency_str ? em.latency_str : "?");
    // How the cable answered the read ("A, PD3, SVDM 1.0"): only when this firmware's scan found it (a clone's or
    // an older reading has none). With VCONN required this is the ninth and last row.
    const emk_variant::Text ans = emk_variant::short_text(fusb302::cable_answered());
    if (!ans.view().empty()) R.add("Answered", kDim).put(ans.view());
}

// One row: "5V 3A 9V 3A" whole entries that fit the 19 cells, then a "+" when more follow (never a cut entry).
void put_sink_pdos(Rows &R, const char *key, std::span<const uint32_t> sink) {
    StrBuf<28> &cur = R.add(key, kText);
    std::size_t shown = 0;
    for (uint32_t raw : sink) {
        const pd::Pdo p = pd::decode_pdo(raw);
        StrBuf<24> t;
        if (p.type == pd::PdoType::Battery) { put_v(t, p.max_mv); t.put("V "); t.put_u32(p.max_mw / 1000).put_char('W'); }
        else if (p.type == pd::PdoType::Pps || p.type == pd::PdoType::Avs) { put_v(t, p.min_mv); t.put_char('-'); put_v(t, p.max_mv); t.put("V "); put_a(t, p.max_ma); }
        else { put_v(t, p.max_mv); t.put("V "); put_a(t, p.max_ma); }
        const std::size_t after = cur.view().size() + (cur.view().empty() ? 0 : 1) + t.view().size();
        if (after + (shown + 1 == sink.size() ? 0 : 2) > kValCells) break;   // a non-last entry keeps room for " +"
        if (!cur.view().empty()) cur.put_char(' ');
        cur.put(t.view());
        shown++;
    }
    if (shown < sink.size()) cur.put(shown ? " +" : "+");
}

// The match summary as two list rows, worded by who holds the number down (pdtabs::power_line / data_line):
// "Pwr 18W of 100W" (the sink draws part of the offer), "Pwr 60W by cable", "Data 5G of 10G". On a link
// without a decoded PD contract the Pwr row is the Type-C default budget instead, labelled as a budget
// ("Pwr up to 2.5W", pdtabs::budget_line): never a bare wattage that reads as a measurement. The row is
// amber only when the cable is what limits the link; a sink that draws less than it is offered is the
// normal state.
[[gnu::noinline]] void strip_rows(Rows &R, const MatchResult &r, const usblink::Link &ul) {
    const pd::Emarker &em = fusb302::emarker();
    const bool attached = r.caps_seen || r.typec_ma;
    {   // power
        StrBuf<28> &v = R.add("Pwr", kText);
        if (!attached) v.put("-");
        else if (!r.caps_seen) {
            // No decoded PD contract: the only figure is the Type-C default budget (5 V x the advertised
            // current), and without PD there is no e-marker either, so no cable term. Budget wording, exact.
            char line[24];
            pdtabs::budget_line(line, r.typec_ma);
            v.put(line);
        } else {
            pdtabs::Terms t;
            t.source = static_cast<int32_t>(r.src_max_mw / 1000);
            if (em.valid) t.cable = em.max_power_w;
            else t.cable = 60;   // no e-marker: 3 A at 20 V assumed
            if (r.request_seen) t.sink = static_cast<int32_t>(r.want_mw / 1000);
            char line[24];
            bool warn = false;
            pdtabs::power_line(line, t, !em.valid, warn);
            v.put(line);
            if (warn) R.colour(kCaution);
        }
    }
    {   // data: only when it says something the USB row does not. A plain rate ("480M": sink-only known,
        // or cable and sink agree) or "?" would just repeat the USB row, so the row is omitted; "5G of 10G",
        // "5G by cable", "10G cable" and "+DP" stay.
        pdtabs::Terms t;
        if (em.valid) t.cable = pdtabs::cable_mbps(em.raw_vdos[4] & 7, em.vid);
        if (ul.family != usblink::Family::Unknown && ul.mbps > 0) t.sink = static_cast<int32_t>(ul.mbps);
        const pdtabs::Limited lim = pdtabs::limit(t);
        const bool informative = lim.value >= 0 &&
            (lim.limit != pdtabs::Who::None || ul.dp || (t.cable >= 0 && t.sink < 0));
        // The Data row only when it says something the USB row does not (nothing known -> no row at
        // all, the same rule as the plain-rate case above).
        if (attached && informative) {
            StrBuf<28> &v = R.add("Data", kText);
            char line[24];
            bool warn = false;
            pdtabs::data_line(line, t, ul.dp, warn);
            v.put(line);
            if (warn) R.colour(kCaution);
        }
    }
}

// The inline VOOC monitor's raw note (`note` = ConnectionPage::vooc_: 1<<16 | the last r82 word; 0 = no live session):
// the steady command and its last reply word, raw (what the word means is a guess, docs/FASTCHARGE_PROTOCOLS.md), so no
// unit; "-" before the first reply. One line, 17 cells of the 19-cell value column.
void vooc_row(Rows &R, uint32_t note) {
    if (!note) return;
    StrBuf<28> &v = R.add("VOOC", kDim);
    v.put("0x82 ");
    const uint32_t w = note & 0xFFFFu;
    if (w == vooc::kNoReply) v.put("-");
    else v.put("0x").put_hex(w, 3).put(" raw");
}

[[gnu::noinline]] void sink_rows(Rows &R, const TabData &d) {
    const MatchResult &r = *d.r;
    const pd::Identity &id = fusb302::sink_vdm().identity;
    if (d.k2_sink) {   // the K2 draws the power itself
        R.add("ID", kFaint).put("K2");
        R.add("Sink", kOk).put(d.k2_sink_ok ? "K2 (sink mode OK)" : "K2 (sink mode PLUS)");
        link_rows(R, d);
        return;
    }
    // The ID row: sniffed when no Identify of this session collected it (inline never asks).
    if (id.valid) {
        StrBuf<28> &v = R.add("ID");
        conn::id_value(v, kValCells, fusb302::identify_current(), id.vid, id.pid, id.vendor);
    } else R.add("ID").put("Unknown");   // until the sink answers a Discover Identity (the charger's own, sniffed)
    if (r.request_seen) {
        put_request(R.add("Request", kAmp), r, *d.caps);
        StrBuf<28> &st = R.add("State", kDim);
        st.put(request_state_name(fusb302::contract()));
        StrBuf<32> fl;
        const std::size_t room = st.view().size() + 2 < static_cast<std::size_t>(kValCells) ? kValCells - st.view().size() - 2 : 0;
        rdo_flags_text(fusb302::contract().flags, room, fl);
        if (!fl.view().empty()) st.put(", ").put(fl.view());
    } else {
        R.add("Request", kFaint).put("None seen");
    }
    strip_rows(R, r, d.ul);
    link_rows(R, d);
    proto_row(R, fusb302::sink_vdm());
    vooc_row(R, d.vooc);   // a live session next to a PD contract: the phone holds both (before Caps, which drops first)
    if (!d.sink.empty()) put_sink_pdos(R, "Caps", d.sink);
    else R.add("Caps", kFaint).put("Not seen");
}

// One survey field as a row of the Details page: what was read, else the word for why it was not (conn::sv_word).
void word_row(Rows &R, const char *key, pd::SvField f) {
    const char *w = conn::sv_word(survey::state().entry(f));
    R.add(key, kFaint).put(w[0] ? w : "Unknown");
}

// The Source tab's second page: what the Refresh survey reads beyond the main rows. An answer that is in is shown whatever
// the survey says (a sniffed one counts); a field without one says its status. Values are built to fit the 19 cells: the
// external name is copied up to them, lists keep whole entries.
[[gnu::noinline]] void details_rows(Rows &R) {
    const pd::SourceCaps &caps = pd::active_caps();
    if (caps.valid) {
        StrBuf<28> &v = R.add("Caps", kText);
        v.put_u32(caps.num_pdos).put(caps.num_pdos == 1 ? " PDO" : " PDOs");
        if (caps.epr) put_if_fits(v, " EPR");
    } else word_row(R, "Caps", pd::SvField::Caps);

    const pd::ManufacturerInfo &mi = fusb302::ext().mfr;
    if (mi.valid && mi.name[0]) {
        StrBuf<28> &v = R.add("Mfr", kText);
        for (std::size_t i = 0; i < sizeof mi.name && mi.name[i] && v.view().size() < static_cast<std::size_t>(kValCells); i++) v.put_char(mi.name[i]);
    } else if (mi.valid) {
        put_id(R.add("Mfr", kText), mi.vid, mi.pid, nullptr);
    } else word_row(R, "Mfr", pd::SvField::MfrInfo);

    const pd::SourceCapsExt &ce = fusb302::ext().caps_ext;
    if (ce.valid) {
        StrBuf<28> &v = R.add("Ext", kText);
        v.put("PDP ").put_u32(ce.pdp_w).put_char('W');
        if (ce.epr_pdp_w) {
            StrBuf<12> e;
            e.put(" EPR ").put_u32(ce.epr_pdp_w).put_char('W');
            put_if_fits(v, e.view());
        }
    } else word_row(R, "Ext", pd::SvField::CapsExt);

    const pd::VdmInfo &vdm = fusb302::vdm();
    if (vdm.num_svids > 0) {   // "2717 2E87 +": whole four-digit entries, a "+" when more follow
        StrBuf<28> &v = R.add("SVIDs", kText);
        std::size_t shown = 0;
        for (; shown < vdm.num_svids; shown++) {
            const bool last = shown + 1 == vdm.num_svids;
            const std::size_t after = v.view().size() + (shown ? 1 : 0) + 4;
            if (after + (last ? 0 : 2) > static_cast<std::size_t>(kValCells)) break;
            if (shown) v.put_char(' ');
            v.put_hex(vdm.svids[shown], 4);
        }
        if (shown < vdm.num_svids) v.put(shown ? " +" : "+");
    } else word_row(R, "SVIDs", pd::SvField::Svids);

    if (vdm.num_modes > 0) {
        StrBuf<28> &v = R.add("Modes", kText);
        v.put_hex(vdm.modes_svid, 4).put(": ").put_u32(vdm.num_modes).put(vdm.num_modes == 1 ? " mode" : " modes");
    } else word_row(R, "Modes", pd::SvField::Modes);

    const pd::SourceCaps &epr = fusb302::epr_info_list();
    if (epr.valid && epr.num_pdos > 0) {   // the EPR positions (8 on) when the list has them, else what it lists
        uint32_t raw[pd::kMaxPdos];
        const std::size_t from = epr.num_pdos > 7 ? 7 : 0;
        std::size_t n = 0;
        for (std::size_t i = from; i < epr.num_pdos && i < pd::kMaxPdos; i++) raw[n++] = epr.pdos[i].raw;
        put_sink_pdos(R, "EPR", std::span<const uint32_t>(raw, n));
    } else word_row(R, "EPR", pd::SvField::EprList);
}

// What the Details page and the status line read from the survey and the extended answers, hashed for the paint signatures.
uint32_t survey_sig(bool more) {
    const pd::Survey &sv = survey::state();
    uint32_t h = fnv(2166136261u, uint32_t(more) | uint32_t(sv.running()) << 1 | uint32_t(sv.ran()) << 2 | sv.resolved() << 8 | sv.planned() << 16);
    for (unsigned i = 0; i < pd::kSvFields; i++) {
        const pd::SvEntry &e = sv.entry(static_cast<pd::SvField>(i));
        h = fnv(h, uint32_t(e.state) | uint32_t(e.why) << 8);
    }
    const pd::ExtInfo &ex = fusb302::ext();
    h = fnv(h, uint32_t(ex.mfr.valid) | uint32_t(ex.mfr.vid) << 8 | uint32_t(ex.mfr.pid) << 24);
    for (char ch : ex.mfr.name) { if (!ch) break; h = fnv(h, uint8_t(ch)); }
    h = fnv(h, uint32_t(ex.caps_ext.valid) | uint32_t(ex.caps_ext.pdp_w) << 8 | uint32_t(ex.caps_ext.epr_pdp_w) << 16);
    const pd::SourceCaps &epr = fusb302::epr_info_list();
    h = fnv(h, uint32_t(epr.valid) | uint32_t(epr.num_pdos) << 8);
    for (std::size_t i = 0; i < epr.num_pdos && i < pd::kMaxPdos; i++) h = fnv(h, epr.pdos[i].raw);
    return h;
}

}  // namespace

// ---------------------------------------------------------------------------------------
// Painting
// ---------------------------------------------------------------------------------------

void ConnectionPage::paint_tab_indicator() {
    // Three cells of the bar's rule pitch, the name centred (S12): the active tab filled kFocusT, its name kFocus over a 2 px
    // kFocus rule (UI_SYSTEM 7.13.4); the others kDim on the ground over a kRule one.
    if (locked_) return;   // a single tool has no strip
    const Surface &c = ctx_.content;
    for (int t = 0; t < conn::kTabs; t++) {
        const bool on = t == static_cast<int>(tab_);
        const std::string_view name = conn::tab_name(static_cast<conn::Tab>(t));
        const int16_t x = static_cast<int16_t>(t * kit::kProgPitch);
        const int16_t w = kit::kProgSegW;
        const kit::TextRun label{static_cast<int16_t>(x + (w - static_cast<int>(name.size()) * kLabelFont.adv) / 2),
                                 static_cast<int16_t>(kTabY + 2), &kLabelFont, name, on ? kFocus : kDim};
        kit::paint_texts(c, {x, kTabY, w, static_cast<int16_t>(kTabH - 2)}, &label, 1, on ? kFocusT : kBg);   // ground and name, once
        gfx_fill(c, x, static_cast<int16_t>(kTabY + kTabH - 2), w, 2, on ? kFocus : kRule);
        if (kit::kProgPitch > w) gfx_fill(c, static_cast<int16_t>(x + w), kTabY, static_cast<int16_t>(kit::kProgPitch - w), kTabH, kBg);   // the gap between cells
    }
}

// The verdict band (UI_SYSTEM 5.8): kSurface, a 20 px dash at x6 (a state with no verdict colour), the word in S24 at x32.
static void paint_band(const Surface &c, int16_t y, std::string_view word) {
    gfx_fill(c, 0, y, c.w, kBandH, kSurface);
    kit::draw_verdict(c, 6, static_cast<int16_t>(y + 8), 20, 20, kit::Verdict::None, "", kSurface);
    gfx_text(c, kMid, 32, static_cast<int16_t>(y + 6), word, kText, kSurface);
}

namespace {

// A single tool's other actions, as rows after the result (items_[0] is the tool's own action: OK runs it with no row
// focused). A greyed row stays in its place; the footer names why while it is focused.
void add_extras(Rows &R, const std::array<conn::Item, conn::kMaxItems> &it, int n, int sel) {
    const bool facts = R.total > 0;   // fact rows above: the first action row carries the perforation
    for (int i = 1; i < n; i++) {
        const conn::Item &x0 = it[static_cast<std::size_t>(i)];
        Rows::R &x = R.push();
        x.key = "";
        x.v.put(x0.label);
        x.act = static_cast<int8_t>(i - 1);
        x.perf = facts && i == 1;
        x.focus = sel == i - 1;
        x.off = !x0.enabled;
        x.tail = x0.tag;
        x.drill = x0.act == conn::Act::Bench || x0.act == conn::Act::SpoofAs || x0.act == conn::Act::SavedCables;
    }
}

// A place page: the identity block's band (y22..67, kit::kIdY) and the list under it, with a 4 px lane on the right for the
// scroll thumb.
using kit::kPlaceListY;
constexpr int16_t kLane = 4;
constexpr int kPlaceSlots = (kFooterY - kPlaceListY) / kRowPitch;   // 7
static_assert(kPlaceSlots == ConnectionPage::kPlaceRows && kPlaceSlots <= kVisRows, "a place page shows seven list rows under its identity block");
static_assert(kValX + kValCells * 8 <= 224 - kLane, "the value column ends before the scroll lane");

// The window of R (already windowed by the builder), painted from y0 down to the footer: one 20 px row per slot, a slot
// repainted alone when its hash moved (Row D), the vacated slots cleared. The tabbed page draws a hairline on every row's top
// and closes the table with one; a place page (`place`) has no hairlines - the first action row carries the dashed
// perforation - and keeps the lane at the right edge for its scroll thumb. An action row: its label (x10, or x8 on a place
// page), focused = kFocusT ground and the 3 px kFocus bar, or on a place page the inverted focus block, a tail word or the
// chevron. Returns the rows painted.
int paint_list(const Surface &c, const Rows &R, int16_t y0, bool full, bool dim, bool blank,
               std::array<uint32_t, kVisRows> &sig_io, uint8_t &n_io, bool place = false) {
    const int slots = std::min(kVisRows, (kFooterY - y0) / kRowPitch);
    const int n = std::clamp(R.n, 0, slots);
    const int16_t w = place ? static_cast<int16_t>(c.w - kLane) : c.w;
    const int close = place ? 0 : 1;   // the closing hairline's row
    constexpr int16_t kKeyDy = kit::row_text_y(kRowPitch, 12), kValDy = kit::row_text_y(kRowPitch, 16);
    const bool count_moved = !full && n_io != n;
    const int clear_from = y0 + std::min<int>(n_io, n) * kRowPitch;   // first slot that may be stale
    std::array<uint32_t, kVisRows> sigs{};
    for (int i = 0; i < n; i++) {
        const Rows::R &row = R.r[i];
        uint32_t h = fnv(2166136261u, uint32_t(row.c) | uint32_t(row.cut) << 16 | uint32_t(dim) << 24);
        h = fnv(h, uint32_t(uint8_t(row.act)) | uint32_t(row.focus) << 8 | uint32_t(row.off) << 9 | uint32_t(row.drill) << 10
                       | uint32_t(row.perf) << 11);
        for (const char *k = row.key; *k; k++) h = fnv(h, uint8_t(*k));
        h = fnv(h, 0x100u);   // key / value separator
        for (char ch : row.v.view()) h = fnv(h, uint8_t(ch));
        for (const char *k = row.tail; *k; k++) h = fnv(h, uint8_t(*k));
        sigs[static_cast<std::size_t>(i)] = h;
    }
    if (full) {
        const int tail = y0 + n * kRowPitch + (n < slots ? close : 0);   // under the closing hairline
        if (!blank && kFooterY > tail) gfx_fill(c, 0, static_cast<int16_t>(tail), w, static_cast<int16_t>(kFooterY - tail), kBg);
    } else if (count_moved && n_io > n) {
        const int clear_to = y0 + n_io * kRowPitch + close;
        gfx_fill(c, 0, static_cast<int16_t>(clear_from), w, static_cast<int16_t>(clear_to - clear_from), kBg);
    }
    char buf[kFitBuf];
    int painted = 0;
    for (int i = 0; i < n; i++) {
        const int16_t y = static_cast<int16_t>(y0 + i * kRowPitch);
        const bool in_cleared = count_moved && y >= clear_from;
        if (!full && !in_cleared && sigs[static_cast<std::size_t>(i)] == sig_io[static_cast<std::size_t>(i)]) continue;
        painted++;
        const Rows::R &row = R.r[i];
        if (!place) gfx_fill(c, 0, y, c.w, 1, kRule);
        else if (row.perf) kit::draw_dashed(c, 0, w, y, kInk3, kBg);   // the perforation above the actions
        else gfx_fill(c, 0, y, w, 1, kBg);
        const kit::Box body{0, static_cast<int16_t>(y + 1), w, static_cast<int16_t>(kRowPitch - 1)};
        if (row.act >= 0 && place) {
            const bool bar = row.focus && kFocusBar != 0;
            const uint16_t bg = row.focus ? kFocusFill : kBg;
            const uint16_t ink = row.off ? (row.focus ? kOnFocusDim : kInk3) : (row.focus ? kOnFocus : kInk);
            const uint16_t dim_ink = row.focus ? kOnFocusDim : kInk2;
            const std::string_view tail = row.tail;
            kit::TextRun t[2];
            int nt = 0;
            t[nt++] = {8, static_cast<int16_t>(y + kValDy), &kSmall, row.v.view(), ink};
            if (!tail.empty())
                t[nt++] = {static_cast<int16_t>(w - 8 - static_cast<int>(tail.size()) * kSmall.adv), static_cast<int16_t>(y + kValDy),
                           &kSmall, tail, dim_ink};
            const kit::Box g = bar ? kit::Box{3, body.y, static_cast<int16_t>(w - 3), body.h} : body;
            if (bar) gfx_fill(c, 0, body.y, 3, body.h, kFocusBar);
            kit::paint_texts(c, g, t, nt, bg);
            if (tail.empty() && row.drill && !row.off)
                kit::draw_drill(c, static_cast<int16_t>(w - 8), static_cast<int16_t>(body.y + body.h / 2), dim_ink);
            continue;
        }
        if (row.act >= 0) {
            const kit::TextRun t{10, static_cast<int16_t>(y + kValDy), &kSmall, row.v.view(), row.off ? kFaint : kText};
            kit::paint_texts(c, body, &t, 1, row.focus ? kFocusT : kBg);
            if (row.focus) gfx_fill(c, 0, body.y, 3, body.h, kFocus);
            continue;
        }
        kit::TextRun runs[3];
        int nr = 0;
        runs[nr++] = {kKeyX, static_cast<int16_t>(y + kKeyDy), &kLabelFont, row.key, kDim};
        const std::string_view all = row.v.view();
        const int16_t vy = static_cast<int16_t>(y + kValDy);
        if (row.cut > 0 && all.size() <= static_cast<std::size_t>(kValCells)) {   // built to fit: head in its colour, current in kAmp
            runs[nr++] = {kValX, vy, &kSmall, all.substr(0, row.cut), dim ? kStale : row.c};
            runs[nr++] = {static_cast<int16_t>(kValX + row.cut * kSmall.adv), vy, &kSmall, all.substr(row.cut), dim ? kStale : kAmp};
        } else {
            // Every row is built to fit the value column (vendor names, tags and flags are added whole or not at all),
            // so a cut here is a copy bug: fit_text fails the host gate on it.
            runs[nr++] = {kValX, vy, &kSmall, fit_text(kSmall, all, c.w - kValX - 4, buf), dim ? kStale : row.c};
        }
        kit::paint_texts(c, body, runs, nr, kBg);
    }
    if (!place && (full || count_moved) && n < slots)
        gfx_fill(c, 0, static_cast<int16_t>(y0 + n * kRowPitch), c.w, 1, kRule);   // closes the table (moved with the count)
    sig_io = sigs;
    n_io = static_cast<uint8_t>(n);
    return painted;
}

// A place page's window: the focused action row stays on screen with the least scroll; with no row focused the window stays
// where the keys left it. Clamped so the list never ends above the last slot.
int place_first(int first, int focus_row, int total) {
    if (focus_row >= 0) {
        if (focus_row < first) first = focus_row;
        if (focus_row >= first + kPlaceSlots) first = focus_row - kPlaceSlots + 1;
    }
    return std::clamp(first, 0, std::max(0, total - kPlaceSlots));
}

// The scroll thumb in the lane (x220..223 of the list band): a 3 px kInk3 bar for the window's share of the list, only when the
// list is longer than the window; the lane's ground otherwise. Painted when (first, total) moved.
void paint_thumb(const Surface &c, int first, int total, bool full, uint32_t &sig) {
    const uint32_t h = (uint32_t(first) << 8 | uint32_t(total)) + 1u;
    if (!full && h == sig) return;
    sig = h;
    const int16_t x = static_cast<int16_t>(c.w - kLane), band = static_cast<int16_t>(kFooterY - kPlaceListY);
    gfx_fill(c, x, kPlaceListY, 1, band, kBg);
    if (total <= kPlaceSlots) { gfx_fill(c, static_cast<int16_t>(x + 1), kPlaceListY, kLane - 1, band, kBg); return; }
    const int track = kPlaceSlots * kRowPitch;
    const int16_t ty = static_cast<int16_t>(kPlaceListY + first * track / total);
    const int16_t th = static_cast<int16_t>(std::max(6, kPlaceSlots * track / total));
    const int16_t tx = static_cast<int16_t>(x + 1);
    gfx_fill(c, tx, kPlaceListY, kLane - 1, static_cast<int16_t>(ty - kPlaceListY), kBg);
    gfx_fill(c, tx, ty, kLane - 1, th, kInk3);
    gfx_fill(c, tx, static_cast<int16_t>(ty + th), kLane - 1, static_cast<int16_t>(kFooterY - ty - th), kBg);
}

// A place page's identity block: the kit's (kit::Ident, kit::paint_identity), shared with the Battery place.
using kit::Ident;
using kit::paint_identity;

// "2B01:0001 Anker": the ids, the vendor only when it fits the line whole.
void put_line_id(StrBuf<36> &l, uint16_t vid, uint16_t pid, const char *vendor) {
    l.put_hex(vid, 4).put_char(':').put_hex(pid, 4);
    if (vendor && vendor[0] && l.view().size() + 1 + std::string_view(vendor).size() <= 35) l.put_char(' ').put(vendor);
}

// The identity of a place with something on it (the empty and no-PD states are the page's own): Charger = the source's
// power, its PD revision and offer kinds, the contract in use and its ID; Cable = the rating, its power and EPR, the chip's
// ID; Device = what it asks, the request's state and offer, and what that is of the offer.
[[gnu::noinline]] void place_ident(Ident &id, conn::Tab tab, const TabData &d, bool stale) {
    const MatchResult &r = *d.r;
    switch (tab) {
    case conn::Tab::Source: {
        if (d.waiting) { id.head.put("Waiting"); id.dim = true; id.line.put("for the charger's offers"); return; }
        if (!r.caps_seen) {
            id.head.put(r.typec_ma || r.typec_rp ? "Type-C" : "No offers");
            id.dim = true;
            id.line.put(r.typec_ma || r.typec_rp ? "no PD offers on the line" : "no PD offers yet");
            return;
        }
        const pd::SourceCaps &caps = *d.caps;
        id.head.put_u32(r.src_max_mw / 1000).put_char('W');
        if (caps.spec_rev == 1 || caps.spec_rev == 2) id.add(pd::wire_revision_name(caps.spec_rev), Ident::Solid);
        bool pps = false, avs = false;
        for (int i = 0; i < r.rows; i++) {
            pps = pps || caps.pdos[i].type == pd::PdoType::Pps;
            avs = avs || caps.pdos[i].type == pd::PdoType::Avs;
        }
        if (caps.epr) id.add("EPR", Ident::Outline);
        if (pps) id.add("PPS", Ident::Outline);
        else if (avs) id.add("AVS", Ident::Outline);
        if (r.request_seen) {
            StrBuf<12> t;
            put_v(t, r.want_mv);
            t.put("V ");
            put_a(t, r.want_ma);
            id.add(t.view(), Ident::Volt);
        }
        id.line.put_u32(uint32_t(r.rows)).put(r.rows == 1 ? " offer, ID " : " offers, ID ");
        const pd::Identity &vid = fusb302::vdm().identity;
        if (vid.valid) put_line_id(id.line, vid.vid, vid.pid, vid.vendor);
        else {
            const char *w = conn::sv_word(survey::state().entry(pd::SvField::Identity));
            id.line.put(w[0] ? w : "Unknown");
        }
        return;
    }
    case conn::Tab::Cable: {
        const pd::Emarker &em = fusb302::emarker();
        if (!em.valid) { id.head.put("Not read"); id.dim = true; id.line.put("No e-marker answer held"); return; }
        id.head.put_u32(em.max_current_a).put("A ").put_u32(em.max_voltage_v).put_char('V');
        id.dim = stale;
        StrBuf<12> w;
        w.put_u32(em.max_power_w).put_char('W');
        id.add(w.view(), Ident::Solid);
        id.add(em.epr_capable ? "EPR" : "no EPR", Ident::Outline);
        put_line_id(id.line, em.vid, em.pid, em.vendor_str);
        const std::string_view kind = em.is_active ? ", active" : ", passive";
        if (id.line.view().size() + kind.size() <= 35) id.line.put(kind);
        return;
    }
    case conn::Tab::Sink: {
        if (d.k2_sink) { id.head.put("K2"); id.line.put("The K2 draws the power itself"); return; }
        if (!r.request_seen) { id.head.put("No request"); id.dim = true; id.line.put("The device asked for no offer"); return; }
        put_v(id.head, r.want_mv);
        id.head.put("V ");
        put_a(id.head, r.want_ma);
        const pd::Contract &ct = fusb302::contract();
        const char *st = request_state_name(ct);
        if (st[0]) id.add(st, ct.ready ? Ident::Ok : Ident::Outline);
        const pd::Pdo *p = r.want_pos > 0 && r.want_pos <= d.caps->num_pdos ? &d.caps->pdos[r.want_pos - 1] : nullptr;
        StrBuf<12> o;
        o.put(p && p->type == pd::PdoType::Pps ? "PPS" : p && p->type == pd::PdoType::Avs ? "AVS" : "PDO").put_u32(uint32_t(r.want_pos));
        id.add(o.view(), Ident::Outline);
        id.line.put("Asks ").put_u32(r.want_mw / 1000).put("W of ").put_u32(r.src_max_mw / 1000).put("W offered");
        return;
    }
    }
}

}  // namespace

int16_t ConnectionPage::row_y0() const { return locked_ ? kTabY : kRowY0; }

uint32_t ConnectionPage::actions_sig() const {
    if (!locked_) return 0;
    uint32_t h = fnv(2166136261u, uint32_t(act_sel_ + 1) | uint32_t(n_items_) << 8 | uint32_t(more_) << 16 | uint32_t(first_) << 20);
    for (int i = 0; i < n_items_; i++) {
        const conn::Item &it = items_[static_cast<std::size_t>(i)];
        h = fnv(h, uint32_t(it.enabled) | uint32_t(it.caution) << 1);
        for (const char *k = it.tag; *k; k++) h = fnv(h, uint8_t(*k));
    }
    return h;
}

namespace {
// What a place page paints under its identity block: the fact rows `build` adds, then the place's actions; built once to
// count, then for the window (the focus decides it, else the keys). The members it moves are the page's.
struct PlaceList {
    int &first;
    uint8_t &res_n, &tot_n;
    uint32_t &thumb_sig;
    std::array<uint32_t, kVisRows> &sig;
    uint8_t &n_io;
};
template <class Build>
void paint_place_list(const Surface &c, const std::array<conn::Item, conn::kMaxItems> &items, int n_items, int act_sel, bool full,
                      bool dim, PlaceList s, Build &&build) {
    Rows R;
    R.reset(Rows::kCountOnly);
    build(R);
    const int results = R.total;
    add_extras(R, items, n_items, act_sel);
    const int total = R.total;
    s.first = place_first(s.first, act_sel >= 0 ? results + act_sel : -1, total);
    s.res_n = static_cast<uint8_t>(results);
    s.tot_n = static_cast<uint8_t>(total);
    R.reset(s.first);
    build(R);
    add_extras(R, items, n_items, act_sel);
    paint_list(c, R, kPlaceListY, full, dim, false, s.sig, s.n_io, true);
    paint_thumb(c, s.first, total, full, s.thumb_sig);
}
}  // namespace

// An empty tab: the band and its one line (the port-aware text of conn::empty_state); a place page: the dim headline and
// that line in its identity block, then its action rows.
void ConnectionPage::paint_empty(bool redraw) {
    const Surface &c = ctx_.content;
    const conn::Empty e = conn::empty_state(tab_, facts_);
    if (locked_) {
        const bool full = redraw || body_ == Body::None || body_ == Body::Popup;
        Ident id;
        id.head.put(e.title);
        id.dim = true;
        id.line.put(e.hint);
        paint_identity(c, id, full, id_sig_);
        paint_place_list(c, items_, n_items_, act_sel_, full, false, {first_, res_n_, tot_n_, thumb_sig_, row_sig_, body_n_},
                         [](Rows &) {});
        body_ = Body::Empty;
        body_tab_ = tab_;
        return;
    }
    paint_tab_indicator();   // the strip owns kBodyY .. kRowY0
    body_ = Body::Empty;
    const int16_t y0 = row_y0(), list_y = static_cast<int16_t>(y0 + kListDy);
    paint_band(c, y0, e.title);
    const kit::TextRun line{kKeyX, static_cast<int16_t>(y0 + kLineDy), &kSmall, e.hint, kDim};
    kit::paint_texts(c, {0, static_cast<int16_t>(y0 + kBandH), c.w, static_cast<int16_t>(list_y - y0 - kBandH)}, &line, 1, kBg);
    Rows R;
    body_n_ = 0;
    paint_list(c, R, list_y, true, false, false, row_sig_, body_n_);
}

void ConnectionPage::paint_body(const MatchResult &r, const pd::SourceCaps &caps, std::span<const uint32_t> sink,
                                bool redraw) {
    g_conn_body_passes++;
    const Surface &c = ctx_.content;
    const pd::Link &link = ctx_.ui->link();
    TabData d;
    d.r = &r; d.caps = &caps; d.sink = sink;
    d.ul = fusb302::usb_link(dp_, dm_);          // the debounced ADC levels (Row D): a jittering pair repaints nothing
    d.waiting = link.attached() && link.waiting();
    d.k2_sink = fusb302::self_powered();
    d.k2_sink_ok = fusb302::pin_rd() == 1;
    d.fc = static_cast<fastcharge::Protocol>(fc_);
    d.in_use = link.inline_mode();
    d.vbus_mv = vbus_;
    d.vooc = vooc_;
    // The no-PD gate (ROADMAP 2026-11-10, Row C): with the settled verdict "no PD" the tab replaces its
    // placeholder rows with the verdict band and the rows that are actually measured. The Sink tab gates
    // on any no-PD verdict (a device on the line that never talks PD); the Source tab only when the source
    // never offered PD at all (an offered-but-unanswered source still shows its caps). The Cable tab and
    // the Details page are untouched, and the empty states (NO DEVICE) are decided before this.
    if (no_pd_body()) {
        paint_no_pd_body(r, redraw);
        return;
    }
    const bool dim = tab_ == conn::Tab::Cable && link.cable_stale();
    if (locked_) {   // a place page: the identity block, then the full fact table (the Source tab's survey rows included)
        const bool full = redraw || body_ == Body::None || body_ == Body::Popup;
        Ident id;
        place_ident(id, tab_, d, dim);
        paint_identity(c, id, full, id_sig_);
        paint_place_list(c, items_, n_items_, act_sel_, full, dim, {first_, res_n_, tot_n_, thumb_sig_, row_sig_, body_n_},
                         [&](Rows &R) {
                             switch (tab_) {
                             case conn::Tab::Source: source_rows(R, d); if (!d.waiting) details_rows(R); break;
                             case conn::Tab::Cable: cable_rows(R, d); break;
                             case conn::Tab::Sink: sink_rows(R, d); break;
                             }
                         });
        body_ = Body::Rows;
        body_tab_ = tab_;
        return;
    }
    Rows R;
    switch (tab_) {
    case conn::Tab::Source: if (more_) details_rows(R); else source_rows(R, d); break;
    case conn::Tab::Cable: cable_rows(R, d); break;
    case conn::Tab::Sink: sink_rows(R, d); break;
    }
    const int16_t y0 = row_y0();
    // Fixed row slots (Row D): a whole-body pass only when the area was wiped or never drawn here (new tab,
    // pop-up closed, empty state left). A row-count change no longer wipes: the vacated slots are cleared and
    // the closing hairline moves; every other row keeps its slot and repaints alone when its hash changed.
    const bool full = redraw || body_ != Body::Rows || body_tab_ != tab_;
    if (full) paint_tab_indicator();
    const int painted = paint_list(c, R, y0, full, dim, ctx_.blank, row_sig_, body_n_);   // the tabbed page never scrolls
    body_ = Body::Rows;
    body_tab_ = tab_;
    K2LOG("[CONN] body full=%d rows=%d\n", full ? 1 : 0, painted);
}

// The no-PD body (ROADMAP 2026-11-10, Row C): the verdict band (NO PD) and the one line of its case, then only the rows
// that are actually measured - the Type-C advertisement (Rp level + VBUS), the draw on the Sink tab, a legacy fast-charge
// answer on the Source tab - and a single tool's action rows. paint_body only calls this when the paint signature moved.
[[gnu::noinline]] void ConnectionPage::paint_no_pd_body(const MatchResult &r, bool redraw) {
    const Surface &c = ctx_.content;
    const bool sink_tab = tab_ == conn::Tab::Sink;
    const char *line = conn::no_pd_line(tab_, none_why_);
    // The measured rows (at most three). The Type-C row uses the match's Rp reading (the active CC line driven into the
    // Rp window counts even before typec_ma()'s 3 s mark).
    const uint32_t rp_ma = r.typec_ma ? r.typec_ma : fusb302::typec_ma();
    auto build = [&](Rows &R) {
        {
            StrBuf<28> &v = R.add("Type-C", kText);
            typec_row_text(vbus_, rp_ma, v);   // the debounced VBUS (Row D), like the list body
            if (v.view().empty()) v.put("Unknown");
        }
        if (sink_tab) {
            put_a(R.add("Draw", kAmp), uint32_t(power::abs_sat(ctx_.pm_avg.current_ma)));
            vooc_row(R, vooc_);
        } else if (legacy_row_protocol(ctx_.fc) && ctx_.fc != fastcharge::Protocol::Pending) {
            StrBuf<6> fcn;
            fc_pill_text(ctx_.fc, fcn);
            if (!fcn.view().empty()) R.add("Legacy", kEnergy).put(fcn.view());
        }
    };
    if (locked_) {   // a place page: "No PD" as the dim headline, its case on the line, then the measured rows and the actions
        const bool full = redraw || body_ == Body::None || body_ == Body::Popup;
        Ident id;
        id.head.put(text::conn::kNoPd);
        id.dim = true;
        id.line.put(line);
        paint_identity(c, id, full, id_sig_);
        paint_place_list(c, items_, n_items_, act_sel_, full, false, {first_, res_n_, tot_n_, thumb_sig_, row_sig_, body_n_}, build);
        body_ = Body::NoPd;
        body_tab_ = tab_;
        return;
    }
    Rows R;
    build(R);
    const int16_t y0 = row_y0(), list_y = static_cast<int16_t>(y0 + kListDy);
    const bool full = redraw || body_ != Body::NoPd || body_tab_ != tab_;
    if (full) {
        gfx_fill(c, 0, kit::kBodyY, c.w, kFooterY - kit::kBodyY, kBg);
        paint_tab_indicator();
        body_n_ = 0;
    }
    // The verdict band and its one line, painted only when the line moved (a live draw must not repaint them).
    uint32_t mh = fnv(2166136261u, uint32_t(tab_) | uint32_t(none_why_) << 4);
    for (const char *p = line; *p; p++) mh = fnv(mh, uint8_t(*p));
    if (full || mh != nopd_msg_sig_) {
        paint_band(c, y0, text::conn::kNoPd);
        const kit::TextRun lr{kKeyX, static_cast<int16_t>(y0 + kLineDy), &kSmall, line, kDim};
        kit::paint_texts(c, {0, static_cast<int16_t>(y0 + kBandH), c.w, static_cast<int16_t>(list_y - y0 - kBandH)}, &lr, 1, kBg);
        nopd_msg_sig_ = mh;
    }
    paint_list(c, R, list_y, full, false, true, row_sig_, body_n_);
    body_ = Body::NoPd;
    body_tab_ = tab_;
    K2LOG("[CONN] body nopd rows=%d\n", R.n);
}

void ConnectionPage::popup_row(void *self, int index, bool, kit::RowSpec &out) {
    const auto *p = static_cast<const ConnectionPage *>(self);
    const conn::Item &it = p->items_[static_cast<std::size_t>(index)];
    out.text = it.label;
    out.text_color = it.enabled ? kText : kFaint;
}

void ConnectionPage::paint_popup(uint32_t now_ms) {
    build_items();
    if (!popup_drawn_) {
        const Surface &c = ctx_.content;
        gfx_fill(c, 0, kit::kBodyY, c.w, kFooterY - kit::kBodyY, kBg);
        paint_tab_indicator();
        ctx_.rows.init(c, kRowY0, popup_row, this);
        ctx_.rows.invalidate();
        popup_drawn_ = true;
        body_ = Body::Popup;
    }
    ctx_.rows.set_count(n_items_);
    ctx_.rows.select(popup_sel_);
    ctx_.rows.draw(now_ms);
}

// The bottom bar: what the last action says, else the state of the shown tab and the topology word. It goes
// through the footer cache, so a steady state writes nothing.
void ConnectionPage::paint_status(uint32_t now_ms) {
    StrBuf<40> txt;
    FooterTone tone = FooterTone::Info;
    const pd::IdStatus &ids = fusb302::identify_status();
    const int focus = locked_ ? (act_sel_ < 0 ? 0 : 1 + act_sel_) : popup_ ? popup_sel_ : -1;
    const bool focus_off = focus >= 0 && focus < n_items_ && !items_[static_cast<std::size_t>(focus)].enabled;
    // The pop-up's or an action row's greyed item names why; the tool's own action says it only when OK is pressed (its
    // reason would repeat the band: NO DEVICE / "No device seen").
    const conn::Item *row = locked_ && act_sel_ >= 0 && focus < n_items_ ? &items_[static_cast<std::size_t>(focus)] : nullptr;
    if (focus_off && (popup_ || act_sel_ >= 0)) {
        txt.put(items_[static_cast<std::size_t>(focus)].why);
        tone = row && row->act == conn::Act::Bench ? FooterTone::Error : FooterTone::Caution;   // a blocked tool, as on the Bench
    } else if (row && row->caution) {
        txt.put(row->why);
        tone = FooterTone::Caution;
    } else if (row && row->act == conn::Act::Bench && !before(now_ms, op_until_)) {   // the focused tool's note
        const bench::Acts b = bench::actions(conn::place_of(tab_));
        if (row->arg >= 0 && row->arg < b.n) {   // the note in footer voice: a sentence
            const std::string_view hint = bench::act_hint(b.a[row->arg]);
            if (!hint.empty()) txt.put_char(hint[0] >= 'a' && hint[0] <= 'z' ? static_cast<char>(hint[0] - 'a' + 'A') : hint[0]).put(hint.substr(1));
        }
    } else if (ctx_.em_scanning) {
        txt.put("Reading cable...");
    } else if (before(now_ms, ctx_.em_listen_until)) {
        txt.put("Listening for cable...");
    } else if (id_asked_ && ids.state == pd::IdState::Running) {
        StrBuf<24> t;
        identify_status_text(ids, t);
        txt.put(t.view());
    } else {
        if (id_asked_) {   // Identify ended: its result stays a few seconds
            StrBuf<24> t;
            identify_status_text(ids, t);
            id_asked_ = false;
            set_op(t.c_str(), now_ms, 4000);
        }
        conn::StatusIn s;
        s.tab = tab_;
        s.f = facts_;
        s.spoof_armed = ctx_.cables.armed();
        s.sv = &survey::state();
        s.no_pd = no_pd_body();
        const bool op = before(now_ms, op_until_);
        if (op) s.op = op_.view();
        // The PD diagnosis verdict, only while its session is open: a finished one would contradict the tab's live state.
        char dv[pd::diag::screen::kVerdictChars + 8];
        const pd::diag::Report &dr = pd::diag::live::report();
        const bool diag_on = pd::diag::live::state() == pd::diag::live::State::Open && !dr.empty && !dr.no_pd;
        if (diag_on) {
            pd::diag::screen::verdict(dr, dv);
            s.diag = dv;
        }
        conn::status_text(s, txt);
        if (!op && s.spoof_armed && facts_.charger) tone = FooterTone::Caution;
        else if (diag_on && txt.view() == s.diag) tone = DiagnosisPage::tone_of(dr);
    }
    status_txt_ = txt;
    draw_footer_cached(ctx_.content, tone, txt.view(), status_);
}

void ConnectionPage::update(uint32_t now_ms) {
    const pd::Link &link = ctx_.ui->link();
    title_badge(false);
    sense(now_ms);
    if (locked_) {
        build_items();   // the action rows and the OK verb follow the live state
        if (act_sel_ + 1 >= n_items_) act_sel_ = n_items_ - 2;
    }
    paint_status(now_ms);
    if (popup_) {
        paint_popup(now_ms);
        return;
    }
    // Cheap pre-check: hash only the inputs that can reach the screen -- link state, caps header fields,
    // the displayed contract fields (flags/accepted/ready/rejected/pos/mv/op_ma/max_ma drive the Request /
    // State / Used rows), sink/identify list headers, e-marker and VDM scalar fields, fc, analog D+/D-, the
    // topology facts. Counters that are never rendered (ct.rdo, ct.requests, vdm.requests) are left out, so
    // they do not even run match_live(). Only when that cheap signature changes do we run match_live() and
    // the full paint signature; the full-sig gate below still decides the repaint, so the rendered output is
    // identical. All reads are O(1) scalars or bounded fixed arrays: no heap, no match_compute, no usb_link()
    // call, no paint.
    const pd::SourceCaps &live_caps = pd::active_caps();
    const pd::Contract &ct = fusb302::contract();
    const pd::SinkCapsSeen &sniffed = fusb302::sink_caps();
    const pd::IdStatus &idst = fusb302::identify_status();
    const pd::Emarker &em = fusb302::emarker();
    const pd::VdmInfo &vdm = fusb302::vdm();
    const pd::VdmInfo &snk = fusb302::sink_vdm();
    // Row D: debounce the volatile inputs before they reach a signature -- the fast-charge verdict, the
    // ADC levels behind the usb-link family and the Rp reading, and the Type-C row's VBUS. A value must
    // hold kHoldMs to commit, so a flapping input never even runs match_live() on a steady link.
    fc_ = fc_h_.commit(uint32_t(ctx_.fc), now_ms);
    dp_ = static_cast<int32_t>(dp_h_.commit(uint32_t(ctx_.analog ? ctx_.analog->dp_mv : -1), now_ms));
    dm_ = static_cast<int32_t>(dm_h_.commit(uint32_t(ctx_.analog ? ctx_.analog->dm_mv : -1), now_ms));
    cc1_ = static_cast<int32_t>(cc1_h_.commit(uint32_t(ctx_.analog ? ctx_.analog->cc1_mv : -1), now_ms));
    cc2_ = static_cast<int32_t>(cc2_h_.commit(uint32_t(ctx_.analog ? ctx_.analog->cc2_mv : -1), now_ms));
    const int32_t vbus_mv = static_cast<int32_t>(
        vbus_h_.commit(uint32_t(typec_vbus_mv(ctx_.pm_online, ctx_.vbus_fast_mv)), now_ms));
    vbus_ = vbus_mv;
    if (!ctx_.vooc.seen) vooc_ = 0;   // a session end clears the note at once
    else if (!vooc_ || uint32_t(now_ms - vooc_at_) >= kVoocNoteMs) { vooc_ = 1u << 16 | ctx_.vooc.r82; vooc_at_ = now_ms; }
    uint32_t c = 2166136261u;
    c = fnv(c, vooc_);
    c = fnv(c, uint32_t(link.attached()) | uint32_t(link.waiting()) << 1 | uint32_t(link.inline_mode()) << 2
              | uint32_t(pd::is_fake()) << 3 | uint32_t(fusb302::active_cc()) << 4
              | uint32_t(tab_) << 8 | uint32_t(link.cable_stale()) << 12 | fc_ << 16
              | uint32_t(facts_.topo) << 24 | uint32_t(facts_.wait) << 26 | uint32_t(fusb302::self_powered()) << 28);
    c = fnv(c, uint32_t(verdict_) | uint32_t(none_why_) << 4);   // the no-PD gate's verdict settles at attach+1.5 s
    c = fnv(c, uint32_t(facts_.cable_fact));                     // the empty Cable tab's words: chip silent / no chip
    c = fnv(c, fusb302::session());
    c = fnv(c, uint32_t(live_caps.valid) | uint32_t(live_caps.num_pdos) << 8 | uint32_t(live_caps.epr) << 16
              | uint32_t(live_caps.spec_rev) << 17 | uint32_t(live_caps.epr_partial) << 19);
    c = fnv(c, live_caps.raw_header);
    c = fnv(c, live_caps.rx_count);
    c = fnv(c, uint32_t(ct.valid) | uint32_t(ct.ready) << 1 | uint32_t(ct.accepted) << 2
              | uint32_t(ct.rejected) << 3 | uint32_t(ct.pos) << 8 | uint32_t(ct.flags) << 16);
    c = fnv(c, uint32_t(ct.mv) | uint32_t(ct.op_ma) << 16);
    c = fnv(c, ct.max_ma);
    c = fnv(c, uint32_t(sniffed.valid) | uint32_t(sniffed.n) << 8);
    for (int i = 0; i < sniffed.n; i++) c = fnv(c, sniffed.raw[i]);
    c = fnv(c, uint32_t(fusb302::identify_current()) | uint32_t(idst.ok_mask) << 8 | uint32_t(idst.num_sink_pdos) << 16);
    for (int i = 0; i < idst.num_sink_pdos; i++) c = fnv(c, idst.sink_pdos[i]);
    c = fnv(c, uint32_t(fusb302::pd_traffic()) | uint32_t(fusb302::pin_rd()) << 2);
    c = fnv(c, uint32_t(em.valid) | uint32_t(em.is_active) << 1 | uint32_t(em.vid) << 8 | uint32_t(em.num_vdos) << 24);
    c = fnv(c, em.raw_vdos[4]);
    c = fnv(c, uint32_t(em.max_power_w) | uint32_t(em.sop) << 16);
    c = fnv(c, fusb302::cable_seq());
    c = fnv(c, uint32_t(vdm.identity.valid) | uint32_t(vdm.identity.vid) << 8 | uint32_t(vdm.identity.pid) << 16);
    c = fnv(c, vdm.identity.id_header);
    c = fnv(c, uint32_t(vdm.num_svids) | uint32_t(vdm.modes_svid) << 8 | uint32_t(vdm.num_modes) << 24);
    for (int i = 0; i < vdm.num_svids; i++) c = fnv(c, vdm.svids[i]);
    for (int i = 0; i < vdm.num_modes; i++) c = fnv(c, vdm.modes[i]);
    c = fnv(c, uint32_t(vdm.cuktech) | uint32_t(vdm.xiaomi) << 1 | uint32_t(vdm.xiaomi_cmd) << 2);
    {   // the lab-only Apple readout's rows (the serial is never displayed nor hashed)
        const pd::AppleInfo &ap = fusb302::apple_info();
        c = fnv(c, uint32_t(ap.valid) | uint32_t(ap.why) << 1 | uint32_t(ap.reads) << 9 | uint32_t(ap.ok) << 17);
        for (char ch : ap.product) c = fnv(c, uint32_t(uint8_t(ch)));
        for (char ch : ap.fw) c = fnv(c, uint32_t(uint8_t(ch)));
        for (char ch : ap.hw) c = fnv(c, uint32_t(uint8_t(ch)));
    }
    c = fnv(c, uint32_t(snk.identity.valid) | uint32_t(snk.identity.vid) << 8 | uint32_t(snk.identity.pid) << 16);
    c = fnv(c, snk.identity.id_header);
    c = fnv(c, uint32_t(snk.num_svids));
    for (int i = 0; i < snk.num_svids; i++) c = fnv(c, snk.svids[i]);
    c = fnv(c, uint32_t(ctx_.analog.has_value()));
    c = fnv(c, uint32_t(dp_));
    c = fnv(c, uint32_t(dm_));   // the debounced ADC levels (Row D): raw jitter is not hashed
    c = fnv(c, uint32_t(cc1_));
    c = fnv(c, uint32_t(cc2_));
    c = fnv(c, uint32_t(vbus_));   // the Type-C row's VBUS (100 mV steps)
    const uint32_t svs = survey_sig(more_);
    c = fnv(c, svs);
    const uint32_t asig = actions_sig();
    c = fnv(c, asig);
    if (sig_valid_ && c == cheap_sig_) return;   // nothing changed: skip match_live + the full sig
    cheap_sig_ = c;

    const bool empty = conn::tab_empty(tab_, facts_) && !(tab_ == conn::Tab::Sink && fusb302::self_powered());
    if (empty) {
        uint32_t h = fnv(2166136261u, uint32_t(tab_) | uint32_t(facts_.topo) << 4 | 1u << 8 | uint32_t(facts_.inline_sink) << 9
                                     | uint32_t(facts_.wait) << 10 | uint32_t(facts_.open_cc_mv) << 12
                                     | uint32_t(facts_.wake_port_off) << 24
                                     | uint32_t(facts_.vbus_mv >= raguard::kVbusAttachedMv) << 25);
        h = fnv(h, uint32_t(facts_.cable_fact));
        h = fnv(h, asig);
        if (sig_valid_ && h == sig_) return;
        const bool redraw = !sig_valid_;
        sig_ = h;
        sig_valid_ = true;
        paint_empty(redraw);
        return;
    }

    MatchResult r = match_live(link);
    // The Rp advertisement of an attached source exists even when the chip's gated typec_ma() reads 0
    // (a default-USB Rp, a load inline, before its 3 s mark): the active CC line driven into the Rp
    // window says so, and its divider level gives the current.
    if (!r.caps_seen && !pd::is_fake() && link.attached() && !fusb302::pd_traffic()) {
        const uint8_t acc = fusb302::active_cc();
        const int32_t cc_mv = acc == 2 ? cc2_ : acc == 1 ? cc1_ : -1;   // the debounced line level (Row D)
        if (cc_line_has_rp(cc_mv)) {
            r.typec_rp = true;
            if (!r.typec_ma) r.typec_ma = typec_ma_from_cc_mv(cc_mv);
        }
    }
    const pd::SourceCaps *caps = &g_book.caps;   // filled by match_live above
    const WantsList wl = wants_now();
    const std::span<const uint32_t> sink = wl.pdos;

    uint32_t h = 2166136261u;
    h = fnv(h, uint32_t(verdict_) | uint32_t(none_why_) << 4);   // the no-PD body switches on the verdict edge
    h = fnv(h, uint32_t(tab_) | uint32_t(link.cable_stale()) << 8 | uint32_t(link.waiting()) << 9 | uint32_t(caps->epr) << 10 | uint32_t(caps->spec_rev) << 11 | uint32_t(r.verdict) << 16 | uint32_t(r.flags) << 24);
    h = fnv(h, r.typec_ma | uint32_t(fusb302::pd_traffic()) << 16 | uint32_t(link.inline_mode()) << 17);
    h = fnv(h, uint32_t(r.typec_rp));
    h = fnv(h, uint32_t(vbus_mv));
    h = fnv(h, r.want_mv); h = fnv(h, r.want_ma); h = fnv(h, r.want_mw); h = fnv(h, uint32_t(r.want_pos));
    h = fnv(h, uint32_t(r.gone) | g_book.offered << 8); h = fnv(h, r.src_max_mw); h = fnv(h, r.sink_max_mv);
    for (int i = 0; i < r.rows; i++) {
        const pd::Pdo &p = caps->pdos[i];
        h = fnv(h, uint32_t(r.row[i]) | uint32_t(p.type) << 8);
        h = fnv(h, uint32_t(p.min_mv) | uint32_t(p.max_mv) << 16);
        h = fnv(h, p.max_ma);
    }
    for (uint32_t raw : sink) h = fnv(h, raw);
    h = fnv(h, svs);
    h = fnv(h, asig);
    h = fnv(h, uint32_t(wl.src));
    h = fnv(h, uint32_t(ct.flags) | uint32_t(ct.accepted) << 8 | uint32_t(ct.ready) << 9 | uint32_t(ct.rejected) << 10);
    {   // the tab bodies read more than the match: cable, identity, USB link, protocol, sink mode
        const pd::Identity &id = vdm.identity;
        const usblink::Link ul = fusb302::usb_link(dp_, dm_);   // debounced levels (Row D)
        h = fnv(h, uint32_t(em.valid) | uint32_t(em.is_active) << 1 | uint32_t(em.vid) << 8 | uint32_t(em.raw_vdos[4]) << 24);
        h = fnv(h, em.raw_vdos[4] | uint32_t(em.max_power_w) << 8 | uint32_t(em.sop) << 24);
        h = fnv(h, uint32_t(id.valid) | uint32_t(id.vid) << 8 | uint32_t(id.pid) << 16);
        h = fnv(h, id.id_header);
        const pd::Identity &sid = snk.identity;
        h = fnv(h, uint32_t(sid.valid) | uint32_t(sid.vid) << 8 | uint32_t(sid.pid) << 16);
        h = fnv(h, sid.id_header);
        h = fnv(h, uint32_t(vdm.num_svids) | uint32_t(snk.num_svids) << 8);
        for (int i = 0; i < vdm.num_svids; i++) h = fnv(h, vdm.svids[i]);
        for (int i = 0; i < snk.num_svids; i++) h = fnv(h, snk.svids[i]);
        h = fnv(h, uint32_t(ul.family) | uint32_t(ul.tbt) << 4 | uint32_t(ul.dp) << 8 | uint32_t(ul.cable_limited) << 9 | ul.mbps << 12);
        h = fnv(h, uint32_t(ul.dp_adv) | uint32_t(ul.tbt_adv) << 1 | uint32_t(ul.dp_2lane) << 2 | uint32_t(ul.dp_4lane) << 3);
        h = fnv(h, fc_ | uint32_t(fusb302::self_powered()) << 8 | uint32_t(fusb302::pin_rd() == 1) << 9);
        h = fnv(h, vooc_);   // the Sink tab's raw VOOC note (sampled every kVoocNoteMs)
    }
    if (sig_valid_ && h == sig_) return;
    const bool redraw = !sig_valid_;   // invalidated (page entered, tab, pop-up closed, repaint pass): the area is stale
    sig_ = h;
    sig_valid_ = true;
    paint_body(r, *caps, sink, redraw);
}

}  // namespace k2
