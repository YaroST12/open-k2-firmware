// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#include "k2/ui/ui_pages.hpp"

#include "k2/ui/readout_fmt.hpp"
#include "k2/ui/trend.hpp"
#include "k2/ui/uiprefs.hpp"
#include "k2/ui/ui_lines.hpp"
#include "k2/ui/ui_tools.hpp"
#include "k2/ui/text/tools.hpp"
#include "k2/ui/ui_emarker.hpp"
#include "k2/ui/ui_orientation.hpp"
#include "k2/core/log.hpp"
#include "k2/pd/pd_emarker_plan.hpp"
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <cstring>
#include <string_view>
#include "k2/drivers/adc.hpp"
#include "k2/fastcharge/fastcharge.hpp"
#include "k2/drivers/fusb302.hpp"
#include "k2/pd/pd_source.hpp"
#include "k2/drivers/power_calc.hpp"
#include "k2/core/storage.hpp"
#include "k2/drivers/usb.hpp"
#include "k2/drivers/systick.hpp"
#include "k2/ui/qrshare_records.hpp"
#include "k2/ui/qrview.hpp"
#include "k2/ui/ui_viewport.hpp"
#include "k2/core/scratch.hpp"
#include "k2/drivers/lcd.hpp"
#include "k2/core/version.hpp"
#include "k2/pd/link.hpp"
#include "k2/ui/ui.hpp"
#include "k2/ui/ui_style.hpp"
#include "k2/ui/text.hpp"

namespace k2 {

namespace {

// Whole volts without a decimal, otherwise one decimal: 5, 20, 3.3, 9.5.
template <size_t N>
void put_volts(StrBuf<N> &s, uint32_t mv) {
    if (mv % 1000 == 0) s.put_u32(mv / 1000);
    else s.put_fixed(static_cast<int32_t>(mv / 100 * 100), 1);
}

constexpr unsigned kModelLabelVersion = 55;   // version digits of the model label ("N6.55")

}  // namespace


void lines_row_text(int i, const std::optional<adc::Reading> &r, fastcharge::Protocol p, StrBuf<24> &s) {
    auto mv_row = [&](const char *label, int32_t mv) {
        s.put(label);
        if (r) s.put_u32(static_cast<uint32_t>(mv < 0 ? 0 : mv)).put(" mV");
        else s.put("--");
    };
    switch (i) {
    case 0: mv_row("D+   ", r ? r->dp_mv : 0); break;
    case 1: mv_row("D-   ", r ? r->dm_mv : 0); break;
    case 2: mv_row("CC1  ", r ? r->cc1_mv : 0); break;
    case 3: mv_row("CC2  ", r ? r->cc2_mv : 0); break;
    case 4:
        s.put("AUX  ");
        if (r) s.put_fixed(r->aux_mv, 3).put(" V"); else s.put("--");
        break;
    case 5:
        s.put("MCU  ");
        if (r && r->mcu.valid) {
            s.put_fixed(r->mcu.deci_c * 100, 1).put(" C");
            if (!r->mcu.calibrated) s.put(" uncal");
        } else {
            s.put("--");
        }
        break;
    case 6:   // board temperature: live data showed no exposed channel (ch1/ch6/ch7) tracks it
        s.put("BRD  n/a (no sensor)");
        break;
    case 7:
        s.put("VREF ");
        if (r) s.put_u32(r->vref_raw).put(" ").put_fixed(r->vdd_mv, 2).put(" V"); else s.put("--");
        break;
    default:
        s.put("FC   ").put(fastcharge::name(p));
    }
}

void cable_row_text(int i, bool online, int32_t vbus_mv, int32_t ibus_ma, const cable_r::Meter &m, StrBuf<24> &s) {
    const auto est = m.estimate();
    switch (i) {
    case 0:
        if (online) s.put_fixed(vbus_mv, 2).put("V ").put_abs_fixed(ibus_ma, 3).put("A");
        else s.put("-- no power monitor");
        break;
    case 1:
        if (m.has_idle()) s.put("IDLE ").put_fixed(m.idle_mv(), 2).put("V ").put_abs_fixed(m.idle_ma(), 2).put("A");
        else s.put("IDLE waiting <0.2A");
        break;
    case 2:
        if (est) s.put("LOAD ").put_fixed(est->load_mv, 2).put("V ").put_abs_fixed(est->load_ma, 2).put("A");
        else s.put(m.has_idle() ? "LOAD need +1.0A step" : "LOAD --");
        break;
    case 3: {
        char t[16];
        if (est) cable_r::fmt_mohm(std::span<char>(t), est->mohm);
        else { t[0] = '-'; t[1] = '-'; t[2] = 0; }
        s.put("R ").put(t);
        break;
    }
    default:
        s.put(cable_r::grade_name(cable_r::grade(est)));
        break;
    }
}

void cable_kelvin_row_text(int i, bool online, int32_t vbus_mv, int32_t ibus_ma,
                           const std::optional<cable_r::Kelvin> &k, StrBuf<24> &s) {
    auto mohm = [](int32_t v, StrBuf<24> &o) {
        char t[16];
        cable_r::fmt_mohm(std::span<char>(t), static_cast<uint32_t>(v < 0 ? 0 : v));
        o.put(t);
    };
    switch (i) {
    case 0:
        if (online) s.put_fixed(vbus_mv, 2).put("V ").put_abs_fixed(ibus_ma, 3).put("A");
        else s.put("-- no power monitor");
        break;
    case 1:
        if (k) { s.put("POS "); mohm(k->pos_mohm, s); s.put(" GND "); mohm(k->gnd_mohm, s); }
        else s.put("POS -- GND --");
        break;
    case 2: {
        const int32_t a = power::abs_sat(ibus_ma);
        if (k) s.put("SENSE ok");
        else s.put(a < cable_r::kKelvinMinMa ? "LOAD need 0.45A+" : "SENSE check fixture");
        break;
    }
    case 3:
        s.put("R ");
        if (k) mohm(k->total_mohm, s); else s.put("--");
        break;
    default:
        s.put(cable_r::grade_name(cable_r::grade(k)));
        break;
    }
}

void temp_short_text(const std::optional<adc::Reading> &r, StrBuf<8> &s) {
    if (!r || !r->mcu.valid) {
        s.put("MCU --");
        return;
    }
    const int32_t d = r->mcu.deci_c;
    const int32_t whole = d >= 0 ? (d + 5) / 10 : -((-d + 5) / 10);   // round half away from zero
    if (whole > 99) s.put("MCU>99");        // keep the field at 6 characters
    else if (whole < -9) s.put("MCU<-9");
    else s.put("MCU").put_fixed(whole * 1000, 0).put_char('C');
}

void model_row_text(power::Chip chip, StrBuf<24> &s) {
    if (chip == power::Chip::None) {
        s.put("--");
        return;
    }
    char m[12];
    s.put(power::format_model(m, chip, kModelLabelVersion));
}

// ---------------------------------------------------------------------------------------
// Shared page helpers
// ---------------------------------------------------------------------------------------

// Short bar names (8 cells at most) of the rotation pages, in Page order (a Bench place names itself: bar_icon_ + title_).
static constexpr const char *kBarNames[] = {"Home", "Numbers", "Charger", "Lines", "Bench", "Settings"};
static_assert(sizeof kBarNames / sizeof *kBarNames == static_cast<size_t>(Page::Count), "one bar name per rotation page");

void PageBase::title_bar() {
    // Slim chrome bar: page name left plus the role badge, 1 px hairline below (kBarH-1). On a rotation
    // page (Home included) the badge names a held Rd pin ("Rd pin CC2", e.g. a leftover pd-reread --pin)
    // instead of the bare boot-role SNK, so a pin still set is visible on the status line; no pin is the
    // passive inline role (PAS). Menus keep the short SNK/PAS: their long titles need the badge width.
    // The band repaint erases the right-side readout; the next title_badge call repaints it.
    bar_valid_ = false;
    const bool menu = is_menu();
    const char *name = menu || bar_icon_ ? title_ : kBarNames[static_cast<int>(id_)];
    char badge[11];
    std::string_view role = menu ? std::string_view(fusb302::self_powered() ? "SNK" : "PAS")
                                 : role_badge(fusb302::pin_rd(), badge);
    if (role == "PAS") role = {};   // Fieldbook: the masthead carries a role only as an exception (a held Rd pin, a sink)
    // The readout area is the page's update() job (title_badge, always called first thing and always repainting after this).
    draw_status_left(ctx_.content, (menu || meter_face() || bar_icon_) && has_icon_, icon_, name, 132, role, true);
}

void PageBase::title_pager(int current) {
    const uint32_t sig = 0x9A6E0000u | static_cast<uint32_t>(current);
    if (bar_valid_ && sig == bar_sig_) return;
    bar_valid_ = true;
    bar_sig_ = sig;
    draw_top_pager(ctx_.content, 3, current);
}

// Right end of the top chrome bar: the compact fmt3(V) x fmt3(A) readout, swapped for the stale-grey
// LAST pill while the page shows previous data. Home keeps the pill when stale (design 6.6) but
// never the live readout: its hero carries the values.
namespace {
// Measured direction as the physical arrow: positive current = bottom port to top port (Up); Idle = deadband.
Arrow arrow_of(const power::Metrics &pm) {
    return pm.flow == power::Flow::Forward ? Arrow::Up : pm.flow == power::Flow::Reverse ? Arrow::Down : Arrow::Idle;
}
}  // namespace

void PageBase::title_badge(bool last) {
    const bool frozen = last && ctx_.live.seen();
    const bool home = id_ == Page::Home;
    const power::Metrics &pm = frozen ? ctx_.last.pm : ctx_.pm;
    const StatusReadout mode = home ? (frozen ? StatusReadout::Stale : StatusReadout::None)
                                    : last ? StatusReadout::Stale
                                           : ctx_.pm_online ? StatusReadout::Live : StatusReadout::None;
    char vb[8], ab[8];
    const std::string_view vs{vb, mode == StatusReadout::Live ? readout::fmt3(std::span(vb), pm.vbus_mv, 'V') : 0};
    const std::string_view as{ab, mode == StatusReadout::Live ? readout::fmt3(std::span(ab), pm.current_ma, 'A') : 0};
    // The chevron slot of the live readout: a direction change or a rotation repaints it, nothing else does.
    const Arrow flow = mode == StatusReadout::Live ? arrow_of(pm) : Arrow::Idle;
    uint32_t sig = 2166136261u ^ static_cast<uint32_t>(mode);
    for (char ch : vs) sig = (sig ^ static_cast<uint8_t>(ch)) * 16777619u;
    for (char ch : as) sig = (sig ^ static_cast<uint8_t>(ch)) * 16777619u;
    sig = (sig ^ static_cast<uint32_t>(flow)) * 16777619u;
    sig = (sig ^ static_cast<uint32_t>(ctx_.rotation & 3u)) * 16777619u;
    if (bar_valid_ && sig == bar_sig_) return;
    bar_valid_ = true;
    bar_sig_ = sig;
    draw_top_readout(ctx_.content, mode, vs, as, flow, ctx_.rotation);
}

bool PageBase::back_event(Event e) {
    if (is_menu() || e.press != Press::Short) return false;
    Ui &ui = *ctx_.ui;
    if (any_face()) {
        if (e.button == Button::Plus || e.button == Button::Minus) {   // Dn = the next face: Flow, Numbers, Lines, Flow...
            ui.flip_face(e.button == Button::Plus ? 1 : -1, ctx_.now);
            return true;
        }
        if (e.button == Button::Ok) {
            ui.open_place(bench::Place::Meter, ctx_.now);   // the Bench's own list
            return true;
        }
        if (e.button != Button::Back || id_ == Page::Home) return false;
        ui.to_meter(ctx_.now);
        return true;
    }
    if (e.button != Button::Back) return false;
    ui.open_place(id_ == Page::Settings ? bench::Place::K2 : id_ == Page::ChargerTest ? bench::Place::Charger : bench::Place::Meter,
                  ctx_.now);
    return true;
}

bool PageBase::scroll_event(ListView &lv, Event e, uint32_t now_ms) {
    if (e.press != Press::Short) return false;
    if (e.button != Button::Plus && e.button != Button::Minus) return false;
    if (!lv.state().overflows()) return false;
    lv.scroll(e.button == Button::Plus ? 1 : -1, now_ms);
    return true;
}

PanelLabels PageBase::base_labels() const {
    if (is_menu()) {   // modal: short [+] / [-] scroll or step, BACK leaves
        PanelLabels p;
        p.b[static_cast<int>(Button::Back)] = {"Back", "", "", false};
        p.b[static_cast<int>(Button::Minus)] = {kUp, "", "", false};
        p.b[static_cast<int>(Button::Plus)] = {kDn, "", "", false};
        return p;
    }
    PanelLabels p = nav_labels();
    if (any_face()) p.b[static_cast<int>(Button::Ok)] = {"Bench", "", "", false};
    return p;
}

// ---------------------------------------------------------------------------------------
// Home v2: chips bar, three badge + D48 + gauge rows, the flow rail, lines and totals cards, footer.
// Pixel positions come from docs/ui_mockups/screens.md (home_live).
// ---------------------------------------------------------------------------------------

void fc_pill_text(fastcharge::Protocol p, StrBuf<6> &out) {
    using P = fastcharge::Protocol;
    switch (p) {
    case P::NoInput: case P::NoQuick: return;
    case P::Pending: out.put("AI.."); return;
    case P::Svooc: out.put("VOOC"); return;
    case P::PdLikely: out.put("PD?"); return;
    default: break;
    }
    const char *n = fastcharge::short_name(p);
    int len = 0;
    while (n[len] != '\0' && n[len] != ' ' && len < 4) len++;
    if (len == 4 && n[3] == '.') len = 3;                           // "QC3.0" -> "QC3"
    if (len >= 3 && n[len - 1] == '.') len--;
    out.put(std::string_view(n, static_cast<size_t>(len)));
}

void home_contract_text(const pd::Contract &c, fastcharge::Protocol p, int32_t vbus_mv, StrBuf<13> &out) {
    if (vbus_mv < 1000) return;
    if (c.valid) {
        out.put("PD ");
        if (c.mv) { put_volts(out, c.mv); out.put_char('V'); } else out.put("?V");
        if (!c.ready) { out.put(" .."); return; }
        out.put_char(' ');
        if (c.op_ma % 1000 == 0) out.put_u32(c.op_ma / 1000); else out.put_fixed(static_cast<int32_t>(c.op_ma), 1, 0);
        out.put_char('A');
        return;
    }
    if (p == fastcharge::Protocol::Pending) return;
    StrBuf<6> name;
    fc_pill_text(p, name);
    if (name.view().empty()) return;
    out.put(name.view()).put_char(' ').put_u32(static_cast<uint32_t>((vbus_mv + 500) / 1000)).put_char('V');
}


void home_contract_short(const pd::Contract &c, fastcharge::Protocol p, int32_t vbus_mv, StrBuf<13> &out) {
    StrBuf<13> t;
    home_contract_text(c, p, vbus_mv, t);
    std::string_view v = t.view();
    if (v.starts_with("PD ")) v.remove_prefix(3);
    out.put(v);
}

void home_cable_text(const pd::Emarker &em, StrBuf<12> &out) {
    if (!em.valid) { out.put("no e-marker"); return; }
    out.put_u32(em.max_current_a).put("A ").put_u32(em.max_power_w).put_char('W');
}

void detail_cable_text(const pd::Emarker &em, StrBuf<28> &out) {
    if (!em.valid) { out.put("no e-marker"); return; }
    out.put_u32(em.max_current_a).put("A ").put_u32(em.max_power_w).put("W ").put_u32(em.max_voltage_v).put_char('V');
    if (em.max_voltage_v >= 48) out.put(" EPR");
    if (em.is_active) out.put(" active");
}

void detail_offer_text(const pd::SourceCaps &caps, StrBuf<28> &out) {
    if (!caps.valid || caps.num_pdos == 0) return;
    uint32_t mw = 0;
    for (int i = 0; i < caps.num_pdos; i++) mw = caps.pdos[i].max_mw > mw ? caps.pdos[i].max_mw : mw;
    out.put_u32(mw / 1000).put("W max, ").put_u32(static_cast<uint32_t>(caps.num_pdos)).put(" PDO");
}


void home_cc_text(int cc, int32_t mv, StrBuf<10> &out) {
    out.put("CC").put_char(cc == 1 ? '1' : cc == 2 ? '2' : '-').put_char(' ');
    if (mv < 0) out.put("-.--"); else out.put_fixed(mv, 2, 0);
    out.put_char('V');
}

const char *home_idle_note(uint8_t mode) { return mode == 1 ? "No source" : mode == 0 ? "" : nullptr; }

// ---------------------------------------------------------------------------------------
// PD source capabilities
// ---------------------------------------------------------------------------------------

void contract_text(const pd::Contract &c, StrBuf<24> &out) {
    if (!c.valid) return;
    StrBuf<24> t;
    t.put("PDO").put_u32(c.pos).put_char(' ');
    if (c.mv) { put_volts(t, c.mv); t.put_char('V'); } else t.put("?V");
    t.put_char(' ');
    if (c.op_ma % 1000 == 0) t.put_u32(c.op_ma / 1000); else t.put_fixed(c.op_ma, 3 - 2, 0);
    t.put_char('A');
    if (!c.ready) t.put(" ..");
    if (t.view().size() + 9 <= kContentChars) out.put("Contract ");
    out.put(t.view());
}


// ---------------------------------------------------------------------------------------
// E-marker
// ---------------------------------------------------------------------------------------

// Live setup for the hints (see setup_hint): the link and the power monitor, nothing else.
SetupState setup_state(const UiContext &c) {
    const pd::Link &link = c.ui->link();
    const pd::SourceCaps &caps = pd::active_caps();
    SetupState st;
    st.attached = link.attached();
    st.inline_mode = link.inline_mode();
    st.caps_valid = caps.valid && caps.num_pdos > 0;
    st.vbus_known = c.pm_online;
    st.vbus_mv = c.pm.vbus_mv;
    return st;
}


// ---- Saved cables: the list (kit RowList), one cable's table (kit KvTable) and the delete question (kit AlertBand) ----------
namespace {
// Detail table: S12 keys at x28, S16 values at x104 (14 chars, to x216). Row 0 is the name, which spans the width under the
// e-marker icon instead of a key.
constexpr kit::KvLayout kEmKv{kit::kBodyY, kit::kRowH, kit::kTextX, &kLabelFont, nullptr, 104, 14, Align::Left};
constexpr kit::KvLayout kEmNameKv{kit::kBodyY, kit::kRowH, kit::kTextX, &kLabelFont, nullptr, kit::kTextX, 24, Align::Left};
constexpr const char *kEmKey[kEmDetailRows] = {"", "VID/PID", "XID", "PD", "ID VDO", "Cable VDO", "Rating"};
static_assert(all_fit(kEmKey, kLabelAdv, 104 - kit::kTextX - 4), "a saved-cable key is wider than its column");
static_assert(kit::kBodyY + kEmDetailRows * kit::kRowH < kFooterY, "the saved-cable table reaches the footer");
constexpr int16_t kEmConfirmY = kit::kBodyY + kit::kAlertH + 10;   // first line under the delete band
}  // namespace

void EmarkerPage::draw_static() {
    title_bar();
    view_sig_ = ~0u;   // a repaint wipes and redraws the body of the current view
    body_.invalidate();
    foot_.invalidate();
    ctx_.rows.init(ctx_.content, kit::kBodyY, row_fn, this);
    ctx_.rows.invalidate();
}

// The saved cable's name for list row `i`, from the slot cache (a window is at most 7 rows, so i % 8 never collides).
std::string_view EmarkerPage::name_of(int i) {
    const int slot = i & (kNameSlots - 1);
    if (name_idx_[slot] != i) {
        name_idx_[slot] = static_cast<int16_t>(i);
        names_[slot][0] = 0;
        cur_seq_ = 0;   // cur_ is the read buffer: the detail reloads it
        if (i >= 0 && i < n_seqs_ && storage::lib_get(seqs_[i], cur_)) {
            for (std::size_t k = 0; k < sizeof names_[slot]; k++) names_[slot][k] = cur_.name[k];
            names_[slot][sizeof names_[slot] - 1] = 0;
        }
    }
    return names_[slot].data();
}

void EmarkerPage::row_fn(void *ctx, int i, bool, kit::RowSpec &out) {
    auto *self = static_cast<EmarkerPage *>(ctx);
    out.marker = kit::Marker::Icon;
    out.icon = Icon::Emark;
    if (self->n_seqs_ == 0) {   // the list is one dim row, not a selectable cable
        out.text = "No saved cables";
        out.text_color = kDim;
        return;
    }
    out.text = self->name_of(i);
    out.data = true;
    out.chevron = true;
}

void EmarkerPage::update(uint32_t now_ms) {
    title_badge(false);
    if (note_until_ && static_cast<int32_t>(now_ms - note_until_) >= 0) { note_until_ = 0; note_ = StrBuf<24>(); }
    const Surface &c = ctx_.content;
    const int n = n_seqs_;
    const int idx = menu_.idx;
    const uint32_t vsig = static_cast<uint32_t>(menu_.view);
    if (vsig != view_sig_) {   // a view change wipes the body once and forgets every cached piece
        view_sig_ = vsig;
        gfx_fill(c, 0, kit::kBodyY, c.w, static_cast<int16_t>(kFooterY - kit::kBodyY), kBg);
        body_.invalidate();
        ctx_.rows.invalidate();
    }
    if (menu_.view != EmView::List) load_cur();
    if (menu_.view == EmView::List) {
        ctx_.rows.set_count(n > 0 ? n : 1);
        ctx_.rows.select(n > 0 ? idx : 0);
        ctx_.rows.draw(now_ms);
    } else {
        const bool confirm = menu_.view == EmView::Confirm;
        uint32_t sg = mix_u32(mix_init(), vsig) + static_cast<uint32_t>(have_cur_) * 3u;
        if (have_cur_) sg = mix(mix_u32(sg, cur_.seq), cur_.name.data());
        body_.paint(sg, [&] {
            if (!have_cur_) return;
            if (confirm) {
                kit::draw_alert_band(c, kit::kBodyY, false, "DELETE CABLE?");
                char buf[kFitBuf];
                gfx_text(c, kSmall, kit::kTextX, kEmConfirmY, fit_data(kSmall, cur_.name.data(), 192, buf), kText, kBg);
                gfx_text(c, kSmall, kit::kTextX, static_cast<int16_t>(kEmConfirmY + 22), "Erased for good", kDim, kBg);
                return;
            }
            for (int i = 0; i < kEmDetailRows; i++) {
                StrBuf<24> v;
                em_detail_value(i, cur_, v);
                kit::kv_row(c, i == 0 ? kEmNameKv : kEmKv, kit::kv_row_y(kEmKv, i), kEmKey[i], v.view(), kText);
            }
            gfx_fill(c, 0, kit::kv_row_y(kEmKv, kEmDetailRows), c.w, 1, kRule);
            draw_icon(c, kit::kMarkX, static_cast<int16_t>(kit::kBodyY + 7), Icon::Emark, kDim, kBg);
        });
    }
    StrBuf<27> st;
    FooterTone tone = FooterTone::Info;   // counts and positions are neutral facts
    if (note_until_) {
        st.put(note_.view());
        tone = note_caution_ ? FooterTone::Caution : FooterTone::Info;
    } else if (n == 0) {
        st.put("Nothing saved yet");
    } else {
        st.put("Cable ").put_u32(static_cast<uint32_t>(idx + 1)).put(" of ").put_u32(static_cast<uint32_t>(n));
        if (menu_.view == EmView::Confirm) { st.put(". Not deleted"); tone = FooterTone::Caution; }
    }
    draw_footer_cached(c, tone, st.view(), foot_);
}

// The shown cable's entry, re-read when the selection or the library changed.
void EmarkerPage::load_cur() {
    const std::uint16_t seq = menu_.idx >= 0 && menu_.idx < n_seqs_ ? seqs_[menu_.idx] : 0;
    if (seq == cur_seq_ && (seq == 0 ? !have_cur_ : have_cur_)) return;
    cur_seq_ = seq;
    have_cur_ = seq != 0 && storage::lib_get(seq, cur_);
}

// The shown saved cable (with its quirk table) as a share record, then the share view. The record goes straight into the
// view's buffer. The quirk table (about 420 bytes) is a static, not a local: the call-depth gate counts this frame on the
// deepest main-loop path (tools/stack_depth.py), and the entry is the page's own cur_.
void EmarkerPage::share_cur() {
    if (menu_.idx < 0 || menu_.idx >= n_seqs_) return;
    static emk_quirk::Quirks quirks;
    if (!storage::lib_get(seqs_[menu_.idx], cur_, quirks)) return;
    cur_seq_ = seqs_[menu_.idx];
    have_cur_ = true;
    const std::size_t len = qrshare::records::emarker(cur_.rec, cur_.name.data(), quirks, ctx_.ui->share_buffer());
    ctx_.ui->open_share(qrshare::Type::Emarker, qrshare::kSchemaEmarker, cur_.name.data(), len, ctx_.now);
}

void EmarkerPage::refresh_lib() {
    n_seqs_ = static_cast<int>(storage::lib_list(std::span<std::uint16_t>(seqs_)));
    menu_.set_count(n_seqs_);
    cur_seq_ = 0;
    for (int16_t &i : name_idx_) i = -1;
}

// What the Detail table shows on row `i` (0 = the name): the saved cable's own record, decoded.
void em_detail_value(int index, const emlib::Entry &ent, StrBuf<24> &out) {
    const emrec::Record &rec = ent.rec;
    const uint32_t id = emrec::vdo(rec, 1), cert = emrec::vdo(rec, 2), prod = emrec::vdo(rec, 3), cab = emrec::vdo(rec, 4);
    switch (index) {
    case 0: out.put(ent.name.data()); break;
    case 1: out.put_hex(id & 0xFFFF, 4).put_char(' ').put_hex(prod >> 16, 4); break;
    case 2: out.put_hex(cert, 8); break;
    case 3: out.put(rec[0x14] == 1 ? "2.0" : "3.x"); break;
    case 4: out.put_hex(id, 8); break;
    case 5: out.put_hex(cab, 8); break;
    default: {   // cable VDO 1: bits 6:5 current (1 = 3 A, 2 = 5 A), bits 10:9 max VBUS (20/30/40/50 V)
        const uint32_t cur = (cab >> 5) & 3, volt = (cab >> 9) & 3;
        out.put(cur == 2 ? "5A " : cur == 1 ? "3A " : "?A ").put_u32(20 + 10 * volt).put("V");
        break;
    }
    }
}

void EmarkerPage::run_cmd(EmCmd cmd) {
    if (cmd != EmCmd::Delete) return;
    const int i = menu_.idx;
    if (i >= 0 && i < n_seqs_) {
        const bool ok = storage::lib_remove(seqs_[i]) == emlib::Err::Ok;
        note_ = StrBuf<24>();
        note_.put(ok ? "Deleted" : "Flash error");
        note_caution_ = !ok;
        note_until_ = ctx_.now + 2500;
        if (note_until_ == 0) note_until_ = 1;
    }
    refresh_lib();
}

// Pushed from Connection's "Saved cables": the list view, a fresh library read, the first cable selected.
void EmarkerPage::enter() {
    menu_ = EmMenu();
    note_ = StrBuf<24>();
    note_until_ = 0;
    refresh_lib();
}

bool EmarkerPage::on_event(Event e) {
    // BACK in the list leaves the page (the Ui pops it); deeper views step out one level at a time.
    if (menu_.view == EmView::Detail && e.button == Button::Ok && e.press == Press::Long) {   // Share: the saved cable as QR
        share_cur();
        return true;
    }
    EmCmd cmd;
    if (menu_.on_event(e, cmd)) {
        run_cmd(cmd);
        return true;
    }
    return false;
}

PanelLabels EmarkerPage::labels() const {
    PanelLabels l = base_labels();
    SlotLabel &ok = l.b[static_cast<int>(Button::Ok)];
    switch (menu_.view) {
    case EmView::List:
        l = list_labels(l);
        ok = {n_seqs_ > 0 ? "View" : "", "", "", false};
        break;
    case EmView::Detail:
        l = list_labels(l);
        ok = {"Delete", "Share", "", false};
        break;
    case EmView::Confirm:
        ok = {"Delete", "", "", false};
        l.b[static_cast<int>(Button::Minus)] = {};
        l.b[static_cast<int>(Button::Plus)] = {};
        break;
    }
    return l;
}

// BACK or the remote's set-page: the next visit starts on the list again.
void EmarkerPage::leave() {
    menu_ = EmMenu();
    note_ = StrBuf<24>();
    note_until_ = 0;
}

void EmarkerPage::invalidate() {
    ctx_.rows.invalidate();
    body_.invalidate();
    foot_.invalidate();
    view_sig_ = ~0u;
}

// ---------------------------------------------------------------------------------------
// Diagnostics
// ---------------------------------------------------------------------------------------

namespace {
// Diag: a kit KvTable of 9 rows at a 22 px pitch (the nine rows need 198 px, 26 px rows would not fit above the footer),
// S12 keys at x28, S16 values right-aligned to x218 in one 17-char box (the widest, "RX"), and a verdict marker at x8 on
// the first row of each group: USB (0..2), PD (3..5), SYS (6..8).
constexpr kit::KvLayout kDgLayout{kit::kBodyY, 22, kit::kTextX, &kLabelFont, nullptr, 218 - 17 * 8, 17, Align::Right};
// Worst case per row ("4294M / 4294M" is 13 chars, RX 17): each cell is only as wide as its text, clear of the S12 keys.
constexpr uint8_t kDgValueChars[DiagPage::kRows] = {7, 13, 13, 7, 17, 13, 7, 11, 11};
const char *const kDgLabel[DiagPage::kRows] = {"Link", "RST/ISR", "TX/drop", "FUSB302", "RX", "Poll gap", "Meter", "Uptime", "Model"};
}  // namespace

void DiagPage::draw_static() {
    title_bar();
    const Surface &c = ctx_.content;
    if (!ctx_.blank) kit::kv_ground(c, kDgLayout, kDgLabel, kRows, kDgValueChars, 1u << 0 | 1u << 3 | 1u << 6);   // the marker rows
    kit::kv_static(c, kDgLayout, kDgLabel, kRows);
    kit::kv_cells(c, kDgLayout, val_, kRows, kDgValueChars);
    marks_ = 0xFF;
    foot_.invalidate();
}

void DiagPage::paint_marks(bool usb_ok, bool fusb_ok, bool meter_ok, bool meter_wait) {
    const uint8_t sig = static_cast<uint8_t>(usb_ok | (fusb_ok << 1) | (meter_ok << 2) | (meter_wait << 3));
    if (sig == marks_) return;
    marks_ = sig;
    const bool ok[3] = {usb_ok, fusb_ok, meter_ok};
    for (int g = 0; g < 3; g++) {
        // USB not configured is "waiting" (dim dash), and so is a meter in its first seconds; a missing FUSB302 or a meter still
        // missing after the grace is a fault (red cross).
        const kit::Verdict v = ok[g] ? kit::Verdict::Pass : (g == 0 || (g == 2 && meter_wait)) ? kit::Verdict::None : kit::Verdict::Fail;
        const int16_t y = static_cast<int16_t>(kit::kv_row_y(kDgLayout, g * 3) + 1 + (kDgLayout.pitch - 1 - kit::kMarkSize) / 2);
        kit::draw_verdict(ctx_.content, kit::kMarkX, y, kit::kMarkSize, kit::kMarkSize, v, {}, kBg);
    }
}

void DiagPage::update(uint32_t now_ms) {
    title_badge(false);
    const bool on = ctx_.pm_online;
    const bool meter_wait = home::meter_starting(on, now_ms);   // still being found: not yet an error
    const uint8_t fusb_id = fusb302::device_id();
    StrBuf<24> t[kRows];
    t[0].put(live_.configured ? "ready" : "waiting");
    // Capped counts (10K, 999M, ">999M") are at most 5 chars, so "a / b" is at most 13 and always fits.
    t[1].put_count_capped(live_.resets).put(" / ").put_count_capped(live_.isrs);
    t[2].put_count_capped(live_.log_sent).put(" / ").put_count_capped(live_.log_dropped);
    t[3].put("ID 0x").put_hex(fusb_id, 2);
    t[4].put_u32(live_.rx_count).put(live_.cc == 1 ? " on CC1" : live_.cc == 2 ? " on CC2" : " on CC-");
    t[5].put_u32(live_.poll_gap_max).put(" ms");
    if (on) t[6].put(power::name(power::chip())); else t[6].put(meter_wait ? "wait" : "missing");
    t[7].put_hms(now_ms / 1000);
    model_row_text(power::chip(), t[8]);
    for (int i = 0; i < kRows; i++) {
        uint16_t col = kText;
        // Amber means "log lines were dropped"; a saturated count is a garbage cue, so it stays neutral.
        if (i == 2 && live_.log_dropped > 0 && !count_overflowed(live_.log_dropped) &&
            !count_overflowed(live_.log_sent))
            col = kCaution;
        if (i == 6 && !on && !meter_wait) col = kError;
        val_[i].set_colors(col, kBg);
        val_[i].draw(t[i].view());
    }
    paint_marks(live_.configured, fusb_id != 0, on, meter_wait);
    const bool usb_ok = live_.configured, fusb_ok = fusb_id != 0;
    const int state = !on ? (meter_wait ? 4 : 1) : !usb_ok ? 2 : !fusb_ok ? 3 : 0;
    static const char *const msg[5] = {"All subsystems up", "Power meter not found", "USB not configured", "FUSB302 not answering",
                                       "Power meter starting"};
    draw_footer_cached(ctx_.content, state == 0 ? FooterTone::Ok : state == 4 ? FooterTone::Info : state == 2 ? FooterTone::Caution : FooterTone::Error,
                       msg[state], foot_);
}

bool DiagPage::on_event(Event e) {
    if (e.button == Button::Ok && e.press == Press::Long) {   // Share: the newest log entries as QR (k2/core/binlog.hpp)
        std::span<std::uint8_t> buf = ctx_.ui->share_buffer();
        if (buf.size() <= qrshare::records::kLogHeadMax) return false;
        std::uint32_t base = 0;
        bool older = false;
        const std::size_t n = usb::log_bin(buf.subspan(qrshare::records::kLogHeadMax), base, older);
        const std::size_t len = qrshare::records::log(kGitHash, systick::millis(), base, older, n, buf);
        ctx_.ui->open_share(qrshare::Type::Log, qrshare::kSchemaLog, "Log", len, ctx_.now);
        return true;
    }
    return back_event(e);
}

PanelLabels DiagPage::labels() const {
    PanelLabels l = base_labels();
    l.b[static_cast<int>(Button::Ok)] = {"", "Share", "", false};
    return l;
}

void DiagPage::sample() {
    const usb::StatsSnapshot u = usb::stats_snapshot();
    live_.resets = u.resets; live_.isrs = u.isrs;
    live_.log_sent = u.log_sent; live_.log_dropped = u.log_dropped;
    live_.poll_gap_max = u.poll_gap_max;
    live_.configured = usb::configured();
    live_.rx_count = fusb302::rx_count();
    live_.cc = fusb302::active_cc();
}

void DiagPage::invalidate() {
    marks_ = 0xFF;
    foot_.invalidate();
    for (Field &f : val_) f.invalidate();
}

// ---------------------------------------------------------------------------------------
// Lines: the analog front end
// ---------------------------------------------------------------------------------------

namespace {
// Meter > Lines (Fieldbook): four 24 px line rows under the masthead (an ink chip as the tag, the value, an ink bar on a bg2
// track), dashed threshold lines at 0.4 V and 2.0 V down through the rows to their axis labels, then the verdict chips and
// the Rp receipt row. The MCU temperature lives on Numbers.
constexpr int16_t kLnRowY0 = 26, kLnRowPitch = 24;
constexpr int16_t kLnRowY(int i) { return static_cast<int16_t>(kLnRowY0 + i * kLnRowPitch); }
constexpr int16_t kLnChipX = 6, kLnChipW = 30, kLnChipDy = 5;
constexpr int16_t kLnValX = 44, kLnValChars = 5, kLnValDy = 4;
constexpr int16_t kLnBarX = 100, kLnBarH = 10, kLnBarDy = 7;
constexpr int16_t kLnAxisY = kLnRowY0 + 4 * kLnRowPitch;   // 122: the dashes stop here, their labels under it
constexpr int16_t kLnAxisTextY = kLnAxisY + 3, kKeyAxisX = 6;
constexpr int16_t kVerdictY = 144;                          // the verdict chips
constexpr int16_t kLnDash = 2;                              // the threshold lines: 2 px dash, 2 px gap
static_assert(kLnBarX + kLinesBarW <= 222, "the bars end inside the content");
constexpr const char *kLnTag[4] = {"D+", "D-", "CC1", "CC2"};
constexpr int32_t kLnFaintMv = 100;                         // a line under 0.1 V reads as nothing on it
// The Rp receipt row: key font 16 at x 6, a dot leader, "3.0A on CC1" right-aligned to x 218.
constexpr kit::KvLayout kLnKv{168, 26, 6, nullptr, nullptr, static_cast<int16_t>(218 - 12 * 8), 12, Align::Right};

int16_t tick_x(int32_t mv) { return static_cast<int16_t>(kLnBarX + lines_tick_px(mv)); }
// One threshold column between y0 and y1 (exclusive): dash pixels in ink3, the gaps in the ground, phase anchored to y 0 so the
// pieces line up across the rows and the bars.
void dash_column(const Surface &c, int16_t x, int16_t y0, int16_t y1) {
    for (int16_t y = y0; y < y1;) {
        const bool on = (y / kLnDash) % 2 == 0;
        const int16_t end = std::min<int16_t>(y1, static_cast<int16_t>((y / kLnDash + 1) * kLnDash));
        gfx_fill(c, x, y, 1, static_cast<int16_t>(end - y), on ? kInk3 : kBg);
        y = end;
    }
}

struct ChipStyle { uint16_t fill, ink; };
ChipStyle tone_chip(Tone t) {
    switch (t) {
    case Tone::Ok:
    case Tone::Info:    return {kInk, kBg};          // a decoded result: the solid ink chip
    case Tone::Caution: return {kCaution, kSpotInk};
    case Tone::Dim:     break;
    }
    return {kBg2, kInk2};
}
}  // namespace

// The ground of the page around what its cells and bars paint (the pills paint over their box: their corners are rounded, so
// the box stays ground). Run when the page opens over a live screen, so it covers every content pixel with no erase pass.
void LinesPage::lay_ground() {
    const Surface &c = ctx_.content;
    const int16_t lo = tick_x(kLinesTickLowMv), hi = tick_x(kLinesTickHighMv);
    {   // the line rows, from under the masthead: everything but the chips, the values, the bars and the threshold columns
        const int16_t y0 = kit::kBelowBar;
        for (int i = 0; i < 4; i++) {
            const int16_t y = kLnRowY(i), top = i ? y : y0, h = static_cast<int16_t>(y + kLnRowPitch - top);
            const kit::Box holes[5] = {
                {kLnChipX, static_cast<int16_t>(y + kLnChipDy), kLnChipW, kit::kChipH},
                {kLnValX, static_cast<int16_t>(y + kLnValDy), static_cast<int16_t>(kLnValChars * kSmall.adv), kSmall.height},
                {kLnBarX, static_cast<int16_t>(y + kLnBarDy), kLinesBarW, kLnBarH},
                {lo, top, 1, h}, {hi, top, 1, h}};
            kit::fill_except(c, {0, top, c.w, h}, holes, 5, kBg);
        }
    }
    {   // the axis labels, then the verdict row around its chip line (paint_verdict owns that line edge to edge)
        const kit::Box tags[3] = {
            {kKeyAxisX, kLnAxisTextY, static_cast<int16_t>(text_width(kLabelFont, "volts")), kLabelFont.height},
            {static_cast<int16_t>(lo - 9), kLnAxisTextY, static_cast<int16_t>(text_width(kLabelFont, "0.4")), kLabelFont.height},
            {static_cast<int16_t>(hi - 9), kLnAxisTextY, static_cast<int16_t>(text_width(kLabelFont, "2.0")), kLabelFont.height}};
        const int16_t chips = kVerdictY + 2;
        kit::fill_except(c, {0, kLnAxisY, c.w, static_cast<int16_t>(chips - kLnAxisY)}, tags, 3, kBg);
        gfx_fill(c, 0, static_cast<int16_t>(chips + kit::kChipH), c.w, static_cast<int16_t>(kLnKv.y0 - chips - kit::kChipH), kBg);
    }
    static const char *const kKeys[1] = {"Rp"};
    kit::kv_ground(c, kLnKv, kKeys, 1);
    const int16_t below = static_cast<int16_t>(kit::kv_row_y(kLnKv, 1) + 1);
    gfx_fill(c, 0, below, c.w, static_cast<int16_t>(kit::kListBottom - below), kBg);
}

void LinesPage::draw_static() {
    title_bar();
    const Surface &c = ctx_.content;
    if (!ctx_.blank) lay_ground();
    const int16_t lo = tick_x(kLinesTickLowMv), hi = tick_x(kLinesTickHighMv);
    for (int i = 0; i < 4; i++) {
        const int16_t y = kLnRowY(i);
        kit::draw_chip(c, kLnChipX, static_cast<int16_t>(y + kLnChipDy), kLnTag[i], kInk, kBg, kLnChipW);
        v_[i].init(c, kSmall, kLnValX, static_cast<int16_t>(y + kLnValDy), kLnValChars, Align::Right, kText, kBg);
        px_[i] = -1;
        for (int16_t x : {lo, hi}) {   // the threshold columns around the bar (the bar paints its own part)
            dash_column(c, x, i ? y : kit::kBelowBar, static_cast<int16_t>(y + kLnBarDy));
            dash_column(c, x, static_cast<int16_t>(y + kLnBarDy + kLnBarH), static_cast<int16_t>(y + kLnRowPitch));
        }
    }
    gfx_text(c, kLabelFont, kKeyAxisX, kLnAxisTextY, "volts", kFaint, kBg);
    gfx_text(c, kLabelFont, static_cast<int16_t>(lo - 9), kLnAxisTextY, "0.4", kDim, kBg);
    gfx_text(c, kLabelFont, static_cast<int16_t>(hi - 9), kLnAxisTextY, "2.0", kDim, kBg);
    static const char *const kLabels[1] = {"Rp"};
    kit::kv_static(c, kLnKv, kLabels, 1);
    kit::kv_cells(c, kLnKv, pv_, 1);
    painted_ = false;
    vsig_ = 0;
    foot_.invalidate();
}

// The bar: ink fill on the bg2 track; the threshold dashes show through the track, the fill covers them.
void LinesPage::paint_bar(int i, int32_t mv, bool have) {
    const int px = have ? lines_bar_px(mv) : 0;
    if (px == px_[i]) return;
    px_[i] = px;
    const Surface &c = ctx_.content;
    const int16_t y = static_cast<int16_t>(kLnRowY(i) + kLnBarDy);
    if (px > 0) gfx_fill(c, kLnBarX, y, static_cast<int16_t>(px), kLnBarH, kInk);
    int16_t x = static_cast<int16_t>(kLnBarX + px);
    for (int32_t t : {kLinesTickLowMv, kLinesTickHighMv}) {
        const int16_t tx = tick_x(t);
        if (tx < x) continue;
        if (tx > x) gfx_fill(c, x, y, static_cast<int16_t>(tx - x), kLnBarH, kBg2);
        for (int16_t r = 0; r < kLnBarH; r++)
            gfx_fill(c, tx, static_cast<int16_t>(y + r), 1, 1, ((y + r) / kLnDash) % 2 == 0 ? kInk3 : kBg2);
        x = static_cast<int16_t>(tx + 1);
    }
    const int16_t end = static_cast<int16_t>(kLnBarX + kLinesBarW);
    if (x < end) gfx_fill(c, x, y, static_cast<int16_t>(end - x), kLnBarH, kBg2);
}

void LinesPage::paint_verdict(const std::optional<adc::Reading> &r, uint32_t now_ms) {
    const Surface &c = ctx_.content;
    const fastcharge::Protocol fp = ctx_.fc;
    const pd::SourceCaps &caps = pd::active_caps();
    // The green PD chip follows a decoded PD message (a contract or source caps), not just the line levels.
    const bool pd_decoded = fusb302::contract().valid || caps.valid;
    LinesVerdict v = lines_verdict(fp, pd_decoded);
    if (caps.valid && caps.num_pdos > 0 && v.extra.view().empty()) {   // a live PD source: its power summary
        const ai::Banner b = ai::recognise(fp, caps, now_ms);
        if (b.source == ai::Source::Pd) v.extra.put(b.line1.view());
    }
    StrBuf<24> rp;
    rp_text(rp, r.has_value(), fusb302::active_cc(), r ? r->cc1_mv : 0, r ? r->cc2_mv : 0);
    uint32_t sig = mix(mix(2166136261u, v.main.view()), v.extra.view());
    sig = sig * 31 + static_cast<uint32_t>(v.tone) + 1;
    if (!painted_ || sig != vsig_) {   // the chips row; the Rp cell below repaints on its own char diff
        vsig_ = sig;
        const ChipStyle ts = tone_chip(v.tone);
        const int16_t y = static_cast<int16_t>(kVerdictY + 2);
        gfx_fill(c, 0, y, 6, kit::kChipH, kBg);
        int16_t x = static_cast<int16_t>(6 + kit::draw_chip(c, 6, y, v.main.view(), ts.fill, ts.ink));
        const std::string_view ex = v.extra.view();
        if (!ex.empty()) {   // the detail: a voltage takes the volt spot, anything else the quiet bg2 chip
            const bool volts = ex.back() == 'V';
            gfx_fill(c, x, y, 6, kit::kChipH, kBg);
            x = static_cast<int16_t>(x + 6 + kit::draw_chip(c, static_cast<int16_t>(x + 6), y, ex, volts ? kSpotVolt : kBg2,
                                                            volts ? kSpotInk : kInk));
        }
        if (x < c.w) gfx_fill(c, x, y, static_cast<int16_t>(c.w - x), kit::kChipH, kBg);   // a shorter row must not leave the old tail
        painted_ = true;
    }
    pv_[0].set_colors(r ? kText : kFaint, kBg);
    pv_[0].draw(rp.view());
    draw_footer_cached(c, v.tone == Tone::Ok ? FooterTone::Ok : v.tone == Tone::Caution ? FooterTone::Caution : FooterTone::Info,
                       v.footer.view(), foot_);   // Info and Dim are neutral
}

void LinesPage::update(uint32_t now_ms) {
    title_pager(2);
    const std::optional<adc::Reading> &r = ctx_.analog;
    const int32_t mv[4] = {r ? r->dp_mv : 0, r ? r->dm_mv : 0, r ? r->cc1_mv : 0, r ? r->cc2_mv : 0};
    for (int i = 0; i < 4; i++) {
        StrBuf<8> s;
        if (r) s.put_fixed(mv[i] < 0 ? 0 : mv[i], 2).put_char('V'); else s.put("--");
        v_[i].set_colors(r && mv[i] >= kLnFaintMv ? kText : kFaint, kBg);   // a line with nothing on it reads faint
        v_[i].draw(s.view());
        paint_bar(i, mv[i], r.has_value());
    }
    paint_verdict(r, now_ms);
}

bool LinesPage::on_event(Event e) {
    return back_event(e);
}

PanelLabels LinesPage::labels() const {
    return base_labels();
}

void LinesPage::invalidate() {
    painted_ = false;
    vsig_ = 0;
    foot_.invalidate();
    for (int &p : px_) p = -1;
    for (Field &f : v_) f.invalidate();
    for (Field &f : pv_) f.invalidate();
}

// ---------------------------------------------------------------------------------------
// PD trigger submenu: a step wizard (OFFER, VOLTAGE, CURRENT, CONFIRM, then the LIVE card). The
// step logic is k2::Wizard (ui_wizard.hpp, host-tested); this part only draws it.
// ---------------------------------------------------------------------------------------

namespace {
// The key/value tables of the wizard: 20 px rows, S12 keys, S16 values (the same table as the FC trigger).
constexpr kit::KvLayout kPtKv{kit::kBodyY, 20, 6, &kLabelFont, nullptr, 72, 18, Align::Left};
constexpr kit::KvLayout kPtLiveKv{kit::kBodyY + kit::kAlertH + 8, 20, 6, &kLabelFont, nullptr, 84, 16, Align::Left};
constexpr int16_t kPtBigY = 112, kPtRangeY = 162, kPtRangeX = 10, kPtRangeW = 204;   // Voltage / Current: big value, range bar
constexpr int16_t kPtSendY = 120, kPtSendH = 72;                                     // Confirm: the SEND cell
constexpr uint8_t kNoStep = 0xFF;
uint32_t sigu(uint32_t h, uint32_t v) { return (h ^ v) * 16777619u; }
template <size_t N>
void put_exact_unit(StrBuf<N> &out, uint32_t milli, char unit) {
    if (milli % 1000 == 0) out.put_u32(milli / 1000);
    else out.put_fixed(static_cast<int32_t>(milli), milli % 100 == 0 ? 1 : milli % 10 == 0 ? 2 : 3);
    out.put_char(unit);
}
int16_t pt_text_y(const kit::KvLayout &l, int row) {
    return static_cast<int16_t>(kit::kv_row_y(l, row) + kit::row_text_y(l.pitch, 16));
}
}  // namespace

void PdTriggerPage::draw_static() {
    reset_paint();
    if (!ctx_.blank) gfx_fill(ctx_.content, 0, kit::kBelowBar, ctx_.content.w, int16_t(kFooterY - kit::kBelowBar), kBg);
    paint_bar();
}

void PdTriggerPage::reset_paint() {
    rsig_ = bsig_ = fsig_ = barsig_ = tsig_ = vsig_ = 0;
    body_step_ = kNoStep;
    send_.invalidate();
}

// The wizard bar: "n/3 PD <step>" with the progress rule (the voltage and current steps share segment 2, a fixed offer
// has no voltage step). LIVE and the refusal view carry the plain name. The V x A readout behind the rule is title_badge's.
void PdTriggerPage::paint_bar() {
    const bool refused = refusal_ != Refusal::None;
    const WizStep st = wiz_.step;
    const uint32_t sg = 1u + (refused ? 8u : static_cast<uint32_t>(st));
    if (sg == barsig_) return;
    barsig_ = sg;
    bar_valid_ = false;   // the band repaint erased the readout: title_badge redraws it
    const char *title = "PD trigger";
    int step = 0;
    if (!refused) {
        switch (st) {
        case WizStep::Offer: title = "PD offer"; step = 1; break;
        case WizStep::Voltage: title = "PD voltage"; step = 2; break;
        case WizStep::Current: title = "PD current"; step = 2; break;
        case WizStep::Confirm: title = "PD confirm"; step = 3; break;
        case WizStep::Live: title = "PD live"; break;
        }
    }
    kit::draw_top_bar(ctx_.content, title, step, fusb302::self_powered() ? "SNK" : "PAS", true);   // (a plain bar also clears the rule's 2nd row)
}

// Opened from Tools: refuse outright inline, unplugged or without caps; else the wizard starts on the
// OFFER step with the cursor on the held contract (or PDO 1).
void PdTriggerPage::enter() {
    const pd::SourceCaps &caps = pd::active_caps();
    const pd::Link &link = ctx_.ui->link();
    ctx_.trig_armed = false;
    active_ = false;
    send_err_ = pd::Err::Ok;
    refusal_ = Refusal::None;
    reset_paint();
    // A live sink on the other side does not hide the wizard: the OFFER step shows why it is blocked.
    if (pd::is_fake() || !link.attached() || !caps.valid || caps.num_pdos == 0) refusal_ = Refusal::NoCaps;
    blocked_ = fusb302::trigger_refusal() != pd::TriggerRefusal::None || link.inline_mode();
    if (refusal_ != Refusal::None) {
        K2LOG("[UI] pd trigger refused (unplugged or no caps)\n");
        return;
    }
    if (blocked_) K2LOG("[PD] trigger refused: inline (%s)\n", link.inline_mode() ? "link inline" : pd::trigger_refusal_name(fusb302::trigger_refusal()));
    const pd::SinkStatus &st = fusb302::sink_status();
    wiz_.enter(caps, st.held ? st.pos : 0);
    active_ = true;
    K2LOG("[UI] pd trigger wizard\n");
}

// Every way out (BACK, paging, the remote's set-page) lands here: release to 5 V when we hold a request.
void PdTriggerPage::leave() {
    const bool was = active_;
    active_ = false;
    ctx_.trig_armed = false;
    refusal_ = Refusal::None;
    if (was && fusb302::sink_status().held) {
        fusb302::sink_release();
        K2LOG("[UI] pd trigger left, released to 5V\n");
    }
}

void PdTriggerPage::update(uint32_t now_ms) {
    caps_ = &pd::active_caps();
    const pd::Link &link = ctx_.ui->link();
    // The source went away or a live sink appeared while the wizard is open: nothing to trigger any more.
    blocked_ = fusb302::trigger_refusal() != pd::TriggerRefusal::None || link.inline_mode();
    if (active_ && blocked_ && wiz_.step != WizStep::Live) wiz_.step = WizStep::Offer;   // never past OFFER while blocked
    if (active_ && ((link.inline_mode() && wiz_.step == WizStep::Live) || !link.attached() || !caps_->valid || caps_->num_pdos == 0)) {
        const Refusal r = link.inline_mode() ? Refusal::Inline : Refusal::NoCaps;
        leave();
        refusal_ = r;
        reset_paint();
    }
    if (refusal_ != Refusal::None) {
        paint_bar();
        title_badge(false);
        paint_refusal();
        return;
    }
    if (wiz_.cursor >= caps_->num_pdos) wiz_.enter(*caps_, 0);   // the source re-advertised a shorter list
    // The second-OK window opened (or closed): the CONFIRM bar drains from the moment it opened.
    if (!ctx_.trig_armed) armed_at_ = 0;
    else if (!armed_at_) armed_at_ = now_ms ? now_ms : 1;
    if (rsig_ || body_step_ != static_cast<uint8_t>(wiz_.step)) {   // back from a refusal view, or a new step
        const uint32_t keep_bar = barsig_;
        reset_paint();
        barsig_ = keep_bar;
        if (!ctx_.blank) gfx_fill(ctx_.content, 0, kit::kBodyY, ctx_.content.w, int16_t(kFooterY - kit::kBodyY), kBg);   // below the progress rule
        body_step_ = static_cast<uint8_t>(wiz_.step);
    }
    paint_bar();
    title_badge(false);
    switch (wiz_.step) {
    case WizStep::Offer: paint_offer(now_ms); break;
    case WizStep::Voltage:
    case WizStep::Current: paint_value(); break;
    case WizStep::Confirm: paint_confirm(now_ms); break;
    case WizStep::Live: paint_live(); break;
    }
    paint_footer();
}

// The OFFER list: kit RowList rows, "TYPE volts amps" and the power at the right. The held contract carries the bolt
// marker; an offer the monitor cap refuses is dim (the footer says why). The list repaints only the rows that changed.
void PdTriggerPage::row_fn(void *self, int index, bool, kit::RowSpec &out) {
    auto *p = static_cast<PdTriggerPage *>(self);
    out = kit::RowSpec{};
    if (!p->caps_ || index < 0 || index >= p->caps_->num_pdos) return;
    pdo_line(p->caps_->pdos[index], p->line_);
    const pd::SinkStatus &st = fusb302::sink_status();
    const bool held = st.held && st.state == pd::SinkState::Ready && st.pos == index + 1;
    const bool avail = Wizard::offer_available(*p->caps_, static_cast<unsigned>(index), p->ctx_.cap_mv);
    if (held) {
        out.marker = kit::Marker::Icon;
        out.icon = Icon::Bolt;
        out.marker_color = kEnergy;
    }
    out.text = p->line_.text.view();
    out.tail = p->line_.tail.view();
    out.text_color = avail ? kText : kDim;
    out.tail_color = avail ? kWatt : kDim;   // the tail is the offer's power_mw
}

void PdTriggerPage::paint_offer(uint32_t now_ms) {
    if (!bsig_) {
        bsig_ = 1;
        ctx_.rows.init(ctx_.content, kit::kBodyY, row_fn, this);
        ctx_.rows.invalidate();
    }
    ctx_.rows.set_count(caps_->num_pdos);
    ctx_.rows.select(wiz_.cursor);
    ctx_.rows.draw(now_ms);
}

// VOLTAGE and CURRENT: the offer, its limit and the resulting power as a three-row table, the big value between two step
// triangles (dim at a limit), a range bar with min / max labels. The static part repaints when the step, the PDO, the monitor
// cap or the fixed voltage changes; the power, the big value, the triangles and the bar are live.
void PdTriggerPage::paint_value() {
    const Surface &c = ctx_.content;
    const bool volt = wiz_.step == WizStep::Voltage;
    const pd::Pdo &d = caps_->pdos[wiz_.cursor];
    const uint32_t cap = ctx_.cap_mv;
    const uint32_t value = volt ? wiz_.mv : wiz_.ma;
    const uint32_t lo = volt ? d.min_mv : 0;                                            // the bar shows the whole offer
    const uint32_t cl = wiz_.current_limit(*caps_, cap);
    const uint32_t hi = volt ? d.max_mv : cl;
    const uint32_t mw_v = !volt && d.type == pd::PdoType::Fixed ? d.max_mv : wiz_.mv;   // the voltage the current is drawn at
    const uint32_t mw = static_cast<uint32_t>(static_cast<uint64_t>(mw_v) * (volt ? cl : wiz_.ma) / 1000);
    const bool can_dn = [&] { Wizard t = wiz_; return t.nudge(*caps_, -1, false, cap); }();
    const bool can_up = [&] { Wizard t = wiz_; return t.nudge(*caps_, +1, false, cap); }();
    const uint16_t col = volt ? kVolt : kAmp;
    uint32_t sig = sigu(sigu(sigu(sigu(2166136261u, volt ? 1u : 2u), wiz_.cursor), cap), volt ? 0u : mw_v);
    sig = sigu(sig, hi) | 1u;
    if (sig != bsig_) {
        bsig_ = sig;
        tsig_ = 0;
        gfx_fill(c, 0, kit::kBodyY, c.w, int16_t(kFooterY - kit::kBodyY), kBg);
        static const char *const kVoltKeys[] = {"Offer", "Max amps", "Max power"};
        static const char *const kAmpKeys[] = {"Offer", "Voltage", "Power"};
        kit::kv_static(c, kPtKv, volt ? kVoltKeys : kAmpKeys, 3);
        pdo_line(d, line_);
        gfx_text(c, kSmall, kPtKv.value_x, pt_text_y(kPtKv, 0), line_.text.view(), kText, kBg);
        StrBuf<12> t;
        if (volt) put_exact_unit(t, d.max_ma, 'A');
        else put_exact_unit(t, mw_v, 'V');
        gfx_text(c, kSmall, kPtKv.value_x, pt_text_y(kPtKv, 1), t.view(), col == kVolt ? kAmp : kVolt, kBg);
        f_[0].init(c, kSmall, kPtKv.value_x, pt_text_y(kPtKv, 2), 8, Align::Left, kWatt, kBg);   // power_mw
        constexpr uint8_t kNumChars = 5;
        const int16_t x0 = static_cast<int16_t>((c.w - (kNumChars * kBig.adv + 2 + kMid.adv)) / 2);
        f_[1].init(c, kBig, x0, kPtBigY, kNumChars, Align::Right, col, kBg);
        gfx_text(c, kMid, static_cast<int16_t>(x0 + kNumChars * kBig.adv + 2), static_cast<int16_t>(kPtBigY + kBig.ascent - kMid.ascent),
                 volt ? "V" : "A", col, kBg);
        range_.init(c, kPtRangeX, kPtRangeY, kPtRangeW);
        StrBuf<8> minl, maxl;
        put_exact_unit(minl, lo, volt ? 'V' : 'A');
        put_exact_unit(maxl, hi, volt ? 'V' : 'A');
        const int16_t ly = static_cast<int16_t>(kPtRangeY + kit::ProgressBar::kH + 5);
        gfx_text(c, kLabelFont, kPtRangeX, ly, minl.view(), kDim, kBg);
        gfx_text(c, kLabelFont, static_cast<int16_t>(kPtRangeX + kPtRangeW - static_cast<int>(maxl.view().size()) * kLabelFont.adv), ly, maxl.view(), kDim, kBg);
    }
    StrBuf<12> s;
    s.put_fixed(static_cast<int32_t>(mw), 1).put_char('W');
    f_[0].draw(s.view());
    s.clear();
    s.put_fixed(static_cast<int32_t>(value), 2);
    f_[1].draw(s.view());
    const uint32_t tri = 1u + (can_dn ? 1u : 0u) + (can_up ? 2u : 0u);
    if (tri != tsig_) {
        tsig_ = tri;
        constexpr int kTriH = 21;
        const int16_t ty = static_cast<int16_t>(kPtBigY + (kBig.height - kTriH) / 2);
        draw_triangle(c, 4, ty, kTriH, false, can_dn ? col : kRule);   // both always drawn; dim at a limit
        draw_triangle(c, static_cast<int16_t>(c.w - 4 - tri_width(kTriH)), ty, kTriH, true, can_up ? col : kRule);
    }
    range_.set(static_cast<int>(value > lo ? value - lo : 0), static_cast<int>(hi > lo ? hi - lo : 0));
}

// CONFIRM: the request as a four-row table (offer, volts, amps, power against what the source offers) and the SEND cell. The
// cell turns into CONFIRM with a draining bar after the first OK above 5 V. A plan the source would refuse shows an alert band
// and the reason instead.
void PdTriggerPage::paint_confirm(uint32_t now_ms) {
    const Surface &c = ctx_.content;
    WizSummary s;
    const bool ok = wiz_summary(*caps_, wiz_.choice(*caps_), ctx_.cap_mv, s);
    uint32_t sig = sigu(sigu(sigu(2166136261u, ok ? 1u : 0u), static_cast<uint32_t>(s.err)), s.needs_confirm ? 1u : 0u);
    sig = mix(mix(mix(mix(mix(sig, s.volts.view()), s.amps.view()), s.watts.view()), s.offer_w.view()), s.kind.view()) | 1u;
    sig = sigu(sig, wiz_.cursor) | 1u;
    if (sig != bsig_) {
        bsig_ = sig;
        gfx_fill(c, 0, kit::kBodyY, c.w, int16_t(kFooterY - kit::kBodyY), kBg);
        send_.invalidate();
        if (ok && s.err != pd::Err::Ok) {   // the plan refused it: say so instead of a summary
            kit::draw_alert_band(c, kit::kBodyY, true, "NOT SENDABLE");
            gfx_text(c, kSmall, 6, static_cast<int16_t>(kit::kBodyY + kit::kAlertH + 10), pd::err_name(s.err), kText, kBg);
        } else if (ok) {
            static const char *const kKeys[] = {"Offer", "Voltage", "Current", "Power"};
            kit::kv_static(c, kPtKv, kKeys, 4);
            pdo_line(caps_->pdos[wiz_.cursor], line_);
            gfx_text(c, kSmall, kPtKv.value_x, pt_text_y(kPtKv, 0), line_.text.view(), kText, kBg);
            gfx_text(c, kSmall, kPtKv.value_x, pt_text_y(kPtKv, 1), s.volts.view(), kVolt, kBg);
            gfx_text(c, kSmall, kPtKv.value_x, pt_text_y(kPtKv, 2), s.amps.view(), kAmp, kBg);
            gfx_text(c, kSmall, kPtKv.value_x, pt_text_y(kPtKv, 3), s.watts.view(), kWatt, kBg);   // power_mw
            StrBuf<12> of;
            of.put(" of ").put(s.offer_w.view());
            gfx_text(c, kSmall, static_cast<int16_t>(kPtKv.value_x + static_cast<int>(s.watts.view().size()) * kSmall.adv),
                     pt_text_y(kPtKv, 3), of.view(), kDim, kBg);
            send_.init(c, kPtSendY, kPtSendH);
        }
    }
    if (!ok || s.err != pd::Err::Ok) return;
    if (ctx_.trig_armed) {
        StrBuf<32> sub;
        sub.put("Send again: ").put(s.volts.view()).put_char(' ').put(s.amps.view());
        const uint32_t used = armed_at_ && now_ms > armed_at_ ? now_ms - armed_at_ : 0;
        const int left = used >= Ui::kConfirmWindowMs ? 0 : static_cast<int>(1000u - used * 1000u / Ui::kConfirmWindowMs);
        send_.draw_armed("CONFIRM", sub.view(), left);
    } else {
        send_.draw_idle("SEND", s.needs_confirm ? "Above 5V: check your load" : "");
    }
}

// LIVE: a verdict cell for the contract (the state in words), then requested next to measured. The measured cells repaint on
// change only; the requested values and the hard-reset count repaint when they change.
void PdTriggerPage::paint_live() {
    const Surface &c = ctx_.content;
    const pd::SinkStatus &st = fusb302::sink_status();
    const LiveState ls = live_state(st);
    WizSummary s;
    wiz_summary(*caps_, wiz_.choice(*caps_), ctx_.cap_mv, s);
    const bool on = ctx_.pm_online;
    const bool good = live_good(ls), bad = live_bad(ls);
    const uint32_t sig = mix(mix(sigu(2166136261u, static_cast<uint32_t>(ls)), s.volts.view()), s.amps.view()) | 1u;
    if (sig != bsig_) {
        bsig_ = sig;
        vsig_ = tsig_ = 0;
        gfx_fill(c, 0, kit::kBodyY, c.w, int16_t(kFooterY - kit::kBodyY), kBg);
        static const char *const kKeys[] = {"Requested V", "Measured V", "Requested A", "Measured A", "Resets"};
        kit::kv_static(c, kPtLiveKv, kKeys, 5);
        gfx_text(c, kSmall, kPtLiveKv.value_x, pt_text_y(kPtLiveKv, 0), s.volts.view(), kVolt, kBg);
        gfx_text(c, kSmall, kPtLiveKv.value_x, pt_text_y(kPtLiveKv, 2), s.amps.view(), kAmp, kBg);
        f_[0].init(c, kSmall, kPtLiveKv.value_x, pt_text_y(kPtLiveKv, 1), 12, Align::Left, kVolt, kBg);
        f_[1].init(c, kSmall, kPtLiveKv.value_x, pt_text_y(kPtLiveKv, 3), 12, Align::Left, kAmp, kBg);
    }
    if (vsig_ != bsig_) {
        vsig_ = bsig_;
        const kit::Verdict v = good ? kit::Verdict::Pass : bad ? kit::Verdict::Fail :
                               (ls == LiveState::Dropping || ls == LiveState::Released) ? kit::Verdict::None : kit::Verdict::Running;
        kit::draw_verdict(c, 0, kit::kBodyY, c.w, kit::kAlertH, v, live_name(ls), kBg);
    }
    StrBuf<12> s1, s2;
    if (on) {
        s1.put_abs_fixed(ctx_.pm.vbus_mv, 2).put_char('V');
        s2.put_abs_fixed(ctx_.pm.current_ma, 2).put_char('A');
    } else {
        s1.put("--");
        s2.put("--");
    }
    f_[0].set_colors(on ? kVolt : kDim, kBg);
    f_[0].draw(s1.view());
    f_[1].set_colors(on ? kAmp : kDim, kBg);
    f_[1].draw(s2.view());
    const uint32_t rs = 1u + st.hard_resets;
    if (rs != tsig_) {
        tsig_ = rs;
        StrBuf<8> n;
        n.put_u32(st.hard_resets);
        gfx_text(c, kSmall, kPtLiveKv.value_x, pt_text_y(kPtLiveKv, 4), n.view(), st.hard_resets ? kCaution : kDim, kBg);
    }
}

void PdTriggerPage::paint_footer() {
    const Surface &c = ctx_.content;
    const pd::SinkStatus &st = fusb302::sink_status();
    StrBuf<32> msg;
    FooterTone tone = FooterTone::Info;
    if (blocked_ && wiz_.step == WizStep::Offer) {
        msg.put("Other port busy: unplug");
        tone = FooterTone::Error;
    } else if (wiz_.step == WizStep::Confirm && send_err_ != pd::Err::Ok) {
        msg.put("Refused: ").put(pd::err_name(send_err_));
        tone = FooterTone::Error;
    } else if (wiz_.step == WizStep::Offer && !Wizard::offer_available(*caps_, wiz_.cursor, ctx_.cap_mv)) {
        const bool over = pd::plan_request(*caps_, pd::Choice{static_cast<uint8_t>(wiz_.cursor + 1), 0, 0}, ctx_.cap_mv, wiz_.cursor >= 7).err == pd::Err::OverCap;
        if (over) msg.put("Over ").put_u32(ctx_.cap_mv / 1000).put("V limit: pick other");
        else msg.put("Not requestable: pick other");
        tone = FooterTone::Caution;
    } else if (wiz_.step == WizStep::Voltage && std::min<uint32_t>(caps_->pdos[wiz_.cursor].max_mv, std::max<uint32_t>(ctx_.cap_mv, pd::kVsafe5vMv)) < caps_->pdos[wiz_.cursor].max_mv) {
        const uint32_t vmax = std::max<uint32_t>(ctx_.cap_mv, pd::kVsafe5vMv);
        msg.put("Capped at ");
        put_exact_unit(msg, vmax, 'V');
        msg.put(" by monitor");
        tone = FooterTone::Caution;
    } else {
        wiz_.footer(*caps_, ctx_.cap_mv, ctx_.trig_armed, msg);
        if (wiz_.step == WizStep::Live) {
            const LiveState ls = live_state(st);
            tone = live_good(ls) ? FooterTone::Ok : live_bad(ls) ? FooterTone::Error : FooterTone::Info;
        } else if (wiz_.step == WizStep::Confirm && trigger_needs_confirm(*caps_, wiz_.choice(*caps_), ctx_.cap_mv)) {
            tone = FooterTone::Caution;
        }
    }
    const uint32_t sig = mix(2166136261u, msg.view()) * 7u + static_cast<uint32_t>(tone) + 1u;
    if (sig == fsig_) return;
    fsig_ = sig;
    draw_footer(c, tone, msg.view());
}

void PdTriggerPage::paint_refusal() {
    const Surface &c = ctx_.content;
    const SetupView v = setup_view(Tool::PdTrigger, refusal_state());
    uint32_t sig = mix(2166136261u, v.footer);
    for (int k = 0; k < v.n; k++) sig = sig * 3 + (v.met[k] ? 1u : 0u);
    sig |= 1u;
    if (sig == rsig_) return;
    rsig_ = sig;
    gfx_fill(c, 0, kBarH, c.w, int16_t(kFooterY - kBarH), kBg);
    FlowSpec f;
    f.top_role = v.top_role; f.bottom_role = v.bottom_role;
    f.top_color = f.bottom_color = kError; f.top_tint = f.bottom_tint = kErrorT;
    f.caption = "none"; f.caption_color = kError;
    draw_physical_flow_diagram(c, 0, 22, 72, f, ctx_.rotation);
    CheckItem items[3];
    for (int k = 0; k < v.n; k++) items[k] = {v.check[k], v.met[k]};
    draw_checklist(c, 68, 22, 156, 72, kError, items, v.n);
    draw_footer(c, FooterTone::Error, v.footer);
}

// The live setup with the page's own verdict applied (a fake PDO load refuses without a real source).
SetupState PdTriggerPage::refusal_state() const {
    SetupState st = setup_state(ctx_);
    if (refusal_ == Refusal::Inline) st.inline_mode = true;
    if (refusal_ == Refusal::NoCaps) st.caps_valid = false;
    return st;
}

// [+] / [-] step the value of this screen (long = coarse), OK moves on or sends, BACK goes one step
// back and is declined on OFFER so the menu pops. On LIVE BACK releases to 5 V first.
bool PdTriggerPage::on_event(Event e) {
    if (refusal_ != Refusal::None) return false;          // nothing to drive; BACK pops
    const pd::SourceCaps &caps = pd::active_caps();
    const bool step = e.button == Button::Plus || e.button == Button::Minus;
    const bool press = e.press == Press::Short || e.press == Press::Long;
    if (step && press) {
        if (wiz_.step != WizStep::Live) {
            wiz_.nudge(caps, e.button == Button::Plus ? 1 : -1, e.press == Press::Long, ctx_.cap_mv);
            ctx_.trig_armed = false;
            send_err_ = pd::Err::Ok;
            update(ctx_.now);
        }
        return true;
    }
    if (e.press == Press::Short && e.button == Button::Ok) {
        if (blocked_ && wiz_.step != WizStep::Live) return true;   // Confirm is greyed out: no TX, no next step
        switch (wiz_.ok(caps, ctx_.cap_mv)) {
        case Wizard::Ok::Send: {
            const Ui::TriggerOutcome o = ctx_.ui->device_trigger(wiz_.choice(caps), ctx_.now);
            if (o.err != pd::Err::Ok) send_err_ = o.err;
            else if (!o.need_confirm) { send_err_ = pd::Err::Ok; wiz_.sent(); }
            break;
        }
        case Wizard::Ok::Next:
            ctx_.trig_armed = false;
            send_err_ = pd::Err::Ok;
            break;
        case Wizard::Ok::Stay: break;
        }
        update(ctx_.now);
        return true;
    }
    if (e.press == Press::Short && e.button == Button::Back) {
        if (wiz_.step == WizStep::Live && fusb302::sink_status().held) ctx_.ui->trigger_release(ctx_.now);
        if (wiz_.back()) return false;                    // BACK on OFFER: leave the wizard
        ctx_.trig_armed = false;
        send_err_ = pd::Err::Ok;
        update(ctx_.now);
        return true;
    }
    if (e.press == Press::Long && e.button == Button::Ok) return true;
    return false;
}

PanelLabels PdTriggerPage::labels() const {
    PanelLabels l = base_labels();
    if (refusal_ != Refusal::None) return l;
    auto &ok = l.b[static_cast<int>(Button::Ok)];
    auto &up = l.b[static_cast<int>(Button::Plus)];
    auto &dn = l.b[static_cast<int>(Button::Minus)];
    switch (wiz_.step) {
    case WizStep::Offer: ok = blocked_ ? SlotLabel{"", "", "", false} : SlotLabel{"Next", "", "", false}; up = {kDn, "", "", false}; dn = {kUp, "", "", false}; break;
    case WizStep::Voltage: ok = {"Next", "", "", false}; up = {"+", "1V", "", false}; dn = {"-", "1V", "", false}; break;
    case WizStep::Current: ok = {"Next", "", "", false}; up = {"+", "0.5A", "", false}; dn = {"-", "0.5A", "", false}; break;
    case WizStep::Confirm: ok = {"Send", "", "", false}; up = {"", "", "", false}; dn = {"", "", "", false}; break;
    case WizStep::Live: ok = {"", "", "", false}; up = {"", "", "", false}; dn = {"", "", "", false}; break;
    }
    return l;
}

void PdTriggerPage::invalidate() { reset_paint(); }

// ---------------------------------------------------------------------------------------
// Cable resistance submenu (passive two-point method)
// ---------------------------------------------------------------------------------------

namespace {
// Three key/value rows (the two steps and the load port), the result block under them up to the footer.
constexpr int kCrRows = 3;
constexpr kit::KvLayout kCrKv{kit::kBodyY, kit::kRowH, kit::kMarkX, &kLabelFont, nullptr, 56, 16, Align::Left};
constexpr int16_t kCrResY = kit::kBodyY + kCrRows * kit::kRowH + 1;   // under the table's closing rule
constexpr int16_t kCrResH = kFooterY - kCrResY;
constexpr int16_t kCrNumX = 8, kCrBadgeX = 150, kCrBadgeW = kit::kTailX - kCrBadgeX, kCrBadgeH = 28;
constexpr int16_t kCrSegX[3] = {8, 76, 144}, kCrSegW = 66, kCrSegH = 6;   // grade scale: flat segments, the held one lit

uint16_t cr_color(CrTone t) {
    switch (t) {
    case CrTone::Ok:      return kOk;
    case CrTone::Caution: return kCaution;
    case CrTone::Error:   return kError;
    default:              return kDim;
    }
}
uint16_t cr_value_color(CrTone t) { return t == CrTone::Dim ? kDim : t == CrTone::Ok ? kVolt : cr_color(t); }
}  // namespace

void CableRPage::draw_static() {
    title_bar();
    csig_ = rsig_ = 0;
    foot_.invalidate();
}

// The table's static part (keys, rules, value cells) or, without a power monitor, the alert band. Redone only when the mode or
// the meter's presence changes, which is what the keys depend on.
void CableRPage::paint_table(const CrView &v) {
    uint32_t sig = v.meter_missing ? 1u : 2u;
    for (const CrCard &cd : v.step) sig = mix(sig * 7, cd.label);
    if (sig == csig_) return;
    csig_ = sig;
    const Surface &c = ctx_.content;
    if (!ctx_.blank) gfx_fill(c, 0, kBarH, c.w, int16_t(kFooterY - kBarH), kBg);   // body between the bars (a blank screen is kBg)
    rsig_ = 0;
    for (Field &f : val_) f.invalidate();
    if (v.meter_missing) {
        kit::draw_alert_band(c, kit::kBodyY, true, "METER MISSING");
        gfx_text(c, kSmall, kit::kMarkX, kit::kBodyY + kit::kAlertH + 10, "Cable R needs INA2xx", kText, kBg);
        gfx_text(c, kSmall, kit::kMarkX, kit::kBodyY + kit::kAlertH + 30, "Diag shows the chip", kDim, kBg);
        return;
    }
    const char *const keys[kCrRows] = {v.step[0].label, v.step[1].label, "Port"};
    kit::kv_static(c, kCrKv, keys, kCrRows);
    // The ports are fixed by the hardware: the load plugs into the top port (the charger into the bottom one), whatever the
    // screen rotation.
    gfx_text(c, kSmall, kCrKv.value_x, static_cast<int16_t>(kit::kv_row_y(kCrKv, 2) + kit::row_text_y(kit::kRowH, kSmall.height)),
             "load on top", kDim, kBg);
    kit::kv_cells(c, kCrKv, val_, 2);
}

void CableRPage::paint_result(const CrView &v) {
    uint32_t sig = v.has_result ? 1u : 0u;
    if (v.has_result) {
        sig = mix(mix(sig, v.number.view()), v.detail.view()) * 31 + static_cast<uint32_t>(v.grade);
        sig = mix(sig, v.unit);
    }
    if (sig == rsig_) return;
    rsig_ = sig;
    const Surface &c = ctx_.content;
    gfx_fill(c, 0, kCrResY, c.w, kCrResH, kBg);
    if (!v.has_result) return;
    const int16_t ny = kCrResY + 6;
    gfx_text(c, kBig, kCrNumX, ny, v.number.view(), kEnergy, kBg);
    gfx_text(c, kMid, static_cast<int16_t>(kCrNumX + text_width(kBig, v.number.view()) + 4),
             static_cast<int16_t>(ny + kBig.ascent - kMid.ascent), v.unit, kEnergy, kBg);
    // The grade is a verdict cell: green for excellent / good, solid red for poor; fair has no verdict style, so it takes the
    // caution fill by hand.
    const int16_t by = kCrResY + 12;
    if (v.grade == cable_r::Grade::Fair) {
        gfx_fill(c, kCrBadgeX, by, kCrBadgeW, kCrBadgeH, kCaution);
        gfx_text(c, kSmall, static_cast<int16_t>(kCrBadgeX + (kCrBadgeW - text_width(kSmall, v.grade_pill)) / 2),
                 static_cast<int16_t>(by + (kCrBadgeH - kSmall.height) / 2), v.grade_pill, kBg, kCaution);
    } else {
        kit::draw_verdict(c, kCrBadgeX, by, kCrBadgeW, kCrBadgeH,
                          v.grade == cable_r::Grade::Poor ? kit::Verdict::Fail : kit::Verdict::Pass, v.grade_pill, kBg);
    }
    // Grade scale: the segment holding the result is lit, the others stay on the rule colour.
    const int lit = v.grade == cable_r::Grade::Poor ? 2 : v.grade == cable_r::Grade::Fair ? 1 : 0;
    const uint16_t segc[3] = {kOk, kCaution, kError};
    static constexpr const char *kScale[3] = {"<150", "<300", ">300"};
    for (int i = 0; i < 3; i++) {
        gfx_fill(c, kCrSegX[i], kCrResY + 64, kCrSegW, kCrSegH, i == lit ? segc[i] : kRule);
        gfx_text(c, kLabelFont, kCrSegX[i], kCrResY + 74, kScale[i], kDim, kBg);
    }
    gfx_text(c, kSmall, kCrNumX, kCrResY + 98, v.detail.view(), kText, kBg);
}

void CableRPage::update(uint32_t) {
    title_badge(false);
    const CrView v = cr_view(ctx_.pm_online, ctx_.pm.vbus_mv, ctx_.pm.current_ma, ctx_.cable_mode, ctx_.cable, ctx_.kelvin);
    paint_table(v);
    if (!v.meter_missing) {
        for (int i = 0; i < 2; i++) {
            val_[i].set_colors(cr_value_color(v.step[i].tone), kBg);
            val_[i].draw(v.step[i].value.view());
        }
        paint_result(v);
    }
    const bool cleared = ctx_.now < cleared_until_;
    const std::string_view foot = cleared ? std::string_view("Cleared, measuring again") : v.foot.view();
    const CrTone tone = cleared ? CrTone::Dim : v.foot_tone;
    // Dim = an instruction or a plain state: the neutral band, a green fill would read as "done".
    draw_footer_cached(ctx_.content, footer_tone_of(cr_color(tone)), foot, foot_);
}

bool CableRPage::on_event(Event e) {
    if (e.press != Press::Short) return false;
    if (e.button == Button::Ok) {
        ctx_.cable.reset();
        ctx_.kelvin.reset();
        cleared_until_ = ctx_.now + 1500;   // the idle reading is retaken at once, so say that it was cleared
        update(ctx_.now);
        return true;
    }
    if (e.button == Button::Plus) {
        ctx_.cable_mode = ctx_.cable_mode == cable_r::Mode::Passive ? cable_r::Mode::Kelvin : cable_r::Mode::Passive;
        ctx_.cable.reset();
        ctx_.kelvin.reset();
        update(ctx_.now);
        return true;
    }
    return false;
}

PanelLabels CableRPage::labels() const {
    PanelLabels l = base_labels();
    l.b[static_cast<int>(Button::Ok)] = {"Reset", "", "", false};
    l.b[static_cast<int>(Button::Minus)] = {"", "", "", true};
    l.b[static_cast<int>(Button::Plus)] = {"Mode", "", "", false};
    return l;
}

void CableRPage::invalidate() {
    for (Field &f : val_) f.invalidate();
    csig_ = rsig_ = 0;
    foot_.invalidate();
}

// ---------------------------------------------------------------------------------------
// Fast-charge trigger submenu
// ---------------------------------------------------------------------------------------

namespace {
// The key/value table of the staged pages: 20 px rows, S12 keys, S16 values from x72.
constexpr kit::KvLayout kFcKv{kit::kBodyY, 20, 6, &kLabelFont, nullptr, 72, 16, Align::Left};
constexpr kit::KvLayout kFcRelKv{kit::kBodyY + kit::kAlertH + 4, 20, 6, &kLabelFont, nullptr, 72, 16, Align::Left};
constexpr int16_t kFcSparkX = 6, kFcSparkY = 110, kFcSparkW = 210, kFcSparkH = 108;   // Live: the VBUS trace under the table
constexpr int16_t kFcBigY = 118, kFcSendY = 164, kFcSendH = 52;   // Edit / Review: the draft value, the SEND cell
constexpr int16_t kFcScanPitch = 22, kFcSumY = kit::kBodyY + 7 * kFcScanPitch + 4, kFcSumH = 36;   // Scan: rows, summary cell
constexpr uint32_t kFcTraceStepMs = 250;

int16_t fc_row_text_y(int i) { return static_cast<int16_t>(kit::kv_row_y(kFcKv, i) + kit::row_text_y(kFcKv.pitch, 16)); }
}

trig::Run TriggerPage::run() const {
    if (scanning_) return trig::Run::Scan;
    if (st_.state == fc::State::Negotiating) return trig::Run::Negotiating;
    if (st_.state == fc::State::Active) return trig::Run::Live;
    return trig::Run::Idle;
}

// A scan that ends starts a measured release; its results stay up while it measures (footer line) and only an unsafe VBUS
// takes the body for the AlertBand.
bool TriggerPage::scan_view() const {
    return (scanning_ || show_scan_) && ctx_.ui->fc_release().state() != trig::Release::State::Unsafe;
}

void TriggerPage::draw_static() {
    kind_ = 255;   // the trace ring and the model survive a repaint, the paint signatures do not
    bsig_ = barsig_ = fsig_ = tsig_ = ysig_ = 0;
    paint_bar();
}

// The wizard bar: "1/3 FC trigger" with the progress rule while a protocol is picked, drafted and reviewed; the live and
// scan views carry the plain name. The live V x A readout behind the rule is title_badge's.
void TriggerPage::paint_bar() {
    const bool scan = scan_view();
    const auto stage = model_.stage();
    const bool retarget = model_.retargeting();
    const uint32_t sg = 1u + unsigned(stage) + 8u * retarget + 16u * scan;
    if (sg == barsig_) return;
    barsig_ = sg;
    bar_valid_ = false;   // the band repaint erased the readout: title_badge redraws it
    const char *title = scan ? "FC scan" : retarget ? "Retarget" : "FC trigger";
    const int step = scan || stage == trig::Stage::Live ? 0 : stage == trig::Stage::Pick ? 1 : stage == trig::Stage::Edit ? 2 : 3;
    kit::draw_top_bar(ctx_.content, title, step, fusb302::self_powered() ? "SNK" : "PAS", true);
}

void TriggerPage::sample() {
    push_trace(ctx_.now);
    model_.session(fusb302::session());
    st_ = fctrig::status();
    if (st_.state == fc::State::Active || st_.state == fc::State::Negotiating)
        model_.observe_live(st_.proto, st_.target_mv);
    scanning_ = fctrig::scan_running();
    scan_step_ = fctrig::scan_step();
    const auto r = fctrig::scan_results();
    res_n_ = r.size() < res_.size() ? r.size() : res_.size();
    for (std::size_t i = 0; i < res_n_; i++) res_[i] = r[i];
    if (scanning_) show_scan_ = true;
}

void TriggerPage::poll() {
    const uint32_t id = fusb302::session();
    const auto status = fctrig::status();
    const auto state = status.state;
    const bool running = fctrig::scan_running() || state == fc::State::Active || state == fc::State::Negotiating;
    if (dirty_ && (id != session_ || state == fc::State::Failed || (was_running_ && !running))) {
        if (id != session_) refused_ = fc::Fail::SourceLost;
        else if (state == fc::State::Failed) refused_ = status.fail;
        ctx_.ui->stop_fc(ctx_.now);
        model_.cancel();
        dirty_ = false;
    }
    was_running_ = running;
    session_ = id;
    model_.session(id);
}

void TriggerPage::push_trace(uint32_t now_ms) {
    if (trace_ms_ != 0 && now_ms - trace_ms_ < kFcTraceStepMs) return;
    trace_ms_ = now_ms ? now_ms : 1;
    const int16_t mv = ctx_.pm_online ? static_cast<int16_t>(ctx_.pm.vbus_mv < 0 ? 0 : ctx_.pm.vbus_mv > 32000 ? 32000 : ctx_.pm.vbus_mv) : 0;
    if (trace_n_ == kTraceLen) {
        for (int i = 1; i < kTraceLen; i++) trace_mv_[i - 1] = trace_mv_[i];
        trace_mv_[kTraceLen - 1] = mv;
    } else {
        trace_mv_[trace_n_++] = mv;
    }
}

void TriggerPage::draw_trace() {
    uint8_t px[kFcSparkW];
    const int n = trace_n_;
    for (int i = 0; i < kFcSparkW; i++) {
        int mv = 0;
        if (n >= 1) {
            // Virtual axis of kTraceLen slots, newest at the right; slots older than the first sample repeat it.
            const int pos = i * (kTraceLen - 1) * 16 / (kFcSparkW - 1);   // 1/16 slot
            const int j = pos >> 4, fr = pos & 15, lo = kTraceLen - n;
            auto at = [&](int s) { return trace_mv_[s <= lo ? 0 : s - lo]; };
            const int a = at(j), b = at(j + 1 < kTraceLen ? j + 1 : j);
            mv = a + (b - a) * fr / 16;
        }
        px[i] = static_cast<uint8_t>(trig::trace_level(mv, kFcSparkH));
    }
    draw_sparkline(ctx_.content, kFcSparkX, kFcSparkY, kFcSparkW, kFcSparkH, px, kVolt, kVoltT, kBg);   // VBUS history
}

void TriggerPage::paint_footer(FooterTone tone, std::string_view text) {
    const uint32_t sg = mix(2166136261u, text) * 31 + static_cast<uint32_t>(tone) + 1u;
    if (sg == fsig_) return;
    fsig_ = sg;
    draw_footer(ctx_.content, tone, text);
}

// The protocol list: kit RowList rows (focus fill and bar), the name and what the entry takes at the right. Picking a row
// sends nothing; the list repaints only the rows that changed.
void TriggerPage::row_fn(void *self, int index, bool, kit::RowSpec &out) {
    (void)self;
    out = kit::RowSpec{};
    if (index < 0 || index >= trig::kEntries) return;
    const trig::Entry &e = trig::entry(index);
    out.text = e.name;
    out.tail = e.kind == trig::Kind::Scan ? "HV sweep" :
        e.kind == trig::Kind::Range ? "adjustable" : e.kind == trig::Kind::Levels ? "levels" : "5V only";
}

// The Offered cell of the table: what the protocol takes.
static void fc_offered_text(const trig::Entry &e, StrBuf<24> &s) {
    if (e.kind == trig::Kind::Range) {
        s.put_fixed(e.lo, 1).put("-").put_fixed(e.hi, 1).put_char('V');
    } else if (e.kind == trig::Kind::Levels) {
        for (int i = 0; i < e.n; ++i) {
            if (i) s.put_char('/');
            s.put_u32(uint32_t(e.mv[i] / 1000));
        }
        s.put_char('V');
    } else if (e.kind == trig::Kind::Scan) {
        s.put("HV sweep");
    } else {
        s.put("5V only");
    }
}

// The release monitor replaces the body while the measured release runs: an AlertBand (amber while it measures, red once
// VBUS did not settle) over the measured VBUS. Nothing else on the page reacts until it is Safe.
void TriggerPage::paint_release(bool fresh) {
    const Surface &c = ctx_.content;
    if (fresh) {
        const bool unsafe = ctx_.ui->fc_release().state() == trig::Release::State::Unsafe;
        kit::draw_alert_band(c, kit::kBodyY, unsafe, unsafe ? "UNSAFE VBUS" : "RELEASING");
        static const char *const labels[] = {"Target", "Measured"};
        kit::kv_static(c, kFcRelKv, labels, 2);
        const int16_t y0 = static_cast<int16_t>(kFcRelKv.y0 + kit::row_text_y(kFcRelKv.pitch, 16));
        gfx_text(c, kSmall, kFcRelKv.value_x, y0, "5V", kVolt, kBg);
        vbus_v_.init(c, kSmall, kFcRelKv.value_x, static_cast<int16_t>(y0 + kFcRelKv.pitch), 8, Align::Left, kVolt, kBg);
    }
    StrBuf<12> s;
    if (ctx_.pm_online) s.put_fixed(ctx_.pm.vbus_mv, 2).put_char('V'); else s.put("--");
    vbus_v_.draw(s.view());
}

void TriggerPage::paint_main(uint32_t now_ms) {
    const Surface &c = ctx_.content;
    const trig::Entry &e = trig::entry(model_.sel());
    const auto stage = model_.stage();
    const bool live = stage == trig::Stage::Live;
    const bool retarget = model_.retargeting();
    const bool scan_sel = model_.scan_selected();
    const auto release = ctx_.ui->fc_release().state();
    const bool cleanup = ctx_.ui->fc_release().busy();
    // The protocol list repaints per row (RowList), so its selection is not part of the body signature: a selection move
    // must not wipe the body.
    const unsigned sel_key = stage == trig::Stage::Pick ? 0u : unsigned(model_.sel());
    const uint32_t sig = (1u + sel_key + 16u * unsigned(stage) + 128u * retarget + 256u * cleanup + 512u * unsigned(release)) * 65537u +
                         unsigned(power::max_vbus_mv());
    const bool fresh = sig != bsig_;
    if (fresh) {
        // Wipe the previous stage's body, unless the screen is blank already (a repaint after a clear or an erase), and for
        // the protocol list only around it: the list writes every row it owns.
        if (!ctx_.blank) {
            if (stage == trig::Stage::Pick && !cleanup) kit::RowList::clear_body_around(c);
            else gfx_fill(c, 0, kit::kBelowBar, c.w, int16_t(kFooterY - kit::kBelowBar), kBg);
        }
        bsig_ = sig;
        fsig_ = 0;
        tsig_ = 0;
        send_.invalidate();
    }
    if (cleanup) {
        paint_release(fresh);
    } else if (stage == trig::Stage::Pick) {
        if (fresh) {
            ctx_.rows.init(c, kit::kBodyY, row_fn, this);
            ctx_.rows.invalidate();
        }
        ctx_.rows.set_count(trig::kEntries);
        ctx_.rows.select(model_.sel());
        ctx_.rows.draw(now_ms);
    } else {
        // The key/value table: Edit and Review show the protocol, what it offers, the measured VBUS and the held voltage
        // (retarget) or the monitor cap; Live shows the state, the protocol, the request and the measurement.
        static const char *const kLiveKeys[] = {"State", "Protocol", "Requested", "Measured"};
        static const char *const kHeldKeys[] = {"Protocol", "Offered", "Measured", "Held"};
        static const char *const kCapKeys[] = {"Protocol", "Offered", "Measured", "Cap"};
        if (fresh) {
            kit::kv_static(c, kFcKv, live ? kLiveKeys : retarget ? kHeldKeys : kCapKeys, 4);
            gfx_text(c, kSmall, kFcKv.value_x, fc_row_text_y(live ? 1 : 0), e.name, kText, kBg);
            if (live) {
                state_v_.init(c, kSmall, kFcKv.value_x, fc_row_text_y(0), 12, Align::Left, kOk, kBg);
                target_v_.init(c, kSmall, kFcKv.value_x, fc_row_text_y(2), 8, Align::Left, kVolt, kBg);
                vbus_v_.init(c, kSmall, kFcKv.value_x, fc_row_text_y(3), 8, Align::Left, kVolt, kBg);
            } else {
                StrBuf<24> offered;
                fc_offered_text(e, offered);
                gfx_text(c, kSmall, kFcKv.value_x, fc_row_text_y(1), offered.view(), e.kind == trig::Kind::Scan ? kText : kVolt, kBg);
                vbus_v_.init(c, kSmall, kFcKv.value_x, fc_row_text_y(2), 8, Align::Left, kVolt, kBg);
                if (retarget) {
                    held_v_.init(c, kSmall, kFcKv.value_x, fc_row_text_y(3), 8, Align::Left, kVolt, kBg);
                } else {
                    StrBuf<12> cap;
                    cap.put_fixed(power::max_vbus_mv(), 1).put_char('V');
                    gfx_text(c, kSmall, kFcKv.value_x, fc_row_text_y(3), cap.view(), kVolt, kBg);
                }
                // The draft: the big number with its unit, or the sweep word.
                const int16_t unit_y = static_cast<int16_t>(kFcBigY + kBig.ascent - kMid.ascent);
                if (scan_sel) {
                    gfx_text(c, kMid, 6, unit_y, "SCAN", kText, kBg);
                } else {
                    constexpr uint8_t kNumChars = 5;
                    target_v_.init(c, kBig, 6, kFcBigY, kNumChars, Align::Right, kVolt, kBg);
                    gfx_text(c, kMid, static_cast<int16_t>(6 + kNumChars * kBig.adv + 2), unit_y, "V", kVolt, kBg);
                }
                if (stage == trig::Stage::Review) send_.init(c, kFcSendY, kFcSendH);
            }
        }
        StrBuf<12> s;
        if (live) {
            const bool waiting = run() == trig::Run::Negotiating;
            state_v_.set_colors(waiting ? kCaution : kOk, kBg);
            state_v_.draw(waiting ? "Requesting" : "Holding");
            s.put_fixed(model_.target_mv(), 2).put_char('V');
            target_v_.draw(s.view());
        } else if (!scan_sel) {
            s.put_fixed(model_.target_mv(), 2);
            target_v_.draw(s.view());
        }
        s.clear();
        if (ctx_.pm_online) s.put_fixed(ctx_.pm.vbus_mv, 2).put_char('V'); else s.put("--");
        vbus_v_.draw(s.view());
        if (retarget) {
            s.clear();
            s.put_fixed(model_.held_mv(), 2).put_char('V');
            held_v_.draw(s.view());
        }
        if (live && tsig_ != trace_ms_ + 1) { tsig_ = trace_ms_ + 1; draw_trace(); }
        if (stage == trig::Stage::Review) {
            if (model_.armed()) {
                StrBuf<32> sub;
                sub.put("Send again: ");
                if (scan_sel) sub.put("SCAN"); else sub.put_fixed(model_.target_mv(), 2).put_char('V');
                send_.draw_armed("CONFIRM", sub.view(), static_cast<int>(model_.arm_left_permille(ctx_.now)));
            } else {
                send_.draw_idle("SEND", scan_sel ? "SCAN requests HV" : model_.needs_confirm() ? "Above 5V: other port empty" :
                                                                                           "Other port must be empty");
            }
        }
    }
    // Footer: facts only. The Edit hint says how far one step moves.
    StrBuf<28> edit_hint;
    if (stage == trig::Stage::Edit && e.kind == trig::Kind::Range)
        edit_hint.put("Step ").put_u32(e.step).put("mV, hold ").put_u32(e.step < 1000 ? 1000 : e.step).put("mV");
    else
        edit_hint.put("Choose offered voltage");
    const char *footer = cleanup ? release == trig::Release::State::Unsafe ?
        "Unsafe VBUS: unplug source" : "Releasing; measuring VBUS" :
        refused_ != fc::Fail::None ? trig::fail_long(refused_) : !model_.available() ? "No voltage within monitor cap" :
        stage == trig::Stage::Pick ? release == trig::Release::State::Safe ? "5V verified. Pick protocol" : "Pick a protocol" :
        stage == trig::Stage::Edit ? edit_hint.c_str() :
        stage == trig::Stage::Review ? model_.needs_confirm() ?
            model_.armed() ? "Above 5V! Send again" : "Above 5V: send twice" : "Ready to send" :
        run() == trig::Run::Negotiating ? "Requesting the voltage" : "VBUS 10s trace, 0-20V";
    const bool unsafe = cleanup && release == trig::Release::State::Unsafe;
    const bool error = unsafe || refused_ != fc::Fail::None || !model_.available();
    const bool caution = cleanup || (model_.needs_confirm() && (stage == trig::Stage::Review || live));
    paint_footer(error ? FooterTone::Error : caution ? FooterTone::Caution : FooterTone::Info, footer);
}

void TriggerPage::paint_scan() {
    const Surface &c = ctx_.content;
    const int acc = trig::scan_accepted(res_.data(), res_n_);
    const int maxmv = trig::scan_max_mv(res_.data(), res_n_);
    // One row per scan step: a verdict marker, the protocol, the voltage it gave. A step finishing repaints only its row.
    for (std::size_t k = 0; k < fc::kScanSteps; k++) {
        const int16_t y = static_cast<int16_t>(kit::kBodyY + static_cast<int>(k) * kFcScanPitch);
        const fc::Outcome o = k < res_n_ ? res_[k].outcome : fc::Outcome::None;
        const bool running = scanning_ && k == scan_step_ && o == fc::Outcome::None;
        kit::Verdict v = kit::Verdict::Pending;
        const char *tail = "";
        switch (o) {
        case fc::Outcome::Accepted: v = kit::Verdict::Pass; break;
        case fc::Outcome::Rejected: v = kit::Verdict::Fail; tail = "no reply"; break;
        case fc::Outcome::Skipped:  v = kit::Verdict::Skip; tail = "skipped"; break;
        case fc::Outcome::None:     v = running ? kit::Verdict::Running : kit::Verdict::Pending; break;
        }
        StrBuf<8> volts;
        if (o == fc::Outcome::Accepted) { volts.put_u32(static_cast<uint32_t>((res_[k].mv + 500) / 1000)).put_char('V'); tail = volts.c_str(); }
        const uint32_t ts = mix(2166136261u, tail) * 31u + static_cast<uint32_t>(v) + 1u;
        if (ts == tile_sig_[k]) continue;
        tile_sig_[k] = ts;
        gfx_fill(c, 0, y, c.w, kFcScanPitch, kBg);
        gfx_fill(c, 0, y, c.w, 1, kRule);
        kit::draw_verdict(c, kit::kMarkX, static_cast<int16_t>(y + (kFcScanPitch - kit::kMarkSize) / 2), kit::kMarkSize, kit::kMarkSize, v, {}, kBg);
        const int16_t ty = static_cast<int16_t>(y + kit::row_text_y(kFcScanPitch, kSmall.height));
        gfx_text(c, kSmall, kit::kTextX, ty, fc::name(fc::kScanOrder[k]), o == fc::Outcome::None && !running ? kDim : kText, kBg);
        const std::string_view tv = tail;
        gfx_text(c, kSmall, static_cast<int16_t>(kit::kTailX - text_width(kSmall, tv)), ty, tv, o == fc::Outcome::Accepted ? kVolt : kDim, kBg);
    }
    // The summary: one verdict cell under the table.
    uint32_t sig = 7;
    sig = sig * 131 + (scanning_ ? 1u + static_cast<uint32_t>(scan_step_) : 0u) + static_cast<uint32_t>(acc) * 17u +
          static_cast<uint32_t>(maxmv / 100) * 257u;
    if (sig != ysig_) {
        ysig_ = sig;
        gfx_fill(c, 0, static_cast<int16_t>(kit::kBodyY + static_cast<int>(fc::kScanSteps) * kFcScanPitch), c.w, 1, kRule);
        StrBuf<24> label;
        kit::Verdict sv = kit::Verdict::None;
        if (scanning_) {
            sv = kit::Verdict::Running;
            label.put("SCAN ").put_u32(static_cast<uint32_t>(scan_step_ + 1)).put(" OF ").put_u32(fc::kScanSteps);
        } else if (acc > 0) {
            sv = kit::Verdict::Pass;
            label.put_u32(static_cast<uint32_t>(acc)).put(" OF ").put_u32(fc::kScanSteps).put("  MAX ");
            trig::put_volts(label, maxmv);
        } else {
            label.put("NONE ANSWERED");
        }
        kit::draw_verdict(c, 0, kFcSumY, c.w, kFcSumH, sv, label.view(), kBg);
    }
    if (ctx_.ui->fc_release().busy()) paint_footer(FooterTone::Caution, "Releasing; measuring VBUS");
    else if (scanning_) paint_footer(FooterTone::Info, "Scanning...");
    else if (acc > 0) paint_footer(FooterTone::Info, "Scan done");
    else paint_footer(FooterTone::Caution, "Scan done: none answered");
}

void TriggerPage::update(uint32_t now_ms) {
    paint_bar();
    title_badge(false);
    model_.set_ceiling(power::max_vbus_mv());
    model_.tick(ctx_.now);
    const uint8_t kind = scan_view() ? 1 : 0;   // the release monitor takes the body from the scan results too
    if (kind != kind_) {
        // First paint after an invalidate on a blank screen (ctx_.blank): the body is kBg already.
        const bool blank = kind_ == 255 && ctx_.blank;
        kind_ = kind;
        if (!blank) gfx_fill(ctx_.content, 0, kit::kBelowBar, ctx_.content.w, int16_t(kFooterY - kit::kBelowBar), kBg);
        bsig_ = fsig_ = tsig_ = ysig_ = 0;
        tile_sig_.fill(0);
    }
    if (kind == 1) paint_scan();
    else paint_main(now_ms);
}

bool TriggerPage::on_event(Event e) {
    poll();
    sample();
    model_.set_ceiling(power::max_vbus_mv());
    if (ctx_.ui->fc_release().busy()) {
        model_.cancel();
        if (e.button == Button::Back && e.press == Press::Short) return false;
        return true;
    }
    trig::Model::Req r;
    const trig::Run before = run();
    const bool consumed = model_.on_event(e, before, ctx_.now, r);
    if (consumed && before == trig::Run::Idle) {   // a selection change leaves the results view
        refused_ = fc::Fail::None;
        show_scan_ = false;
    }
    switch (r.kind) {
    case trig::Model::Req::Start:
        refused_ = fctrig::start(r.proto, r.mv, r.confirmed, false);
        if (refused_ == fc::Fail::None) { dirty_ = true; model_.applied(); }
        show_scan_ = false;
        break;
    case trig::Model::Req::Scan:
        refused_ = fctrig::scan_start(r.confirmed, false);
        if (refused_ == fc::Fail::None) dirty_ = true;
        show_scan_ = refused_ == fc::Fail::None;
        break;
    case trig::Model::Req::Retarget:
        refused_ = link::inline_now() != link::Reason::None ? fc::Fail::Inline :
                   fctrig::set_target(r.mv, r.confirmed);
        if (refused_ == fc::Fail::None) model_.applied();
        else { ctx_.ui->stop_fc(ctx_.now); model_.cancel(); dirty_ = false; }
        break;
    case trig::Model::Req::Stop:
        ctx_.ui->stop_fc(ctx_.now);
        dirty_ = false;
        break;
    case trig::Model::Req::None:
        break;
    }
    if (dirty_) session_ = fusb302::session();
    if (consumed) {
        sample();
        ctx_.ui->refresh(ctx_.now);   // not resync: every paint is signature-gated, an invalidate would wipe the body
    }
    return consumed;
}

PanelLabels TriggerPage::labels() const {
    PanelLabels l = base_labels();
    const trig::Run r = run();
    l.b[static_cast<int>(Button::Minus)] = {};
    l.b[static_cast<int>(Button::Plus)] = {};
    if (ctx_.ui->fc_release().busy()) {
        l.b[static_cast<int>(Button::Ok)] = {};
        return l;
    }
    if (r != trig::Run::Idle || model_.retargeting())
        l.b[static_cast<int>(Button::Back)] = {"Stop", "", "", false};   // any BACK press stops; one word
    l.b[static_cast<int>(Button::Ok)] = trig::ok_label(model_, r);
    if (r != trig::Run::Scan && r != trig::Run::Negotiating) {
        l.b[static_cast<int>(Button::Plus)] = trig::plus_label(model_);
        l.b[static_cast<int>(Button::Minus)] = trig::minus_label(model_);
    }
    return l;
}

void TriggerPage::leave() {
    // Draft selection and arming never touch D+/D-. Every exit after a request releases once.
    const bool touched = dirty_ || scanning_ ||
                         fctrig::status().state != fc::State::Idle || fctrig::scan_running();
    if (touched && !ctx_.ui->fc_release().busy()) ctx_.ui->stop_fc(ctx_.now);
    model_.cancel();
    refused_ = fc::Fail::None;
    show_scan_ = false;
    dirty_ = false;
    was_running_ = false;
}

void TriggerPage::invalidate() {
    target_v_.invalidate(); vbus_v_.invalidate(); held_v_.invalidate(); state_v_.invalidate();
    send_.invalidate();
    bsig_ = barsig_ = fsig_ = tsig_ = ysig_ = 0;
    tile_sig_.fill(0);
    kind_ = 255;
}

// ---------------------------------------------------------------------------------------
// Settings and orientation mode
// ---------------------------------------------------------------------------------------

namespace {
int rotation_choice(const Ui &ui) {
    return ui.rotation_locked() ? 1 + ui.rotation() : 0;
}

PanelLabels setting_labels(PanelLabels labels, const char *action) {
    labels = list_labels(labels);
    labels.b[static_cast<int>(Button::Ok)] = {action, "", "", false};
    return labels;
}
}

void SettingsPage::draw_static() {
    title_bar();
    ctx_.rows.init(ctx_.content, kit::kBodyY, row_fn, this);
    ctx_.rows.set_count(kRows);
    ctx_.rows.select(sel_);
    foot_.invalidate();
}

// Row i: a neutral icon, the setting name, its current value at the right (green when ON; a chevron on the row that only
// opens a page) and, on the focused row, one dim line saying what the value means. Everything is read live from the
// settings, so the rows keep no value text of their own.
void SettingsPage::row_fn(void *ctx, int i, bool, kit::RowSpec &out) {
    static constexpr const char *kTitles[kRows] = {"USB Link", "Autorotate", "Rotate buttons", "Screenshot", "Smoothing",
                                                   "Wake sleeping port", "Theme"};
    static constexpr Icon kIcons[kRows] = {Icon::Usb, Icon::Rotate, Icon::Keys, Icon::Camera, Icon::Wave, Icon::Plug, Icon::Info};
    static constexpr const char *kPresets[smoothing::kPresetCount] = {"Off", "Low", "Medium", "High"};
    static constexpr const char *kPresetNotes[smoothing::kPresetCount] = {"Raw readings", "Light calming", "Balanced", "Steady, slower"};
    const auto *self = static_cast<const SettingsPage *>(ctx);
    out.marker = kit::Marker::Icon;
    out.icon = kIcons[i];
    out.text = kTitles[i];
    switch (i) {
    case 0:
        out.chevron = true;
#if K2_NET
        out.note = "HID, drive and network";
#else
        out.note = "HID and drive";   // this build has no Network row (kUlRows below)
#endif
        break;
    case 1:
        out.tail = self->ctx_.ui->rotation_locked() ? "Locked" : "Auto";
        out.note = self->ctx_.ui->rotation_locked() ? "Fixed orientation" : "Follows gravity";
        break;
    case 2: {
        const bool on = self->ctx_.ui->rotate_buttons();
        out.tail = on ? "ON" : "OFF";
        out.tail_color = on ? kOk : kDim;
        out.note = on ? "Follow the display" : "Physical keys";
        break;
    }
    case 3: {
        const bool on = uiprefs::screenshot_enabled;
        out.tail = on ? "ON" : "OFF";
        out.tail_color = on ? kOk : kDim;
        out.note = on ? "Remote and web capture" : "No capture";
        break;
    }
    case 4: {
        const auto p = static_cast<unsigned>(power::smoothing_preset());
        out.tail = kPresets[p < smoothing::kPresetCount ? p : 0];
        out.note = kPresetNotes[p < smoothing::kPresetCount ? p : 0];
        break;
    }
    case 5: {   // the opt-in exception to "no Rd at idle": a sleeping source's port is asked with our Rd
        const bool on = fusb302::idle_wake();
        out.tail = on ? "ON" : "OFF";
        out.tail_color = on ? kOk : kDim;
        out.note = on ? "Idle Rd on a sleeping port" : "Idle stays passive";
        break;
    }
    default: {   // the theme the next boot applies; a change waits for the reboot (no run-time repaint path)
        static constexpr const char *kNotes[theme::kCount] = {"light ink on a dark ground", "dark ink on paper",
                                                              "the first firmware colours"};
        out.tail = theme::name(theme::chosen());
        out.note = theme::reboot_pending() ? "reboot to apply" : kNotes[static_cast<int>(theme::chosen())];
        break;
    }
    }
}

void SettingsPage::update(uint32_t) {
    title_badge(false);
    ctx_.rows.select(sel_);
    ctx_.rows.draw(ctx_.now);
    static constexpr const char *kReboot[theme::kCount] = {"Reboot to apply: Night", "Reboot to apply: Paper",
                                                           "Reboot to apply: Classic"};
    if (theme::reboot_pending())
        draw_footer_cached(ctx_.content, FooterTone::Caution, kReboot[static_cast<int>(theme::chosen())], foot_);
    else
        draw_footer_cached(ctx_.content, FooterTone::Info,
                           sel_ >= 2 ? "Remembered across boots" : "Opens a settings page", foot_);
}

bool SettingsPage::on_event(Event e) {
    if (e.press == Press::Short && (e.button == Button::Plus || e.button == Button::Minus)) {
        sel_ = std::clamp(sel_ + (e.button == Button::Plus ? 1 : -1), 0, kRows - 1);
        update(ctx_.now);
        return true;
    }
    if (e.press == Press::Short && e.button == Button::Ok) {
        if (sel_ == 4) {   // Off -> Low -> Medium -> High -> Off; remembered, applied at once
            uiprefs::Prefs p = uiprefs::stored();
            p.smoothing = smoothing::preset_from_index((static_cast<unsigned>(power::smoothing_preset()) + 1) % smoothing::kPresetCount);
            uiprefs::set(p);
            power::set_smoothing(p.smoothing);
            update(ctx_.now);
        } else if (sel_ == 6) {   // Night -> Paper -> Classic -> Night; remembered, in force from the next boot
            uiprefs::Prefs p = uiprefs::stored();
            const auto next = theme::id_from((static_cast<unsigned>(theme::chosen()) + 1) % theme::kCount);
            p.theme = static_cast<uint8_t>(next);
            uiprefs::set(p);
            theme::choose(next);
            update(ctx_.now);
        } else if (sel_ == 5) {   // remembered in the same record, applied at once (off drops a running hold)
            uiprefs::Prefs p = uiprefs::stored();
            p.idle_wake = !fusb302::idle_wake();
            uiprefs::set(p);
            fusb302::set_idle_wake(p.idle_wake);
            update(ctx_.now);
        } else if (sel_ == 2) {
            ctx_.ui->set_rotate_buttons(!ctx_.ui->rotate_buttons());
            update(ctx_.now);
        } else if (sel_ == 3) {
            uiprefs::Prefs p = uiprefs::stored();
            p.screenshot = !uiprefs::screenshot_enabled;
            uiprefs::set(p);
            uiprefs::screenshot_enabled = p.screenshot;
            update(ctx_.now);
        } else {
            ctx_.ui->push_menu(sel_ == 0 ? Menu::UsbLink : Menu::Autorotate, ctx_.now);
        }
        return true;
    }
    return back_event(e);
}

PanelLabels SettingsPage::labels() const {
    PanelLabels labels = setting_labels(base_labels(), sel_ >= 2 ? sel_ == 4 || sel_ == 6 ? "Cycle" : "Toggle" : "Open");
    labels.b[static_cast<int>(Button::Plus)] = {"Dn", "", "", false};
    return labels;
}

void AutorotatePage::enter() {
    sel_ = rotation_choice(*ctx_.ui);
}

void AutorotatePage::draw_static() {
    title_bar();
    ctx_.rows.init(ctx_.content, kit::kBodyY, row_fn, this);
    ctx_.rows.set_count(5);
    ctx_.rows.select(sel_);
    foot_.invalidate();
}

// The active choice carries the radio dot; the focused row says what it does.
void AutorotatePage::row_fn(void *ctx, int i, bool, kit::RowSpec &out) {
    static constexpr const char *kNames[5] = {"Auto", "Manual 0 deg", "Manual 90 deg", "Manual 180 deg", "Manual 270 deg"};
    const auto *self = static_cast<const AutorotatePage *>(ctx);
    out.marker = kit::Marker::Radio;
    out.state = rotation_choice(*self->ctx_.ui) == i ? 1 : 0;
    out.text = kNames[i];
    out.note = i == 0 ? "Follows gravity" : "Fixed orientation";
}

void AutorotatePage::update(uint32_t) {
    title_badge(false);
    ctx_.rows.select(sel_);
    ctx_.rows.draw(ctx_.now);
    draw_footer_cached(ctx_.content, FooterTone::Info, "Applies now. Remembered", foot_);
}

bool AutorotatePage::on_event(Event e) {
    if (e.press != Press::Short) return false;
    if (e.button == Button::Plus || e.button == Button::Minus) {
        sel_ = std::clamp(sel_ + (e.button == Button::Plus ? 1 : -1), 0, 4);
        update(ctx_.now);
        return true;
    }
    if (e.button == Button::Ok) {
        ctx_.ui->configure_rotation(sel_ == 0, sel_ == 0 ? 0 : static_cast<uint8_t>(sel_ - 1));
        update(ctx_.now);
        return true;
    }
    return false;
}

PanelLabels AutorotatePage::labels() const { return setting_labels(base_labels(), "Apply"); }

// ---------------------------------------------------------------------------------------
// USB link submenu (what enumerates at the next boot)
// ---------------------------------------------------------------------------------------

namespace {
constexpr int kUlRows = usbcfg::rows();   // 2 when the build has no network function (K2_NET=0): no Network row
const char *ul_name(int i) { return i == 0 ? "HID" : i == 1 ? "Drive" : "Network"; }
const char *ul_hint(int i) { return i == 0 ? "logs and remote" : i == 1 ? "read-only files" : "web 192.168.7.1"; }
constexpr usbcfg::Row ul_row(int i) { return i == 0 ? usbcfg::Row::Hid : i == 1 ? usbcfg::Row::Drive : usbcfg::Row::Net; }
bool ul_on(const usbcfg::Setting &s, int i) { return i == 0 ? s.hid : i == 1 ? s.drive : s.net; }
}  // namespace

void UsbLinkPage::enter() {
    pending_ = usbcfg::stored();
    sel_ = 0;
}

void UsbLinkPage::draw_static() {
    title_bar();
    ctx_.rows.init(ctx_.content, kit::kBodyY, row_fn, this);
    ctx_.rows.set_count(kUlRows);
    ctx_.rows.select(sel_);
    foot_.invalidate();
}

// One row per USB function: its icon (green when on), the name, ON / OFF at the right and, focused, what it carries.
void UsbLinkPage::row_fn(void *ctx, int i, bool, kit::RowSpec &out) {
    const auto *self = static_cast<const UsbLinkPage *>(ctx);
    const bool on = ul_on(self->pending_, i);
    out.marker = kit::Marker::Icon;
    out.icon = i == 0 ? Icon::Plug : i == 1 ? Icon::Chip : Icon::Pc;
    out.marker_color = on ? kOk : 0;
    out.text = ul_name(i);
    out.tail = on ? "ON" : "OFF";
    out.tail_color = on ? kOk : kDim;
    out.note = ul_hint(i);
}

void UsbLinkPage::update(uint32_t) {
    title_badge(false);
    const bool differs = pending_.hid != usbcfg::current.hid || pending_.drive != usbcfg::current.drive ||
                         pending_.net != usbcfg::current.net;
    ctx_.rows.select(sel_);
    ctx_.rows.draw(ctx_.now);
    if (differs) {
        draw_footer_cached(ctx_.content, FooterTone::Caution, "Replug to apply", foot_);
    } else if (!usbcfg::any(pending_)) {
        draw_footer_cached(ctx_.content, FooterTone::Info, "USB link off", foot_);   // off is neutral information, never ok-green
    } else {
        draw_footer_cached(ctx_.content, FooterTone::Ok, "Active this boot", foot_);
    }
}

bool UsbLinkPage::on_event(Event e) {
    if (e.press != Press::Short) return false;
    if (e.button == Button::Plus || e.button == Button::Minus) {
        sel_ = (sel_ + (e.button == Button::Plus ? 1 : kUlRows - 1)) % kUlRows;
        update(ctx_.now);
        return true;
    }
    if (e.button == Button::Ok) {
        const usbcfg::Setting n = usbcfg::toggle(pending_, ul_row(sel_));
        if (usbcfg::set(n)) pending_ = n;   // flash refused: keep showing what is stored
        update(ctx_.now);
        return true;
    }
    return false;
}

PanelLabels UsbLinkPage::labels() const {
    PanelLabels l = base_labels();
    l.b[static_cast<int>(Button::Ok)] = {"Toggle", "", "", false};
    return l;
}

void UsbLinkPage::invalidate() {
    ctx_.rows.invalidate();
    foot_.invalidate();
}

// ---------------------------------------------------------------------------------------
// QR scan-test screen (see k2/ui/ui_qrtest.hpp)
// ---------------------------------------------------------------------------------------

void QrTestPage::enter() {
    const UiContext::QrTestReq r = ctx_.qrtest;   // set by the remote before the page is pushed
    version_ = r.version;
    px_ = r.px;
    quiet_ = r.quiet;
    n_ = 0;
    link_len_ = 0;
    painted_ = false;
}

void QrTestPage::draw_static() {
    // Nothing here: the code is drawn in update(), which runs after draw_panel(), so it covers the button strip
    // (chrome hidden while the test screen is shown). The erase pass replays update() the same way.
}

void QrTestPage::update(uint32_t) {
    if (!painted_) paint();
}

void QrTestPage::paint() {
    ContentViewport::Pause unscaled;   // the code is drawn in panel pixels over the whole 240x240
    struct LineWork { std::uint16_t line[qrview::kPanelPx]; };
    scratch::Lease lease("qrtest");   // only the pixel line is leased; the workspace is the page's own member
    auto &lw = scratch::as<LineWork>();
    link_len_ = qrtest::build_link(version_, px_, std::span<char>(reinterpret_cast<char*>(ws_.temp), qrtest::kWsBufLen));
    n_ = link_len_ != 0 && version_ >= qrtest::kVersionMin && version_ <= qrtest::kMaxShownVersion
             ? qr::encode_placed(ws_.code, ws_.temp, link_len_, version_)
             : 0;
    lcd::fill(qrview::kWhite);   // the whole panel, button strip included (erase pass: to the background)
    if (n_ != 0)
        qrview::draw_code_raw_in(qrview::Area{}, ws_.code, n_, 0, px_, lw.line, quiet_, px_);
    painted_ = true;
}

bool QrTestPage::on_event(Event) {
    return false;   // every button is declined: a short BACK pops the page (the Ui does it)
}

PanelLabels QrTestPage::labels() const {
    PanelLabels l = base_labels();
    for (Button b : {Button::Ok, Button::Minus, Button::Plus}) l.b[static_cast<int>(b)] = {};
    return l;
}

}  // namespace k2
