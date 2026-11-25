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
// from the pop-up, through Ui::run_action and its inline confirm. Opening a tab starts nothing: the tests read the offers.
#include "k2/core/log.hpp"
#include "k2/core/strpack.hpp"
#include "k2/core/held.hpp"
#include "k2/core/survey.hpp"
#include "k2/pd/cable_full.hpp"
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
#include "k2/ui/offer_rows.hpp"
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
    else g_book.merge(held::shown_caps());   // the live caps, else the held offers (core/held)
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
    port_sense_.reset();
    wake_.reset();
    wake_asleep_ = asleep_run_ = false;
    op_ = StrBuf<24>();
    op_until_ = 0;
    em_read_pending_ = false;
    id_asked_ = false;
    iq_asked_ = false;
    iq_armed_until_ = 0;
    reset_armed_until_ = 0;
    sig_valid_ = false;
    match_valid_ = false;
    first_ = 0;
    res_n_ = 0;
    tot_n_ = 0;
    id_sig_ = thumb_sig_ = 0;
    slice_sig_.fill(0);
    actions_gap_cleared_ = false;
    sense(ctx_.now);                   // facts only; the topology window starts with this one sample
    build_items();
    if (view_ == View::Actions) {
        act_sel_ = std::clamp<int>(ctx_.keep.place_sel[static_cast<int>(conn::place_of(tab_))], 0, std::max(0, act_count() - 1));
    } else {
        act_sel_ = -1;
    }
}

void ConnectionPage::set_overview(conn::Tab focus) {
    tab_ = focus;
    view_ = View::Overview;
    direct_spec_ = false;
    act_sel_ = -1;
    reset_armed_until_ = 0;
    iq_armed_until_ = 0;
    em_read_pending_ = false;
    first_ = 0;
    const bench::Place p = conn::place_of(focus);
    title_ = rail::place(static_cast<unsigned>(p));
    icon_ = place_icon(p);
    bar_icon_ = true;
    sig_valid_ = false;
    slice_sig_.fill(0);
}

// Direct Spec entry (remote / open_connection): locks to one slice's full scrollable fact sheet.
void ConnectionPage::lock(conn::Tab t) {
    tab_ = t;
    view_ = View::Spec;
    direct_spec_ = true;
    act_sel_ = -1;
    reset_armed_until_ = 0;
    iq_armed_until_ = 0;
    em_read_pending_ = false;
    first_ = 0;
    const bench::Place p = conn::place_of(t);
    title_ = rail::place(static_cast<unsigned>(p));
    icon_ = place_icon(p);
    bar_icon_ = true;
    sig_valid_ = false;
}

void ConnectionPage::set_actions_mode(bool on) {
    if (on) {
        if (view_ == View::Actions) return;
        view_ = View::Actions;
        direct_spec_ = false;
        reset_armed_until_ = 0;
        actions_gap_cleared_ = false;
        if (act_count() > 0)
            act_sel_ = std::clamp<int>(ctx_.keep.place_sel[static_cast<int>(conn::place_of(tab_))], 0, act_count() - 1);
        else act_sel_ = 0;
        invalidate();
    } else {
        if (view_ != View::Actions) return;
        view_ = View::Spec;
        act_sel_ = -1;
        reset_armed_until_ = 0;
        ctx_.rows.detach();
        invalidate();
    }
}

void ConnectionPage::enter_overview(conn::Tab t) {
    if (view_ == View::Actions) ctx_.rows.detach();
    tab_ = t;
    view_ = View::Overview;
    direct_spec_ = false;
    act_sel_ = -1;
    reset_armed_until_ = 0;
    iq_armed_until_ = 0;
    em_read_pending_ = false;
    const bench::Place p = conn::place_of(t);
    title_ = rail::place(static_cast<unsigned>(p));
    icon_ = place_icon(p);
    invalidate();
}

void ConnectionPage::enter_actions() {
    view_ = View::Actions;
    reset_armed_until_ = 0;
    iq_armed_until_ = 0;
    actions_gap_cleared_ = false;
    const bench::Place p = conn::place_of(tab_);
    title_ = rail::place(static_cast<unsigned>(p));
    icon_ = place_icon(p);
    build_items();
    act_sel_ = std::clamp<int>(ctx_.keep.place_sel[static_cast<int>(p)], 0, std::max(0, act_count() - 1));
    ctx_.rows.detach();
    ctx_.rows.init(ctx_.content, kit::kPlaceListY, &ConnectionPage::row_fn, this);
    invalidate();
}

void ConnectionPage::enter_spec() {
    if (view_ == View::Actions) ctx_.rows.detach();
    ctx_.toast.clear();
    view_ = View::Spec;
    first_ = 0;
    act_sel_ = -1;
    reset_armed_until_ = 0;
    iq_armed_until_ = 0;
    em_read_pending_ = false;
    const bench::Place p = conn::place_of(tab_);
    title_ = rail::place(static_cast<unsigned>(p));
    icon_ = place_icon(p);
    invalidate();
}

void ConnectionPage::exit_actions() {
    ctx_.toast.clear();
    enter_spec();
}

void ConnectionPage::leave() {
    if (view_ == View::Actions) ctx_.rows.detach();
    op_ = StrBuf<24>();
    op_until_ = 0;
    em_read_pending_ = false;
    id_asked_ = false;
    if (iq_asked_ && fusb302::interrogate_running()) fusb302::interrogate_abort();
    iq_asked_ = false;
    iq_armed_until_ = 0;
    reset_armed_until_ = 0;
    ctx_.em_listen_until = 0;    // nor does a listen window or its "heard nothing" verdict
    ctx_.em_quiet = false;
}

void ConnectionPage::draw_static() {
    if (view_ == View::Actions) {
        ctx_.rows.init(ctx_.content, kit::kPlaceListY, &ConnectionPage::row_fn, this);
        actions_gap_cleared_ = false;
    }
    invalidate();
}

void ConnectionPage::invalidate() {
    bar_valid_ = false;
    sig_valid_ = false;
    body_pending_ = false;
    match_valid_ = false;
    body_ = Body::None;
    body_n_ = 0;
    tail_cleared_ = false;
    close_done_ = false;
    body_clear_y_ = kFooterY;
    row_sig_.fill(0);
    slice_sig_.fill(0);
    id_sig_ = thumb_sig_ = 0;
    actions_gap_cleared_ = false;
    status_.invalidate();
    if (view_ == View::Actions) ctx_.rows.invalidate();
}

Strip ConnectionPage::labels() const {
    const bool share = (tab_ == conn::Tab::Source && held::shown_caps().valid) || (tab_ == conn::Tab::Cable && held::cable_em().valid);
    const sl::Id back = (iq_asked_ && fusb302::interrogate_running()) ? sl::Stop : sl::Back;
    if (view_ == View::Overview) {
        const sl::Id up = tab_ == conn::Tab::Source ? sl::UpDim : sl::Up;
        const sl::Id dn = tab_ == conn::Tab::Sink ? sl::DnDim : sl::Dn;
        return {{back, up, share ? sl::OpenShare : sl::Open, dn}};
    }
    if (view_ == View::Spec) {
        return {{back, sl::Up, share ? sl::ActionsShare : sl::Actions, sl::Dn}};
    }
    const int i = act_sel_;
    const bool valid = i >= 0 && i < n_items_;
    const bool reset_confirm = valid && items_[static_cast<std::size_t>(i)].act == conn::Act::ResetLink &&
                               before(ctx_.now, reset_armed_until_);
    const bool iq_confirm = tab_ == conn::Tab::Sink && valid && items_[static_cast<std::size_t>(i)].act == conn::Act::Identify &&
                            before(ctx_.now, iq_armed_until_);
    const sl::Id ok = (reset_confirm || iq_confirm) ? (share ? sl::ConfirmShare : sl::Confirm)
                    : valid ? sl::dim_if(conn::act_slot(items_[static_cast<std::size_t>(i)].act, false, share),
                                         !items_[static_cast<std::size_t>(i)].enabled)   // a greyed row: its verb greys too
                    : share ? sl::ShareLong : sl::None;
    Strip l = list_strip({{back, sl::Up, ok, sl::Dn}}, i, act_count());
    return l;
}

bool ConnectionPage::on_event(Event e) {
    const bool shrt = e.press == Press::Short;
    const bool iq_busy = iq_asked_ && fusb302::interrogate_running();
    if (shrt && e.button == Button::Back && iq_busy) {
        fusb302::interrogate_abort();
        iq_asked_ = false;
        set_op(K2_P("Cancelled"), ctx_.now);
        sig_valid_ = false;
        update(ctx_.now);
        return true;
    }
    if (shrt && e.button == Button::Back) {
        if (view_ == View::Spec) {
            if (!direct_spec_) {
                enter_actions();
                update(ctx_.now);
                return true;
            }
            return false;   // direct Spec (open_connection): let Ui leave cleanly
        }
        if (view_ == View::Actions) {
            ctx_.toast.clear();
            enter_overview(tab_);
            update(ctx_.now);
            return true;
        }
        return false;   // View::Overview: let Ui leave back to ring view
    }
    if (shrt && e.button != Button::Back) {
        if (view_ == View::Overview) {
            switch (e.button) {
            case Button::Plus:
                if (tab_ == conn::Tab::Source) tab_ = conn::Tab::Cable;
                else if (tab_ == conn::Tab::Cable) tab_ = conn::Tab::Sink;
                title_ = rail::place(static_cast<unsigned>(conn::place_of(tab_)));
                icon_ = place_icon(conn::place_of(tab_));
                sig_valid_ = false;
                update(ctx_.now);
                return true;
            case Button::Minus:
                if (tab_ == conn::Tab::Sink) tab_ = conn::Tab::Cable;
                else if (tab_ == conn::Tab::Cable) tab_ = conn::Tab::Source;
                title_ = rail::place(static_cast<unsigned>(conn::place_of(tab_)));
                icon_ = place_icon(conn::place_of(tab_));
                sig_valid_ = false;
                update(ctx_.now);
                return true;
            case Button::Ok:
                enter_actions();
                update(ctx_.now);
                return true;
            default: return false;
            }
        }
        switch (e.button) {
        case Button::Plus:
            if (view_ == View::Actions) {
                if (act_sel_ + 1 < act_count()) act_sel_++;
                ctx_.keep.place_sel[static_cast<int>(conn::place_of(tab_))] = static_cast<uint8_t>(act_sel_);
            } else if (first_ + kPlaceRows < res_n_) {
                first_++;
            }
            break;
        case Button::Minus:
            if (view_ == View::Actions) {
                if (act_sel_ > 0) act_sel_--;
                ctx_.keep.place_sel[static_cast<int>(conn::place_of(tab_))] = static_cast<uint8_t>(act_sel_);
            } else if (first_ > 0) {
                first_--;
            }
            break;
        case Button::Ok: {
            if (view_ == View::Spec) {
                enter_actions();
                update(ctx_.now);
                return true;
            }
            const int i = act_sel_;
            if (i < 0 || i >= n_items_) break;
            const conn::Item &it = items_[static_cast<std::size_t>(i)];
            if (it.act == conn::Act::Details) {
                enter_spec();
                update(ctx_.now);
                return true;
            }
            if (it.act == conn::Act::Bench) {   // a tool or a page of the place: it may replace this page, so nothing here is touched after
                const bench::Acts b = bench::actions(conn::place_of(tab_));
                if (it.arg >= 0 && it.arg < b.n) open_bench_act(*ctx_.ui, b.a[it.arg], ctx_.now);   // a blocked tool opens and says why
                return true;
            }
            if (it.enabled) activate(it, ctx_.now);
            else set_op(it.why, ctx_.now);
            if (ctx_.ui->menu() != Menu::Connection) return true;
            break;
        }
        default: return false;
        }
        if (view_ == View::Actions && (e.button == Button::Plus || e.button == Button::Minus)) {
            if (act_sel_ != 0) reset_armed_until_ = 0;
            const int at = act_sel_;
            if (at >= 0 && at < n_items_) {
                if (items_[static_cast<std::size_t>(at)].act != conn::Act::Identify) iq_armed_until_ = 0;
                if (!items_[static_cast<std::size_t>(at)].enabled)
                    set_op(items_[static_cast<std::size_t>(at)].why, ctx_.now);
            }
        }
        sig_valid_ = false;
        update(ctx_.now);
        return true;
    }
    if (e.press == Press::Long && e.button == Button::Ok) {   // Share (QR): the shown tab's record
        if (tab_ == conn::Tab::Source) {
            share_caps(ctx_.now);
            if (ctx_.ui->menu() == Menu::Connection) update(ctx_.now);
            return true;
        }
        if (tab_ == conn::Tab::Cable) {
            share_cable(ctx_.now);
            if (ctx_.ui->menu() == Menu::Connection) update(ctx_.now);
            return true;
        }
        return false;
    }
    return false;
}

void ConnectionPage::set_op(const char *text, uint32_t now_ms, uint32_t ms) {
    op_ = StrBuf<24>();
    op_.put(text);
    op_until_ = now_ms + ms;
    if (op_until_ == 0) op_until_ = 1;
    const std::string_view sv = op_.view();
    if (view_ != View::Actions && sv == "Device read") return;
    const FooterTone tone = (sv == "Cable saved" || sv == "Device read" || sv == "Rescanned (rebooted)") ? FooterTone::Ok
                          : (sv == "Flash error" || sv == "Library full" || sv == "Failed to read") ? FooterTone::Error
                          : (sv == "Power K2 via USB port" || sv == "OK again: resets link") ? FooterTone::Caution
                          : FooterTone::Info;
    ctx_.toast.show(tone, sv, now_ms, ms);
}

void ConnectionPage::share_caps(uint32_t now_ms) {
    const pd::SourceCaps &caps = held::shown_caps();
    if (!caps.valid) { set_op(K2_P("No caps yet"), now_ms); return; }
    const std::size_t len = qrshare::records::pdcaps(caps, ctx_.ui->share_buffer());
    ctx_.ui->open_share(qrshare::Type::PdCaps, qrshare::kSchemaPdCaps, "Source caps", len, now_ms);
}

// The held cable as a share record (no quirk table: that belongs to a saved clone).
void ConnectionPage::share_cable(uint32_t now_ms) {
    const pd::Emarker &em = held::cable_em();
    if (!em.valid || em.num_vdos < 5 || (em.spec_rev != 1 && em.spec_rev != 2)) { set_op(K2_P("No cable read"), now_ms); return; }
    const emrec::Record rec = emrec::make({em.raw_vdos.data(), 5}, em.spec_rev, "Live cable");
    const std::size_t len = qrshare::records::emarker(rec, "Live cable", nullptr, ctx_.ui->share_buffer());
    ctx_.ui->open_share(qrshare::Type::Emarker, qrshare::kSchemaEmarker, "Live cable", len, now_ms);
}

void ConnectionPage::build_items() {
    conn::ActFacts a;
    const pd::Link &link = ctx_.ui->link();
    a.inline_sink = link.inline_mode() || ctx_.inline_mode;
    a.reset_inline = ctx_.ui->link_reset_inline();
    a.attached = facts_.charger || facts_.inline_sink;
    // A device alone on the K2 (a dual-role phone): offer the active Identify there too (the driver presents Rd)
    a.alone = pd::identify_alone(a.inline_sink, ctx_.pm_online, ctx_.pm_online ? ctx_.vbus_fast_mv : -1,
                                 fusb302::caps_this_attach(), link.wait_kind());
    a.caps_valid = held::shown_caps().valid;
    a.cable_valid = held::cable_em().valid;
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

// Inline the Vconn read is refused by the driver (an inline K2 never transmits); the sniff method stays available and
// touches nothing.
void ConnectionPage::read_cable(uint32_t now_ms) {
    em_read_pending_ = false;
    const bool ran = ctx_.ui->run_action(Action::EmarkerScan, now_ms);
    if (ran && ctx_.em_found) {
        held::mark_cable_probed();
        if (view_ == View::Actions) exit_actions();   // the chip details are the result of the read
    } else if (ran && ctx_.em_listen_until != 0) {
        em_read_pending_ = true;
        set_op(K2_P("Listening..."), now_ms);
    } else {
        read_failed(now_ms);
    }
}

// A read that found nothing still opens the chip details when a cable record is held (they were one press away before
// Chip details merged into Read e-marker). The footer says it failed; with no record the menu stays.
void ConnectionPage::read_failed(uint32_t now_ms) {
    if (view_ == View::Actions && held::cable_em().valid) enter_spec();   // clears the toast: say it after
    set_op(K2_P("Failed to read"), now_ms);
}

void ConnectionPage::activate(const conn::Item &it, uint32_t now_ms) {
    if (!it.enabled) return;   // the status bar names the reason while the greyed item is selected
    switch (it.act) {
    case conn::Act::ResetLink: {
        const bool confirmed = before(now_ms, reset_armed_until_);
        reset_armed_until_ = 0;
        if (confirmed) wake_.spend(now_ms);   // a manual re-read takes the sleeping episode
        switch (ctx_.ui->link_reset(confirmed, now_ms)) {
        case Ui::LinkResetResult::NeedConfirm:
            reset_armed_until_ = now_ms + Ui::kConfirmWindowMs;
            set_op(K2_P("OK again: resets link"), now_ms, Ui::kConfirmWindowMs);
            ctx_.rows.invalidate();
            break;
        case Ui::LinkResetResult::SelfPowered:
            set_op(K2_P("Power K2 via USB port"), now_ms);
            break;
        case Ui::LinkResetResult::HardReset:
            set_op(K2_P("Resetting link..."), now_ms, 2500);
            enter_overview(tab_);
            break;
        case Ui::LinkResetResult::Cycled:
            set_op(K2_P("Rescanning..."), now_ms, 2500);
            enter_overview(tab_);
            break;
        case Ui::LinkResetResult::Busy:
            set_op(K2_P("Busy, retry"), now_ms, 1500);
            break;
        case Ui::LinkResetResult::NoLink:
            set_op(K2_P("No active link"), now_ms, 1500);
            break;
        }
        break;
    }
    case conn::Act::Refresh:
        wake_.spend(now_ms);
        // Inline the K2 only listens (a re-read asks nothing of the charger or the device): say so, instead of a rescan that
        // shows nothing. "Ask device" is what asks.
        if (ctx_.ui->run_action(Action::PdRescan, now_ms))
            set_op(ctx_.ui->link().inline_mode() || ctx_.inline_mode ? K2_P("Inline: listens only") : K2_P("Rescanning..."), now_ms, 2500);
        else set_op(K2_P("Busy, retry"), now_ms, 1500);   // a negotiation in flight: the refresh waits for it
        break;
    case conn::Act::Details: break;
    case conn::Act::ShareCaps:
        share_caps(now_ms);
        return;
    case conn::Act::ReadCable:
        read_cable(now_ms);
        break;
    case conn::Act::SaveCable: {
        const emlib::Result r = storage::lib_save_current();
        set_op(r.err == emlib::Err::Ok ? (r.existed ? K2_P("Already saved") : K2_P("Cable saved"))
               : r.err == emlib::Err::Exists ? K2_P("Already saved")
               : r.err == emlib::Err::Full ? K2_P("Library full")
               : r.err == emlib::Err::Invalid ? K2_P("No cable read") : K2_P("Flash error"), now_ms);
        break;
    }
    case conn::Act::SpoofAs:
        if (!ctx_.ui->push_menu(Menu::Spoof, now_ms)) set_op(K2_P("Test running"), now_ms);
        return;
    case conn::Act::SavedCables:
        if (!ctx_.ui->push_menu(Menu::Emarker, now_ms)) set_op(K2_P("Test running"), now_ms);
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
                ctx_.toast.clear();
                break;
            case pd::IqRefusal::NeedConfirm:
                iq_armed_until_ = now_ms + Ui::kConfirmWindowMs;
                set_op(K2_P("OK again: resets link"), now_ms, Ui::kConfirmWindowMs);
                break;
            case pd::IqRefusal::SelfPowered: set_op(K2_P("Powered from VBUS"), now_ms); break;
            case pd::IqRefusal::OwnContract: set_op(K2_P("K2 holds contract"), now_ms); break;
            case pd::IqRefusal::Running:
            case pd::IqRefusal::Busy:
            case pd::IqRefusal::BadSpoof: set_op(K2_P("Busy, retry"), now_ms); break;
            case pd::IqRefusal::NoCc:
            case pd::IqRefusal::NotInline: set_op(K2_P("No device inline"), now_ms); break;
            }
            break;
        }
        const bool started = ctx_.ui->run_action(Action::Identify, now_ms);
        id_asked_ = started;
        if (!started) set_op(ctx_.id_no_device ? K2_P("No device to ask") : K2_P("Identify refused"), now_ms);
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
    f.idle_rd_held = fusb302::idle_wake_line() != 0;
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

// The Charger overview wakes a dark port (conn::wake_candidate: a sleeping source, a bank, or nothing at all) once per page open and per replug, through the Refresh path:
// the woken attach is a new session, so the survey's auto read runs on it once. Never while anything blocks the auto read.
void ConnectionPage::auto_wake(uint32_t now_ms) {
    if (fusb302::line_trial().live) return;   // the direct-plug fallback is moving our Rd on this attach: its own wake, not ours
    // Scrolling the ring past the page is not opening it (a cycle there would hold the ring for the toast): OK enters.
    const bool open = view_ == View::Overview && tab_ == conn::Tab::Source && !ctx_.ui->ring_view();
    if (!wake_.step(wake_asleep_, open, ctx_.ui->auto_read_block() != pd::SvWhy::None, now_ms)) return;
    K2LOG("[UI] dark port: waking it from the Charger overview\n");
    if (ctx_.ui->wake_source()) set_op(conn::tx::kWakingCharger, now_ms, 2500);   // a refusal stays silent: the episode is spent
}

void ConnectionPage::sense(uint32_t now_ms) {
    const pd::Link &link = ctx_.ui->link();
    if (!guard_primed_ || uint32_t(now_ms - guard_t_) >= raguard::kSampleMs) {
        guard_.add(ctx_.analog ? ctx_.analog->cc1_mv : -1, ctx_.analog ? ctx_.analog->cc2_mv : -1);
        guard_t_ = now_ms;
        guard_primed_ = true;
    }
    held::poll(now_ms, link.inline_mode());
    if (survey::state().ran() && survey::state().got() > 0) held::mark_charger_probed();
    conn::Facts raw_f;
    conn::sense_passive(raw_f, ctx_, link, guard_);
    // The charger went quiet but its offers are held: the Overview and Source views keep showing them.
    if ((view_ == View::Overview || tab_ == conn::Tab::Source) && !raw_f.charger && held::showing_held()) raw_f.charger = true;
    if (held::cable_em().valid) raw_f.cable = true;   // a read cable stays after it is unplugged
    // The wake rule is the facts of this tick held 600 ms (a page opened on a dark port waits too); the sense below stays the words.
    const bool wakeable = conn::wake_candidate(raw_f);
    const conn::PortSense stable_ps = port_sense_.commit(conn::classify_port_sense(raw_f), now_ms);
    if (wakeable) {
        if (!asleep_run_) { asleep_run_ = true; asleep_since_ = now_ms; }
        wake_asleep_ = uint32_t(now_ms - asleep_since_) >= conn::DebouncedSense::kHoldMs;
    } else {
        asleep_run_ = wake_asleep_ = false;
    }
    // A replug re-arms the wake; our own Rd, re-read attach and its release do not.
    const bool own = fusb302::rd_for_run_on() || fusb302::reread_own_hold() || fusb302::line_trial().live;
    wake_.sense(stable_ps, raw_f.vbus_mv >= raguard::kVbusAttachedMv, own, now_ms);
    conn::apply_port_sense(raw_f, stable_ps);
    facts_ = raw_f;
    // The one "PD present" verdict of this attach, cached for build_items / paint_body / the tests.
    verdict_ = link.pd_verdict();
    none_why_ = link.pd_none_why();
}

// ---------------------------------------------------------------------------------------
// Rows
// ---------------------------------------------------------------------------------------

namespace {

// Layout (Option B 208x240): identity block (kit::kIdY..kPlaceListY-1), then a place list of 9 x 21 px rows
// (12 px dim key at x 4, 16 px value at x 52) with a 4 px scrollbar lane on the right.
constexpr int16_t kRowPitch = 21, kValX = 52, kKeyX = 4;
constexpr int kMaxList = 9;
constexpr int kValCells = (208 - kValX - 4) / 8;   // 19 cells of S16 in the value column
constexpr int kVisRows = kMaxList;   // row_sig_ capacity in ConnectionPage
static_assert(conn::kStatusCells <= 27, "the survey texts fit the status buffer");
// The Cable tab's Replied row: every orientation and table row fits the value column, and the 12 px key fits the
// 48 px before it (6 px per cell) with a 4 px gap.
constexpr bool answered_rows_fit() {
    for (std::uint8_t o = 1; o <= 2; o++)
        for (std::size_t i = 0; i < emk_variant::kAttempts; i++)
            if (emk_variant::short_text(emk_variant::answered_from(o, i)).view().size() > static_cast<std::size_t>(kValCells)) return false;
    return std::string_view("Replied").size() * 6 + 4 <= static_cast<std::size_t>(kValX - kKeyX);
}
static_assert(answered_rows_fit(), "the Replied row's key and value fit their columns");
// The Cable tab's full-read rows: every key fits the 12 px key column and the fixed texts the value column. Appended
// tags go through put_if_fits / put_tag; the table texts (U3 power, TBT speed) are walked by test_ui_no_truncation.
constexpr bool cable_full_rows_fit() {
    for (std::string_view k : {"Temp", "U3 pwr", "Build", "USB", "Lanes", "SVIDs", "SVIDs 2", "TBT3", "TBT ext", "DP", "DP pins",
                               "Mode", "Mode 2", "Plug 2"})
        if (k.size() * 6 + 4 > static_cast<std::size_t>(kValX - kKeyX)) return false;
    return std::string_view("max 255C off 255C").size() <= static_cast<std::size_t>(kValCells) &&
           std::string_view("Optical retimer").size() <= static_cast<std::size_t>(kValCells);
}
static_assert(cable_full_rows_fit(), "the Cable tab's full-read rows fit their columns");

// A value row. `cut` > 0 splits the value there: the head in `c`, the tail (a current, or watts) in `ct`; so a "20/28V 5A" line
// reads in unit colours. 0 = one colour.
// The rows are built in order and only the window is kept: `skip` rows are counted and dropped, the next kCap stored, the rest
// counted. A place page builds twice - once to count (skip = kCountOnly), once for the window it then shows.
struct Rows {
    struct R {
        const char *key = ""; StrBuf<28> v; uint16_t c = kText, ct = kAmp; uint8_t cut = 0;
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
    void cut_at(uint8_t n, uint16_t tail) { open->cut = n; open->ct = tail; }   // the open row's tail (a current, or watts) from cell n
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
    int32_t ibus_ma = -1;   // debounced |IBUS| magnitude (50 mA < 1 A, 100 mA >= 1 A), -1 unknown
    uint32_t live_mw = 0;   // vbus_mv * ibus_ma / 1000 when both are known
    uint32_t vooc = 0;      // the Sink tab's raw VOOC note (ConnectionPage::vooc_), 0 = no live session
    bool iq_ok = false;     // fusb302::interrogate_status() belongs to the current session
    int32_t cable_mohm = -1;   // measured cable resistance in milliohms, -1 = none
    // The shown data belongs to a session that ended: the charger detached, the cable was read before the last CC detach, the
    // sink that sent the Request left. The slices show it grey with a LAST pill.
    bool stale_charger = false, stale_cable = false, stale_sink = false;
};

int32_t current_cable_mohm(const UiContext &ctx) {
    if (ctx.cable_mode == cable_r::Mode::Kelvin && ctx.kelvin) return ctx.kelvin->total_mohm;
    if (const auto est = ctx.cable.estimate()) return static_cast<int32_t>(est->mohm);
    return -1;
}

// The Type-C row's VBUS: the un-averaged reading, 100 mV steps (the row's resolution), -1 when the
// monitor is offline or VBUS is down.
int32_t typec_vbus_mv(bool pm_online, int32_t vbus_fast_mv) {
    if (!pm_online || vbus_fast_mv < 1000) return -1;
    return (vbus_fast_mv / 100) * 100;
}

// Quantized live current magnitude for Overview inline power fallbacks (50 mA steps < 1 A, 100 mA >= 1 A).
int32_t typec_ibus_ma(bool pm_online, int32_t current_ma) {
    if (!pm_online) return -1;
    const uint32_t ma = power::abs_sat(current_ma);
    if (ma < 25u) return 0;
    if (ma < 1000u) return static_cast<int32_t>(((ma + 25u) / 50u) * 50u);
    return static_cast<int32_t>(((ma + 50u) / 100u) * 100u);
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

// Appends `tag` with a separating space when `v` is non-empty, only if it fits `kValCells`.
void put_tag(StrBuf<28> &v, std::string_view tag) {
    if (v.view().empty()) {
        put_if_fits(v, tag);
        return;
    }
    if (v.view().size() + 1 + tag.size() <= static_cast<std::size_t>(kValCells))
        v.put_char(' ').put(tag);
}

// Appends a Manufacturer_Info row (name clipped to kValCells, or VID:PID fallback).
void put_mfr_row(Rows &R, const char *key, const pd::ManufacturerInfo &mfr) {
    if (mfr.name[0]) {
        StrBuf<28> &v = R.add(key, kText);
        for (std::size_t i = 0; i < sizeof mfr.name && mfr.name[i] && v.view().size() < static_cast<std::size_t>(kValCells); i++)
            v.put_char(mfr.name[i]);
    } else if (mfr.vid || mfr.pid) {
        put_id(R.add(key, kText), mfr.vid, mfr.pid, nullptr);
    }
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

// One Line of the offer model as a row: volts in kVolt, the tail (a current, or the watts of an AVS) in its own colour.
void put_line(Rows &R, const offers::Line &l) {
    R.add(l.key, l.dim ? kFaint : kVolt).put(l.text);
    if (l.cut) R.cut_at(l.cut, l.tail_watts ? kWatt : kAmp);
}

// Only one tab builder runs per paint, but inlined all three their locals add up in paint_body()'s frame,
// which already holds the Rows (stack budget, tools/stack_depth.py).
[[gnu::noinline]] void source_rows(Rows &R, const TabData &d) {
    const MatchResult &r = *d.r;
    const pd::Identity &id = fusb302::vdm().identity;
    if (d.waiting) {
        R.add("PD", kFaint).put(K2_P("Waiting for caps..."));
        return;
    }
    const pd::SourceCaps &caps = *d.caps;
    const bool traffic = fusb302::pd_traffic();
    if (r.caps_seen) {
        // The headline and chips carry the revision and the watts; the rows are the four offer groups, then who made them.
        auto emit = [&R](const offers::Line &l) { put_line(R, l); };
        offers::rows(caps, static_cast<std::size_t>(r.rows), held::epr_raw(), offers::epr_capable(caps),
                     conn::sv_word(survey::state().entry(pd::SvField::EprList)), kValCells, emit);
        if (r.request_seen) put_request(R.add("Used", kOk), r, caps);
    }
    if (id.valid) put_id(R.add("ID"), id.vid, id.pid, id.vendor.c_str());
    else {   // the row says why it is empty: not asked yet (Refresh reads it), asking, no answer, not supported...
        const char *w = conn::sv_word(survey::state().entry(pd::SvField::Identity));
        R.add("ID", kFaint).put(w[0] ? w : K2_P("Unknown"));
    }
    if (r.caps_seen) {
        if (caps.spec_rev != 1 && caps.spec_rev != 2) R.add("PD", kFaint).put(K2_P("No PD yet"));
    } else if (!traffic && (r.typec_ma || r.typec_rp)) {
        R.add("PD", kFaint).put(d.in_use ? K2_P("None, in use") : "None");
        // A Type-C Rp advertisement exists (the source is attached): show it with the measured VBUS
        // and the advertised current, a default-USB Rp included.
        StrBuf<28> &v = R.add(K2_P("Type-C"), kText);
        typec_row_text(d.vbus_mv, r.typec_ma, v);
    } else {
        R.add("PD", kFaint).put(traffic ? K2_P("Active, no caps") : d.in_use ? K2_P("None, in use") : K2_P("No caps yet"));
    }
    if (!r.caps_seen && legacy_row_protocol(d.fc)) {   // legacy fast-charge protocols the K2 detected
        StrBuf<6> fcn;
        fc_pill_text(d.fc, fcn);
        if (!fcn.view().empty() && d.fc != fastcharge::Protocol::Pending) R.add(K2_P("Legacy"), kEnergy).put(fcn.view());
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

void cable_full_rows(Rows &R, const pd::Emarker &em);

[[gnu::noinline]] void cable_rows(Rows &R, const TabData &d) {
    const pd::Emarker &em = held::cable_em();
    put_id(R.add("ID"), em.vid, em.pid, em.vendor_str.c_str());
    StrBuf<28> &sp = R.add("Speed", kText);
    char sb[24];
    pdtabs::speed_short(sb, em.speed_str ? em.speed_str : "?");
    sp.put(sb);
    // The lane and Thunderbolt / USB4 tags follow while they fit; the speed itself is always whole.
    if (em.is_active && em.vdo2_valid && em.usb_lanes == 2) put_if_fits(sp, " x2");
    if (em.vid == 0x8087 || held::cable_thunderbolt()) { if (sp.view().substr(0, 2) != "TB") put_if_fits(sp, " TB"); }
    else if (em.usb4_supported && sp.view().substr(0, 4) != "USB4") put_if_fits(sp, " USB4");
    put_a(R.add(K2_P("Current"), kAmp), uint32_t(em.max_current_a) * 1000);
    StrBuf<28> &volt = R.add(K2_P("Voltage"), kVolt);
    volt.put_u32(em.max_voltage_v).put_char('V').put_char(' ').put_u32(em.max_power_w).put_char('W');
    R.add("EPR", em.epr_capable ? kOk : kDim).put(em.epr_capable ? "yes" : "no");
    StrBuf<28> &ty = R.add("Type", kText);
    ty.put(em.is_active ? K2_P("Active") : K2_P("Passive"));
    ty.put(em.type_c_plug ? K2_P(", Type-C") : K2_P(", captive"));
    if (em.vconn_required) R.add("VCONN", kText).put(K2_P("required"));
    if (d.cable_mohm >= 0) {
        char rb[16];
        cable_r::fmt_mohm(rb, static_cast<uint32_t>(d.cable_mohm));
        R.add("Res.", kText).put(rb);
    }
    R.add(K2_P("Length"), kText).put(em.latency_str ? em.latency_str : "?");
    // How the cable answered the read ("A, PD3, SVDM 1.0"): only when this firmware's scan found it (a clone's or
    // an older reading has none). With VCONN required this is the ninth and last row.
    const emk_variant::Text ans = emk_variant::short_text(held::cable_answered());
    if (!ans.view().empty()) R.add(K2_P("Replied"), kDim).put(ans.view());
    cable_full_rows(R, em);
}

// "2717 2E87 +": whole four-digit entries, a "+" when more follow (the Source and Cable tabs' SVIDs rows).
void put_svid_list(StrBuf<28> &v, const std::uint16_t *s, std::size_t n) {
    std::size_t shown = 0;
    for (; shown < n; shown++) {
        const bool last = shown + 1 == n;
        const std::size_t after = v.view().size() + (shown ? 1 : 0) + 4;
        if (after + (last ? 0 : 2) > static_cast<std::size_t>(kValCells)) break;
        if (shown) v.put_char(' ');
        v.put_hex(s[shown], 4);
    }
    if (shown < n) v.put(shown ? " +" : "+");
}

// One answered Discover Modes word, raw: "FF02 0000AB12".
void put_mode_row(Rows &R, const char *key, std::uint16_t svid, std::uint32_t vdo) {
    R.add(key, kText).put_hex(svid, 4).put_char(' ').put_hex(vdo, 8);
}

// The active cable's VDO 2 rows (temperatures, U3 power, build, USB, lanes); each only when the cable stated it.
void cable_active_rows(Rows &R, const pd::Emarker &em) {
    if (em.max_temp_c || em.shutdown_temp_c)
        R.add("Temp", kText).put("max ").put_u32(em.max_temp_c).put(K2_P("C off ")).put_u32(em.shutdown_temp_c).put_char('C');
    R.add(K2_P("U3 pwr"), kText).put(cable_full::u3_power_text(em.u3_power));
    const bool v13 = em.vdo_version >= 3;
    const cable_full::ActiveExtra x = cable_full::decode_active_extra(em.raw_vdos[5], v13);
    if (v13) {
        StrBuf<28> &b = R.add("Build", kText);
        b.put(x.optical ? K2_P("Optical") : K2_P("Copper")).put(x.retimer ? K2_P(" retimer") : K2_P(" redriver"));
        if (em.optical_isolated) put_if_fits(b, " iso");
    }
    StrBuf<28> &u = R.add("USB", kText);
    if (x.usb2) u.put("USB2");
    if (x.usb32) put_tag(u, "USB3");
    if (em.usb4_supported) put_tag(u, "USB4");
    StrBuf<28> &l = R.add("Lanes", kText);
    l.put_u32(em.usb_lanes).put(em.usb_lanes == 1 ? " lane" : K2_P(" lanes")).put(em.usb_gen2 ? K2_P(" Gen2+") : " Gen1");
    if (x.hub_hops) {
        const char hub[] = {' ', 'h', 'u', 'b', ' ', static_cast<char>('0' + x.hub_hops), 0};   // hops are 1..3
        put_if_fits(l, hub);
    }
    if (x.asym) put_if_fits(l, " asym");
}

// What a plug answered after its identity: SVIDs, then the decoded Thunderbolt and DisplayPort modes or raw words.
// SOP'' (`second`) keeps raw words and the "2" keys.
void cable_plug_rows(Rows &R, const cable_full::Plug &p, bool second) {
    if (p.svids_ack) {
        StrBuf<28> &v = R.add(second ? K2_P("SVIDs 2") : "SVIDs", kText);
        if (p.nsvid) put_svid_list(v, p.svid.data(), p.nsvid);
        else v.put("none");
    }
    for (unsigned i = 0; i < p.nsvid && i < cable_full::kMaxSvids; i++) {
        if (!(p.mode_ack >> i & 1u)) continue;
        const std::uint32_t m = p.mode[i];
        if (second || (p.svid[i] != cable_full::kSvidTbt && p.svid[i] != cable_full::kSvidDp)) {
            put_mode_row(R, second ? K2_P("Mode 2") : "Mode", p.svid[i], m);
        } else if (p.svid[i] == cable_full::kSvidTbt) {
            const cable_full::Tbt t = cable_full::decode_tbt(m);
            StrBuf<28> &v = R.add("TBT3", kText);
            v.put(cable_full::tbt_speed_text(t.speed)).put(t.active ? K2_P(" active") : K2_P(" passive"));
            if (t.rounded) put_if_fits(v, " rnd");
            StrBuf<28> e;
            if (t.optical) put_tag(e, "optical");
            if (t.retimer) put_tag(e, "retimer");
            if (t.uni_training) put_tag(e, "uni");
            if (!e.view().empty()) R.add(K2_P("TBT ext"), kText).put(e.view());
        } else {
            const cable_full::Dp d = cable_full::decode_dp(m);
            StrBuf<28> &v = R.add("DP", kText);
            v.put(cable_full::dp_rate_text(d));
            put_tag(v, cable_full::dp_cable_text(d.cable));
            if (d.pins) {
                StrBuf<28> &pn = R.add(K2_P("DP pins"), kText);
                for (unsigned k = 0; k < 6; k++)
                    if (d.pins >> k & 1u) { if (!pn.view().empty()) pn.put_char(' '); pn.put_char(char('A' + k)); }
            }
        }
    }
}

void cable_full_rows(Rows &R, const pd::Emarker &em) {
    if (em.is_active && em.vdo2_valid) cable_active_rows(R, em);
    const cable_full::Full &f = held::cable_extra();
    if (!f.valid) return;
    cable_plug_rows(R, f.p1, false);
    if (f.id2_ack) {
        put_id(R.add(K2_P("Plug 2"), kText), static_cast<std::uint16_t>(f.id2[0]), static_cast<std::uint16_t>(f.id2[2] >> 16), nullptr);
        cable_plug_rows(R, f.p2, true);
    }
}

void sink_pdo_rows(Rows &R, std::span<const uint32_t> sink) {
    auto emit = [&R](const offers::Line &l) { put_line(R, l); };
    offers::FixedRun run("Fixed");
    for (uint32_t raw : sink) {
        const pd::Pdo p = pd::decode_pdo(raw);
        if (p.type == pd::PdoType::Fixed) run.add(p, kValCells, emit);
    }
    run.flush(emit);

    for (uint32_t raw : sink) {
        const pd::Pdo p = pd::decode_pdo(raw);
        if (p.type == pd::PdoType::Pps || p.type == pd::PdoType::Avs) offers::range_line(p.type == pd::PdoType::Pps ? "PPS" : "AVS", p, false, emit);
    }

    for (uint32_t raw : sink) {
        const pd::Pdo p = pd::decode_pdo(raw);
        if (p.type != pd::PdoType::Battery) continue;
        StrBuf<28> &v = R.add(K2_P("BatPDO"), kVolt);
        if (p.min_mv != p.max_mv) { put_v(v, p.min_mv); v.put_char('-'); }
        put_v(v, p.max_mv);
        v.put("V ");
        v.put_u32(p.max_mw / 1000).put_char('W');
    }
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
    if (mfr.valid && (iq_ok || id_cur || mfr.name[0])) put_mfr_row(R, "Mfr", mfr);

    // PD Revision (RMDO): from inline interrogation or Identify.
    const uint32_t rev = (iq_ok && iq.revision) ? iq.revision : (id_cur ? fusb302::identify_status().revision : 0u);
    if (rev != 0) {
        StrBuf<28> &v = R.add("Rev", kText);
        v.put("PD ").put_u32((rev >> 28) & 0xFu).put_char('.').put_u32((rev >> 24) & 0xFu)
         .put(" v").put_u32((rev >> 20) & 0xFu).put_char('.').put_u32((rev >> 16) & 0xFu);
    }

    // Sink_Capabilities_Extended (SKEDB): SPR/EPR operational/max PDP and sink modes.
    if (iq_ok && iq.sink_ext_len >= 21) {
        StrBuf<28> &v = R.add("PDP", kText);
        const uint8_t spr_min = iq.sink_ext[18];
        const uint8_t spr_op = iq.sink_ext[19];
        const uint8_t spr_max = iq.sink_ext[20];
        const uint8_t epr_max = iq.sink_ext_len >= 24 ? iq.sink_ext[23] : 0;
        const uint8_t modes = iq.sink_ext[17];
        if (spr_max) {
            if (spr_op && spr_op != spr_max && !epr_max)
                v.put("SPR ").put_u32(spr_op).put_char('/').put_u32(spr_max).put_char('W');
            else
                v.put("SPR ").put_u32(spr_max).put_char('W');
        }
        if (epr_max) {
            StrBuf<12> e;
            e.put("EPR ").put_u32(epr_max).put_char('W');
            put_tag(v, e.view());
        }
        if (modes & 0x01) put_tag(v, "PPS");
        if (modes & 0x20) put_tag(v, "AVS");
        if (spr_max && spr_min && spr_min != spr_max && !spr_op && !epr_max) {
            StrBuf<12> m;
            m.put("min ").put_u32(spr_min);
            put_tag(v, m.view());
        }
        if (v.view().empty()) v.put(K2_P("Present"));
    }

    // SKEDB Sink details: FW/HW versions, power source flags, battery count, and load step.
    if (iq_ok && iq.sink_ext_len >= 18) {
        StrBuf<28> sv;
        const uint8_t fw = iq.sink_ext[8];
        const uint8_t hw = iq.sink_ext[9];
        const uint8_t load_step = iq.sink_ext[11] & 3u;
        const uint8_t batt_info = iq.sink_ext[16];
        const uint8_t modes = iq.sink_ext[17];
        if (fw || hw) sv.put("FW").put_u32(fw).put(" HW").put_u32(hw);
        if (modes & 0x02) put_tag(sv, "VBUS");
        if (modes & 0x04) put_tag(sv, "AC");
        if (modes & 0x08) put_tag(sv, "Batt");
        unsigned nb = 0;
        for (uint8_t x = batt_info; x; x = uint8_t(x & (x - 1u))) nb++;
        if (nb > 0) {
            StrBuf<8> b;
            b.put_u32(nb).put("bat");
            put_tag(sv, b.view());
        }
        if (load_step & 1u) put_tag(sv, "500mA/us");
        if (!sv.view().empty()) R.add("Sink", kText).put(sv.view());
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
            R.add("Batt", kFaint).put(K2_P("Not present"));
        } else {
            const int soc = pd::battery_soc_pct(bat, bst);
            const char *st = bst.charging == 0 ? "chg" : bst.charging == 1 ? K2_P("dischg") : bst.charging == 2 ? "idle" : "";
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
                v.put(st[0] ? st : K2_P("Present"));
            }
        }
    }
    if (bat.design_known() || bat.last_full_known()) {
        const int hlth = (bat.design_known() && bat.last_full_known()) ? pd::battery_health_pct(bat) : -1;
        const uint16_t hc = hlth >= 80 ? kOk : hlth >= 0 ? kCaution : kText;
        StrBuf<28> &v = R.add(K2_P("Health"), hc);
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
            v.put(K2_P("Wh design"));
        } else {
            put_dwh(v, bat.last_full_dwh);
            v.put(K2_P("Wh full"));
        }
    }

    // Battery Manufacturer_Info (MIDB target 1).
    const pd::ManufacturerInfo &batt_mfr = (iq_ok && iq.batt_mfr.valid) ? iq.batt_mfr
                                         : ext.batt_mfr.valid ? ext.batt_mfr
                                         : id_cur ? id_ext.batt_mfr : ext.batt_mfr;
    if (batt_mfr.valid && (batt_mfr.name[0] || batt_mfr.vid || batt_mfr.pid)) put_mfr_row(R, K2_P("Batt mfr"), batt_mfr);

    // Status (SDB): internal temperature, thermal state, faults, and present power input.
    const pd::Status &st = (iq_ok && iq.status.valid) ? iq.status
                         : ext.status.valid ? ext.status
                         : id_cur ? id_ext.status : ext.status;
    if (st.valid && (st.internal_temp_c || st.temp_status || st.present_input || st.event_flags)) {
        const bool otp_ts = st.temp_status == 3;
        const uint16_t col = (otp_ts || (st.event_flags & 0x0E) != 0) ? kError : st.temp_status == 2 ? kCaution : kText;
        StrBuf<28> sv;
        if (st.internal_temp_c == 1) sv.put("<2C");
        else if (st.internal_temp_c >= 2) sv.put_u32(st.internal_temp_c).put_char('C');
        if (st.temp_status == 2) put_tag(sv, "Warn");
        else if (otp_ts) put_tag(sv, "OTP");
        else if (st.internal_temp_c == 0 && st.temp_status == 1) put_tag(sv, "Norm");
        if (st.event_flags & 0x02) put_tag(sv, "OCP");
        if ((st.event_flags & 0x04) && !otp_ts) put_tag(sv, "OTP");
        if (st.event_flags & 0x08) put_tag(sv, "OVP");
        if (st.present_input & 0x02) put_tag(sv, (st.present_input & 0x04) ? "AC" : "Ext");
        if (st.present_input & 0x08) put_tag(sv, "Batt");
        if (st.present_input & 0x10) put_tag(sv, "Int");
        if (!sv.view().empty()) R.add(K2_P("Status"), col).put(sv.view());
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
        R.add("Sink", kOk).put(d.k2_sink_ok ? K2_P("K2 (sink mode OK)") : K2_P("K2 (sink mode PLUS)"));
        link_rows(R, d);
        return;
    }
    // The ID row: sniffed when neither Identify nor inline interrogation collected it.
    if (id.valid) {
        StrBuf<28> &v = R.add("ID");
        conn::id_value(v, kValCells, id_cur || (iq_ok && iq.id.valid), id.vid, id.pid, id.vendor.c_str());
    } else R.add("ID").put(K2_P("Unknown"));   // until the sink answers a Discover Identity (the charger's own, sniffed)
    if (id_cur && (d.k2_sink || !r.request_seen)) {
        if (!d.sink.empty()) sink_pdo_rows(R, d.sink);
        sink_device_rows(R, iq_ok);
        return;
    }
    if (r.request_seen) {
        put_request(R.add(K2_P("Request"), kAmp), r, *d.caps);
        StrBuf<28> &st = R.add("State", kDim);
        st.put(request_state_name(fusb302::contract()));
        StrBuf<32> fl;
        const std::size_t room = st.view().size() + 2 < static_cast<std::size_t>(kValCells) ? kValCells - st.view().size() - 2 : 0;
        rdo_flags_text(fusb302::contract().flags, room, fl);
        if (!fl.view().empty()) st.put(", ").put(fl.view());
    } else {
        R.add(K2_P("Request"), kFaint).put(K2_P("None seen"));
    }
    strip_rows(R, r, d.ul);
    link_rows(R, d);
    proto_row(R, fusb302::sink_vdm());
    vooc_row(R, d.vooc);   // a live session next to a PD contract: the phone holds both (before Caps, which drops first)
    if (!d.sink.empty()) sink_pdo_rows(R, d.sink);
    else R.add("Caps", kFaint).put(K2_P("Not seen"));
    sink_device_rows(R, iq_ok);
}

// One survey field as a row of the Details page: what was read, else the word for why it was not (conn::sv_word).
void word_row(Rows &R, const char *key, pd::SvField f) {
    const char *w = conn::sv_word(survey::state().entry(f));
    R.add(key, kFaint).put(w[0] ? w : K2_P("Unknown"));
}

// The Source tab's second page: what the Refresh survey reads beyond the main rows. An answer that is in is shown whatever
// the survey says (a sniffed one counts); a field without one says its status. Values are built to fit the 19 cells: the
// external name is copied up to them, lists keep whole entries.
[[gnu::noinline]] void details_rows(Rows &R) {
    const pd::SourceCaps &caps = held::shown_caps();
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

    // The headline shows the rating; a row only when the source has a separate EPR one.
    const pd::SourceCapsExt &ce = fusb302::ext().caps_ext;
    if (ce.valid) {
        if (ce.epr_pdp_w) R.add("Ext", kText).put("PDP ").put_u32(ce.pdp_w).put(K2_P("W EPR ")).put_u32(ce.epr_pdp_w).put_char('W');
    } else word_row(R, "Ext", pd::SvField::CapsExt);

    const pd::VdmInfo &vdm = fusb302::vdm();
    if (vdm.num_svids > 0) put_svid_list(R.add("SVIDs", kText), vdm.svids.data(), vdm.num_svids);
    else word_row(R, "SVIDs", pd::SvField::Svids);

    if (vdm.num_modes > 0) {
        StrBuf<28> &v = R.add("Modes", kText);
        v.put_hex(vdm.modes_svid, 4).put(": ").put_u32(vdm.num_modes).put(vdm.num_modes == 1 ? " mode" : K2_P(" modes"));
    } else word_row(R, "Modes", pd::SvField::Modes);
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
    // the held offers: shown when the live ones are gone
    const pd::SourceCaps &kept = held::shown_caps();
    h = mix_u32(h, uint32_t(held::showing_held()) | uint32_t(kept.valid) << 1 | uint32_t(kept.num_pdos) << 8);
    h = pg::mix_u32s(h, held::epr_raw().data(), held::epr_raw().size());
    return h;
}

}  // namespace

// ---------------------------------------------------------------------------------------
// Painting
// ---------------------------------------------------------------------------------------

namespace {

// A place page's Info list: the identity block's band (y22..67, kit::kIdY) and the fact list under it, with a 4 px lane on
// the right for the scroll thumb. Actions live on the Actions sub-page (kit::RowList).
using kit::kPlaceListY;
constexpr int16_t kLane = 4;
constexpr int kPlaceSlots = (kFooterY - kPlaceListY) / kRowPitch;   // 9
static_assert(kPlaceSlots == ConnectionPage::kPlaceRows && kPlaceSlots <= kVisRows, "a place page shows nine list rows under its identity block");
static_assert(kValX + kValCells * 8 <= 208 - kLane, "the value column ends before the scroll lane");

// The window of R (already windowed by the builder), painted from kPlaceListY down to the footer: one 21 px row per
// slot, a slot repainted alone when its hash moved (Row D), the vacated slots cleared, and a 4 px lane at the right edge
// for its scroll thumb.
struct ListDrawState {
    bool &tail_cleared;
    int16_t &clear_y;
    PaintBudget *bg = nullptr;
};

bool paint_list(const Surface &c, const Rows &R, bool dim,
                std::array<uint32_t, kVisRows> &sig_io, uint8_t &n_io, ListDrawState &st) {
    constexpr int16_t y0 = kPlaceListY;
    constexpr int slots = kPlaceSlots;
    const int n = std::clamp(R.n, 0, slots);
    const int16_t w = static_cast<int16_t>(c.w - kLane);
    constexpr int16_t kKeyDy = kit::row_text_y(kRowPitch, 12), kValDy = kit::row_text_y(kRowPitch, 16);
    std::array<uint32_t, kVisRows> sigs{};
    for (int i = 0; i < n; i++) {
        const Rows::R &row = R.r[i];
        uint32_t h = mix_u32(2166136261u, uint32_t(row.c) | uint32_t(row.cut) << 16 | uint32_t(dim) << 24 | uint32_t(row.ct == kWatt) << 25);
        h = pg::mix_cstr(h, row.key);
        h = mix_u32(h, 0x100u);   // key / value separator
        h = pg::mix_str(h, row.v.view());
        sigs[static_cast<std::size_t>(i)] = h | 1u;
    }
    if (!st.tail_cleared) {
        const int16_t tail = static_cast<int16_t>(y0 + n * kRowPitch);
        if (kFooterY > tail) {
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
        const int16_t clear_to = static_cast<int16_t>(y0 + n_io * kRowPitch);
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
    }
    char buf[kFitBuf];
    for (int i = 0; i < n; i++) {
        const int16_t y = static_cast<int16_t>(y0 + i * kRowPitch);
        if (sigs[static_cast<std::size_t>(i)] == sig_io[static_cast<std::size_t>(i)]) continue;
        if (st.bg && !st.bg->admit(static_cast<int>(c.w) * kRowPitch, true)) return false;
        sig_io[static_cast<std::size_t>(i)] = sigs[static_cast<std::size_t>(i)];
        const Rows::R &row = R.r[i];
        gfx_fill(c, 0, y, w, 1, kBg);
        const kit::Box body{0, static_cast<int16_t>(y + 1), w, static_cast<int16_t>(kRowPitch - 1)};
        kit::TextRun runs[3];
        int nr = 0;
        runs[nr++] = {kKeyX, static_cast<int16_t>(y + kKeyDy), &kLabelFont, row.key, kDim};
        const std::string_view all = row.v.view();
        const int16_t vy = static_cast<int16_t>(y + kValDy);
        if (row.cut > 0 && all.size() <= static_cast<std::size_t>(kValCells)) {   // built to fit: head in its colour, current in kAmp
            runs[nr++] = {kValX, vy, &kSmall, all.substr(0, row.cut), dim ? kStale : row.c};
            runs[nr++] = {kValX, vy, &kSmall, all.substr(row.cut), dim ? kStale : row.ct, static_cast<int16_t>(row.cut * kSmall.adv)};
        } else {
            // Every row is built to fit the value column (vendor names, tags and flags are added whole or not at all),
            // so a cut here is a copy bug: fit_text fails the host gate on it.
            runs[nr++] = {kValX, vy, &kSmall, fit_text(kSmall, all, c.w - kValX - 4, buf), dim ? kStale : row.c};
        }
        kit::paint_texts(c, body, runs, nr, kBg);
    }
    n_io = static_cast<uint8_t>(n);
    return true;
}

// Clamped so the Info fact list never ends above the last slot.
int place_first(int first, int total) {
    return std::clamp(first, 0, std::max(0, total - kPlaceSlots));
}

// The scroll thumb in the lane (x220..223 of the list band): a 3 px (ListView::kBarW) kInk3 bar for the window's share
// of the list, only when the list overflows; the lane's ground otherwise. Painted when (first, total) moved.
bool paint_thumb(const Surface &c, int first, int total, uint32_t &sig, PaintBudget *bg) {
    const uint32_t h = (uint32_t(first) << 8 | uint32_t(total)) + 1u;
    if (h == sig) return true;
    const int16_t x = static_cast<int16_t>(c.w - kLane), band = static_cast<int16_t>(kFooterY - kPlaceListY);
    if (bg && !bg->admit(static_cast<int>(kLane) * band, true)) return false;
    sig = h;
    const ListState st{total, kPlaceSlots, first};
    if (!st.overflows()) { gfx_fill(c, x, kPlaceListY, kLane, band, kBg); return true; }
    int pos = 0, len = 0;
    st.thumb(band, pos, len);
    const kit::Box thumb{static_cast<int16_t>(x + 1), static_cast<int16_t>(kPlaceListY + pos),
                         ListView::kBarW, static_cast<int16_t>(len)};
    kit::fill_except(c, {x, kPlaceListY, kLane, band}, &thumb, 1, kBg);
    gfx_fill(c, thumb.x, thumb.y, thumb.w, thumb.h, kInk3);
    return true;
}

// A place page's identity block: the kit's (kit::Ident, kit::paint_identity), shared with the Battery place.
using kit::Ident;
using kit::paint_identity;

// "2B01:0001 Anker": the ids, the vendor only when it fits the line whole.
void put_line_id(StrBuf<36> &l, uint16_t vid, uint16_t pid, const char *vendor) {
    l.put_hex(vid, 4).put_char(':').put_hex(pid, 4);
    if (vendor && vendor[0] && l.view().size() + 1 + std::string_view(vendor).size() <= kit::kIdLineCells) l.put_char(' ').put(vendor);
}

// The identity of a place with something on it (the empty and no-PD states are the page's own): Charger = the source's
// power, its PD revision and offer kinds, the contract in use and its ID; Cable = the rating, its power and EPR, the chip's
// ID; Device = what it asks, the request's state and offer, and what that is of the offer.
template <size_t N>
void put_va(StrBuf<N> &s, uint32_t mv, uint32_t ma) {
    put_v(s, mv);
    s.put("V ");
    put_a(s, ma);
}

void add_pps_avs(Ident &id, bool pps, bool avs) {
    if (pps) id.add("PPS", Ident::Outline);
    else if (avs) id.add("AVS", Ident::Outline);
}

void add_soc_chip(Ident &id, const pd::BatteryCaps &bat, const pd::BatteryStatus &bst, bool chg_fallback) {
    if (!bst.valid || bst.invalid_ref || !bst.present) return;
    const int soc = pd::battery_soc_pct(bat, bst);
    if (soc >= 0) {
        StrBuf<8> sc;
        sc.put_u32(static_cast<uint32_t>(soc)).put_char('%');
        id.add(sc.view(), bst.charging == 0 ? Ident::Ok : Ident::Outline);
    } else if (chg_fallback && bst.charging == 0) {
        id.add("chg", Ident::Ok);
    }
}

[[gnu::noinline]] void place_ident(Ident &id, conn::Tab tab, const TabData &d) {
    const MatchResult &r = *d.r;
    switch (tab) {
    case conn::Tab::Source: {
        if (d.waiting) { id.head.put(K2_P("Waiting")); id.dim = true; id.line.put(K2_P("for the charger's offers")); return; }
        if (!r.caps_seen) {
            const bool typec = r.typec_ma || r.typec_rp;
            id.head.put(typec ? K2_P("Type-C") : text::conn::kOffersNone);
            id.dim = true;
            id.line.put(typec ? K2_P("no PD offers on the line") : text::conn::kOffersNoneL);
            return;
        }
        const pd::SourceCaps &caps = *d.caps;
        const pd::SourceCapsExt &ce = fusb302::ext().caps_ext;
        id.head.put_u32(offers::hero_w(ce.valid, ce.pdp_w, ce.epr_pdp_w, r.src_max_mw)).put_char('W');
        if (caps.spec_rev == 1 || caps.spec_rev == 2) id.add(pd::wire_revision_name(caps.spec_rev), Ident::Solid);
        const offers::Summary sm = offers::summarize(caps, static_cast<std::size_t>(r.rows), held::epr_raw());
        if (sm.epr) id.add("EPR", Ident::Outline);
        add_pps_avs(id, sm.pps, sm.avs);
        if (r.request_seen) {
            StrBuf<12> t;
            put_va(t, r.want_mv, r.want_ma);
            id.add(t.view(), Ident::Volt);
        }
        const std::size_t n_offers = offers::count(caps, static_cast<std::size_t>(r.rows), held::epr_raw());
        id.line.put_u32(uint32_t(n_offers)).put(n_offers == 1 ? K2_P(" offer, ID ") : K2_P(" offers, ID "));
        const pd::Identity &vid = fusb302::vdm().identity;
        if (vid.valid) put_line_id(id.line, vid.vid, vid.pid, vid.vendor.c_str());
        else {
            const char *w = conn::sv_word(survey::state().entry(pd::SvField::Identity));
            id.line.put(w[0] ? w : K2_P("Unknown"));
        }
        return;
    }
    case conn::Tab::Cable: {
        const pd::Emarker &em = held::cable_em();
        if (!em.valid) { id.head.put(text::conn::kCableNone); id.dim = true; id.line.put(text::conn::kCableNoneL); return; }
        id.head.put_u32(em.max_current_a).put("A ").put_u32(em.max_voltage_v).put_char('V');
        id.dim = false;
        StrBuf<12> w;
        w.put_u32(em.max_power_w).put_char('W');
        id.add(w.view(), Ident::Solid);
        if (em.epr_capable) id.add("EPR", Ident::Outline);
        if (em.speed_str && em.speed_str[0]) {
            char sb[24];
            pdtabs::speed_short(sb, em.speed_str);
            if (sb[0] && sb[0] != '?') id.add(sb, Ident::Outline);
        }
        put_line_id(id.line, em.vid, em.pid, em.vendor_str.c_str());
        const std::string_view kind = em.is_active ? ", active" : ", passive";
        if (id.line.view().size() + kind.size() <= kit::kIdLineCells) id.line.put(kind);
        return;
    }
    case conn::Tab::Sink: {
        if ((!r.request_seen || d.k2_sink) && fusb302::identify_current()) {
            const pd::Identity &pid = fusb302::identify_identity();
            // The name is the headline when it fits the head (15 cells, and the band's width); else the ID line carries it.
            const bool has_vendor = pid.valid && !pid.vendor.empty() && pid.vendor.view().size() < 16 &&
                                    text_width(kMid, pid.vendor.c_str()) <= 224;
            if (has_vendor) id.head.put(pid.vendor.c_str());
            else id.head.put(K2_P("Device read"));
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
            add_pps_avs(id, pps, avs);

            const pd::ExtInfo &ext = fusb302::ext();
            const pd::ExtInfo &iext = fusb302::identify_ext();
            const pd::BatteryCaps &bat = ext.battery.valid ? ext.battery : iext.battery;
            const pd::BatteryStatus &bst = ext.battery_status.valid ? ext.battery_status : iext.battery_status;
            add_soc_chip(id, bat, bst, true);

            const char *fallback = d.k2_sink ? K2_P("Self-powered PD read") : K2_P("Standalone PD read");
            const std::string_view tag = d.k2_sink ? " \xC2\xB7 self-powered" : " \xC2\xB7 standalone";
            if (pid.valid) {
                put_line_id(id.line, pid.vid, pid.pid, has_vendor ? nullptr : pid.vendor.c_str());
                // A name that did not fit the head sits on the line; it wins over the tag (whole or not at all).
                if (id.line.view().size() + tag.size() <= kit::kIdLineCells) id.line.put(tag);
                else if (has_vendor || pid.vendor.empty()) { id.line.clear(); id.line.put(fallback); }
            } else {
                id.line.put(fallback);
            }
            return;
        }
        if (d.k2_sink) { id.head.put("K2"); id.line.put(K2_P("The K2 draws the power itself")); return; }
        if (!r.request_seen) { id.head.put(K2_P("No request")); id.dim = true; id.line.put(K2_P("Press OK to interrogate")); return; }
        put_va(id.head, r.want_mv, r.want_ma);
        const pd::Contract &ct = fusb302::contract();
        const char *st = request_state_name(ct);
        if (st[0]) id.add(st, ct.ready ? Ident::Ok : Ident::Outline);
        const pd::Pdo *p = r.want_pos > 0 && r.want_pos <= d.caps->num_pdos ? &d.caps->pdos[r.want_pos - 1] : nullptr;
        StrBuf<12> o;
        o.put(p && p->type == pd::PdoType::Pps ? "PPS" : p && p->type == pd::PdoType::Avs ? "AVS" : "PDO").put_u32(uint32_t(r.want_pos));
        id.add(o.view(), Ident::Outline);
        if (d.iq_ok) {
            const pd::IqStatus &iq = fusb302::interrogate_status();
            const pd::ExtInfo &ext = fusb302::ext();
            const pd::BatteryCaps &bat = iq.battery.valid ? iq.battery : ext.battery;
            const pd::BatteryStatus &bst = iq.battery_status.valid ? iq.battery_status : ext.battery_status;
            add_soc_chip(id, bat, bst, false);
        }
        id.line.put("Asks ").put_u32(r.want_mw / 1000).put("W of ").put_u32(r.src_max_mw / 1000).put(K2_P("W offered"));
        return;
    }
    }
}

}  // namespace

namespace {
// What the Info page paints under its identity block: the fact rows `build` adds (no action rows). Built once to
// count, then for the window.
struct PlaceList {
    int &first;
    uint8_t &res_n, &tot_n;
    uint32_t &thumb_sig;
    std::array<uint32_t, kVisRows> &sig;
    uint8_t &n_io;
    ListDrawState &st;
};
using BuildRowsFn = void (*)(Rows &, const void *);
[[gnu::noinline]] bool paint_place_list(const Surface &c, bool dim, PlaceList s, BuildRowsFn build, const void *ctx) {
    Rows R;
    R.reset(Rows::kCountOnly);
    build(R, ctx);
    const int total = R.total;
    s.first = place_first(s.first, total);
    s.res_n = static_cast<uint8_t>(total);
    s.tot_n = static_cast<uint8_t>(total);
    R.reset(s.first);
    build(R, ctx);
    if (!paint_list(c, R, dim, s.sig, s.n_io, s.st)) return false;
    return paint_thumb(c, s.first, total, s.thumb_sig, s.st.bg);
}

[[gnu::noinline]] bool paint_ident_dim(const Surface &c, const char *head, const char *line, uint32_t &sig, PaintBudget *bg) {
    Ident id;
    id.head.put(head);
    id.dim = true;
    id.line.put(line);
    return paint_identity(c, id, false, sig, bg);
}

[[gnu::noinline]] bool paint_place_ident(const Surface &c, conn::Tab tab, const TabData &d, uint32_t &sig, PaintBudget *bg) {
    Ident id;
    place_ident(id, tab, d);
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
        StrBuf<28> &v = R.add(K2_P("Type-C"), kText);
        typec_row_text(ctx.vbus_mv, ctx.rp_ma, v);   // the debounced VBUS (Row D), like the list body
        if (v.view().empty()) v.put(K2_P("Unknown"));
    }
    if (ctx.tab == conn::Tab::Sink) {
        put_a(R.add("Draw", kAmp), uint32_t(power::abs_sat(ctx.current_ma)));
        vooc_row(R, ctx.vooc);
    } else if (legacy_row_protocol(ctx.fc) && ctx.fc != fastcharge::Protocol::Pending) {
        StrBuf<6> fcn;
        fc_pill_text(ctx.fc, fcn);
        if (!fcn.view().empty()) R.add(K2_P("Legacy"), kEnergy).put(fcn.view());
    }
}

void noop_build_rows(Rows &, const void *) {}

// ---- 3-slice Overview (Charger / Cable / Sink on 208 x 238, y 2..239) ------------------------------------------------
constexpr int16_t kSliceY[3] = {2, 81, 160};
constexpr int16_t kSliceH[3] = {79, 79, 80};
static_assert(kSliceY[2] + kSliceH[2] == kFooterY, "3-slice Overview tiles y 2..239");

struct SliceSpec {
    enum ChipKind : uint8_t { Solid, Ok, Warn, Neutral, Last };
    struct Chip { StrBuf<10> t; ChipKind k = Solid; };
    const char *tag = "";
    StrBuf<16> head;
    Chip chip[3];
    uint8_t n_chips = 0;
    StrBuf<40> l1;
    StrBuf<40> l2;
    bool focused = false;
    bool warn = false;
    bool dim = false;
    bool l2_warn = false;
    void add_chip(std::string_view t, ChipKind k) {
        if (n_chips >= 3 || t.empty()) return;
        chip[n_chips].t.clear();
        chip[n_chips].t.put(t);
        chip[n_chips++].k = k;
    }
    // A past session's data: the words stay, the exception colours and chips go, a LAST pill ends the row.
    void make_stale() {
        n_chips = 0;
        add_chip("LAST", Last);
        dim = true;
        warn = false;
        l2_warn = false;
    }
};

void put_mohm_chip(StrBuf<10> &out, int32_t mohm) {
    if (mohm < 1000) {
        out.put_u32(static_cast<uint32_t>(mohm)).put("m\xCE\xA9");
    } else {
        const uint32_t tenths = (static_cast<uint32_t>(mohm) + 50u) / 100u;
        out.put_u32(tenths / 10u).put_char('.').put_u32(tenths % 10u).put("\xCE\xA9");
    }
}

bool put_slice_if_fits(StrBuf<40> &dst, std::string_view seg) {
    if (seg.empty()) return true;
    if (text_width(kLabelFont, dst.view()) + text_width(kLabelFont, seg) > text::conn::kSliceLineMaxPx) return false;
    dst.put(seg);
    return true;
}

void put_w_hero(StrBuf<16> &out, uint32_t mw) {
    if (mw >= 9950u) {
        out.put_u32((mw + 500u) / 1000u).put_char('W');
    } else if (mw >= 50u) {
        const uint32_t dw = (mw + 50u) / 100u;
        out.put_u32(dw / 10u);
        if (dw % 10u != 0u) out.put_char('.').put_u32(dw % 10u);
        out.put_char('W');
    } else {
        out.put("0W");
    }
}

void put_offers_summary(StrBuf<40> &out, const pd::SourceCaps &caps, int rows) {
    bool pps = false, avs = false;
    int groups = 0;
    StrBuf<24> grp;
    uint32_t grp_ma = 0;
    auto flush = [&] {
        if (grp.view().empty()) return;
        grp.put("V ");
        put_a(grp, grp_ma);
        StrBuf<28> seg;
        if (groups > 0) seg.put(" \xC2\xB7 ");
        seg.put(grp.view());
        put_slice_if_fits(out, seg.view());
        groups++;
        grp.clear();
        grp_ma = 0;
    };
    for (int i = 0; i < rows && i < int(pd::kMaxPdos); i++) {
        const pd::Pdo &p = caps.pdos[i];
        if (p.type == pd::PdoType::Pps) { pps = true; continue; }
        if (p.type == pd::PdoType::Avs) { avs = true; continue; }
        if (p.type != pd::PdoType::Fixed) continue;
        if (grp_ma != 0 && p.max_ma != grp_ma) flush();
        if (!grp.view().empty()) grp.put_char('/');
        put_v(grp, p.max_mv);
        grp_ma = p.max_ma;
    }
    flush();
    if (pps) put_slice_if_fits(out, out.view().empty() ? "PPS" : " +PPS");
    if (avs) put_slice_if_fits(out, out.view().empty() ? "AVS" : " +AVS");
}

[[gnu::noinline]] void build_slice_charger(SliceSpec &sl, const TabData &d, const conn::Facts &f,
                                           pd::PdVerdict verdict, pd::PdNoneWhy none_why) {
    sl.tag = text::conn::kSliceTagCharger;
    const MatchResult &r = *d.r;
    // First, so the 3-chip cap never drops it: the user's PD limit hides offers (a chip that explains a short list).
    if (const pd::LimStatus &ls = fusb302::limit_status(); ls.state != pd::LimState::Off) {
        char lb[pd::kLimLabelMax + 1]{};
        const std::size_t ln = pd::lim_label(ls.cfg, lb);
        StrBuf<12> lim;
        lim.put("LIM ").put(std::string_view{lb, ln});
        sl.add_chip(lim.view(), SliceSpec::Warn);
    }
    if (r.caps_seen) {
        const pd::SourceCaps &caps = *d.caps;
        uint32_t best_mw = 0, best_mv = 0, best_ma = 0;
        for (int i = 0; i < r.rows && i < int(pd::kMaxPdos); i++) {
            const pd::Pdo &p = caps.pdos[i];
            const uint32_t mw = p.max_mw ? p.max_mw : (p.max_mv > 0 && p.max_ma > 0 ? uint32_t(p.max_mv) * p.max_ma / 1000u : 0u);
            if (mw > best_mw) { best_mw = mw; best_mv = p.max_mv; best_ma = p.max_ma; }
        }
        if (best_mw == 0) best_mw = r.src_max_mw;
        put_w_hero(sl.head, best_mw);
        const pd::OfferSource src = held::offer_source();
        const bool throttled = held::offer_throttled() && !d.stale_charger;
        if (throttled) sl.add_chip("PEAK", SliceSpec::Ok);
        uint32_t max_ma = 0;
        for (int i = 0; i < r.rows && i < int(pd::kMaxPdos); i++)
            if (caps.pdos[i].max_ma > max_ma) max_ma = caps.pdos[i].max_ma;
        if (src == pd::OfferSource::Sniffed && f.inline_sink && max_ma > 0 && max_ma <= 3000u) {
            sl.add_chip("3A?", SliceSpec::Warn);
        } else if (caps.epr) {
            sl.add_chip("PD3.1", SliceSpec::Solid);
        } else if (caps.spec_rev == 1 || caps.spec_rev == 2) {
            sl.add_chip(pd::wire_revision_name(caps.spec_rev), SliceSpec::Solid);
        }
        if (best_mv > 0 && best_ma > 0) put_va(sl.l1, best_mv, best_ma);
        const pd::Identity &vid = fusb302::vdm().identity;
        StrBuf<36> seg;
        if (!sl.l1.view().empty()) seg.put(" \xC2\xB7 ");
        if (vid.valid) {
            if (!vid.vendor.empty()) {
                seg.put(vid.vendor.c_str()).put_char(' ');
                seg.put_hex(vid.vid, 4).put_char(':').put_hex(vid.pid, 4);
                if (!put_slice_if_fits(sl.l1, seg.view())) {
                    seg.clear();
                    if (!sl.l1.view().empty()) seg.put(" \xC2\xB7 ");
                    seg.put_hex(vid.vid, 4).put_char(':').put_hex(vid.pid, 4);
                    put_slice_if_fits(sl.l1, seg.view());
                }
            } else {
                seg.put_hex(vid.vid, 4).put_char(':').put_hex(vid.pid, 4);
                put_slice_if_fits(sl.l1, seg.view());
            }
        } else {
            seg.put_u32(static_cast<uint32_t>(r.rows)).put(r.rows == 1 ? K2_P(" offer") : K2_P(" offers"));
            put_slice_if_fits(sl.l1, seg.view());
        }
        if (caps.epr) put_slice_if_fits(sl.l1, " \xC2\xB7 EPR");
        if (throttled) {
            sl.l2_warn = true;
            sl.l2.put(K2_P("Now offering ")).put_u32(held::live_offer_mw() / 1000u).put("W \xC2\xB7 throttled");
        } else {
            put_offers_summary(sl.l2, caps, r.rows);
        }
        return;
    }
    const bool live_inline = f.inline_sink || d.in_use || (d.vbus_mv > pd::kVSafe5MaxMv && !d.k2_sink);
    if (live_inline) {
        if (d.vbus_mv >= 1000 && d.live_mw >= 500u) {
            put_w_hero(sl.head, d.live_mw);
            put_va(sl.l1, static_cast<uint32_t>(d.vbus_mv), static_cast<uint32_t>(d.ibus_ma));
            put_slice_if_fits(sl.l1, (verdict == pd::PdVerdict::None && d.vbus_mv <= pd::kVSafe5MaxMv)
                                         ? " \xC2\xB7 no PD seen"
                                         : " \xC2\xB7 PD missed");
            sl.l2.put(K2_P("Reset & re-read captures caps"));
            return;
        }
        sl.dim = (d.vbus_mv < 1000);
        sl.head.put(K2_P("Missed PD"));
        if (d.vbus_mv >= 1000 && d.ibus_ma >= 0) {
            put_va(sl.l1, static_cast<uint32_t>(d.vbus_mv), static_cast<uint32_t>(d.ibus_ma));
            put_slice_if_fits(sl.l1, " \xC2\xB7 0W live");
        } else if (d.vbus_mv >= 1000) {
            put_v(sl.l1, static_cast<uint32_t>(d.vbus_mv));
            sl.l1.put("V \xC2\xB7 PD missed");
        } else {
            sl.l1.put(text::conn::kSliceNoPdL1);
        }
        sl.l2.put(K2_P("Reset & re-read captures caps"));
        return;
    }
    sl.dim = true;
    const uint32_t rp_ma = r.typec_ma ? r.typec_ma : fusb302::typec_ma();
    auto put_legacy_fc = [&] {
        if (legacy_row_protocol(d.fc) && d.fc != fastcharge::Protocol::Pending) {
            StrBuf<6> fcn;
            fc_pill_text(d.fc, fcn);
            if (!fcn.view().empty()) sl.l2.put(K2_P("Legacy ")).put(fcn.view());
            return true;
        }
        return false;
    };
    auto put_rp_live = [&](const char *chip, const char *tag, const char *l2_txt) {
        sl.dim = false;
        put_w_hero(sl.head, static_cast<uint32_t>(d.vbus_mv) * rp_ma / 1000u);
        sl.add_chip(chip, SliceSpec::Neutral);
        put_va(sl.l1, static_cast<uint32_t>(d.vbus_mv), rp_ma);
        put_slice_if_fits(sl.l1, tag);
        if (l2_txt) sl.l2.put(l2_txt);
    };
    if (verdict == pd::PdVerdict::None && none_why == pd::PdNoneWhy::SourceNeverOffered) {
        if (d.vbus_mv >= 1000 && rp_ma > 0) {
            put_rp_live("NO PD", " \xC2\xB7 No PD offers", nullptr);
            if (!put_legacy_fc()) sl.l2.put(text::conn::kNoPdNeverL);
            return;
        }
        sl.head.put("No PD");
        sl.l1.put(text::conn::kNoPdNeverL);
        StrBuf<28> tc;
        typec_row_text(d.vbus_mv, rp_ma, tc);
        if (!tc.view().empty()) sl.l2.put(K2_P("Type-C ")).put(tc.view());
        else put_legacy_fc();
    } else if (r.typec_ma || r.typec_rp) {
        if (d.vbus_mv >= 1000 && rp_ma > 0) {
            put_rp_live(K2_P("TYPE-C"), " \xC2\xB7 Type-C Rp", K2_P("No PD offers on the line"));
            return;
        }
        sl.head.put(K2_P("Type-C"));
        sl.l1.put(K2_P("No PD offers on the line"));
        StrBuf<28> tc;
        typec_row_text(d.vbus_mv, r.typec_ma, tc);
        if (!tc.view().empty()) sl.l2.put(tc.view());
    } else if (f.cross == raguard::CrossKind::Crossed) {
        sl.head.put(K2_P("CC crossed"));
        sl.add_chip("WARN", SliceSpec::Warn);
        sl.l1.put(text::conn::kCrossedL);
    } else if (f.cross == raguard::CrossKind::Hint) {
        sl.head.put(K2_P("Flip cable?"));
        sl.add_chip("WARN", SliceSpec::Warn);
        sl.l1.put(text::conn::kFlipL);
    } else if (conn::source_asleep(f)) {
        sl.dim = false;
        sl.head.put("Ready");
        sl.add_chip(K2_P("ASLEEP"), SliceSpec::Ok);
        sl.l1.put(text::conn::kSliceReadyL1);
        sl.l2.put(K2_P("Open to wake or test"));
    } else if (f.wait == raguard::WaitKind::ChargerWaiting) {
        sl.dim = false;
        sl.head.put("Ready");
        sl.add_chip("WAIT", SliceSpec::Ok);
        sl.l1.put(text::conn::kSliceReadyL1);
        sl.l2.put(K2_P("Open to read or test"));
    } else if (f.wait == raguard::WaitKind::DualRolePort) {
        sl.head.put(K2_P("Dual-role"));
        sl.l1.put(text::conn::kDualRoleL);
    } else {
        sl.head.put(K2_P("No charger"));
        sl.l1.put(K2_P("Use either port"));
    }
}

[[gnu::noinline]] void build_slice_cable(SliceSpec &sl, const TabData &d, const conn::Facts &f) {
    sl.tag = text::conn::kSliceTagCable;
    const MatchResult &r = *d.r;
    const pd::Emarker &em = held::cable_em();
    const uint32_t cable_w = em.valid
        ? (em.max_power_w ? uint32_t(em.max_power_w) : uint32_t(em.max_voltage_v) * uint32_t(em.max_current_a))
        : 0u;
    const bool power_limits =
        (r.caps_seen && em.valid && cable_w > 0 && cable_w * 1000u < r.src_max_mw) ||
        (r.caps_seen && !em.valid && f.cable_fact == cablefact::Fact::NoChip && r.src_max_mw > 60000u);
    const bool cable_limits = d.ul.cable_limited || power_limits;
    sl.warn = cable_limits;
    if (em.valid) {
        if (cable_w > 0) sl.head.put_u32(cable_w).put_char('W');
        else sl.head.put_u32(em.max_voltage_v).put("V ").put_u32(em.max_current_a).put_char('A');
        if (cable_limits) sl.add_chip("LIMIT", SliceSpec::Warn);
        if (d.cable_mohm >= 0) {
            StrBuf<10> rc;
            put_mohm_chip(rc, d.cable_mohm);
            sl.add_chip(rc.view(), SliceSpec::Neutral);
        } else if (em.epr_capable) {
            sl.add_chip("EPR", SliceSpec::Solid);
        }
        if (!em.vendor_str.empty()) sl.l1.put(em.vendor_str.c_str());
        else sl.l1.put_hex(em.vid, 4).put_char(':').put_hex(em.pid, 4);
        if (em.speed_str && em.speed_str[0]) {
            char sb[24];
            pdtabs::speed_short(sb, em.speed_str);
            if (sb[0] && sb[0] != '?') {
                StrBuf<28> seg;
                seg.put(" \xC2\xB7 ").put(sb);
                put_slice_if_fits(sl.l1, seg.view());
            }
        }
        put_slice_if_fits(sl.l1, em.is_active ? " \xC2\xB7 Active" : " \xC2\xB7 Passive");
        if (em.max_voltage_v > 0 && em.max_current_a > 0)
            sl.l2.put_u32(em.max_voltage_v).put("V ").put_u32(em.max_current_a).put_char('A');
        if (cable_limits) {
            sl.l2_warn = true;
            const bool has_va = !sl.l2.view().empty();
            if (!put_slice_if_fits(sl.l2, has_va ? " \xC2\xB7 Bottleneck: caps link" : text::conn::kSliceBottleneck))
                put_slice_if_fits(sl.l2, " \xC2\xB7 Bottleneck");
        } else if (em.latency_str && em.latency_str[0]) {
            StrBuf<20> seg;
            if (!sl.l2.view().empty()) seg.put(" \xC2\xB7 ");
            seg.put(em.latency_str);
            put_slice_if_fits(sl.l2, seg.view());
        }
        return;
    }
    if (f.cable_fact == cablefact::Fact::NoChip) {
        sl.head.put("60W");
        sl.add_chip(cable_limits ? "LIMIT" : "PLAIN", cable_limits ? SliceSpec::Warn : SliceSpec::Neutral);
        if (d.cable_mohm >= 0) {
            StrBuf<10> rc;
            put_mohm_chip(rc, d.cable_mohm);
            sl.add_chip(rc.view(), SliceSpec::Neutral);
        }
        sl.l1.put("20V 3A \xC2\xB7 Plain cable, no chip");
        if (cable_limits) {
            sl.l2_warn = true;
            sl.l2.put(text::conn::kSliceBottleneck);
        } else {
            sl.l2.put(K2_P("No e-marker chip"));
        }
        return;
    }
    sl.dim = true;
    if (f.cable_fact == cablefact::Fact::ChipSilent) {
        sl.head.put(K2_P("Silent chip"));
        sl.add_chip("CHIP", SliceSpec::Warn);
        sl.l1.put(text::conn::kChipSilentL);
    } else if (f.topo == raguard::Topology::DirectReceptacle) {
        sl.head.put(K2_P("No cable"));
        sl.l1.put(text::conn::kNoCableL);
    } else {
        sl.head.put(K2_P("Not read"));
        if (!f.inline_sink && f.vbus_mv >= 0 && f.vbus_mv < raguard::kVbusAttachedMv) {
            sl.add_chip(K2_P("VCONN OK"), SliceSpec::Ok);
            sl.l1.put(text::conn::kSliceNoVbusL1);
        } else if (f.inline_sink) {
            sl.l1.put("Inline link \xC2\xB7 sniff or unplug");
        } else {
            sl.l1.put(K2_P("Open to read e-marker chip"));
        }
        if (d.cable_mohm >= 0) {
            StrBuf<10> rc;
            put_mohm_chip(rc, d.cable_mohm);
            sl.add_chip(rc.view(), SliceSpec::Neutral);
        }
    }
}

[[gnu::noinline]] void build_slice_sink(SliceSpec &sl, const TabData &d, const conn::Facts &f,
                                        pd::PdVerdict verdict, pd::PdNoneWhy none_why) {
    sl.tag = text::conn::kSliceTagSink;
    const MatchResult &r = *d.r;
    if (r.request_seen && !d.k2_sink) {
        const uint32_t mv = r.want_mv ? r.want_mv : (d.vbus_mv >= 1000 ? static_cast<uint32_t>(d.vbus_mv) : 0u);
        const uint32_t ma = r.want_ma;
        const uint32_t mw = r.want_mw ? r.want_mw : (mv > 0 && ma > 0 ? mv * ma / 1000u : 0u);
        if (mw > 0) put_w_hero(sl.head, mw);
        else if (mv > 0 || ma > 0) put_va(sl.head, mv, ma);
        else sl.head.put(K2_P("Request"));
        const pd::Contract &ct = fusb302::contract();
        if (ct.ready) sl.add_chip("OK", SliceSpec::Ok);
        if (mv > 0 && ma > 0) {
            put_va(sl.l1, mv, ma);
            if (r.src_max_mw > 0) {
                StrBuf<24> seg;
                seg.put(" \xC2\xB7 of ").put_u32((r.src_max_mw + 500u) / 1000u).put(K2_P("W offered"));
                if (!put_slice_if_fits(sl.l1, seg.view())) {
                    seg.clear();
                    seg.put(" \xC2\xB7 of ").put_u32((r.src_max_mw + 500u) / 1000u).put_char('W');
                    put_slice_if_fits(sl.l1, seg.view());
                }
            }
        } else if (r.src_max_mw > 0) {
            sl.l1.put("Asks ").put_u32((mw + 500u) / 1000u).put("W of ").put_u32((r.src_max_mw + 500u) / 1000u).put(K2_P("W offered"));
        } else {
            sl.l1.put("Asks ").put_u32((mw + 500u) / 1000u).put_char('W');
        }
        const pd::Identity &sid = fusb302::sink_vdm().identity;
        const pd::Identity &iqid = d.iq_ok ? fusb302::interrogate_status().id : pd::Identity{};
        if (sid.valid) {
            sl.l2.put_hex(sid.vid, 4).put_char(':').put_hex(sid.pid, 4);
            if (!sid.vendor.empty()) {
                StrBuf<24> seg;
                seg.put_char(' ').put(sid.vendor.c_str());
                put_slice_if_fits(sl.l2, seg.view());
            }
        } else if (iqid.valid) {
            sl.l2.put_hex(iqid.vid, 4).put_char(':').put_hex(iqid.pid, 4);
            if (!iqid.vendor.empty()) {
                StrBuf<24> seg;
                seg.put_char(' ').put(iqid.vendor.c_str());
                put_slice_if_fits(sl.l2, seg.view());
            }
        } else {
            const pd::Pdo *p = r.want_pos > 0 && r.want_pos <= d.caps->num_pdos ? &d.caps->pdos[r.want_pos - 1] : nullptr;
            sl.l2.put(p && p->type == pd::PdoType::Pps ? "PPS" : p && p->type == pd::PdoType::Avs ? "AVS" : "PDO")
                 .put_u32(static_cast<uint32_t>(r.want_pos));
            const char *st = request_state_name(ct);
            if (st[0]) sl.l2.put(" \xC2\xB7 ").put(st);
        }
        return;
    }
    if (fusb302::identify_current()) {
        const pd::Identity &pid = fusb302::identify_identity();
        uint32_t smw = 0, smv = 0, sma = 0;
        for (uint32_t raw : d.sink) {
            const pd::Pdo p = pd::decode_pdo(raw);
            const uint32_t mw = p.max_mw ? p.max_mw : (p.max_mv > 0 && p.max_ma > 0 ? uint32_t(p.max_mv) * p.max_ma / 1000u : 0u);
            if (mw > smw) { smw = mw; smv = p.max_mv; sma = p.max_ma; }
        }
        if (smw > 0) put_w_hero(sl.head, smw);
        else if (pid.valid && !pid.vendor.empty() && text_width(kMid, pid.vendor.c_str()) <= 132) sl.head.put(pid.vendor.c_str());
        else sl.head.put(K2_P("Device read"));
        const uint32_t irev = fusb302::identify_status().revision;
        if (irev != 0) {
            StrBuf<10> rv;
            rv.put("PD").put_u32((irev >> 28) & 0xFu).put_char('.').put_u32((irev >> 24) & 0xFu);
            sl.add_chip(rv.view(), SliceSpec::Solid);
        }
        if (smv > 0 && sma > 0) put_va(sl.l1, smv, sma);
        if (pid.valid) {
            StrBuf<36> seg;
            if (!sl.l1.view().empty()) seg.put(" \xC2\xB7 ");
            seg.put_hex(pid.vid, 4).put_char(':').put_hex(pid.pid, 4);
            if (!pid.vendor.empty()) seg.put_char(' ').put(pid.vendor.c_str());
            if (!put_slice_if_fits(sl.l1, seg.view())) {
                seg.clear();
                if (!sl.l1.view().empty()) seg.put(" \xC2\xB7 ");
                seg.put_hex(pid.vid, 4).put_char(':').put_hex(pid.pid, 4);
                put_slice_if_fits(sl.l1, seg.view());
            }
        } else if (sl.l1.view().empty()) {
            sl.l1.put(K2_P("Standalone PD read"));
        }
        if (!d.sink.empty()) {
            StrBuf<32> ss;
            sink_summary(d.sink, ss);
            sl.l2.put(ss.view());
        } else {
            sl.l2.put(K2_P("Open for full sink details"));
        }
        return;
    }
    if (d.k2_sink) {
        sl.head.put("K2");
        sl.add_chip("SELF", SliceSpec::Solid);
        if (d.vbus_mv >= 1000 && d.ibus_ma >= 0) {
            put_va(sl.l1, static_cast<uint32_t>(d.vbus_mv), static_cast<uint32_t>(d.ibus_ma));
            StrBuf<16> ws;
            put_w_hero(ws, d.live_mw);
            StrBuf<20> seg;
            seg.put(" \xC2\xB7 ").put(ws.view());
            put_slice_if_fits(sl.l1, seg.view());
            sl.l2.put(K2_P("K2 draws power itself"));
        } else {
            sl.l1.put(K2_P("K2 draws power itself"));
        }
        return;
    }
    const bool live_inline = f.inline_sink || d.in_use || (d.vbus_mv > pd::kVSafe5MaxMv && !d.k2_sink);
    if (live_inline) {
        if (d.vbus_mv >= 1000 && d.ibus_ma >= 0) {
            put_w_hero(sl.head, d.live_mw);
            put_va(sl.l1, static_cast<uint32_t>(d.vbus_mv), static_cast<uint32_t>(d.ibus_ma));
            put_slice_if_fits(sl.l1, (verdict == pd::PdVerdict::None && d.vbus_mv <= pd::kVSafe5MaxMv)
                                         ? " \xC2\xB7 no PD"
                                         : " \xC2\xB7 live draw");
            sl.l2.put(K2_P("Reset & re-read or interrogate"));
            return;
        }
        sl.head.put(K2_P("In use"));
        if (d.vbus_mv >= 1000) {
            put_v(sl.l1, static_cast<uint32_t>(d.vbus_mv));
            sl.l1.put("V \xC2\xB7 live inline");
        } else if (verdict == pd::PdVerdict::None) {
            sl.l1.put(conn::no_pd_line(conn::Tab::Sink, none_why));
        } else {
            sl.l1.put(K2_P("Device drawing power inline"));
        }
        sl.l2.put(K2_P("Open to identify or interrogate"));
        return;
    }
    sl.dim = true;
    if (f.wait == raguard::WaitKind::DualRolePort) {
        sl.head.put(K2_P("Dual-role"));
        sl.l1.put(K2_P("Phone or tablet on port?"));
        sl.l2.put(text::conn::kDualRoleSinkL);
    } else {
        sl.head.put("None");
        sl.l1.put(K2_P("No device on far port"));
        sl.l2.put(K2_P("Identify or interrogate"));
    }
}

[[gnu::noinline]] bool paint_slice(const Surface &c, int idx, const SliceSpec &sl, uint32_t &sig, PaintBudget *bg) {
    uint32_t h = mix_u32(mix_init(), uint32_t(idx) | uint32_t(sl.focused) << 2 | uint32_t(sl.warn) << 3 |
                                         uint32_t(sl.dim) << 4 | uint32_t(sl.l2_warn) << 5 | uint32_t(sl.n_chips) << 6);
    h = pg::mix_cstr(h, sl.tag);
    h = pg::mix_str(h, sl.head.view());
    for (int i = 0; i < sl.n_chips; i++) {
        h = mix_u32(h, 0x100u | sl.chip[i].k);
        h = pg::mix_str(h, sl.chip[i].t.view());
    }
    h = pg::mix_str(mix_u32(h, 0x200u), sl.l1.view());
    h = pg::mix_str(mix_u32(h, 0x300u), sl.l2.view()) | 1u;
    if (h == sig) return true;
    const int16_t y = kSliceY[idx], sh = kSliceH[idx];
    if (bg && !bg->admit(static_cast<int>(c.w) * (sh + (idx == 0 ? 2 : 0)), true)) return false;
    sig = h;
    if (idx == 0) gfx_fill(c, 0, 0, c.w, 2, kBg);

    const bool focus_bar = sl.focused && kFocusBar != 0;
    const bool warn_bar = !sl.focused && sl.warn;
    const bool has_bar = focus_bar || warn_bar;
    const uint16_t bg_col = sl.focused ? kFocusFill : (sl.warn ? kCautionT : kBg);
    const uint16_t top_col = sl.focused ? (kFocusBar ? kFocusBar : kFocusFill) : (idx > 0 ? kRule : kBg);
    const uint16_t inv = sl.focused && !focus_bar;
    const uint16_t tag_col = inv ? kOnFocusDim : (sl.warn ? kCaution : kInk2);
    const uint16_t head_col = inv ? kOnFocus : (sl.dim ? kInk2 : kInk);
    const uint16_t l1_col = inv ? kOnFocus : kInk2;
    const uint16_t l2_col = inv ? kOnFocusDim : (sl.l2_warn ? kCaution : kInk2);

    gfx_fill(c, 0, y, c.w, 1, top_col);
    const int16_t gx = has_bar ? 3 : 0;
    if (has_bar) gfx_fill(c, 0, static_cast<int16_t>(y + 1), 3, static_cast<int16_t>(sh - 1), focus_bar ? kFocusBar : kCaution);

    char b1[kFitBuf], b2[kFitBuf];
    const std::string_view l1_v = sl.l1.view().empty() ? std::string_view{} : fit_data(kLabelFont, sl.l1.view(), c.w - 12, b1);
    const std::string_view l2_v = sl.l2.view().empty() ? std::string_view{} : fit_data(kLabelFont, sl.l2.view(), c.w - 12, b2);
    const int16_t tag_y = static_cast<int16_t>(y + 5);
    const int16_t head_y = static_cast<int16_t>(y + 19);
    const int16_t chip_y = static_cast<int16_t>(y + 24);
    const int16_t l1_y = static_cast<int16_t>(y + 47);
    const int16_t l2_y = static_cast<int16_t>(y + 61);

    const int16_t tw = static_cast<int16_t>(text_width(kLabelFont, sl.tag));
    const int16_t hw = static_cast<int16_t>(text_width(kMid, sl.head.view()));
    const int16_t l1w = static_cast<int16_t>(text_width(kLabelFont, l1_v));
    const int16_t l2w = static_cast<int16_t>(text_width(kLabelFont, l2_v));

    kit::Box holes[8];
    int nh = 0;
    if (tw > 0) holes[nh++] = {8, tag_y, tw, static_cast<int16_t>(kLabelFont.height)};
    if (hw > 0) holes[nh++] = {8, head_y, hw, static_cast<int16_t>(kMid.height)};
    int16_t cx[3] = {};
    int shown = 0;
    for (int16_t x = static_cast<int16_t>(8 + hw + 6); shown < sl.n_chips; shown++) {
        const int16_t w = static_cast<int16_t>(badge_text_bounds(kLabelFont, sl.chip[shown].t.view()).w + 8);
        if (x + w > c.w - 6) break;
        cx[shown] = x;
        if (nh < 8) holes[nh++] = kit::Box{x, chip_y, w, kit::kChipH};
        x = static_cast<int16_t>(x + w + 4);
    }
    if (l1w > 0 && nh < 8) holes[nh++] = {8, l1_y, l1w, static_cast<int16_t>(kLabelFont.height)};
    if (l2w > 0 && nh < 8) holes[nh++] = {8, l2_y, l2w, static_cast<int16_t>(kLabelFont.height)};

    kit::fill_except(c, {gx, static_cast<int16_t>(y + 1), static_cast<int16_t>(c.w - gx), static_cast<int16_t>(sh - 1)},
                     holes, nh, bg_col);
    if (tw > 0) gfx_text(c, kLabelFont, 8, tag_y, sl.tag, tag_col, bg_col);
    if (sl.focused) kit::draw_drill(c, kit::kTailX, static_cast<int16_t>(y + 11), inv ? kOnFocus : kFocusBar);
    if (hw > 0) gfx_text(c, kMid, 8, head_y, sl.head.view(), head_col, bg_col);
    for (int i = 0; i < shown; i++) {
        const SliceSpec::Chip &ch = sl.chip[i];
        switch (ch.k) {
        case SliceSpec::Solid: kit::draw_chip(c, cx[i], chip_y, ch.t.view(), kChip, kOnChip, 0, bg_col); break;
        case SliceSpec::Ok: kit::draw_chip(c, cx[i], chip_y, ch.t.view(), kOkT, kOk, 0, bg_col); break;
        case SliceSpec::Warn: kit::draw_chip(c, cx[i], chip_y, ch.t.view(), kCautionT, kCaution, 0, bg_col); break;
        case SliceSpec::Neutral: kit::draw_chip(c, cx[i], chip_y, ch.t.view(), kBg2, kInk, 0, bg_col); break;
        case SliceSpec::Last: kit::draw_chip(c, cx[i], chip_y, ch.t.view(), kStale, kBg, 0, bg_col); break;
        }
    }
    if (l1w > 0) gfx_text(c, kLabelFont, 8, l1_y, l1_v, l1_col, bg_col);
    if (l2w > 0) gfx_text(c, kLabelFont, 8, l2_y, l2_v, l2_col, bg_col);
    return true;
}

}  // namespace

namespace {
// The K2 is the sink only when no device is inline: self_powered() is the boot pin latch, which stays set while the
// driver has the switches open for a device behind the K2.
bool k2_is_sink(const pd::Link &link, bool ctx_inline) {
    return fusb302::self_powered() && !link.inline_mode() && !ctx_inline;
}
}  // namespace

bool ConnectionPage::paint_overview(const MatchResult &r, const pd::SourceCaps &caps, std::span<const uint32_t> sink) {
    const pd::Link &link = ctx_.ui->link();
    TabData d;
    d.r = &r; d.caps = &caps; d.sink = sink;
    d.ul = fusb302::usb_link(dp_, dm_);
    d.waiting = link.attached() && link.waiting();
    d.k2_sink = k2_is_sink(link, ctx_.inline_mode);
    d.k2_sink_ok = fusb302::pin_rd() == 1;
    d.fc = static_cast<fastcharge::Protocol>(fc_);
    d.in_use = ctx_.ui->link_reset_inline();
    d.vbus_mv = vbus_;
    d.ibus_ma = ibus_;
    d.live_mw = (vbus_ >= 1000 && ibus_ >= 0) ? (static_cast<uint32_t>(vbus_) * static_cast<uint32_t>(ibus_) / 1000u) : 0u;
    d.vooc = vooc_;
    d.iq_ok = iq_have_ && iq_session_ == fusb302::session() && fusb302::interrogate_status().phase != pd::IqPhase::Idle;
    d.cable_mohm = current_cable_mohm(ctx_);
    // A detach ends the session: what the slices still hold is the last one's, grey with a LAST pill until the next attach.
    d.stale_charger = !link.attached() && r.caps_seen;
    d.stale_cable = link.cable_stale() && held::cable_em().valid;
    d.stale_sink = link.sink_left() && r.request_seen && !d.k2_sink;

    const int focus_slice = ctx_.ui->ring_view() ? -1 : static_cast<int>(tab_);
    for (int i = 0; i < 3; i++) {
        SliceSpec sl;
        sl.focused = (i == focus_slice);
        if (i == 0) build_slice_charger(sl, d, facts_, verdict_, none_why_);
        else if (i == 1) build_slice_cable(sl, d, facts_);
        else build_slice_sink(sl, d, facts_, verdict_, none_why_);
        if (i == 0 ? d.stale_charger : i == 1 ? d.stale_cable : d.stale_sink) sl.make_stale();
        if (!paint_slice(ctx_.content, i, sl, slice_sig_[static_cast<std::size_t>(i)], ctx_.budget)) return false;
    }
    g_conn_body_passes++;
    return true;
}

bool ConnectionPage::paint_actions_ident(const MatchResult &r, const pd::SourceCaps &caps, std::span<const uint32_t> sink) {
    const bool empty = conn::tab_empty(tab_, facts_) &&
                       !(tab_ == conn::Tab::Sink && (k2_is_sink(ctx_.ui->link(), ctx_.inline_mode) || fusb302::identify_current()));
    if (empty) {
        const conn::Empty e = conn::empty_state(tab_, facts_);
        return paint_ident_dim(ctx_.content, e.title, e.hint, id_sig_, ctx_.budget);
    }
    if (no_pd_body()) {
        return paint_ident_dim(ctx_.content, text::conn::kNoPd, conn::no_pd_line(tab_, none_why_), id_sig_, ctx_.budget);
    }
    const pd::Link &link = ctx_.ui->link();
    TabData d;
    d.r = &r; d.caps = &caps; d.sink = sink;
    d.ul = fusb302::usb_link(dp_, dm_);
    d.waiting = link.attached() && link.waiting();
    d.k2_sink = k2_is_sink(link, ctx_.inline_mode);
    d.k2_sink_ok = fusb302::pin_rd() == 1;
    d.fc = static_cast<fastcharge::Protocol>(fc_);
    d.in_use = link.inline_mode();
    d.vbus_mv = vbus_;
    d.ibus_ma = ibus_;
    d.live_mw = (vbus_ >= 1000 && ibus_ >= 0) ? (static_cast<uint32_t>(vbus_) * static_cast<uint32_t>(ibus_) / 1000u) : 0u;
    d.vooc = vooc_;
    d.iq_ok = iq_have_ && iq_session_ == fusb302::session() && fusb302::interrogate_status().phase != pd::IqPhase::Idle;
    d.cable_mohm = current_cable_mohm(ctx_);
    return paint_place_ident(ctx_.content, tab_, d, id_sig_, ctx_.budget);
}

// An empty Info page: the dim headline and hint in its identity block, no fact or action rows.
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
    ListDrawState st{tail_cleared_, body_clear_y_, bg};
    return paint_place_list(c, false, {first_, res_n_, tot_n_, thumb_sig_, row_sig_, body_n_, st}, noop_build_rows, nullptr);
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
    d.k2_sink = k2_is_sink(link, ctx_.inline_mode);
    d.k2_sink_ok = fusb302::pin_rd() == 1;
    d.fc = static_cast<fastcharge::Protocol>(fc_);
    d.in_use = link.inline_mode();
    d.vbus_mv = vbus_;
    d.ibus_ma = ibus_;
    d.live_mw = (vbus_ >= 1000 && ibus_ >= 0) ? (static_cast<uint32_t>(vbus_) * static_cast<uint32_t>(ibus_) / 1000u) : 0u;
    d.vooc = vooc_;
    d.iq_ok = iq_have_ && iq_session_ == fusb302::session() && fusb302::interrogate_status().phase != pd::IqPhase::Idle;
    d.cable_mohm = current_cable_mohm(ctx_);
    const bool done = paint_body_locked(&d);
    if (done) g_conn_body_passes++;
    return done;
}

bool ConnectionPage::paint_body_locked(const void *tab_data) {
    const auto &d = *static_cast<const TabData *>(tab_data);
    const Surface &c = ctx_.content;
    PaintBudget *const bg = ctx_.budget;
    const bool full = body_ != Body::Rows || body_tab_ != tab_;
    if (full) {
        if (body_ == Body::Empty) first_ = 0;
        body_ = Body::Rows; body_tab_ = tab_;
        row_sig_.fill(0); body_n_ = 0; tail_cleared_ = ctx_.blank; close_done_ = false;
        body_clear_y_ = kFooterY; id_sig_ = thumb_sig_ = 0;
    }
    if (!paint_place_ident(c, tab_, d, id_sig_, bg)) return false;
    ListDrawState st{tail_cleared_, body_clear_y_, bg};
    const BodyBuildCtx bctx{tab_, &d};
    return paint_place_list(c, false, {first_, res_n_, tot_n_, thumb_sig_, row_sig_, body_n_, st}, build_body_rows, &bctx);
}

// The no-PD body (ROADMAP 2026-11-10, Row C): the dim identity headline (NO PD) and its one line, then only the rows
// that are actually measured - the Type-C advertisement (Rp level + VBUS), the draw on the Sink tab, a legacy fast-charge
// answer on the Source tab. paint_body only calls this when the paint signature moved.
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
        if (body_ == Body::Empty) first_ = 0;
        body_ = Body::NoPd; body_tab_ = tab_;
        row_sig_.fill(0); body_n_ = 0; tail_cleared_ = ctx_.blank; close_done_ = false;
        body_clear_y_ = kFooterY; id_sig_ = thumb_sig_ = 0;
    }
    if (!paint_ident_dim(c, text::conn::kNoPd, line, id_sig_, bg)) return false;
    ListDrawState st{tail_cleared_, body_clear_y_, bg};
    const NoPdBuildCtx bctx{tab_, static_cast<fastcharge::Protocol>(ctx_.fc), ctx_.pm_avg.current_ma, vbus_, vooc_, rp_ma};
    return paint_place_list(c, false, {first_, res_n_, tot_n_, thumb_sig_, row_sig_, body_n_, st}, build_no_pd_rows, &bctx);
}

void ConnectionPage::row_fn(void *ctx, int index, bool focused, kit::RowSpec &out) {
    static constexpr const char *kNotes[] = {
        K2_P("read chip, open its details"),
        K2_P("save read to library"),
        K2_P("view or arm saved clones"),
        K2_P("spoof e-marker to charger"),
        K2_P("identify attached device"),
    };
    static_assert(all_fit_p(kNotes, kLabelAdv, kit::kNoteMaxPx), "an action note is wider than the note box");
    auto *p = static_cast<ConnectionPage *>(ctx);
    out = kit::RowSpec{};
    const int idx = index;
    if (idx < 0 || idx >= p->n_items_) return;
    const conn::Item &it = p->items_[static_cast<std::size_t>(idx)];
    out.text = it.label;
    out.section_before = (idx == 1);
    if (it.act == conn::Act::ResetLink) {
        const bool armed = before(p->ctx_.now, p->reset_armed_until_);
        if (armed) {
            out.text_color = kCaution;
            out.tail = "ARMED";
            out.tail_color = kCaution;
            out.tail_tint = kCautionT;
            if (focused) out.note = text::conn::kNoteResetArmed;
        } else {
            out.text_color = kText;
            out.tail = it.tag;
            out.tail_color = kDim;
            if (focused) out.note = it.caution ? text::conn::kNoteResetInline : text::conn::kNoteResetStand;
        }
        return;
    }
    if (!it.enabled) {
        out.text_color = kInk3;
        out.tail = it.tag;
        out.tail_color = kInk3;
        out.chevron = false;
    } else {
        if (it.caution) {
            out.tail = it.tag;
            out.tail_color = kCaution;
        }
        out.chevron = !it.caution && (it.act == conn::Act::Details || it.act == conn::Act::Bench ||
                                      it.act == conn::Act::SavedCables || it.act == conn::Act::SpoofAs);
    }
    if (focused) {
        if ((!it.enabled || it.caution) && it.why && it.why[0]) {
            out.note = it.why;
        } else if (it.act == conn::Act::Details) {
            out.note = p->tab_ == conn::Tab::Source ? text::conn::kNoteSpecSource : text::conn::kNoteSpecSink;
        } else if (it.act == conn::Act::Bench) {
            const bench::Acts b = bench::actions(conn::place_of(p->tab_));
            if (it.arg >= 0 && it.arg < b.n) out.note = bench::act_hint(b.a[it.arg]);
        } else {
            switch (it.act) {
            case conn::Act::ReadCable:   out.note = kNotes[0]; break;
            case conn::Act::SaveCable:   out.note = kNotes[1]; break;
            case conn::Act::SavedCables: out.note = kNotes[2]; break;
            case conn::Act::SpoofAs:     out.note = kNotes[3]; break;
            case conn::Act::Identify:    out.note = kNotes[4]; break;
            default: break;
            }
        }
    }
}

// The bottom bar: what the last action says, else the state of the shown place and the topology word. It goes
// through the footer cache, so a steady state writes nothing.
void ConnectionPage::paint_status(uint32_t now_ms) {
    StrBuf<40> txt;
    FooterTone tone = FooterTone::Info;
    const pd::IdStatus &ids = fusb302::identify_status();
    const int focus = act_sel_ < 0 ? 0 : first_act() + act_sel_;
    const bool focus_off = focus >= 0 && focus < n_items_ && !items_[static_cast<std::size_t>(focus)].enabled;
    // An action row's greyed item names why; the tool's own action says it only when OK is pressed.
    const conn::Item *row = act_sel_ >= 0 && focus < n_items_ ? &items_[static_cast<std::size_t>(focus)] : nullptr;
    if (focus_off && act_sel_ >= 0) {
        txt.put(items_[static_cast<std::size_t>(focus)].why);
        tone = row && row->act == conn::Act::Bench ? FooterTone::Error : FooterTone::Caution;   // a blocked tool, as on the Bench
    } else if (row && row->caution && row->why && row->why[0]) {
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
        txt.put(p == pd::IqPhase::Reset1 || p == pd::IqPhase::Sniff1 ? K2_P("Resetting link...")
              : p == pd::IqPhase::Reset2 || p == pd::IqPhase::Sniff2 ? K2_P("Restoring link...")
                                                                     : K2_P("Asking device..."));
    } else if (ctx_.em_scanning) {
        txt.put(K2_P("Reading cable..."));
    } else if (before(now_ms, ctx_.em_listen_until)) {
        txt.put(K2_P("Listening for cable..."));
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
    // The bottom bar is not painted in the twin-rail chrome: the survey's progress and summary reach the screen as a toast.
    if (!before(now_ms, op_until_) && txt.view() != status_txt_.view()) {
        const pd::Survey &sv = survey::state();
        if (sv.running()) {
            ctx_.toast.show(FooterTone::Info, txt.view(), now_ms, 1500);
        } else if (status_txt_.view().substr(0, 8) == "Reading " && sv.ran()) {
            StrBuf<conn::kStatusCells + 1> sum;
            conn::read_summary(sv, sum);
            ctx_.toast.show(sv.silent() ? FooterTone::Caution : FooterTone::Ok, sum.view(), now_ms, 3000);
        }
    }
    status_txt_ = txt;
    (void)tone;
}

static uint32_t mix_u16s(uint32_t h, const uint16_t *w, std::size_t n) {
    for (std::size_t i = 0; i < n; i++) h = mix_u32(h, w[i]);
    return h;
}

static uint32_t mix_plug(uint32_t h, const cable_full::Plug &p) {
    h = mix_u32(h, uint32_t(p.svids_ack) | uint32_t(p.nsvid) << 1 | uint32_t(p.mode_ack) << 8);
    h = mix_u16s(h, p.svid.data(), p.svid.size());
    return pg::mix_u32s(h, p.mode.data(), p.mode.size());
}

// The Cable tab's full-read data (SVIDs, modes, SOP'' identity): a finished read repaints the rows.
static uint32_t mix_cable_full(uint32_t h, const cable_full::Full &f) {
    h = mix_u32(h, uint32_t(f.valid) | uint32_t(f.id2_ack) << 1 | uint32_t(f.id2_n) << 2);
    h = pg::mix_u32s(h, f.id2.data(), f.id2.size());
    return mix_plug(mix_plug(h, f.p1), f.p2);
}

static uint32_t mix_identity(uint32_t h, const pd::Identity &id) {
    h = mix_u32(h, uint32_t(id.valid) | uint32_t(id.vid) << 8 | uint32_t(id.pid) << 16);
    return mix_u32(h, id.id_header);
}

static uint32_t mix_mfr_info(uint32_t h, const pd::ManufacturerInfo &mfr) {
    h = mix_u32(h, uint32_t(mfr.valid));
    h = mix_u32(h, uint32_t(mfr.vid) | (uint32_t(mfr.pid) << 16));
    return pg::mix_cstr(h, mfr.name, sizeof mfr.name);
}

static uint32_t mix_bat_mfr(uint32_t h, const pd::BatteryCaps &bat, const pd::BatteryStatus &bst,
                            const pd::ManufacturerInfo &mfr) {
    h = mix_u32(h, uint32_t(bat.valid) | uint32_t(bat.invalid_ref) << 1 | uint32_t(bat.design_dwh) << 16);
    h = mix_u32(h, uint32_t(bat.last_full_dwh) | uint32_t(bst.valid) << 16 | uint32_t(bst.present) << 17
                   | uint32_t(bst.invalid_ref) << 18 | uint32_t(bst.charging) << 19);
    h = mix_u32(h, bst.present_dwh);
    return mix_mfr_info(h, mfr);
}

static uint32_t mix_dev_extra(uint32_t h, const pd::ManufacturerInfo &batt_mfr, const pd::Status &st) {
    h = mix_mfr_info(h, batt_mfr);
    h = mix_u32(h, uint32_t(st.valid) | uint32_t(st.internal_temp_c) << 8
                   | uint32_t(st.present_input) << 16 | uint32_t(st.battery_input) << 24);
    return mix_u32(h, uint32_t(st.event_flags) | uint32_t(st.temp_status) << 8
                      | uint32_t(st.power_status) << 16);
}

void ConnectionPage::update(uint32_t now_ms) {
    PaintBudget *const bg = ctx_.budget;
    const pd::Link &link = ctx_.ui->link();
    title_badge(ctx_.stale());
    if (bg && bg->more) return;
    sense(now_ms);
    auto_wake(now_ms);
    if (iq_asked_ && !fusb302::interrogate_running()) {
        iq_asked_ = false;
        const pd::IqStatus &iq = fusb302::interrogate_status();
        const char *msg = iq.why == pd::IqWhy::SourceResets ? K2_P("Charger resets")
                        : iq.why == pd::IqWhy::SourceRestarts ? K2_P("Charger restarts")
                        : iq.ok_mask != 0 ? K2_P("Device read") : K2_P("No answer");
        set_op(msg, now_ms, 4000);
    }
    build_items();   // the action rows and the OK verb follow the live state
    if (em_read_pending_ && ctx_.em_listen_until == 0) {
        em_read_pending_ = false;
        if (ctx_.em_quiet) {
            read_failed(now_ms);
        } else if (view_ == View::Actions) {
            exit_actions();
        }
    }
    if (view_ == View::Actions) {
        if (act_sel_ < 0) act_sel_ = 0;
        if (act_sel_ >= act_count()) act_sel_ = std::max(0, act_count() - 1);
    } else {
        act_sel_ = -1;
    }
    paint_status(now_ms);
    if (bg && bg->more) return;

    // Cheap pre-check: hash only the inputs that can reach the screen -- view, ring focus, link state,
    // settled/shown caps (not live rx_count/raw_header so rapid renegotiation storms before settle do
    // not churn), displayed contract fields, sink/identify headers, e-marker & cable resistance, VDM,
    // debounced fc/ADC/VBUS, and debounced port-sense facts.
    const pd::SourceCaps &shown_caps = held::shown_caps();
    const pd::Contract &ct = fusb302::contract();
    const pd::SinkCapsSeen &sniffed = fusb302::sink_caps();
    const pd::IdStatus &idst = fusb302::identify_status();
    const pd::Emarker &em = fusb302::emarker();
    const pd::VdmInfo &vdm = fusb302::vdm();
    const pd::VdmInfo &snk = fusb302::sink_vdm();
    const int32_t cable_mohm = current_cable_mohm(ctx_);
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
    ibus_ = static_cast<int32_t>(
        ibus_h_.commit(uint32_t(typec_ibus_ma(ctx_.pm_online, ctx_.pm_avg.current_ma)), now_ms));
    if (!ctx_.vooc.seen) vooc_ = 0;   // a session end clears the note at once
    else if (!vooc_ || uint32_t(now_ms - vooc_at_) >= kVoocNoteMs) { vooc_ = 1u << 16 | ctx_.vooc.r82; vooc_at_ = now_ms; }
    uint32_t c = 2166136261u;
    c = mix_u32(c, vooc_);
    c = mix_u32(c, uint32_t(view_) | uint32_t(ctx_.ui->ring_view()) << 2 | uint32_t(facts_.charger) << 3 |
                   uint32_t(facts_.inline_sink) << 4 | uint32_t(facts_.sink_seen) << 5 | uint32_t(facts_.cable) << 6 |
                   uint32_t(conn::source_asleep(facts_)) << 7 | uint32_t(facts_.cross) << 8 |
                   uint32_t(facts_.vbus_mv >= raguard::kVbusAttachedMv) << 10 |
                   uint32_t(held::offer_source()) << 12 | uint32_t(held::offer_throttled()) << 14 |
                   uint32_t(held::cable_source()) << 16);
    c = mix_u32(c, held::live_offer_mw());
    c = mix_u32(c, static_cast<uint32_t>(cable_mohm));
    c = mix_u32(c, uint32_t(link.attached()) | uint32_t(link.waiting()) << 1 | uint32_t(link.inline_mode()) << 2
              | uint32_t(pd::is_fake()) << 3 | uint32_t(fusb302::active_cc()) << 4
              | uint32_t(tab_) << 8 | fc_ << 16
              | uint32_t(facts_.topo) << 24 | uint32_t(facts_.wait) << 26 | uint32_t(fusb302::self_powered()) << 28
              | uint32_t(link.cable_stale()) << 29 | uint32_t(link.sink_left()) << 30);
    c = mix_u32(c, uint32_t(verdict_) | uint32_t(none_why_) << 4);   // the no-PD gate's verdict settles at attach+1.5 s
    c = mix_u32(c, uint32_t(facts_.cable_fact));                     // the empty Cable tab's words: chip silent / no chip
    c = mix_u32(c, fusb302::session());
    c = mix_u32(c, uint32_t(shown_caps.valid) | uint32_t(shown_caps.num_pdos) << 8 | uint32_t(shown_caps.epr) << 16
              | uint32_t(shown_caps.spec_rev) << 17 | uint32_t(shown_caps.epr_partial) << 19);
    if (shown_caps.valid) {
        const std::size_t np = std::min<std::size_t>(shown_caps.num_pdos, pd::kMaxPdos);
        for (std::size_t i = 0; i < np; i++) {
            c = mix_u32(c, shown_caps.pdos[i].raw);
            c = mix_u32(c, uint32_t(shown_caps.pdos[i].max_mv) | uint32_t(shown_caps.pdos[i].max_ma) << 16);
        }
    }
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
    c = mix_cable_full(c, held::cable_extra());
    c = mix_u32(c, uint32_t(held::cable_em().valid) | uint32_t(held::cable_em().vid) << 8 | uint32_t(held::cable_em().raw_vdos[4]) << 24);
    c = mix_u32(c, held::cable_em().raw_vdos[3] ^ uint32_t(held::cable_answered().orientation) << 28);
    c = mix_identity(c, vdm.identity);
    c = mix_u32(c, uint32_t(vdm.num_svids) | uint32_t(vdm.modes_svid) << 8 | uint32_t(vdm.num_modes) << 24);
    c = mix_u16s(c, vdm.svids.data(), vdm.num_svids);
    c = pg::mix_u32s(c, vdm.modes.data(), vdm.num_modes);
    c = mix_u32(c, uint32_t(vdm.cuktech) | uint32_t(vdm.xiaomi) << 1 | uint32_t(vdm.xiaomi_cmd) << 2);
    {   // the lab-only Apple readout's rows (the serial is never displayed nor hashed)
        const pd::AppleInfo &ap = fusb302::apple_info();
        c = mix_u32(c, uint32_t(ap.valid) | uint32_t(ap.why) << 1 | uint32_t(ap.reads) << 9 | uint32_t(ap.ok) << 17);
        c = pg::mix_cstr(c, ap.product.data(), ap.product.size());
        c = pg::mix_cstr(c, ap.fw.data(), ap.fw.size());
        c = pg::mix_cstr(c, ap.hw.data(), ap.hw.size());
    }
    c = mix_identity(c, snk.identity);
    c = mix_u32(c, uint32_t(snk.num_svids));
    c = mix_u16s(c, snk.svids.data(), snk.num_svids);
    c = mix_u32(c, uint32_t(ctx_.analog.has_value()));
    c = mix_u32(c, uint32_t(dp_));
    c = mix_u32(c, uint32_t(dm_));   // the debounced ADC levels (Row D): raw jitter is not hashed
    c = mix_u32(c, uint32_t(cc1_));
    c = mix_u32(c, uint32_t(cc2_));
    c = mix_u32(c, uint32_t(vbus_));   // the Type-C row's VBUS (100 mV steps)
    if (view_ == View::Overview) {   // the Charger slice's LIM chip
        const pd::LimStatus &ls = fusb302::limit_status();
        std::array<uint8_t, pd::kLimPackBytes> lb{};   // Off hashes as all zero: an armed limit keeps one tech on
        if (ls.state != pd::LimState::Off) pd::lim_pack(ls.cfg, lb);
        c = pg::mix_str(c, {reinterpret_cast<const char *>(lb.data()), lb.size()});
    }
    if (view_ == View::Overview && (!shown_caps.valid || !ct.valid || fusb302::self_powered())) {
        c = mix_u32(c, uint32_t(ibus_));
        c = mix_u32(c, uint32_t(ctx_.ui->link_reset_inline()));
    }
    const uint32_t svs = survey_sig(view_ == View::Overview || tab_ == conn::Tab::Source);
    c = mix_u32(c, svs);
    const bool iq_ok = iq_have_ && iq_session_ == fusb302::session() && fusb302::interrogate_status().phase != pd::IqPhase::Idle;
    uint32_t dev_sig = 2166136261u;
    {
        const pd::ExtInfo &ext = fusb302::ext();
        dev_sig = mix_bat_mfr(dev_sig, ext.battery, ext.battery_status, ext.mfr);
        dev_sig = mix_dev_extra(dev_sig, ext.batt_mfr, ext.status);
        if (iq_ok) {
            const pd::IqStatus &iq = fusb302::interrogate_status();
            dev_sig = mix_u32(dev_sig, uint32_t(iq.phase) | uint32_t(iq.ok_mask) << 8 | uint32_t(iq.num_sink_pdos) << 24);
            dev_sig = pg::mix_u32s(dev_sig, iq.sink_pdos.data(), iq.num_sink_pdos);
            dev_sig = mix_u32(dev_sig, uint32_t(iq.num_epr_pdos) | uint32_t(iq.epr_partial) << 8
                                     | uint32_t(iq.sink_ext_len) << 16 | uint32_t(iq.num_svids) << 24);
            const std::size_t n_ext = std::min<std::size_t>(iq.sink_ext_len, iq.sink_ext.size());
            for (std::size_t i = 0; i < n_ext; i++) dev_sig = mix_u32(dev_sig, iq.sink_ext[i]);
            dev_sig = mix_u32(dev_sig, iq.revision);
            dev_sig = mix_u32(dev_sig, uint32_t(iq.id.valid) | uint32_t(iq.id.vid) << 8 | uint32_t(iq.id.pid) << 16);
            dev_sig = mix_bat_mfr(dev_sig, iq.battery, iq.battery_status, iq.mfr);
            dev_sig = mix_dev_extra(dev_sig, iq.batt_mfr, iq.status);
        }
        if (fusb302::identify_current()) {
            const pd::Identity &iid = fusb302::identify_identity();
            const pd::ExtInfo &iext = fusb302::identify_ext();
            dev_sig = mix_u32(dev_sig, idst.revision);
            dev_sig = mix_u32(dev_sig, uint32_t(iid.valid) | uint32_t(iid.vid) << 8 | uint32_t(iid.pid) << 16);
            if (iid.valid && !iid.vendor.empty()) dev_sig = pg::mix_cstr(dev_sig, iid.vendor.c_str());
            dev_sig = mix_bat_mfr(dev_sig, iext.battery, iext.battery_status, iext.mfr);
            dev_sig = mix_dev_extra(dev_sig, iext.batt_mfr, iext.status);
        }
    }
    const uint32_t c_base = mix_u32(c, dev_sig);
    const uint32_t fsig = static_cast<uint32_t>(first_);
    c = mix_u32(c_base, fsig);

    auto ensure_match = [&] {
        g_last_match = match_live(link);
        if (!g_last_match.caps_seen && !pd::is_fake() && link.attached() && !fusb302::pd_traffic()) {
            const uint8_t acc = fusb302::active_cc();
            const int32_t cc_mv = acc == 2 ? cc2_ : acc == 1 ? cc1_ : -1;
            if (cc_line_has_rp(cc_mv)) {
                g_last_match.typec_rp = true;
                if (!g_last_match.typec_ma) g_last_match.typec_ma = typec_ma_from_cc_mv(cc_mv);
            }
        }
        match_valid_ = true;
    };

    if (view_ == View::Actions) {
        const bool need_ident = !sig_valid_ || !match_valid_ || c != cheap_sig_ || id_sig_ == 0;
        if (need_ident) {
            if (!match_valid_ || c != cheap_sig_) ensure_match();
            if (!paint_actions_ident(g_last_match, g_book.caps, wants_now(iq_ok).pdos)) return;
            cheap_sig_ = c;
            sig_valid_ = true;
        }
        if (!actions_gap_cleared_) {
            const int16_t gap_y = kit::list_end(kit::kPlaceListY);
            if (gap_y < kFooterY) {
                if (bg && !bg->admit(static_cast<int>(ctx_.content.w) * (kFooterY - gap_y), true)) return;
                gfx_fill(ctx_.content, 0, gap_y, ctx_.content.w, static_cast<int16_t>(kFooterY - gap_y), kBg);
            }
            actions_gap_cleared_ = true;
        }
        ctx_.rows.set_count(act_count());
        ctx_.rows.select(act_sel_);
        ctx_.rows.draw(now_ms, bg);
        return;
    }

    if (!body_pending_ && sig_valid_ && c == cheap_sig_) return;   // nothing changed: skip match_live + the full sig
    const bool inputs_changed = !match_valid_ || c != cheap_sig_;
    cheap_sig_ = c;

    if (view_ == View::Overview) {
        if (inputs_changed) ensure_match();
        body_pending_ = true;
        const bool painted_all = paint_overview(g_last_match, g_book.caps, wants_now(iq_ok).pdos);
        if (painted_all) {
            sig_valid_ = true;
            body_pending_ = false;
        }
        return;
    }

    const bool empty = conn::tab_empty(tab_, facts_) && !(tab_ == conn::Tab::Sink && (k2_is_sink(ctx_.ui->link(), ctx_.inline_mode) || fusb302::identify_current()));
    if (empty) {
        match_valid_ = false;
        uint32_t h_base = mix_u32(2166136261u, uint32_t(tab_) | uint32_t(facts_.topo) << 4 | 1u << 8 | uint32_t(facts_.inline_sink) << 9
                                            | uint32_t(facts_.wait) << 10 | uint32_t(conn::source_asleep(facts_)) << 12
                                            | uint32_t(facts_.cross) << 13
                                            | uint32_t(facts_.wake_port_off) << 24
                                            | uint32_t(facts_.vbus_mv >= raguard::kVbusAttachedMv) << 25);
        h_base = mix_u32(h_base, uint32_t(facts_.cable_fact));
        const uint32_t h = mix_u32(h_base, fsig);
        if (!body_pending_ && sig_valid_ && h == sig_) return;
        const bool redraw = !sig_valid_;
        body_pending_ = true;
        const bool painted_all = paint_empty(redraw);
        const uint32_t new_fsig = static_cast<uint32_t>(first_);
        cheap_sig_ = mix_u32(c_base, new_fsig);   // reflect any first_ clamping done by paint_place_list()
        if (painted_all) {
            sig_ = mix_u32(h_base, new_fsig);
            sig_valid_ = true;
            body_pending_ = false;
        }
        return;
    }

    if (inputs_changed) ensure_match();
    const MatchResult &r = g_last_match;
    const pd::SourceCaps *caps = &g_book.caps;   // filled by match_live above
    const WantsList wl = wants_now(iq_ok);
    const std::span<const uint32_t> sink = wl.pdos;

    uint32_t h = 2166136261u;
    h = mix_u32(h, uint32_t(verdict_) | uint32_t(none_why_) << 4);   // the no-PD body switches on the verdict edge
    h = mix_u32(h, uint32_t(tab_) | uint32_t(link.waiting()) << 9 | uint32_t(caps->epr) << 10 | uint32_t(caps->spec_rev) << 11 | uint32_t(r.verdict) << 16 | uint32_t(r.flags) << 24);
    h = mix_u32(h, r.typec_ma | uint32_t(fusb302::pd_traffic()) << 16 | uint32_t(link.inline_mode()) << 17);
    h = mix_u32(h, uint32_t(r.typec_rp));
    h = mix_u32(h, uint32_t(vbus_mv));
    h = mix_u32(h, static_cast<uint32_t>(cable_mohm));
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
    h = mix_u32(h, fsig);
    h = mix_u32(h, uint32_t(wl.src));
    h = mix_u32(h, uint32_t(ct.flags) | uint32_t(ct.accepted) << 8 | uint32_t(ct.ready) << 9 | uint32_t(ct.rejected) << 10);
    {   // the tab bodies read more than the match: cable, identity, USB link, protocol, sink mode
        const usblink::Link ul = fusb302::usb_link(dp_, dm_);   // debounced levels (Row D)
        h = mix_u32(h, uint32_t(em.valid) | uint32_t(em.is_active) << 1 | uint32_t(em.vid) << 8 | uint32_t(em.raw_vdos[4]) << 24);
        h = mix_u32(h, em.raw_vdos[4] | uint32_t(em.max_power_w) << 8 | uint32_t(em.sop) << 24);
        h = mix_u32(h, fusb302::cable_seq());
        h = mix_cable_full(h, held::cable_extra());
        h = mix_u32(h, uint32_t(held::cable_em().valid) | uint32_t(held::cable_em().vid) << 8 | uint32_t(held::cable_em().raw_vdos[4]) << 24);
        h = mix_u32(h, held::cable_em().raw_vdos[3] ^ uint32_t(held::cable_answered().orientation) << 28);
        h = mix_u32(h, uint32_t(fusb302::identify_current()));
        h = mix_identity(h, vdm.identity);
        h = mix_identity(h, snk.identity);
        h = mix_u32(h, uint32_t(vdm.num_svids) | uint32_t(snk.num_svids) << 8);
        h = mix_u16s(h, vdm.svids.data(), vdm.num_svids);
        h = mix_u16s(h, snk.svids.data(), snk.num_svids);
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
    cheap_sig_ = mix_u32(c_base, static_cast<uint32_t>(first_));   // reflect any scroll clamping done by place_first() during paint
    if (painted_all) {
        sig_ = h;
        sig_valid_ = true;
        body_pending_ = false;
    }
}

}  // namespace k2
