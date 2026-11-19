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
// (cleared by the driver on every attach), the ones Identify collected in this attach session, or inline interrogation's.
WantsList wants_now(bool iq_ok = false) {
    WantsList w = pick_wants(fusb302::sink_caps(), fusb302::identify_current(), fusb302::identify_status());
    if (w.pdos.empty() && iq_ok) {
        const pd::IqStatus &iq = fusb302::interrogate_status();
        if (iq.num_sink_pdos > 0) {
            w.pdos = std::span<const uint32_t>(iq.sink_pdos.data(), iq.num_sink_pdos);
            w.src = WantsSrc::Identify;
        }
    }
    return w;
}
std::span<const uint32_t> sniffed_sink_caps() { return wants_now().pdos; }
// The mismatch bit is part of the RDO we sniffed; unknown while no Request was seen.
Tri sniffed_mismatch() {
    const pd::Contract &c = fusb302::contract();
    if (!c.valid || pd::is_fake()) return Tri::Unknown;
    return (c.flags & pd::kRdoFMismatch) ? Tri::Yes : Tri::No;
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
static MatchResult g_last_match{};

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
    op_ = StrBuf<24>();
    op_until_ = 0;
    id_asked_ = false;
    iq_asked_ = false;
    iq_armed_until_ = 0;
    sig_valid_ = false;
    match_valid_ = false;
    act_sel_ = -1;
    first_ = 0;
    id_sig_ = thumb_sig_ = 0;
    sense(ctx_.now);                   // facts only; the topology window starts with this one sample
    build_items();
    act_sel_ = ctx_.keep.place_sel[static_cast<int>(conn::place_of(tab_))];
    if (act_sel_ + 1 >= n_items_) act_sel_ = n_items_ - 2;
}

// A place page (Charger, Cable, Device): one tab, no tab strip, the place's name and icon in the masthead.
// OK with no row focused runs the place's own read; its actions are rows under the fact table.
void ConnectionPage::lock(conn::Tab t) {
    tab_ = t;
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
    op_ = StrBuf<24>();
    op_until_ = 0;
    id_asked_ = false;
    if (iq_asked_ && fusb302::interrogate_running()) fusb302::interrogate_abort();
    iq_asked_ = false;
    iq_armed_until_ = 0;
    ctx_.em_listen_until = 0;    // nor does a listen window or its "heard nothing" verdict
    ctx_.em_quiet = false;
}

void ConnectionPage::draw_static() {
    if (ctx_.budget && !ctx_.budget->admit(140 * kBarH, true)) return;
    title_bar();
    invalidate();
}

void ConnectionPage::invalidate() {
    sig_valid_ = false;
    body_pending_ = false;
    match_valid_ = false;
    body_ = Body::None;
    body_n_ = 0;
    tail_cleared_ = false;
    close_done_ = false;
    body_clear_y_ = kFooterY;
    row_sig_.fill(0);
    id_sig_ = thumb_sig_ = 0;
    status_.invalidate();
}

Strip ConnectionPage::labels() const {
    const bool share = (tab_ == conn::Tab::Source && pd::active_caps().valid) || (tab_ == conn::Tab::Cable && fusb302::emarker().valid);
    const int i = act_sel_ < 0 ? 0 : 1 + act_sel_;
    const bool iq_confirm = tab_ == conn::Tab::Sink && i < n_items_ && items_[static_cast<std::size_t>(i)].act == conn::Act::Identify
                         && before(ctx_.now, iq_armed_until_);
    const sl::Id ok = iq_confirm ? sl::Confirm
                    : i < n_items_ ? conn::act_slot(items_[static_cast<std::size_t>(i)].act, false, share)
                    : share ? sl::ShareLong : sl::None;
    return {{(iq_asked_ && fusb302::interrogate_running()) ? sl::Stop : sl::Back, sl::Up, ok, sl::Dn}};
}

bool ConnectionPage::on_event(Event e) {
    const bool shrt = e.press == Press::Short;
    if (shrt && e.button == Button::Back && iq_asked_ && fusb302::interrogate_running()) {
        fusb302::interrogate_abort();
        iq_asked_ = false;
        set_op("Cancelled", ctx_.now);
        sig_valid_ = false;
        update(ctx_.now);
        return true;
    }
    if (shrt && e.button != Button::Back) {
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
    const emrec::Record rec = emrec::make({em.raw_vdos.data(), 5}, em.spec_rev, "Live cable");
    const std::size_t len = qrshare::records::emarker(rec, "Live cable", nullptr, ctx_.ui->share_buffer());
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
    // The no-PD gate (ROADMAP 2026-11-10, Row C): with the settled verdict "no PD" a device on the line
    // cannot be identified. The alone path (a dual-role phone) is PD-capable and stays offered.
    a.pd_none = verdict_ == pd::PdVerdict::None && (facts_.inline_sink || facts_.sink_seen) && !a.alone;
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
}

void ConnectionPage::activate(const conn::Item &it, uint32_t now_ms) {
    if (!it.enabled) return;   // the status bar names the reason while the greyed item is selected
    switch (it.act) {
    case conn::Act::Refresh:
        // Inline the K2 only listens (a re-read asks nothing of the charger or the device): say so, instead of a rescan that
        // shows nothing. "Ask device" is what asks.
        if (ctx_.ui->run_action(Action::PdRescan, now_ms))
            set_op(ctx_.ui->link().inline_mode() || ctx_.inline_mode ? "Inline: listens only" : "Rescanning...", now_ms, 2500);
        else set_op("Busy, retry", now_ms, 1500);   // a negotiation in flight: the refresh waits for it
        break;
    case conn::Act::Details: break;
    case conn::Act::ShareCaps:
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
        if (!ctx_.ui->push_menu(Menu::Spoof, now_ms)) set_op("Test running", now_ms);
        return;
    case conn::Act::SavedCables:
        if (!ctx_.ui->push_menu(Menu::Emarker, now_ms)) set_op("Test running", now_ms);
        return;
    case conn::Act::ShareCable:
        share_cable(now_ms);
        return;
    case conn::Act::Identify: {
        const bool inl = ctx_.ui->link().inline_mode() || ctx_.inline_mode;
        if (inl) {
            const bool confirm = before(now_ms, iq_armed_until_);
            iq_armed_until_ = 0;
            pd::IqFacts f{};
            f.inline_link = true;
            f.busy = ctx_.ui->ports_busy();
            f.confirmed = confirm;
            switch (fusb302::interrogate_start(f)) {
            case pd::IqRefusal::None:
                iq_asked_ = true;
                iq_have_ = true;
                iq_session_ = fusb302::session();
                op_.clear();
                op_until_ = 0;
                break;
            case pd::IqRefusal::NeedConfirm:
                iq_armed_until_ = now_ms + Ui::kConfirmWindowMs;
                set_op("OK again: resets link", now_ms, Ui::kConfirmWindowMs);
                break;
            case pd::IqRefusal::SelfPowered: set_op("Powered from VBUS", now_ms); break;
            case pd::IqRefusal::OwnContract: set_op("K2 holds contract", now_ms); break;
            case pd::IqRefusal::Running:
            case pd::IqRefusal::Busy:
            case pd::IqRefusal::BadSpoof: set_op("Busy, retry", now_ms); break;
            case pd::IqRefusal::NoCc:
            case pd::IqRefusal::NotInline: set_op("No device inline", now_ms); break;
            }
            break;
        }
        const bool started = ctx_.ui->run_action(Action::Identify, now_ms);
        id_asked_ = started;
        if (!started) set_op(ctx_.id_no_device ? "No device to ask" : "Identify refused", now_ms);
        break;
    }
    case conn::Act::Bench: break;   // on_event opens it (it may leave this page)
    }
    sig_valid_ = false;
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
    bool iq_ok = false;     // fusb302::interrogate_status() belongs to the current session
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

void flush_fixed(Rows &R, StrBuf<28> &head, uint16_t cur_ma) {
    if (head.view().empty()) return;
    StrBuf<28> &v = R.add("Fixed", kVolt);
    v.put(head.view()).put_char('V');
    R.cut_here();
    v.put_char(' ');
    put_a(v, cur_ma);
    head.clear();
}

void step_fixed(Rows &R, StrBuf<28> &head, uint16_t &cur_ma, const pd::Pdo &p) {
    if (p.type != pd::PdoType::Fixed) return;
    StrBuf<12> vs;
    put_v(vs, p.max_mv);
    StrBuf<12> as;
    put_a(as, p.max_ma);
    const std::size_t joined = head.view().size() + (head.view().empty() ? 0 : 1) + vs.view().size();
    if (!head.view().empty() && (p.max_ma != cur_ma || joined + 2 + as.view().size() > kValCells)) {
        flush_fixed(R, head, cur_ma);
    }
    if (!head.view().empty()) head.put_char('/');
    head.put(vs.view());
    cur_ma = p.max_ma;
}

// The fixed PDOs, one row per current group and per line of 19 cells ("5/9/15V 3A", "20/28/36/48V 5A"). Every line
// ends with its own current, so no line needs the cut and none reads as another group's.
void fixed_rows(Rows &R, const pd::SourceCaps &caps, int rows) {
    StrBuf<28> head;   // the voltages of the open line, "5/9/15"
    uint16_t cur_ma = 0;
    for (int i = 0; i < rows; i++) step_fixed(R, head, cur_ma, caps.pdos[i]);
    flush_fixed(R, head, cur_ma);
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

void sink_pdo_rows(Rows &R, std::span<const uint32_t> sink) {
    StrBuf<28> head;
    uint16_t cur_ma = 0;
    for (uint32_t raw : sink) step_fixed(R, head, cur_ma, pd::decode_pdo(raw));
    flush_fixed(R, head, cur_ma);

    for (uint32_t raw : sink) {
        const pd::Pdo p = pd::decode_pdo(raw);
        if (p.type != pd::PdoType::Pps && p.type != pd::PdoType::Avs) continue;
        range_row(R, p.type == pd::PdoType::Pps ? "PPS" : "AVS", p);
    }

    for (uint32_t raw : sink) {
        const pd::Pdo p = pd::decode_pdo(raw);
        if (p.type != pd::PdoType::Battery) continue;
        StrBuf<28> &v = R.add("BatPDO", kVolt);
        if (p.min_mv != p.max_mv) { put_v(v, p.min_mv); v.put_char('-'); }
        put_v(v, p.max_mv);
        v.put("V ");
        v.put_u32(p.max_mw / 1000).put_char('W');
    }
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

void put_dwh(StrBuf<28> &v, uint16_t dwh) {
    v.put_u32(dwh / 10u).put_char('.').put_u32(dwh % 10u);
}

[[gnu::noinline]] void sink_device_rows(Rows &R, bool iq_ok) {
    const pd::IqStatus &iq = fusb302::interrogate_status();
    const pd::ExtInfo &ext = fusb302::ext();
    const bool id_cur = fusb302::identify_current();
    const pd::ExtInfo &id_ext = fusb302::identify_ext();

    // Manufacturer_Info (MIDB): from inline interrogation, live ext, or standalone Identify snapshot.
    const pd::ManufacturerInfo &mfr = (iq_ok && iq.mfr.valid) ? iq.mfr
                                    : ext.mfr.valid ? ext.mfr
                                    : id_cur ? id_ext.mfr : ext.mfr;
    if (mfr.valid && (iq_ok || id_cur || mfr.name[0])) {
        if (mfr.name[0]) {
            StrBuf<28> &v = R.add("Mfr", kText);
            for (std::size_t i = 0; i < sizeof mfr.name && mfr.name[i] && v.view().size() < static_cast<std::size_t>(kValCells); i++)
                v.put_char(mfr.name[i]);
        } else if (mfr.vid || mfr.pid) {
            put_id(R.add("Mfr", kText), mfr.vid, mfr.pid, nullptr);
        }
    }

    // PD Revision (RMDO): from inline interrogation or Identify.
    const uint32_t rev = (iq_ok && iq.revision) ? iq.revision : (id_cur ? fusb302::identify_status().revision : 0u);
    if (rev != 0) {
        StrBuf<28> &v = R.add("Rev", kText);
        v.put("PD ").put_u32((rev >> 28) & 0xFu).put_char('.').put_u32((rev >> 24) & 0xFu)
         .put(" v").put_u32((rev >> 20) & 0xFu).put_char('.').put_u32((rev >> 16) & 0xFu);
    }

    // Sink_Capabilities_Extended (SKEDB): SPR/EPR max PDP and sink modes.
    if (iq_ok && iq.sink_ext_len >= 21) {
        StrBuf<28> &v = R.add("PDP", kText);
        const uint8_t spr_max = iq.sink_ext[20];
        const uint8_t epr_max = iq.sink_ext_len >= 24 ? iq.sink_ext[23] : 0;
        const uint8_t modes = iq.sink_ext[17];
        if (spr_max) v.put("SPR ").put_u32(spr_max).put_char('W');
        if (epr_max) {
            StrBuf<12> e;
            e.put(v.view().empty() ? "EPR " : " EPR ").put_u32(epr_max).put_char('W');
            put_if_fits(v, e.view());
        }
        if (modes & 0x01) put_if_fits(v, v.view().empty() ? "PPS" : " PPS");
        if (modes & 0x20) put_if_fits(v, v.view().empty() ? "AVS" : " AVS");
        if (v.view().empty()) v.put("Present");
    }

    // Battery_Status (BSDO) + Battery_Capabilities (BCDB): live SoC % + state, and design / full / health.
    const pd::BatteryCaps &bat = (iq_ok && iq.battery.valid) ? iq.battery
                               : ext.battery.valid ? ext.battery
                               : id_cur ? id_ext.battery : ext.battery;
    const pd::BatteryStatus &bst = (iq_ok && iq.battery_status.valid) ? iq.battery_status
                                 : ext.battery_status.valid ? ext.battery_status
                                 : id_cur ? id_ext.battery_status : ext.battery_status;
    if (bst.valid && !bst.invalid_ref) {
        if (!bst.present) {
            R.add("Batt", kFaint).put("Not present");
        } else {
            const int soc = pd::battery_soc_pct(bat, bst);
            const char *st = bst.charging == 0 ? "chg" : bst.charging == 1 ? "dischg" : bst.charging == 2 ? "idle" : "";
            StrBuf<28> &v = R.add("Batt", bst.charging == 0 ? kOk : kText);
            if (soc >= 0) {
                v.put_u32(static_cast<uint32_t>(soc)).put_char('%');
                if (st[0]) v.put_char(' ').put(st);
                if (bst.capacity_known()) {
                    StrBuf<12> wh;
                    wh.put_char(' ');
                    wh.put_u32(bst.present_dwh / 10u).put_char('.').put_u32(bst.present_dwh % 10u).put("Wh");
                    put_if_fits(v, wh.view());
                }
            } else if (bst.capacity_known()) {
                put_dwh(v, bst.present_dwh);
                v.put("Wh");
                if (st[0]) v.put_char(' ').put(st);
            } else {
                v.put(st[0] ? st : "Present");
            }
        }
    }
    if (bat.design_known() || bat.last_full_known()) {
        const int hlth = (bat.design_known() && bat.last_full_known()) ? pd::battery_health_pct(bat) : -1;
        const uint16_t hc = hlth >= 80 ? kOk : hlth >= 0 ? kCaution : kText;
        StrBuf<28> &v = R.add("Health", hc);
        if (bat.design_known() && bat.last_full_known()) {
            put_dwh(v, bat.last_full_dwh);
            v.put_char('/');
            put_dwh(v, bat.design_dwh);
            v.put("Wh");
            if (hlth >= 0) {
                StrBuf<8> hs;
                hs.put_char(' ').put_u32(static_cast<uint32_t>(hlth)).put_char('%');
                put_if_fits(v, hs.view());
            }
        } else if (bat.design_known()) {
            put_dwh(v, bat.design_dwh);
            v.put("Wh design");
        } else {
            put_dwh(v, bat.last_full_dwh);
            v.put("Wh full");
        }
    }

    // SVIDs (from inline interrogation or sniffed sink_vdm).
    const std::uint16_t *svids = nullptr;
    unsigned n_svids = 0;
    if (iq_ok && iq.num_svids > 0) { svids = iq.svids.data(); n_svids = iq.num_svids; }
    else if (fusb302::sink_vdm().num_svids > 0) { svids = fusb302::sink_vdm().svids.data(); n_svids = fusb302::sink_vdm().num_svids; }
    else if (id_cur && fusb302::vdm().num_svids > 0) { svids = fusb302::vdm().svids.data(); n_svids = fusb302::vdm().num_svids; }
    if (n_svids > 0) {
        StrBuf<28> &v = R.add("SVIDs", kDim);
        for (unsigned i = 0; i < n_svids; i++) {
            const bool last = i + 1 == n_svids;
            const std::size_t need = (i ? 1 : 0) + 4 + (last ? 0 : 2);
            if (v.view().size() + need > static_cast<std::size_t>(kValCells)) {
                if (v.view().size() + 2 <= static_cast<std::size_t>(kValCells)) v.put(" +");
                break;
            }
            if (i) v.put_char(' ');
            v.put_hex(svids[i], 4);
        }
    }

    // EPR_Sink_Capabilities count (from inline interrogation).
    if (iq_ok && iq.num_epr_pdos > 0) {
        StrBuf<28> &v = R.add("EPR", kText);
        v.put_u32(iq.num_epr_pdos).put(iq.num_epr_pdos == 1 ? " PDO" : " PDOs");
        if (iq.epr_partial) put_if_fits(v, " (part)");
    }
}

[[gnu::noinline]] void sink_rows(Rows &R, const TabData &d) {
    const MatchResult &r = *d.r;
    const pd::IqStatus &iq = fusb302::interrogate_status();
    const bool iq_ok = d.iq_ok;
    const bool id_cur = fusb302::identify_current();
    const pd::Identity &id = (iq_ok && iq.id.valid) ? iq.id
                           : (id_cur && fusb302::identify_identity().valid) ? fusb302::identify_identity()
                           : fusb302::sink_vdm().identity;
    if (d.k2_sink && !id_cur) {   // the K2 draws the power itself (before Identify)
        R.add("ID", kFaint).put("K2");
        R.add("Sink", kOk).put(d.k2_sink_ok ? "K2 (sink mode OK)" : "K2 (sink mode PLUS)");
        link_rows(R, d);
        return;
    }
    // The ID row: sniffed when neither Identify nor inline interrogation collected it.
    if (id.valid) {
        StrBuf<28> &v = R.add("ID");
        conn::id_value(v, kValCells, id_cur || (iq_ok && iq.id.valid), id.vid, id.pid, id.vendor);
    } else R.add("ID").put("Unknown");   // until the sink answers a Discover Identity (the charger's own, sniffed)
    if (id_cur && (d.k2_sink || !r.request_seen)) {
        if (!d.sink.empty()) sink_pdo_rows(R, d.sink);
        sink_device_rows(R, iq_ok);
        return;
    }
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
    if (!d.sink.empty()) sink_pdo_rows(R, d.sink);
    else R.add("Caps", kFaint).put("Not seen");
    sink_device_rows(R, iq_ok);
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
    uint32_t h = mix_u32(2166136261u, uint32_t(more) | uint32_t(sv.running()) << 1 | uint32_t(sv.ran()) << 2 | sv.resolved() << 8 | sv.planned() << 16);
    for (unsigned i = 0; i < pd::kSvFields; i++) {
        const pd::SvEntry &e = sv.entry(static_cast<pd::SvField>(i));
        h = mix_u32(h, uint32_t(e.state) | uint32_t(e.why) << 8);
    }
    const pd::ExtInfo &ex = fusb302::ext();
    h = mix_u32(h, uint32_t(ex.mfr.valid) | uint32_t(ex.mfr.vid) << 8 | uint32_t(ex.mfr.pid) << 24);
    h = pg::mix_cstr(h, ex.mfr.name, sizeof ex.mfr.name);
    h = mix_u32(h, uint32_t(ex.caps_ext.valid) | uint32_t(ex.caps_ext.pdp_w) << 8 | uint32_t(ex.caps_ext.epr_pdp_w) << 16);
    const pd::SourceCaps &epr = fusb302::epr_info_list();
    h = mix_u32(h, uint32_t(epr.valid) | uint32_t(epr.num_pdos) << 8);
    for (std::size_t i = 0; i < epr.num_pdos && i < pd::kMaxPdos; i++) h = mix_u32(h, epr.pdos[i].raw);
    return h;
}

}  // namespace

// ---------------------------------------------------------------------------------------
// Painting
// ---------------------------------------------------------------------------------------

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
struct ListDrawState {
    bool &tail_cleared;
    bool &close_done;
    int16_t &clear_y;
    PaintBudget *bg = nullptr;
};

bool paint_list(const Surface &c, const Rows &R, int16_t y0, bool dim, bool blank,
                std::array<uint32_t, kVisRows> &sig_io, uint8_t &n_io, ListDrawState &st, int &painted_out, bool place = false) {
    const int slots = std::min(kVisRows, (kFooterY - y0) / kRowPitch);
    const int n = std::clamp(R.n, 0, slots);
    const int16_t w = place ? static_cast<int16_t>(c.w - kLane) : c.w;
    const int close = place ? 0 : 1;   // the closing hairline's row
    constexpr int16_t kKeyDy = kit::row_text_y(kRowPitch, 12), kValDy = kit::row_text_y(kRowPitch, 16);
    std::array<uint32_t, kVisRows> sigs{};
    for (int i = 0; i < n; i++) {
        const Rows::R &row = R.r[i];
        uint32_t h = mix_u32(2166136261u, uint32_t(row.c) | uint32_t(row.cut) << 16 | uint32_t(dim) << 24);
        h = mix_u32(h, uint32_t(uint8_t(row.act)) | uint32_t(row.focus) << 8 | uint32_t(row.off) << 9 | uint32_t(row.drill) << 10
                           | uint32_t(row.perf) << 11);
        h = pg::mix_cstr(h, row.key);
        h = mix_u32(h, 0x100u);   // key / value separator
        h = pg::mix_str(h, row.v.view());
        h = pg::mix_cstr(h, row.tail);
        sigs[static_cast<std::size_t>(i)] = h | 1u;
    }
    if (!st.tail_cleared) {
        const int16_t tail = static_cast<int16_t>(y0 + n * kRowPitch + (n < slots ? close : 0));   // under the closing hairline
        if (!blank && kFooterY > tail) {
            if (st.clear_y < tail || st.clear_y >= kFooterY) st.clear_y = tail;
            while (st.clear_y < kFooterY) {
                const int16_t h = std::min<int16_t>(PaintBudget::kStripRows, static_cast<int16_t>(kFooterY - st.clear_y));
                if (st.bg && !st.bg->admit(static_cast<int>(w) * h, true)) return false;
                gfx_fill(c, 0, st.clear_y, w, h, kBg);
                st.clear_y = static_cast<int16_t>(st.clear_y + h);
            }
        }
        st.clear_y = kFooterY;
        st.tail_cleared = true;
    }
    if (n_io > n) {
        const int16_t clear_from = static_cast<int16_t>(y0 + n * kRowPitch);
        const int16_t clear_to = static_cast<int16_t>(y0 + n_io * kRowPitch + close);
        while (clear_from < clear_to && n_io > n) {
            if (st.clear_y < clear_from || st.clear_y >= clear_to) st.clear_y = clear_from;
            while (st.clear_y < clear_to) {
                const int16_t h = std::min<int16_t>(PaintBudget::kStripRows, static_cast<int16_t>(clear_to - st.clear_y));
                if (st.bg && !st.bg->admit(static_cast<int>(w) * h, true)) return false;
                gfx_fill(c, 0, st.clear_y, w, h, kBg);
                st.clear_y = static_cast<int16_t>(st.clear_y + h);
            }
            st.clear_y = kFooterY;
            break;
        }
        for (int i = n; i < n_io && i < kVisRows; i++) sig_io[static_cast<std::size_t>(i)] = 0;
        n_io = static_cast<uint8_t>(n);
        st.close_done = false;
    } else if (n > n_io) {
        st.close_done = false;
    }
    char buf[kFitBuf];
    painted_out = 0;
    for (int i = 0; i < n; i++) {
        const int16_t y = static_cast<int16_t>(y0 + i * kRowPitch);
        if (sigs[static_cast<std::size_t>(i)] == sig_io[static_cast<std::size_t>(i)]) continue;
        if (st.bg && !st.bg->admit(static_cast<int>(c.w) * kRowPitch, true)) return false;
        painted_out++;
        sig_io[static_cast<std::size_t>(i)] = sigs[static_cast<std::size_t>(i)];
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
    if (!place && !st.close_done && n < slots) {
        if (st.bg && !st.bg->admit(c.w, true)) return false;
        gfx_fill(c, 0, static_cast<int16_t>(y0 + n * kRowPitch), c.w, 1, kRule);   // closes the table (moved with the count)
        st.close_done = true;
    }
    n_io = static_cast<uint8_t>(n);
    return true;
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
bool paint_thumb(const Surface &c, int first, int total, bool full, uint32_t &sig, PaintBudget *bg) {
    const uint32_t h = (uint32_t(first) << 8 | uint32_t(total)) + 1u;
    if (!full && h == sig) return true;
    const int16_t x = static_cast<int16_t>(c.w - kLane), band = static_cast<int16_t>(kFooterY - kPlaceListY);
    if (bg && !bg->admit(static_cast<int>(kLane) * band, true)) return false;
    sig = h;
    gfx_fill(c, x, kPlaceListY, 1, band, kBg);
    if (total <= kPlaceSlots) { gfx_fill(c, static_cast<int16_t>(x + 1), kPlaceListY, kLane - 1, band, kBg); return true; }
    const int track = kPlaceSlots * kRowPitch;
    const int16_t ty = static_cast<int16_t>(kPlaceListY + first * track / total);
    const int16_t th = static_cast<int16_t>(std::max(6, kPlaceSlots * track / total));
    const int16_t tx = static_cast<int16_t>(x + 1);
    gfx_fill(c, tx, kPlaceListY, kLane - 1, static_cast<int16_t>(ty - kPlaceListY), kBg);
    gfx_fill(c, tx, ty, kLane - 1, th, kInk3);
    gfx_fill(c, tx, static_cast<int16_t>(ty + th), kLane - 1, static_cast<int16_t>(kFooterY - ty - th), kBg);
    return true;
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
        if ((!r.request_seen || d.k2_sink) && fusb302::identify_current()) {
            const pd::Identity &pid = fusb302::identify_identity();
            const bool has_vendor = pid.valid && pid.vendor && pid.vendor[0];
            if (has_vendor) id.head.put(pid.vendor);
            else id.head.put("Device read");
            id.dim = false;

            const uint32_t irev = fusb302::identify_status().revision;
            if (irev != 0) {
                StrBuf<12> rv;
                rv.put("PD ").put_u32((irev >> 28) & 0xFu).put_char('.').put_u32((irev >> 24) & 0xFu);
                id.add(rv.view(), Ident::Solid);
            }
            bool pps = false, avs = false;
            for (uint32_t raw : d.sink) {
                const pd::Pdo p = pd::decode_pdo(raw);
                pps = pps || p.type == pd::PdoType::Pps;
                avs = avs || p.type == pd::PdoType::Avs;
            }
            if (pps) id.add("PPS", Ident::Outline);
            else if (avs) id.add("AVS", Ident::Outline);

            const pd::ExtInfo &ext = fusb302::ext();
            const pd::ExtInfo &iext = fusb302::identify_ext();
            const pd::BatteryCaps &bat = ext.battery.valid ? ext.battery : iext.battery;
            const pd::BatteryStatus &bst = ext.battery_status.valid ? ext.battery_status : iext.battery_status;
            if (bst.valid && !bst.invalid_ref && bst.present) {
                const int soc = pd::battery_soc_pct(bat, bst);
                if (soc >= 0) {
                    StrBuf<8> sc;
                    sc.put_u32(static_cast<uint32_t>(soc)).put_char('%');
                    id.add(sc.view(), bst.charging == 0 ? Ident::Ok : Ident::Outline);
                } else if (bst.charging == 0) {
                    id.add("chg", Ident::Ok);
                }
            }

            const char *fallback = d.k2_sink ? "Self-powered PD read" : "Standalone PD read";
            const std::string_view tag = d.k2_sink ? " \xC2\xB7 self-powered" : " \xC2\xB7 standalone";
            if (pid.valid) {
                put_line_id(id.line, pid.vid, pid.pid, has_vendor ? nullptr : pid.vendor);
                if (id.line.view().size() + tag.size() <= 35) id.line.put(tag);
                else { id.line.clear(); id.line.put(fallback); }
            } else {
                id.line.put(fallback);
            }
            return;
        }
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

uint32_t ConnectionPage::actions_sig() const {
    uint32_t h = mix_u32(2166136261u, uint32_t(act_sel_ + 1) | uint32_t(n_items_) << 8 | uint32_t(first_) << 20);
    for (int i = 0; i < n_items_; i++) {
        const conn::Item &it = items_[static_cast<std::size_t>(i)];
        h = mix_u32(h, uint32_t(it.enabled) | uint32_t(it.caution) << 1);
        h = pg::mix_cstr(h, it.tag);
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
    ListDrawState &st;
};
using BuildRowsFn = void (*)(Rows &, const void *);
[[gnu::noinline]] bool paint_place_list(const Surface &c, const std::array<conn::Item, conn::kMaxItems> &items, int n_items,
                                        int act_sel, bool full, bool dim, PlaceList s, BuildRowsFn build, const void *ctx) {
    Rows R;
    R.reset(Rows::kCountOnly);
    build(R, ctx);
    const int results = R.total;
    add_extras(R, items, n_items, act_sel);
    const int total = R.total;
    s.first = place_first(s.first, act_sel >= 0 ? results + act_sel : -1, total);
    s.res_n = static_cast<uint8_t>(results);
    s.tot_n = static_cast<uint8_t>(total);
    R.reset(s.first);
    build(R, ctx);
    add_extras(R, items, n_items, act_sel);
    int painted = 0;
    if (!paint_list(c, R, kPlaceListY, dim, false, s.sig, s.n_io, s.st, painted, true)) return false;
    return paint_thumb(c, s.first, total, full, s.thumb_sig, s.st.bg);
}

[[gnu::noinline]] bool paint_ident_dim(const Surface &c, const char *head, const char *line, uint32_t &sig, PaintBudget *bg) {
    Ident id;
    id.head.put(head);
    id.dim = true;
    id.line.put(line);
    return paint_identity(c, id, false, sig, bg);
}

[[gnu::noinline]] bool paint_place_ident(const Surface &c, conn::Tab tab, const TabData &d, bool dim, uint32_t &sig, PaintBudget *bg) {
    Ident id;
    place_ident(id, tab, d, dim);
    return paint_identity(c, id, false, sig, bg);
}

struct BodyBuildCtx {
    conn::Tab tab;
    const TabData *d;
};
void build_body_rows(Rows &R, const void *p) {
    const auto &ctx = *static_cast<const BodyBuildCtx *>(p);
    switch (ctx.tab) {
    case conn::Tab::Source: source_rows(R, *ctx.d); if (!ctx.d->waiting) details_rows(R); break;
    case conn::Tab::Cable: cable_rows(R, *ctx.d); break;
    case conn::Tab::Sink: sink_rows(R, *ctx.d); break;
    }
}

struct NoPdBuildCtx {
    conn::Tab tab;
    fastcharge::Protocol fc;
    int32_t current_ma;
    int32_t vbus_mv;
    uint32_t vooc;
    uint32_t rp_ma;
};
void build_no_pd_rows(Rows &R, const void *p) {
    const auto &ctx = *static_cast<const NoPdBuildCtx *>(p);
    {
        StrBuf<28> &v = R.add("Type-C", kText);
        typec_row_text(ctx.vbus_mv, ctx.rp_ma, v);   // the debounced VBUS (Row D), like the list body
        if (v.view().empty()) v.put("Unknown");
    }
    if (ctx.tab == conn::Tab::Sink) {
        put_a(R.add("Draw", kAmp), uint32_t(power::abs_sat(ctx.current_ma)));
        vooc_row(R, ctx.vooc);
    } else if (legacy_row_protocol(ctx.fc) && ctx.fc != fastcharge::Protocol::Pending) {
        StrBuf<6> fcn;
        fc_pill_text(ctx.fc, fcn);
        if (!fcn.view().empty()) R.add("Legacy", kEnergy).put(fcn.view());
    }
}

void noop_build_rows(Rows &, const void *) {}
}  // namespace

// An empty place page: the dim headline and hint in its identity block, then its action rows.
bool ConnectionPage::paint_empty(bool) {
    const conn::Empty e = conn::empty_state(tab_, facts_);
    const Surface &c = ctx_.content;
    PaintBudget *const bg = ctx_.budget;
    const bool full = body_ != Body::Empty || body_tab_ != tab_;
    if (full) {
        body_ = Body::Empty; body_tab_ = tab_;
        row_sig_.fill(0); body_n_ = 0; tail_cleared_ = ctx_.blank; close_done_ = false;
        body_clear_y_ = kFooterY; id_sig_ = thumb_sig_ = 0;
    }
    if (!paint_ident_dim(c, e.title, e.hint, id_sig_, bg)) return false;
    ListDrawState st{tail_cleared_, close_done_, body_clear_y_, bg};
    return paint_place_list(c, items_, n_items_, act_sel_, false, false,
                            {first_, res_n_, tot_n_, thumb_sig_, row_sig_, body_n_, st}, noop_build_rows, nullptr);
}

bool ConnectionPage::paint_body(const MatchResult &r, const pd::SourceCaps &caps, std::span<const uint32_t> sink,
                                bool redraw) {
    // The no-PD gate (ROADMAP 2026-11-10, Row C): checked before allocating TabData / Rows on this frame.
    if (no_pd_body()) {
        const bool done = paint_no_pd_body(r, redraw);
        if (done) g_conn_body_passes++;
        return done;
    }
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
    d.iq_ok = iq_have_ && iq_session_ == fusb302::session() && fusb302::interrogate_status().phase != pd::IqPhase::Idle;
    const bool dim = tab_ == conn::Tab::Cable && link.cable_stale();
    const bool done = paint_body_locked(&d, dim);
    if (done) g_conn_body_passes++;
    return done;
}

bool ConnectionPage::paint_body_locked(const void *tab_data, bool dim) {
    const auto &d = *static_cast<const TabData *>(tab_data);
    const Surface &c = ctx_.content;
    PaintBudget *const bg = ctx_.budget;
    const bool full = body_ != Body::Rows || body_tab_ != tab_;
    if (full) {
        body_ = Body::Rows; body_tab_ = tab_;
        row_sig_.fill(0); body_n_ = 0; tail_cleared_ = ctx_.blank; close_done_ = false;
        body_clear_y_ = kFooterY; id_sig_ = thumb_sig_ = 0;
    }
    if (!paint_place_ident(c, tab_, d, dim, id_sig_, bg)) return false;
    ListDrawState st{tail_cleared_, close_done_, body_clear_y_, bg};
    const BodyBuildCtx bctx{tab_, &d};
    return paint_place_list(c, items_, n_items_, act_sel_, false, dim,
                            {first_, res_n_, tot_n_, thumb_sig_, row_sig_, body_n_, st}, build_body_rows, &bctx);
}

// The no-PD body (ROADMAP 2026-11-10, Row C): the dim identity headline (NO PD) and its one line, then only the rows
// that are actually measured - the Type-C advertisement (Rp level + VBUS), the draw on the Sink tab, a legacy fast-charge
// answer on the Source tab - and the place's action rows. paint_body only calls this when the paint signature moved.
bool ConnectionPage::paint_no_pd_body(const MatchResult &r, bool) {
    const char *line = conn::no_pd_line(tab_, none_why_);
    // The measured rows (at most three). The Type-C row uses the match's Rp reading (the active CC line driven into the
    // Rp window counts even before typec_ma()'s 3 s mark).
    const uint32_t rp_ma = r.typec_ma ? r.typec_ma : fusb302::typec_ma();
    return paint_no_pd_locked(line, rp_ma);
}

bool ConnectionPage::paint_no_pd_locked(const char *line, uint32_t rp_ma) {
    const Surface &c = ctx_.content;
    PaintBudget *const bg = ctx_.budget;
    const bool full = body_ != Body::NoPd || body_tab_ != tab_;
    if (full) {
        body_ = Body::NoPd; body_tab_ = tab_;
        row_sig_.fill(0); body_n_ = 0; tail_cleared_ = ctx_.blank; close_done_ = false;
        body_clear_y_ = kFooterY; id_sig_ = thumb_sig_ = 0;
    }
    if (!paint_ident_dim(c, text::conn::kNoPd, line, id_sig_, bg)) return false;
    ListDrawState st{tail_cleared_, close_done_, body_clear_y_, bg};
    const NoPdBuildCtx bctx{tab_, static_cast<fastcharge::Protocol>(ctx_.fc), ctx_.pm_avg.current_ma, vbus_, vooc_, rp_ma};
    return paint_place_list(c, items_, n_items_, act_sel_, false, false,
                            {first_, res_n_, tot_n_, thumb_sig_, row_sig_, body_n_, st}, build_no_pd_rows, &bctx);
}

// The bottom bar: what the last action says, else the state of the shown place and the topology word. It goes
// through the footer cache, so a steady state writes nothing.
void ConnectionPage::paint_status(uint32_t now_ms) {
    StrBuf<40> txt;
    FooterTone tone = FooterTone::Info;
    const pd::IdStatus &ids = fusb302::identify_status();
    const int focus = act_sel_ < 0 ? 0 : 1 + act_sel_;
    const bool focus_off = focus >= 0 && focus < n_items_ && !items_[static_cast<std::size_t>(focus)].enabled;
    // An action row's greyed item names why; the tool's own action says it only when OK is pressed.
    const conn::Item *row = act_sel_ >= 0 && focus < n_items_ ? &items_[static_cast<std::size_t>(focus)] : nullptr;
    if (focus_off && act_sel_ >= 0) {
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
    } else if (tab_ == conn::Tab::Sink && fusb302::interrogate_running()) {
        const pd::IqPhase p = fusb302::interrogate_status().phase;
        txt.put(p == pd::IqPhase::Reset1 || p == pd::IqPhase::Sniff1 ? "Resetting link..."
              : p == pd::IqPhase::Reset2 || p == pd::IqPhase::Sniff2 ? "Restoring link..."
                                                                     : "Asking device...");
    } else if (ctx_.em_scanning) {
        txt.put("Reading cable...");
    } else if (before(now_ms, ctx_.em_listen_until)) {
        txt.put("Listening for cable...");
    } else {
        bool showed_id = false;
        if (id_asked_) {
            StrBuf<24> t;
            identify_status_text(ids, t);
            if (ids.state == pd::IdState::Running) {
                txt.put(t.view());
                showed_id = true;
            } else {   // Identify ended: its result stays a few seconds
                id_asked_ = false;
                set_op(t.c_str(), now_ms, 4000);
            }
        }
        if (!showed_id) {
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
    }
    status_txt_ = txt;
    draw_footer_budgeted(ctx_.content, tone, txt.view(), status_, ctx_.budget);
}

void ConnectionPage::update(uint32_t now_ms) {
    PaintBudget *const bg = ctx_.budget;
    const pd::Link &link = ctx_.ui->link();
    title_badge(false);
    if (bg && bg->more) return;
    sense(now_ms);
    if (iq_asked_ && !fusb302::interrogate_running()) {
        iq_asked_ = false;
        const pd::IqStatus &iq = fusb302::interrogate_status();
        const char *msg = iq.why == pd::IqWhy::SourceResets ? "Charger resets"
                        : iq.why == pd::IqWhy::SourceRestarts ? "Charger restarts"
                        : iq.ok_mask != 0 ? "Device read" : "No answer";
        set_op(msg, now_ms, 4000);
    }
    build_items();   // the action rows and the OK verb follow the live state
    if (act_sel_ + 1 >= n_items_) act_sel_ = n_items_ - 2;
    paint_status(now_ms);
    if (bg && bg->more) return;
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
    c = mix_u32(c, vooc_);
    c = mix_u32(c, uint32_t(link.attached()) | uint32_t(link.waiting()) << 1 | uint32_t(link.inline_mode()) << 2
              | uint32_t(pd::is_fake()) << 3 | uint32_t(fusb302::active_cc()) << 4
              | uint32_t(tab_) << 8 | uint32_t(link.cable_stale()) << 12 | fc_ << 16
              | uint32_t(facts_.topo) << 24 | uint32_t(facts_.wait) << 26 | uint32_t(fusb302::self_powered()) << 28);
    c = mix_u32(c, uint32_t(verdict_) | uint32_t(none_why_) << 4);   // the no-PD gate's verdict settles at attach+1.5 s
    c = mix_u32(c, uint32_t(facts_.cable_fact));                     // the empty Cable tab's words: chip silent / no chip
    c = mix_u32(c, fusb302::session());
    c = mix_u32(c, uint32_t(live_caps.valid) | uint32_t(live_caps.num_pdos) << 8 | uint32_t(live_caps.epr) << 16
              | uint32_t(live_caps.spec_rev) << 17 | uint32_t(live_caps.epr_partial) << 19);
    c = mix_u32(c, live_caps.raw_header);
    c = mix_u32(c, live_caps.rx_count);
    c = mix_u32(c, uint32_t(ct.valid) | uint32_t(ct.ready) << 1 | uint32_t(ct.accepted) << 2
              | uint32_t(ct.rejected) << 3 | uint32_t(ct.pos) << 8 | uint32_t(ct.flags) << 16);
    c = mix_u32(c, uint32_t(ct.mv) | uint32_t(ct.op_ma) << 16);
    c = mix_u32(c, ct.max_ma);
    c = mix_u32(c, uint32_t(sniffed.valid) | uint32_t(sniffed.n) << 8);
    c = pg::mix_u32s(c, sniffed.raw.data(), sniffed.n);
    c = mix_u32(c, uint32_t(fusb302::identify_current()) | uint32_t(idst.ok_mask) << 8 | uint32_t(idst.num_sink_pdos) << 16);
    c = pg::mix_u32s(c, idst.sink_pdos.data(), idst.num_sink_pdos);
    c = mix_u32(c, uint32_t(fusb302::pd_traffic()) | uint32_t(fusb302::pin_rd()) << 2);
    c = mix_u32(c, uint32_t(em.valid) | uint32_t(em.is_active) << 1 | uint32_t(em.vid) << 8 | uint32_t(em.num_vdos) << 24);
    c = mix_u32(c, em.raw_vdos[4]);
    c = mix_u32(c, uint32_t(em.max_power_w) | uint32_t(em.sop) << 16);
    c = mix_u32(c, fusb302::cable_seq());
    c = mix_u32(c, uint32_t(vdm.identity.valid) | uint32_t(vdm.identity.vid) << 8 | uint32_t(vdm.identity.pid) << 16);
    c = mix_u32(c, vdm.identity.id_header);
    c = mix_u32(c, uint32_t(vdm.num_svids) | uint32_t(vdm.modes_svid) << 8 | uint32_t(vdm.num_modes) << 24);
    for (int i = 0; i < vdm.num_svids; i++) c = mix_u32(c, vdm.svids[i]);
    c = pg::mix_u32s(c, vdm.modes.data(), vdm.num_modes);
    c = mix_u32(c, uint32_t(vdm.cuktech) | uint32_t(vdm.xiaomi) << 1 | uint32_t(vdm.xiaomi_cmd) << 2);
    {   // the lab-only Apple readout's rows (the serial is never displayed nor hashed)
        const pd::AppleInfo &ap = fusb302::apple_info();
        c = mix_u32(c, uint32_t(ap.valid) | uint32_t(ap.why) << 1 | uint32_t(ap.reads) << 9 | uint32_t(ap.ok) << 17);
        c = pg::mix_cstr(c, ap.product.data(), ap.product.size());
        c = pg::mix_cstr(c, ap.fw.data(), ap.fw.size());
        c = pg::mix_cstr(c, ap.hw.data(), ap.hw.size());
    }
    c = mix_u32(c, uint32_t(snk.identity.valid) | uint32_t(snk.identity.vid) << 8 | uint32_t(snk.identity.pid) << 16);
    c = mix_u32(c, snk.identity.id_header);
    c = mix_u32(c, uint32_t(snk.num_svids));
    for (int i = 0; i < snk.num_svids; i++) c = mix_u32(c, snk.svids[i]);
    c = mix_u32(c, uint32_t(ctx_.analog.has_value()));
    c = mix_u32(c, uint32_t(dp_));
    c = mix_u32(c, uint32_t(dm_));   // the debounced ADC levels (Row D): raw jitter is not hashed
    c = mix_u32(c, uint32_t(cc1_));
    c = mix_u32(c, uint32_t(cc2_));
    c = mix_u32(c, uint32_t(vbus_));   // the Type-C row's VBUS (100 mV steps)
    const uint32_t svs = survey_sig(tab_ == conn::Tab::Source);
    c = mix_u32(c, svs);
    const bool iq_ok = iq_have_ && iq_session_ == fusb302::session() && fusb302::interrogate_status().phase != pd::IqPhase::Idle;
    uint32_t dev_sig = 2166136261u;
    {
        const pd::ExtInfo &ext = fusb302::ext();
        dev_sig = mix_u32(dev_sig, uint32_t(ext.battery.valid) | uint32_t(ext.battery.invalid_ref) << 1
                                 | uint32_t(ext.battery.design_dwh) << 16);
        dev_sig = mix_u32(dev_sig, uint32_t(ext.battery.last_full_dwh)
                                 | uint32_t(ext.battery_status.valid) << 16 | uint32_t(ext.battery_status.present) << 17
                                 | uint32_t(ext.battery_status.invalid_ref) << 18 | uint32_t(ext.battery_status.charging) << 19);
        dev_sig = mix_u32(dev_sig, ext.battery_status.present_dwh);
        dev_sig = mix_u32(dev_sig, uint32_t(ext.mfr.valid) | uint32_t(ext.mfr.vid) << 8 | uint32_t(ext.mfr.pid) << 24);
        dev_sig = pg::mix_cstr(dev_sig, ext.mfr.name, sizeof ext.mfr.name);
        if (iq_ok) {
            const pd::IqStatus &iq = fusb302::interrogate_status();
            dev_sig = mix_u32(dev_sig, uint32_t(iq.phase) | uint32_t(iq.ok_mask) << 8 | uint32_t(iq.num_sink_pdos) << 24);
            dev_sig = pg::mix_u32s(dev_sig, iq.sink_pdos.data(), iq.num_sink_pdos);
            dev_sig = mix_u32(dev_sig, uint32_t(iq.num_epr_pdos) | uint32_t(iq.epr_partial) << 8
                                     | uint32_t(iq.sink_ext_len) << 16 | uint32_t(iq.num_svids) << 24);
            dev_sig = mix_u32(dev_sig, iq.revision);
            dev_sig = mix_u32(dev_sig, uint32_t(iq.id.valid) | uint32_t(iq.id.vid) << 8 | uint32_t(iq.id.pid) << 16);
            dev_sig = mix_u32(dev_sig, uint32_t(iq.battery.valid) | uint32_t(iq.battery.invalid_ref) << 1
                                     | uint32_t(iq.battery.design_dwh) << 8 | uint32_t(iq.battery.last_full_dwh) << 24);
            dev_sig = mix_u32(dev_sig, uint32_t(iq.battery_status.valid) | uint32_t(iq.battery_status.present) << 1
                                     | uint32_t(iq.battery_status.invalid_ref) << 2
                                     | uint32_t(iq.battery_status.charging) << 4 | uint32_t(iq.battery_status.present_dwh) << 16);
            dev_sig = mix_u32(dev_sig, uint32_t(iq.mfr.valid) | uint32_t(iq.mfr.vid) << 8);
            dev_sig = pg::mix_cstr(dev_sig, iq.mfr.name, sizeof iq.mfr.name);
        }
        if (fusb302::identify_current()) {
            const pd::Identity &iid = fusb302::identify_identity();
            const pd::ExtInfo &iext = fusb302::identify_ext();
            dev_sig = mix_u32(dev_sig, idst.revision);
            dev_sig = mix_u32(dev_sig, uint32_t(iid.valid) | uint32_t(iid.vid) << 8 | uint32_t(iid.pid) << 16);
            if (iid.valid && iid.vendor && iid.vendor[0]) dev_sig = pg::mix_cstr(dev_sig, iid.vendor);
            dev_sig = mix_u32(dev_sig, uint32_t(iext.battery.valid) | uint32_t(iext.battery.invalid_ref) << 1
                                     | uint32_t(iext.battery.design_dwh) << 8 | uint32_t(iext.battery.last_full_dwh) << 24);
            dev_sig = mix_u32(dev_sig, uint32_t(iext.battery_status.valid) | uint32_t(iext.battery_status.present) << 1
                                     | uint32_t(iext.battery_status.invalid_ref) << 2
                                     | uint32_t(iext.battery_status.charging) << 4 | uint32_t(iext.battery_status.present_dwh) << 16);
            dev_sig = mix_u32(dev_sig, uint32_t(iext.mfr.valid) | uint32_t(iext.mfr.vid) << 8);
            dev_sig = pg::mix_cstr(dev_sig, iext.mfr.name, sizeof iext.mfr.name);
        }
    }
    const uint32_t c_base = mix_u32(c, dev_sig);
    const uint32_t asig = actions_sig();
    c = mix_u32(c_base, asig);
    if (!body_pending_ && sig_valid_ && c == cheap_sig_) return;   // nothing changed: skip match_live + the full sig
    const bool inputs_changed = !match_valid_ || c != cheap_sig_;
    cheap_sig_ = c;

    const bool empty = conn::tab_empty(tab_, facts_) && !(tab_ == conn::Tab::Sink && (fusb302::self_powered() || fusb302::identify_current()));
    if (empty) {
        match_valid_ = false;
        uint32_t h = mix_u32(2166136261u, uint32_t(tab_) | uint32_t(facts_.topo) << 4 | 1u << 8 | uint32_t(facts_.inline_sink) << 9
                                      | uint32_t(facts_.wait) << 10 | uint32_t(facts_.open_cc_mv) << 12
                                      | uint32_t(facts_.wake_port_off) << 24
                                      | uint32_t(facts_.vbus_mv >= raguard::kVbusAttachedMv) << 25);
        h = mix_u32(h, uint32_t(facts_.cable_fact));
        h = mix_u32(h, asig);
        if (!body_pending_ && sig_valid_ && h == sig_) return;
        const bool redraw = !sig_valid_;
        body_pending_ = true;
        if (paint_empty(redraw)) {
            sig_ = h;
            sig_valid_ = true;
            body_pending_ = false;
        }
        return;
    }

    if (inputs_changed) {
        g_last_match = match_live(link);
        // The Rp advertisement of an attached source exists even when the chip's gated typec_ma() reads 0
        // (a default-USB Rp, a load inline, before its 3 s mark): the active CC line driven into the Rp
        // window says so, and its divider level gives the current.
        if (!g_last_match.caps_seen && !pd::is_fake() && link.attached() && !fusb302::pd_traffic()) {
            const uint8_t acc = fusb302::active_cc();
            const int32_t cc_mv = acc == 2 ? cc2_ : acc == 1 ? cc1_ : -1;   // the debounced line level (Row D)
            if (cc_line_has_rp(cc_mv)) {
                g_last_match.typec_rp = true;
                if (!g_last_match.typec_ma) g_last_match.typec_ma = typec_ma_from_cc_mv(cc_mv);
            }
        }
        match_valid_ = true;
    }
    const MatchResult &r = g_last_match;
    const pd::SourceCaps *caps = &g_book.caps;   // filled by match_live above
    const WantsList wl = wants_now(iq_ok);
    const std::span<const uint32_t> sink = wl.pdos;

    uint32_t h = 2166136261u;
    h = mix_u32(h, uint32_t(verdict_) | uint32_t(none_why_) << 4);   // the no-PD body switches on the verdict edge
    h = mix_u32(h, uint32_t(tab_) | uint32_t(link.cable_stale()) << 8 | uint32_t(link.waiting()) << 9 | uint32_t(caps->epr) << 10 | uint32_t(caps->spec_rev) << 11 | uint32_t(r.verdict) << 16 | uint32_t(r.flags) << 24);
    h = mix_u32(h, r.typec_ma | uint32_t(fusb302::pd_traffic()) << 16 | uint32_t(link.inline_mode()) << 17);
    h = mix_u32(h, uint32_t(r.typec_rp));
    h = mix_u32(h, uint32_t(vbus_mv));
    h = mix_u32(h, r.want_mv); h = mix_u32(h, r.want_ma); h = mix_u32(h, r.want_mw); h = mix_u32(h, uint32_t(r.want_pos));
    h = mix_u32(h, uint32_t(r.gone) | g_book.offered << 8); h = mix_u32(h, r.src_max_mw); h = mix_u32(h, r.sink_max_mv);
    for (int i = 0; i < r.rows; i++) {
        const pd::Pdo &p = caps->pdos[i];
        h = mix_u32(h, uint32_t(r.row[i]) | uint32_t(p.type) << 8);
        h = mix_u32(h, uint32_t(p.min_mv) | uint32_t(p.max_mv) << 16);
        h = mix_u32(h, p.max_ma);
    }
    h = pg::mix_u32s(h, sink.data(), sink.size());
    h = mix_u32(h, svs);
    h = mix_u32(h, asig);
    h = mix_u32(h, uint32_t(wl.src));
    h = mix_u32(h, uint32_t(ct.flags) | uint32_t(ct.accepted) << 8 | uint32_t(ct.ready) << 9 | uint32_t(ct.rejected) << 10);
    {   // the tab bodies read more than the match: cable, identity, USB link, protocol, sink mode
        const pd::Identity &id = vdm.identity;
        const usblink::Link ul = fusb302::usb_link(dp_, dm_);   // debounced levels (Row D)
        h = mix_u32(h, uint32_t(em.valid) | uint32_t(em.is_active) << 1 | uint32_t(em.vid) << 8 | uint32_t(em.raw_vdos[4]) << 24);
        h = mix_u32(h, em.raw_vdos[4] | uint32_t(em.max_power_w) << 8 | uint32_t(em.sop) << 24);
        h = mix_u32(h, fusb302::cable_seq());
        h = mix_u32(h, uint32_t(fusb302::identify_current()));
        h = mix_u32(h, uint32_t(id.valid) | uint32_t(id.vid) << 8 | uint32_t(id.pid) << 16);
        h = mix_u32(h, id.id_header);
        const pd::Identity &sid = snk.identity;
        h = mix_u32(h, uint32_t(sid.valid) | uint32_t(sid.vid) << 8 | uint32_t(sid.pid) << 16);
        h = mix_u32(h, sid.id_header);
        h = mix_u32(h, uint32_t(vdm.num_svids) | uint32_t(snk.num_svids) << 8);
        for (int i = 0; i < vdm.num_svids; i++) h = mix_u32(h, vdm.svids[i]);
        for (int i = 0; i < snk.num_svids; i++) h = mix_u32(h, snk.svids[i]);
        h = mix_u32(h, uint32_t(ul.family) | uint32_t(ul.tbt) << 4 | uint32_t(ul.dp) << 8 | uint32_t(ul.cable_limited) << 9 | ul.mbps << 12);
        h = mix_u32(h, uint32_t(ul.dp_adv) | uint32_t(ul.tbt_adv) << 1 | uint32_t(ul.dp_2lane) << 2 | uint32_t(ul.dp_4lane) << 3);
        h = mix_u32(h, fc_ | uint32_t(fusb302::self_powered()) << 8 | uint32_t(fusb302::pin_rd() == 1) << 9);
        h = mix_u32(h, vooc_);   // the Sink tab's raw VOOC note (sampled every kVoocNoteMs)
        h = mix_u32(h, dev_sig);
    }
    if (!body_pending_ && sig_valid_ && h == sig_) return;
    const bool redraw = !sig_valid_;   // invalidated (page entered, tab, pop-up closed, repaint pass): the area is stale
    body_pending_ = true;
    const bool painted_all = paint_body(r, *caps, sink, redraw);
    cheap_sig_ = mix_u32(c_base, actions_sig());   // reflect any scroll clamping done by place_first() during paint
    if (painted_all) {
        sig_ = h;
        sig_valid_ = true;
        body_pending_ = false;
    }
}

}  // namespace k2
