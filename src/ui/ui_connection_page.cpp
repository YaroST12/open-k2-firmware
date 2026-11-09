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
#include "k2/pd/emrec.hpp"
#include "k2/drivers/fusb302.hpp"
#include "k2/pd/pd_source.hpp"
#include "k2/pd/pd_tabs.hpp"
#include "k2/pd/pd_vdm.hpp"
#include "k2/ui/qrshare_records.hpp"
#include "k2/core/storage.hpp"
#include "k2/pd/usb_link.hpp"
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
    op_ = StrBuf<24>();
    op_until_ = 0;
    id_asked_ = false;
    sig_valid_ = false;
    sense(ctx_.now);                    // facts only; the topology window starts with this one sample
    tab_ = conn::default_tab(facts_);   // Sink if a device is inline or was seen on CC, else Source if a charger is seen, else Cable if a cable is held
    defaulted_ = true;
}

void ConnectionPage::leave() {
    popup_ = false;
    popup_drawn_ = false;
    op_ = StrBuf<24>();
    op_until_ = 0;
    id_asked_ = false;
    ctx_.inline_armed = false;   // a half-given confirm does not outlive the page
    ctx_.em_listen_until = 0;    // nor does a listen window or its "heard nothing" verdict
    ctx_.em_quiet = false;
}

void ConnectionPage::draw_static() {
    title_bar();
    sig_valid_ = false;
    popup_drawn_ = false;   // the pop-up re-binds and repaints the shared RowList
    status_.invalidate();
}

void ConnectionPage::invalidate() {
    sig_valid_ = false;
    popup_drawn_ = false;
    status_.invalidate();
}

void ConnectionPage::open_tab(conn::Tab t) {
    tab_ = t;
    defaulted_ = false;
    if (popup_) close_popup();
    sig_valid_ = false;
}

void ConnectionPage::close_popup() {
    popup_ = false;
    popup_drawn_ = false;
    sig_valid_ = false;
    ctx_.inline_armed = false;   // BACK on the pop-up also cancels a half-given inline confirm
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
    if (shrt && (e.button == Button::Plus || e.button == Button::Minus)) {
        tab_ = e.button == Button::Plus ? pdtabs::next(tab_) : pdtabs::prev(tab_);
        defaulted_ = false;
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
    a.inline_sink = ctx_.ui->link().inline_mode() || ctx_.inline_mode;
    a.attached = facts_.charger || facts_.inline_sink;
    a.caps_valid = pd::active_caps().valid;
    a.cable_valid = fusb302::emarker().valid;
    n_items_ = conn::actions(tab_, a, items_);
    if (popup_sel_ >= n_items_) popup_sel_ = n_items_ - 1;
    if (popup_sel_ < 0) popup_sel_ = 0;
}

void ConnectionPage::activate(const conn::Item &it, uint32_t now_ms) {
    if (!it.enabled) return;   // the status bar names the reason while the greyed item is selected
    bool keep_open = false;
    switch (it.act) {
    case conn::Act::Refresh:
        ctx_.ui->run_action(Action::PdRescan, now_ms);
        set_op("Rescanning...", now_ms, 1500);
        break;
    case conn::Act::ShareCaps:
        close_popup();
        share_caps(now_ms);
        return;
    case conn::Act::ReadCable:
        if (!ctx_.ui->run_action(Action::EmarkerScan, now_ms) && !ctx_.inline_armed) set_op("Test running", now_ms);
        keep_open = ctx_.inline_armed;   // a live sink: the first OK only armed the confirm, the second reads
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
        const bool started = ctx_.ui->run_action(Action::Identify, now_ms);
        id_asked_ = started;
        if (!started && !ctx_.inline_armed) set_op(ctx_.id_no_device ? "No device to ask" : "Identify refused", now_ms);
        keep_open = ctx_.inline_armed;
        break;
    }
    }
    if (!keep_open) close_popup();
}

// ---------------------------------------------------------------------------------------
// Sensing: facts and the passive topology
// ---------------------------------------------------------------------------------------

void ConnectionPage::sense(uint32_t now_ms) {
    const pd::Link &link = ctx_.ui->link();
    if (!guard_primed_ || uint32_t(now_ms - guard_t_) >= raguard::kSampleMs) {
        guard_.add(ctx_.analog ? ctx_.analog->cc1_mv : -1, ctx_.analog ? ctx_.analog->cc2_mv : -1);
        guard_t_ = now_ms;
        guard_primed_ = true;
    }
    // The same passive recipe the charger test's Ra guard uses: valid only while the K2 holds nothing on the
    // lines and VBUS is down (a charger waiting for a sink). Anything else is Unknown. Nothing here drives CC.
    raguard::Reading r;
    r.cc1_mv = guard_.cc1();
    r.cc2_mv = guard_.cc2();
    r.vbus_mv = ctx_.pm_online ? ctx_.vbus_fast_mv : -1;
    r.passive = !fusb302::rd_for_run_on() && !fusb302::ra_active() && !fusb302::self_powered() && !ctx_.inline_mode && !link.inline_mode();
    facts_.topo = raguard::classify(r);
    facts_.charger = link.attached();
    facts_.cable = fusb302::emarker().valid;
    facts_.inline_sink = link.inline_mode();
    // A decoded Request or Sink_Capabilities was received from a sink (the engine only sees received
    // frames, so the K2's own trigger requests never set these): the sink stays seen after a power-role
    // swap and after the inline latch expires.
    facts_.sink_seen = fusb302::contract().valid || fusb302::sink_caps().valid;
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

// A value row. `cut` > 0 splits the value there: the head in `c`, the tail (the current) in kAmp; so a "20/28V 5A" line
// reads in unit colours. 0 = one colour.
struct Rows {
    struct R { const char *key = ""; StrBuf<28> v; uint16_t c = kText; uint8_t cut = 0; };
    R r[kMaxList];
    R spill;   // rows past the last visible one land here and are dropped
    R *open = &spill;
    int n = 0;
    StrBuf<28> &add(const char *key, uint16_t c = kText) {
        R &x = n < kMaxList ? r[n++] : spill;
        x.key = key; x.v.clear(); x.c = c; x.cut = 0;
        open = &x;
        return x.v;
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
    fastcharge::Protocol fc;
    int32_t vbus_mv = -1;   // the fast VBUS reading in 100 mV steps (what the Type-C row shows), -1 unknown
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
    else R.add("ID").put("unknown");
    const pd::SourceCaps &caps = *d.caps;
    StrBuf<28> &pdv = R.add("PD", r.caps_seen ? kEnergy : kFaint);
    const bool traffic = fusb302::pd_traffic();
    if (r.caps_seen) {
        pdv.put(caps.spec_rev == 1 || caps.spec_rev == 2 ? pd::wire_revision_name(caps.spec_rev) : "no PD yet");
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
        pdv.put("none");
        StrBuf<28> &v = R.add("Type-C", kText);
        typec_row_text(d.vbus_mv, r.typec_ma, v);
    } else {
        pdv.put(traffic ? "active, no caps" : "no caps yet");
    }
    if (!r.caps_seen && legacy_row_protocol(d.fc)) {   // legacy fast-charge protocols the K2 detected
        StrBuf<6> fcn;
        fc_pill_text(d.fc, fcn);
        if (!fcn.view().empty() && d.fc != fastcharge::Protocol::Pending) R.add("Legacy", kEnergy).put(fcn.view());
    }
    link_rows(R, d);
    proto_row(R, fusb302::vdm());
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
}

// One row: "5V 3A 9V 3A" whole entries that fit the 19 cells, then a "+" when more follow (never a cut entry).
void put_sink_pdos(Rows &R, std::span<const uint32_t> sink) {
    StrBuf<28> &cur = R.add("Caps", kText);
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

[[gnu::noinline]] void sink_rows(Rows &R, const TabData &d) {
    const MatchResult &r = *d.r;
    const pd::Identity &id = fusb302::sink_vdm().identity;
    if (d.k2_sink) {   // the K2 draws the power itself
        R.add("ID", kFaint).put("K2");
        R.add("Sink", kOk).put(d.k2_sink_ok ? "K2 (sink mode OK)" : "K2 (sink mode PLUS)");
        link_rows(R, d);
        return;
    }
    if (id.valid) put_id(R.add("ID"), id.vid, id.pid, id.vendor);
    else R.add("ID").put("unknown");   // until the sink answers a Discover Identity (the charger's, or Identify's)
    if (r.request_seen) {
        put_request(R.add("Request", kAmp), r, *d.caps);
        StrBuf<28> &st = R.add("State", kDim);
        st.put(request_state_name(fusb302::contract()));
        StrBuf<32> fl;
        const std::size_t room = st.view().size() + 2 < static_cast<std::size_t>(kValCells) ? kValCells - st.view().size() - 2 : 0;
        rdo_flags_text(fusb302::contract().flags, room, fl);
        if (!fl.view().empty()) st.put(", ").put(fl.view());
    } else {
        R.add("Request", kFaint).put("none seen");
    }
    strip_rows(R, r, d.ul);
    link_rows(R, d);
    proto_row(R, fusb302::sink_vdm());
    if (!d.sink.empty()) put_sink_pdos(R, d.sink);
    else R.add("Caps", kFaint).put("not seen");
}

}  // namespace

// ---------------------------------------------------------------------------------------
// Painting
// ---------------------------------------------------------------------------------------

void ConnectionPage::paint_tab_indicator() {
    // Three cells of the bar's rule pitch, the name centred (S12): the active tab in kFocus over a 2 px kFocus rule, the
    // others kDim over a kRule one, so the strip reads as the bar's progress rule continued.
    const Surface &c = ctx_.content;
    for (int t = 0; t < conn::kTabs; t++) {
        const bool on = t == static_cast<int>(tab_);
        const std::string_view name = conn::tab_name(static_cast<conn::Tab>(t));
        const int16_t x = static_cast<int16_t>(t * kit::kProgPitch);
        const int16_t w = kit::kProgSegW;
        gfx_fill(c, x, kTabY, w, kTabH, kBg);
        gfx_text(c, kLabelFont, static_cast<int16_t>(x + (w - static_cast<int>(name.size()) * kLabelFont.adv) / 2),
                 static_cast<int16_t>(kTabY + 2), name, on ? kFocus : kDim, kBg);
        gfx_fill(c, x, static_cast<int16_t>(kTabY + kTabH - 2), w, 2, on ? kFocus : kRule);
    }
}

// A large message and one wrapped hint line group (the port-aware text of conn::empty_state).
void ConnectionPage::paint_empty() {
    const Surface &c = ctx_.content;
    gfx_fill(c, 0, kit::kBodyY, c.w, kFooterY - kit::kBodyY, kBg);
    paint_tab_indicator();
    body_ = Body::Empty;
    const conn::Empty e = conn::empty_state(tab_, facts_);
    char buf[kFitBuf];
    gfx_text(c, kMid, 8, 74, fit_text(kMid, e.title, c.w - 16, buf), kText, kBg);
    std::array<std::string_view, 4> lines;
    const int n = conn::wrap(e.hint, static_cast<std::size_t>((c.w - 16) / kSmall.adv), lines);
    for (int i = 0; i < n; i++)
        gfx_text(c, kSmall, 8, static_cast<int16_t>(112 + i * 20), lines[static_cast<std::size_t>(i)], kDim, kBg);
}

void ConnectionPage::paint_body(const MatchResult &r, const pd::SourceCaps &caps, std::span<const uint32_t> sink,
                                bool redraw) {
    static_assert(kBodyRows == kMaxList, "one stored hash per visible row");
    const Surface &c = ctx_.content;
    const pd::Link &link = ctx_.ui->link();
    TabData d;
    d.r = &r; d.caps = &caps; d.sink = sink;
    d.ul = fusb302::usb_link(ctx_.analog ? ctx_.analog->dp_mv : -1, ctx_.analog ? ctx_.analog->dm_mv : -1);
    d.waiting = link.attached() && link.waiting();
    d.k2_sink = fusb302::self_powered();
    d.k2_sink_ok = fusb302::pin_rd() == 1;
    d.fc = ctx_.fc;
    d.vbus_mv = typec_vbus_mv(ctx_.pm_online, ctx_.vbus_fast_mv);
    Rows R;
    switch (tab_) {
    case conn::Tab::Source: source_rows(R, d); break;
    case conn::Tab::Cable: cable_rows(R, d); break;
    case conn::Tab::Sink: sink_rows(R, d); break;
    }
    const bool dim = tab_ == conn::Tab::Cable && link.cable_stale();
    char buf[kFitBuf];
    constexpr int16_t kKeyDy = kit::row_text_y(kRowPitch, 12), kValDy = kit::row_text_y(kRowPitch, 16);

    // A whole-body pass when the area was wiped or never drawn here (new tab, pop-up closed, empty state left) or the
    // row count moved (the closing hairline shifts); otherwise a row repaints alone when its hash changed. A live PPS
    // request moves the Request, Used and Pwr rows every second or two, the other rows stay put.
    const bool full = redraw || body_ != Body::Rows || body_tab_ != tab_ || body_n_ != R.n;
    std::array<uint32_t, kBodyRows> sigs{};
    for (int i = 0; i < R.n; i++) {
        const Rows::R &row = R.r[i];
        uint32_t h = fnv(2166136261u, uint32_t(row.c) | uint32_t(row.cut) << 16 | uint32_t(dim) << 24);
        for (const char *k = row.key; *k; k++) h = fnv(h, uint8_t(*k));
        h = fnv(h, 0x100u);   // key / value separator
        for (char ch : row.v.view()) h = fnv(h, uint8_t(ch));
        sigs[static_cast<std::size_t>(i)] = h;
    }
    int painted = 0;
    if (full) {
        gfx_fill(c, 0, kit::kBodyY, c.w, kFooterY - kit::kBodyY, kBg);   // below the bar's rule
        paint_tab_indicator();
    }
    for (int i = 0; i < R.n; i++) {
        if (!full && sigs[static_cast<std::size_t>(i)] == row_sig_[static_cast<std::size_t>(i)]) continue;
        painted++;
        const Rows::R &row = R.r[i];
        const int16_t y = static_cast<int16_t>(kRowY0 + i * kRowPitch);
        if (!full) gfx_fill(c, 0, y, c.w, kRowPitch, kBg);   // the row's own slot only; the next row's hairline is its own
        gfx_fill(c, 0, y, c.w, 1, kRule);
        gfx_text(c, kLabelFont, kKeyX, static_cast<int16_t>(y + kKeyDy), row.key, kDim, kBg);
        const std::string_view all = row.v.view();
        const int16_t vy = static_cast<int16_t>(y + kValDy);
        if (row.cut > 0 && all.size() <= static_cast<std::size_t>(kValCells)) {   // built to fit: head in its colour, current in kAmp
            gfx_text(c, kSmall, kValX, vy, all.substr(0, row.cut), dim ? kStale : row.c, kBg);
            gfx_text(c, kSmall, static_cast<int16_t>(kValX + row.cut * kSmall.adv), vy, all.substr(row.cut), dim ? kStale : kAmp, kBg);
        } else {
            // Every row is built to fit the value column (vendor names, tags and flags are added whole or not at all),
            // so a cut here is a copy bug: fit_text fails the host gate on it.
            gfx_text(c, kSmall, kValX, vy, fit_text(kSmall, all, c.w - kValX - 4, buf), dim ? kStale : row.c, kBg);
        }
    }
    if (full && R.n < kMaxList) gfx_fill(c, 0, static_cast<int16_t>(kRowY0 + R.n * kRowPitch), c.w, 1, kRule);   // closes the table
    body_ = Body::Rows;
    body_tab_ = tab_;
    body_n_ = static_cast<uint8_t>(R.n);
    row_sig_ = sigs;
    K2LOG("[CONN] body full=%d rows=%d\n", full ? 1 : 0, painted);
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
    if (popup_ && popup_sel_ < n_items_ && !items_[static_cast<std::size_t>(popup_sel_)].enabled) {
        txt.put(items_[static_cast<std::size_t>(popup_sel_)].why);
        tone = FooterTone::Caution;
    } else if (ctx_.inline_armed) {
        txt.put("Live sink! OK again");
        tone = FooterTone::Caution;
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
        const bool op = before(now_ms, op_until_);
        if (op) s.op = op_.view();
        conn::status_text(s, txt);
        if (!op && s.spoof_armed && facts_.charger) tone = FooterTone::Caution;
    }
    draw_footer_cached(ctx_.content, tone, txt.view(), status_);
}

void ConnectionPage::update(uint32_t now_ms) {
    const pd::Link &link = ctx_.ui->link();
    title_badge(false);
    sense(now_ms);
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
    const int32_t dp_mv = ctx_.analog ? ctx_.analog->dp_mv : -1;
    const int32_t dm_mv = ctx_.analog ? ctx_.analog->dm_mv : -1;
    const int32_t cc1_mv = ctx_.analog ? ctx_.analog->cc1_mv : -1;
    const int32_t cc2_mv = ctx_.analog ? ctx_.analog->cc2_mv : -1;
    const int32_t vbus_mv = typec_vbus_mv(ctx_.pm_online, ctx_.vbus_fast_mv);
    uint32_t c = 2166136261u;
    c = fnv(c, uint32_t(link.attached()) | uint32_t(link.waiting()) << 1 | uint32_t(link.inline_mode()) << 2
              | uint32_t(pd::is_fake()) << 3 | uint32_t(fusb302::active_cc()) << 4
              | uint32_t(tab_) << 8 | uint32_t(link.cable_stale()) << 12 | uint32_t(ctx_.fc) << 16
              | uint32_t(facts_.topo) << 24 | uint32_t(fusb302::self_powered()) << 28);
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
    c = fnv(c, uint32_t(snk.identity.valid) | uint32_t(snk.identity.vid) << 8 | uint32_t(snk.identity.pid) << 16);
    c = fnv(c, snk.identity.id_header);
    c = fnv(c, uint32_t(snk.num_svids));
    for (int i = 0; i < snk.num_svids; i++) c = fnv(c, snk.svids[i]);
    c = fnv(c, uint32_t(ctx_.analog.has_value()));
    c = fnv(c, uint32_t(dp_mv));
    c = fnv(c, uint32_t(dm_mv));
    c = fnv(c, uint32_t(cc1_mv));
    c = fnv(c, uint32_t(cc2_mv));
    c = fnv(c, uint32_t(vbus_mv));   // the Type-C row's VBUS (100 mV steps)
    if (sig_valid_ && c == cheap_sig_) return;   // nothing changed: skip match_live + the full sig
    cheap_sig_ = c;

    const bool empty = conn::tab_empty(tab_, facts_) && !(tab_ == conn::Tab::Sink && fusb302::self_powered());
    if (empty) {
        uint32_t h = fnv(2166136261u, uint32_t(tab_) | uint32_t(facts_.topo) << 4 | 1u << 8);
        if (sig_valid_ && h == sig_) return;
        sig_ = h;
        sig_valid_ = true;
        paint_empty();
        return;
    }

    MatchResult r = match_live(link);
    // The Rp advertisement of an attached source exists even when the chip's gated typec_ma() reads 0
    // (a default-USB Rp, a load inline, before its 3 s mark): the active CC line driven into the Rp
    // window says so, and its divider level gives the current.
    if (!r.caps_seen && !pd::is_fake() && link.attached() && !fusb302::pd_traffic()) {
        const uint8_t acc = fusb302::active_cc();
        const int32_t cc_mv = !ctx_.analog ? -1
            : acc == 2 ? int32_t(ctx_.analog->cc2_mv)
            : acc == 1 ? int32_t(ctx_.analog->cc1_mv) : -1;
        if (cc_line_has_rp(cc_mv)) {
            r.typec_rp = true;
            if (!r.typec_ma) r.typec_ma = typec_ma_from_cc_mv(cc_mv);
        }
    }
    const pd::SourceCaps *caps = &g_book.caps;   // filled by match_live above
    const WantsList wl = wants_now();
    const std::span<const uint32_t> sink = wl.pdos;

    uint32_t h = 2166136261u;
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
    h = fnv(h, uint32_t(wl.src));
    h = fnv(h, uint32_t(ct.flags) | uint32_t(ct.accepted) << 8 | uint32_t(ct.ready) << 9 | uint32_t(ct.rejected) << 10);
    {   // the tab bodies read more than the match: cable, identity, USB link, protocol, sink mode
        const pd::Identity &id = vdm.identity;
        const usblink::Link ul = fusb302::usb_link(dp_mv, dm_mv);
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
        h = fnv(h, uint32_t(ctx_.fc) | uint32_t(fusb302::self_powered()) << 8 | uint32_t(fusb302::pin_rd() == 1) << 9);
    }
    if (sig_valid_ && h == sig_) return;
    const bool redraw = !sig_valid_;   // invalidated (page entered, tab, pop-up closed, repaint pass): the area is stale
    sig_ = h;
    sig_valid_ = true;
    paint_body(r, *caps, sink, redraw);
}

}  // namespace k2
