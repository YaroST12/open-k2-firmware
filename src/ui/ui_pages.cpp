// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#include "k2/ui/ui_pages.hpp"

#include "k2/ui/readout_fmt.hpp"
#include "k2/ui/trend.hpp"
#include "k2/ui/uiprefs.hpp"
#include "k2/ui/ui_display.hpp"
#include "k2/core/held.hpp"
#include "k2/core/idle_hw.hpp"
#include "k2/ui/ui_lines.hpp"
#include "k2/ui/ui_tools.hpp"
#include "k2/ui/text/tools.hpp"
#include "k2/ui/ui_emarker.hpp"
#include "k2/core/build_config.h"
#include "k2/core/log.hpp"
#include "k2/core/strpack.hpp"
#include "k2/core/math_opt.hpp"
#include "k2/pd/pd_emarker_plan.hpp"
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
#include "k2/core/hw_revision.hpp"
#include "k2/core/version.hpp"
#include "k2/core/bootinfo_hw.hpp"
#include "k2/drivers/usbcfg.hpp"
#include "k2/pd/link.hpp"
#include "k2/ui/ui.hpp"
#include "k2/ui/ui_style.hpp"
#include "k2/ui/text.hpp"

namespace k2 {

namespace {

// Whole volts without a decimal, otherwise one decimal: 5, 20, 3.3, 9.5.
void put_volts(StrBufBase &s, uint32_t mv) {
    pg::put_exact_unit(s, (mv / 100u) * 100u, 0);
}

}  // namespace

namespace pg {
bool put_field_buf(FieldBase &f, char *cur, PaintBudget *bg, std::string_view s, uint16_t fg, uint16_t bg_col) {
    if (bg && !f.valid() && !bg->admit(f.box_px(), true)) return false;
    f.set_colors(fg, bg_col);
    char dec[FieldBase::kMaxChars + 1];
    f.draw_buf(cur, strpack::unpack(s, dec));   // a Field diffs raw bytes
    return true;
}

void invalidate_fields(Field *f, int n) {
    for (int i = 0; i < n; i++) f[i].invalidate();
}

void clear_body(const Surface &c, int16_t y0) {
    if (y0 < kFooterY) gfx_fill(c, 0, y0, c.w, static_cast<int16_t>(kFooterY - y0), kBg);
}

std::string_view stash_str(char *dst, std::size_t cap, std::string_view s) {
    K2_EXPECT_PLAIN(s);
    const std::size_t n = std::min(s.size(), cap - 1);
    std::memcpy(dst, s.data(), n);
    dst[n] = 0;
    return {dst, n};
}

[[gnu::noinline]] uint32_t mix_cstr(uint32_t h, const char *s, std::size_t max_len) {
    if (!s) return h;
    for (std::size_t i = 0; i < max_len && s[i]; i++) h = (h ^ static_cast<uint8_t>(s[i])) * 16777619u;
    return h;
}

[[gnu::noinline]] uint32_t mix_u32s(uint32_t h, const uint32_t *w, std::size_t n) {
    for (std::size_t i = 0; i < n; i++) h = mix_u32(h, w[i]);
    return h;
}

[[gnu::noinline]] void put_exact_unit(StrBufBase &out, uint32_t milli, char unit) {
    if (milli % 1000 == 0) out.put_u32(milli / 1000);
    else out.put_fixed(static_cast<int32_t>(milli), milli % 100 == 0 ? 1 : milli % 10 == 0 ? 2 : 3);
    if (unit) out.put_char(unit);
}

[[gnu::noinline]] void put_unit_or_dash(StrBufBase &out, bool ok, int32_t val, int dec, char unit, bool abs_val) {
    out.clear();
    if (!ok) { out.put("--"); return; }
    if (abs_val) out.put_abs_fixed(val, dec);
    else out.put_fixed(val < 0 ? 0 : val, dec);
    if (unit) out.put_char(unit);
}

[[gnu::noinline]] std::string_view trim_left(std::string_view s) {
    while (!s.empty() && s.front() == ' ') s.remove_prefix(1);
    return s;
}
}  // namespace pg


void lines_row_text(int i, const std::optional<adc::Reading> &r, fastcharge::Protocol p, StrBuf<24> &s) {
    auto mv_row = [&](const char *label, int32_t mv) {
        s.put(label);
        if (r) s.put_u32(static_cast<uint32_t>(mv < 0 ? 0 : mv)).put(" mV");
        else s.put("--");
    };
    switch (i) {
    case 0: mv_row(K2_P("D+   "), r ? r->dp_mv : 0); break;
    case 1: mv_row(K2_P("D-   "), r ? r->dm_mv : 0); break;
    case 2: mv_row(K2_P("CC1  "), r ? r->cc1_mv : 0); break;
    case 3: mv_row(K2_P("CC2  "), r ? r->cc2_mv : 0); break;
    case 4:
        s.put(K2_P("AUX  "));
        if (r) s.put_fixed(r->aux_mv, 3).put(" V"); else s.put("--");
        break;
    case 5:
        s.put(K2_P("MCU  "));
        if (r && r->mcu.valid) {
            s.put_fixed(r->mcu.deci_c * 100, 1).put(" C");
            if (!r->mcu.calibrated) s.put(K2_P(" uncal"));
        } else {
            s.put("--");
        }
        break;
    case 6:   // board temperature: live data showed no exposed channel (ch1/ch6/ch7) tracks it
        s.put(K2_P("BRD  n/a (no sensor)"));
        break;
    case 7:
        s.put(K2_P("VREF "));
        if (r) s.put_u32(r->vref_raw).put(" ").put_fixed(r->vdd_mv, 2).put(" V"); else s.put("--");
        break;
    default:
        s.put(K2_P("FC   ")).put(fastcharge::name(p));
    }
}

void cable_row_text(int i, bool online, int32_t vbus_mv, int32_t ibus_ma, const cable_r::Meter &m, StrBuf<24> &s) {
    const auto est = m.estimate();
    switch (i) {
    case 0:
        if (online) s.put_fixed(vbus_mv, 2).put("V ").put_abs_fixed(ibus_ma, 3).put("A");
        else s.put(K2_P("-- no power monitor"));
        break;
    case 1:
        if (m.has_idle()) s.put(K2_P("IDLE ")).put_fixed(m.idle_mv(), 2).put("V ").put_abs_fixed(m.idle_ma(), 2).put("A");
        else s.put(K2_P("IDLE waiting <0.2A"));
        break;
    case 2:
        if (est) s.put(K2_P("LOAD ")).put_fixed(est->load_mv, 2).put("V ").put_abs_fixed(est->load_ma, 2).put("A");
        else s.put(m.has_idle() ? K2_P("LOAD need +1.0A step") : K2_P("LOAD --"));
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
        else s.put(K2_P("-- no power monitor"));
        break;
    case 1:
        if (k) { s.put("POS "); mohm(k->pos_mohm, s); s.put(K2_P(" GND ")); mohm(k->gnd_mohm, s); }
        else s.put(K2_P("POS -- GND --"));
        break;
    case 2: {
        const int32_t a = power::abs_sat(ibus_ma);
        if (k) s.put(K2_P("SENSE ok"));
        else s.put(a < cable_r::kKelvinMinMa ? K2_P("LOAD need 0.45A+") : K2_P("SENSE check fixture"));
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
        s.put(K2_P("MCU --"));
        return;
    }
    const int32_t d = r->mcu.deci_c;
    const int32_t whole = d >= 0 ? (d + 5) / 10 : -((-d + 5) / 10);   // round half away from zero
    if (whole > 99) s.put(K2_P("MCU>99"));        // keep the field at 6 characters
    else if (whole < -9) s.put(K2_P("MCU<-9"));
    else s.put("MCU").put_fixed(whole * 1000, 0).put_char('C');
}

void model_row_text(StrBuf<24> &s) {
    s.put("HW ").put(hw_revision::text());
}

// ---------------------------------------------------------------------------------------
// Shared page helpers
// ---------------------------------------------------------------------------------------

// Short bar names of the base pages, in Page order (a place page names itself: bar_icon_ + title_).
static constexpr rail::Title kBarNames[] = {rail::kHome, rail::kNum, rail::kTest, rail::kLine, rail::kK2, rail::kGraph, rail::kRipple};
static_assert(sizeof kBarNames / sizeof *kBarNames == static_cast<size_t>(Page::Count), "one bar name per rotation page");

kit::OppRailSpec PageBase::opp_rail() const {
    return {(is_menu() || bar_icon_) ? title_ : kBarNames[static_cast<int>(id_)], 0, 0, kit::OppTelemetry::Vaw};
}

// Option B (Pure 2-Side-Bar): title, pager dots and live telemetry live on the 16 px Opposite Telemetry Rail,
// so the legacy horizontal TopBar helpers are no-ops.
void PageBase::title_bar() { bar_valid_ = true; }
void PageBase::title_badge(bool) { bar_valid_ = true; }

bool PageBase::scroll_event(ListView &lv, Event e, uint32_t now_ms) {
    if (e.press != Press::Short) return false;
    if (e.button != Button::Plus && e.button != Button::Minus) return false;
    if (!lv.state().overflows()) return false;
    lv.scroll(e.button == Button::Plus ? 1 : -1, now_ms, ctx_.budget);
    return true;
}

Strip PageBase::base_labels() const {
    return nav_strip();   // modal menus too: short [+] / [-] scroll or step, BACK leaves
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
    char dec[12];
    const std::string_view n = strpack::unpack(fastcharge::short_name(p), dec);
    std::size_t len = 0;
    while (len < n.size() && n[len] != ' ' && len < 4) len++;
    if (len == 4 && n[3] == '.') len = 3;                           // "QC3.0" -> "QC3"
    if (len >= 3 && n[len - 1] == '.') len--;
    out.put(n.substr(0, len));
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
    if (!em.valid) { out.put(K2_P("no e-marker")); return; }
    out.put_u32(em.max_current_a).put("A ").put_u32(em.max_power_w).put_char('W');
}

void detail_cable_text(const pd::Emarker &em, StrBuf<28> &out) {
    if (!em.valid) { out.put(K2_P("no e-marker")); return; }
    out.put_u32(em.max_current_a).put("A ").put_u32(em.max_power_w).put("W ").put_u32(em.max_voltage_v).put_char('V');
    if (em.epr_capable) out.put(" EPR");
    if (em.is_active) out.put(K2_P(" active"));
}

void detail_offer_text(const pd::SourceCaps &caps, StrBuf<28> &out) {
    if (!caps.valid || caps.num_pdos == 0) return;
    uint32_t mw = 0;
    for (int i = 0; i < caps.num_pdos; i++) mw = caps.pdos[i].max_mw > mw ? caps.pdos[i].max_mw : mw;
    out.put_u32(mw / 1000).put(K2_P("W max, ")).put_u32(static_cast<uint32_t>(caps.num_pdos)).put(" PDO");
}


void home_cc_text(int cc, int32_t mv, StrBuf<10> &out) {
    out.put("CC").put_char(cc == 1 ? '1' : cc == 2 ? '2' : '-').put_char(' ');
    if (mv < 0) out.put("-.--"); else out.put_fixed(mv, 2, 0);
    out.put_char('V');
}

const char *home_idle_note(uint8_t mode) { return mode == 1 ? K2_P("No source") : mode == 0 ? "" : nullptr; }

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
    if (t.view().size() + 9 <= kContentChars) out.put(K2_P("Contract "));
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
constexpr const char *kEmKey[kEmDetailRows] = {"", K2_P("VID/PID"), "XID", "PD", K2_P("ID VDO"), K2_P("Cable VDO"), K2_P("Rating")};
static_assert(all_fit_p(kEmKey, kLabelAdv, 104 - kit::kTextX - 4), "a saved-cable key is wider than its column");
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

// The saved cable's name for list row `i`, from the slot cache (a window is at most 7 rows, so i % 7 never collides).
std::string_view EmarkerPage::name_of(int i) {
    const int slot = i % kNameSlots;
    if (name_idx_[slot] != i) {
        name_idx_[slot] = static_cast<uint8_t>(i);
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
        out.text = K2_P("No saved cables");
        out.text_color = kDim;
        return;
    }
    out.text = self->name_of(i);
    out.data = true;
    out.chevron = true;
}

void EmarkerPage::update(uint32_t now_ms) {
    title_badge(false);
    if (note_until_ && static_cast<int32_t>(now_ms - note_until_) >= 0) { note_until_ = 0; note_.clear(); }
    const Surface &c = ctx_.content;
    const int n = n_seqs_;
    const int idx = menu_.idx;
    const uint32_t vsig = static_cast<uint32_t>(menu_.view);
    if (vsig != view_sig_) {   // a view change wipes the body once and forgets every cached piece
        view_sig_ = vsig;
        pg::clear_body(c, kit::kBodyY);
        body_.invalidate();
        ctx_.rows.invalidate();
    }
    if (menu_.view != EmView::List) load_cur();
    if (menu_.view == EmView::List) {
        ctx_.rows.set_count(n > 0 ? n : 1);
        ctx_.rows.select(n > 0 ? idx : 0);
        ctx_.rows.draw(now_ms, ctx_.budget);
    } else {
        const bool confirm = menu_.view == EmView::Confirm;
        uint32_t sg = mix_u32(mix_init(), vsig) + static_cast<uint32_t>(have_cur_) * 3u;
        if (have_cur_) sg = pg::mix_str(mix_u32(sg, cur_.seq), cur_.name.data());
        body_.paint(sg, [&] {
            if (!have_cur_) return;
            if (confirm) {
                kit::draw_alert_band(c, kit::kBodyY, false, K2_P("DELETE CABLE?"));
                char buf[kFitBuf];
                gfx_text(c, kSmall, kit::kTextX, kEmConfirmY, fit_data(kSmall, cur_.name.data(), 192, buf), kText, kBg);
                gfx_text(c, kSmall, kit::kTextX, static_cast<int16_t>(kEmConfirmY + 22), K2_P("Erased for good"), kDim, kBg);
                return;
            }
            StrBuf<24> v;
            for (int i = 0; i < kEmDetailRows; i++) {
                v.clear();
                em_detail_value(i, cur_, v);
                kit::kv_row(c, i == 0 ? kEmNameKv : kEmKv, kit::kv_row_y(kEmKv, i), kEmKey[i], v.view(), kText);
            }
            gfx_fill(c, 0, kit::kv_row_y(kEmKv, kEmDetailRows), c.w, 1, kRule);
            draw_icon(c, kit::kMarkX, static_cast<int16_t>(kit::kBodyY + 7), Icon::Emark, kDim, kBg);
        });
    }
    if (ctx_.budget && ctx_.budget->more) return;
    StrBuf<27> st;
    FooterTone tone = FooterTone::Info;   // counts and positions are neutral facts
    if (note_until_) {
        st.put(note_.view());
        tone = note_caution_ ? FooterTone::Caution : FooterTone::Info;
    } else if (n == 0) {
        st.put(K2_P("Nothing saved yet"));
    } else {
        st.put(K2_P("Cable ")).put_u32(static_cast<uint32_t>(idx + 1)).put(" of ").put_u32(static_cast<uint32_t>(n));
        if (menu_.view == EmView::Confirm) { st.put(K2_P(". Not deleted")); tone = FooterTone::Caution; }
    }
    if (note_until_ || menu_.view == EmView::Confirm) {
        draw_footer_cached(c, tone, st.view(), foot_);
    } else {
        foot_.paint_budgeted(1, ctx_.budget, static_cast<int>(c.w) * 20, [&] {
            gfx_fill(c, 0, static_cast<int16_t>(c.h - 20), c.w, 20, kBg);
        });
    }
}

// The shown cable's entry, re-read when the selection or the library changed.
void EmarkerPage::load_cur() {
    const std::uint16_t seq = menu_.idx >= 0 && menu_.idx < n_seqs_ ? seqs_[menu_.idx] : 0;
    if (seq == cur_seq_ && (seq == 0 ? !have_cur_ : have_cur_)) return;
    cur_seq_ = seq;
    have_cur_ = seq != 0 && storage::lib_get(seq, cur_);
}

// The shown saved cable (with its quirk table) as a share record, then the share view. The record goes straight into the
// view's buffer. The quirk table (420 bytes) is leased from scratch::arena() and released before open_share() paints.
void EmarkerPage::share_cur() {
    if (menu_.idx < 0 || menu_.idx >= n_seqs_) return;
    std::size_t len = 0;
    {
        const scratch::Lease lease("emk.share");   // k2-plain: scratch owner tag
        auto &quirks = scratch::make<emk_quirk::Quirks>();
        if (!storage::lib_get(seqs_[menu_.idx], cur_, quirks)) return;
        cur_seq_ = seqs_[menu_.idx];
        have_cur_ = true;
        len = qrshare::records::emarker(cur_.rec, cur_.name.data(), quirks, ctx_.ui->share_buffer());
    }
    ctx_.ui->open_share(qrshare::Type::Emarker, qrshare::kSchemaEmarker, cur_.name.data(), len, ctx_.now);
}

void EmarkerPage::refresh_lib() {
    n_seqs_ = static_cast<int>(storage::lib_list(std::span<std::uint16_t>(seqs_)));
    menu_.set_count(n_seqs_);
    cur_seq_ = 0;
    for (uint8_t &i : name_idx_) i = 0xFF;
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
        note_.clear();
        note_.put(ok ? K2_P("Deleted") : K2_P("Flash error"));
        note_caution_ = !ok;
        note_until_ = ctx_.now + 2500;
        if (note_until_ == 0) note_until_ = 1;
    }
    refresh_lib();
}

// Pushed from Connection's "Saved cables": the list view, a fresh library read, the first cable selected.
void EmarkerPage::enter() {
    menu_ = EmMenu();
    note_.clear();
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

Strip EmarkerPage::labels() const {
    Strip l = base_labels();
    switch (menu_.view) {
    case EmView::List:
        l = list_strip(l);
        l[Button::Ok] = n_seqs_ > 0 ? sl::View : sl::None;
        break;
    case EmView::Detail:
        l = list_strip(l);
        l[Button::Ok] = sl::DeleteShare;
        break;
    case EmView::Confirm:
        l[Button::Ok] = sl::Delete;
        l[Button::Minus] = l[Button::Plus] = sl::None;
        break;
    }
    return l;
}

// BACK or the remote's set-page: the next visit starts on the list again.
void EmarkerPage::leave() {
    menu_ = EmMenu();
    note_.clear();
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
constexpr kit::KvLayout kDgLayout{kit::kBodyY, 22, kit::kTextX, &kLabelFont, nullptr, 204 - 17 * 8, 17, Align::Right};
// Worst case per row ("4294M / 4294M" is 13 chars, RX 17): each cell is only as wide as its text, clear of the S12 keys.
constexpr uint8_t kDgValueChars[DiagPage::kRows] = {7, 13, 13, 7, 17, 13, 7, 11, 11};
const char *const kDgLabel[DiagPage::kRows] = {"Link", K2_P("RST/ISR"), K2_P("TX/drop"), K2_P("FUSB302"), "RX", K2_P("Poll gap"), K2_P("Meter"), K2_P("Uptime"), K2_P("Model")};
}  // namespace

void DiagPage::draw_static() {
    const Surface &c = ctx_.content;
    PaintBudget *const bg = ctx_.budget;
    if (static_step_ == 0) {
        if (bg && !bg->admit(140 * kBarH, true)) return;
        title_bar();
        kit::kv_cells(c, kDgLayout, val_, kRows, kDgValueChars);
        marks_ = 0xFF;
        foot_.invalidate();
        pg::invalidate_fields(val_, kRows);
        static_step_ = 1;
    }
    constexpr uint32_t kMarksMask = (1u << 0) | (1u << 3) | (1u << 6);
    while (static_step_ <= kRows) {
        const int i = static_step_ - 1;
        const int row_h = kDgLayout.pitch + (i + 1 == kRows ? 1 : 0);
        const int cost = ctx_.blank ? 900 : static_cast<int>(c.w) * row_h;
        if (bg && !bg->admit(cost, true)) return;
        if (!ctx_.blank) kit::kv_ground_row(c, kDgLayout, kDgLabel[i], i, kDgValueChars, kMarksMask);
        kit::kv_static_row(c, kDgLayout, kDgLabel[i], i, i + 1 == kRows);
        static_step_++;
    }
    if (static_step_ == kRows + 1) {
        const int16_t below = static_cast<int16_t>(kit::kv_row_y(kDgLayout, kRows) + 1);
        if (!ctx_.blank && below < c.h) {
            const int cost = static_cast<int>(c.w) * (c.h - below);
            if (bg && !bg->admit(cost, true, true)) return;
            gfx_fill(c, 0, below, c.w, static_cast<int16_t>(c.h - below), kBg);
        }
        static_step_++;
    }
}

void DiagPage::paint_marks(bool usb_ok, bool fusb_ok, bool meter_ok, bool meter_wait) {
    const uint8_t sig = static_cast<uint8_t>(usb_ok | (fusb_ok << 1) | (meter_ok << 2) | (meter_wait << 3));
    if (sig == marks_) return;
    if (ctx_.budget && !ctx_.budget->admit(3 * kit::kMarkSize * kit::kMarkSize, true)) return;
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
    PaintBudget *const bg = ctx_.budget;
    title_badge(false);
    if (bg && bg->more) return;
    const bool on = ctx_.pm_online;
    const bool meter_wait = home::meter_starting(on, now_ms);   // still being found: not yet an error
    const uint8_t fusb_id = fusb302::device_id();
    StrBuf<24> t;
    for (int i = 0; i < kRows; i++) {
        t.clear();
        uint16_t col = kText;
        switch (i) {
        case 0: t.put(live_.configured ? K2_P("ready") : K2_P("waiting")); break;
        // Capped counts (10K, 999M, ">999M") are at most 5 chars, so "a / b" is at most 13 and always fits.
        case 1: t.put_count_capped(live_.resets).put(" / ").put_count_capped(live_.isrs); break;
        case 2:
            t.put_count_capped(live_.log_sent).put(" / ").put_count_capped(live_.log_dropped);
            // Amber means "log lines were dropped"; a saturated count is a garbage cue, so it stays neutral.
            if (live_.log_dropped > 0 && !count_overflowed(live_.log_dropped) && !count_overflowed(live_.log_sent))
                col = kCaution;
            break;
        case 3: t.put(K2_P("ID 0x")).put_hex(fusb_id, 2); break;
        case 4: t.put_u32(live_.rx_count).put(live_.cc == 1 ? K2_P(" on CC1") : live_.cc == 2 ? K2_P(" on CC2") : K2_P(" on CC-")); break;
        case 5: t.put_u32(live_.poll_gap_max).put(" ms"); break;
        case 6:
            if (on) t.put(power::name(power::chip()));
            else { t.put(meter_wait ? "wait" : K2_P("missing")); if (!meter_wait) col = kError; }
            break;
        case 7: t.put_hms(now_ms / 1000); break;
        default: model_row_text(t); break;
        }
        if (!pg::put_field(val_[i], bg, t.view(), col)) return;
    }
    paint_marks(live_.configured, fusb_id != 0, on, meter_wait);
    if (bg && bg->more) return;
    const bool usb_ok = live_.configured, fusb_ok = fusb_id != 0;
    const int state = !on ? (meter_wait ? 4 : 1) : !usb_ok ? 2 : !fusb_ok ? 3 : 0;
    struct DiagFoot { FooterTone tone; const char *msg; };
    static constexpr DiagFoot kDiagFoot[5] = {
        {FooterTone::Ok, K2_P("All subsystems up")},
        {FooterTone::Error, K2_P("Power meter not found")},
        {FooterTone::Caution, K2_P("USB not configured")},
        {FooterTone::Error, K2_P("FUSB302 not answering")},
        {FooterTone::Info, K2_P("Power meter starting")},
    };
    if (kDiagFoot[state].tone == FooterTone::Error || kDiagFoot[state].tone == FooterTone::Caution)
        draw_toast(ctx_.content, kDiagFoot[state].tone, kDiagFoot[state].msg);
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
    return false;
}

Strip DiagPage::labels() const {
    Strip l = base_labels();
    l[Button::Ok] = sl::ShareLong;
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
    static_step_ = 0;
    marks_ = 0xFF;
    foot_.invalidate();
    for (Field &f : val_) f.invalidate();
}

// ---------------------------------------------------------------------------------------
// Memory: stacks, static RAM, littlefs and the firmware image, from the cached memstat snapshot
// ---------------------------------------------------------------------------------------

namespace {
// Nine rows at a 22 px pitch (as Diag), S12 keys at x6, S16 values right-aligned to x204 in one 16-char box (208 px canvas).
constexpr kit::KvLayout kMemLayout{kit::kBodyY, 22, 6, &kLabelFont, nullptr, 204 - memstat::kValueMax * 8, memstat::kValueMax, Align::Right};
constexpr std::array<uint8_t, memstat::kRows> kMemValueChars = [] {
    std::array<uint8_t, memstat::kRows> a{};
    for (auto &v : a) v = memstat::kValueMax;
    return a;
}();
}  // namespace

void MemoryPage::draw_static() {
    const Surface &c = ctx_.content;
    PaintBudget *const bg = ctx_.budget;
    if (static_step_ == 0) {
        if (bg && !bg->admit(140 * kBarH, true)) return;
        title_bar();
        if (!ctx_.blank && kit::kBodyY > 0) gfx_fill(c, 0, 0, c.w, kit::kBodyY, kBg);
        kit::kv_cells(c, kMemLayout, val_, kRows, kMemValueChars.data());
        foot_.invalidate();
        pg::invalidate_fields(val_, kRows);
        static_step_ = 1;
    }
    while (static_step_ <= kRows) {
        const int i = static_step_ - 1;
        const int row_h = kMemLayout.pitch + (i + 1 == kRows ? 1 : 0);
        const int cost = ctx_.blank ? 900 : static_cast<int>(c.w) * row_h;
        if (bg && !bg->admit(cost, true)) return;
        const char *label = memstat::row_label(static_cast<memstat::Row>(i));
        if (!ctx_.blank) kit::kv_ground_row(c, kMemLayout, label, i, kMemValueChars.data(), 0);
        kit::kv_static_row(c, kMemLayout, label, i, i + 1 == kRows);
        static_step_++;
    }
    if (static_step_ == kRows + 1) {
        const int16_t below = static_cast<int16_t>(kit::kv_row_y(kMemLayout, kRows) + 1);
        if (!ctx_.blank && below < c.h) {
            const int cost = static_cast<int>(c.w) * (c.h - below);
            if (bg && !bg->admit(cost, true)) return;
            gfx_fill(c, 0, below, c.w, static_cast<int16_t>(c.h - below), kBg);
        }
        static_step_++;
    }
}

void MemoryPage::update(uint32_t) {
    PaintBudget *const bg = ctx_.budget;
    title_badge(false);
    if (bg && bg->more) return;
    const memstat::Snapshot s = memstat::snapshot();   // the cached figures; the main loop refreshes them
    char t[memstat::kValueMax + 1];
    for (int i = 0; i < kRows; i++) {
        const auto row = static_cast<memstat::Row>(i);
        const std::size_t n = memstat::row_text(row, s, std::span<char>(t, memstat::kValueMax));
        const memstat::Level lv = memstat::row_level(row, s);
        if (!pg::put_field(val_[i], bg, std::string_view(t, n),
                       lv == memstat::Level::Error ? kError : lv == memstat::Level::Caution ? kCaution : kText))
            return;
    }
    if (bg && bg->more) return;
    const memstat::Level lv = memstat::footer_level(s);
    char f[memstat::kFooterMax + 1];
    const std::size_t n = lv == memstat::Level::Ok ? 0 : memstat::footer_text(s, std::span<char>(f, memstat::kFooterMax));
    const FooterTone tone = lv == memstat::Level::Error ? FooterTone::Error
                          : lv == memstat::Level::Caution ? FooterTone::Caution : FooterTone::Info;
    const std::string_view fv(f, n);
    const uint32_t fsig = mix(mix_u32(mix_init(), static_cast<uint32_t>(lv)), fv);
    const bool had_toast = foot_.valid;
    foot_.paint_budgeted(fsig, bg, static_cast<int>(ctx_.content.w) * 16, [&] {
        if (lv != memstat::Level::Ok) {
            draw_toast(ctx_.content, tone, fv);
        } else if (had_toast && !ctx_.toast.active(ctx_.now)) {
            erase_toast(ctx_.content);
        }
    });
}

bool MemoryPage::on_event(Event e) {
    if (e.press == Press::Short && e.button == Button::Ok) {
        std::uint32_t erased = 0;
        const bool ok = memstat::scrub_storage(erased);
        if (!ok) {
            ctx_.toast.show(FooterTone::Error, K2_P("Scrub failed"), ctx_.now);
        } else if (erased == 0) {
            ctx_.toast.show(FooterTone::Ok, K2_P("Storage clean"), ctx_.now);
        } else {
            StrBuf<24> msg;
            msg.put(K2_P("Scrubbed ")).put_u32(erased).put(erased == 1 ? K2_P(" block") : K2_P(" blocks"));
            ctx_.toast.show(FooterTone::Ok, msg.view(), ctx_.now);
        }
        return true;
    }
    return false;
}

Strip MemoryPage::labels() const {
    Strip s = base_labels();
    s[Button::Ok] = sl::Scrub;
    return s;
}

void MemoryPage::invalidate() {
    static_step_ = 0;
    foot_.invalidate();
    pg::invalidate_fields(val_, kRows);
}

// ---------------------------------------------------------------------------------------
// Lines: the analog front end
// ---------------------------------------------------------------------------------------

namespace {
// Meter > Lines (Option B 208x240): four 26 px line rows from y 4, dashed threshold lines at 0.4 V and 2.0 V
// down to axis labels, verdict chips, and 3 receipt rows (Rp, D+/D-, VREF + MCU temp). Zero footer.
constexpr int16_t kLnRowY0 = 4, kLnRowPitch = 26;
constexpr int16_t kLnRowY(int i) { return static_cast<int16_t>(kLnRowY0 + i * kLnRowPitch); }
constexpr int16_t kLnChipX = 6, kLnChipW = 28, kLnChipDy = 6;
constexpr int16_t kLnValX = 40, kLnValChars = 5, kLnValDy = 5;
constexpr int16_t kLnBarX = 94, kLnBarH = 10, kLnBarDy = 8;
constexpr int16_t kLnAxisY = kLnRowY0 + 4 * kLnRowPitch;   // 108: the dashes stop here, their labels under it
constexpr int16_t kLnAxisTextY = kLnAxisY + 3, kKeyAxisX = 6;
constexpr int16_t kVerdictY = 128;                          // the verdict chips
constexpr int16_t kLnDash = 2;                              // the threshold lines: 2 px dash, 2 px gap
static_assert(kLnBarX + kLinesBarW <= 206, "the bars end inside the 208 px content");
constexpr const char *kLnTag[4] = {"D+", "D-", "CC1", "CC2"};
constexpr int32_t kLnFaintMv = 100;                         // a line under 0.1 V reads as nothing on it
// Three receipt rows (Rp, D+/D-, VREF): key font 16 at x 6, dot leader, 12-char value right-aligned to x 202.
constexpr kit::KvLayout kLnKv{154, 26, 6, nullptr, nullptr, static_cast<int16_t>(202 - 12 * 8), 12, Align::Right};
static_assert(kLnBarX + kLinesBarW == 202, "the bars end on the receipt values' right edge");
constexpr const char *kLnReceiptLabels[3] = {"Rp", K2_P("D+/D-"), "VREF"};

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
void LinesPage::draw_static() {
    const Surface &c = ctx_.content;
    PaintBudget *const bg = ctx_.budget;
    const int16_t lo = tick_x(kLinesTickLowMv), hi = tick_x(kLinesTickHighMv);
    if (static_step_ == 0) {
        if (bg && !bg->admit(140 * kBarH, true)) return;
        title_bar();
        for (int i = 0; i < 4; i++) {
            const int16_t y = kLnRowY(i);
            v_[i].init(c, kSmall, kLnValX, static_cast<int16_t>(y + kLnValDy), kLnValChars, Align::Right, kText, kBg);
            px_[i] = -1;
        }
        kit::kv_cells(c, kLnKv, pv_, 3);
        painted_ = false;
        vsig_ = 0;
        static_step_ = 1;
    }
    while (static_step_ <= 4) {
        const int i = static_step_ - 1;
        const int16_t y = kLnRowY(i), top = i ? y : kit::kBelowBar;
        const int16_t h = static_cast<int16_t>(y + kLnRowPitch - top);
        const int cost = ctx_.blank ? 1000 : 3800;
        if (bg && !bg->admit(cost, true)) return;
        if (!ctx_.blank) {
            const kit::Box holes[5] = {
                {kLnChipX, static_cast<int16_t>(y + kLnChipDy), kLnChipW, kit::kChipH},
                {kLnValX, static_cast<int16_t>(y + kLnValDy), static_cast<int16_t>(kLnValChars * kSmall.adv), kSmall.height},
                {kLnBarX, static_cast<int16_t>(y + kLnBarDy), kLinesBarW, kLnBarH},
                {lo, top, 1, h}, {hi, top, 1, h}};
            kit::fill_except(c, {0, top, c.w, h}, holes, 5, kBg);
        }
        kit::draw_chip(c, kLnChipX, static_cast<int16_t>(y + kLnChipDy), kLnTag[i], kInk, kBg, kLnChipW);
        for (int16_t x : {lo, hi}) {   // the threshold columns around the bar (the bar paints its own part)
            dash_column(c, x, top, static_cast<int16_t>(y + kLnBarDy));
            dash_column(c, x, static_cast<int16_t>(y + kLnBarDy + kLnBarH), static_cast<int16_t>(y + kLnRowPitch));
        }
        static_step_++;
    }
    if (static_step_ == 5) {
        const int cost = ctx_.blank ? 600 : 5200;
        if (bg && !bg->admit(cost, true)) return;
        if (!ctx_.blank) {
            const kit::Box tags[3] = {
                {kKeyAxisX, kLnAxisTextY, static_cast<int16_t>(text_width(kLabelFont, K2_P("volts"))), kLabelFont.height},
                {static_cast<int16_t>(lo - 9), kLnAxisTextY, static_cast<int16_t>(text_width(kLabelFont, "0.4")), kLabelFont.height},
                {static_cast<int16_t>(hi - 9), kLnAxisTextY, static_cast<int16_t>(text_width(kLabelFont, "2.0")), kLabelFont.height}};
            const int16_t chips = kVerdictY + 2;
            kit::fill_except(c, {0, kLnAxisY, c.w, static_cast<int16_t>(chips - kLnAxisY)}, tags, 3, kBg);
            gfx_fill(c, 0, static_cast<int16_t>(chips + kit::kChipH), c.w, static_cast<int16_t>(kLnKv.y0 - chips - kit::kChipH), kBg);
        }
        gfx_text(c, kLabelFont, kKeyAxisX, kLnAxisTextY, K2_P("volts"), kFaint, kBg);
        gfx_text(c, kLabelFont, static_cast<int16_t>(lo - 9), kLnAxisTextY, "0.4", kDim, kBg);
        gfx_text(c, kLabelFont, static_cast<int16_t>(hi - 9), kLnAxisTextY, "2.0", kDim, kBg);
        static_step_ = 6;
    }
    while (static_step_ >= 6 && static_step_ < 9) {
        const int r = static_step_ - 6;
        const int cost = ctx_.blank ? 600 : 3800;
        if (bg && !bg->admit(cost, true)) return;
        if (!ctx_.blank) kit::kv_ground_row(c, kLnKv, kLnReceiptLabels[r], r, nullptr, 0);
        kit::kv_static_row(c, kLnKv, kLnReceiptLabels[r], r, r == 2);
        static_step_++;
    }
    if (static_step_ == 9) {
        const int16_t below = static_cast<int16_t>(kit::kv_row_y(kLnKv, 3) + 1);
        if (!ctx_.blank && below < c.h) {
            const int cost = static_cast<int>(c.w) * (c.h - below);
            if (bg && !bg->admit(cost, true)) return;
            gfx_fill(c, 0, below, c.w, static_cast<int16_t>(c.h - below), kBg);
        }
        static_step_ = 10;
    }
}

// The bar: ink fill on the bg2 track; the threshold dashes show through the track, the fill covers them.
void LinesPage::paint_bar(int i, int32_t mv, bool have) {
    const int px = have ? lines_bar_px(mv) : 0;
    if (px == px_[i]) return;
    if (ctx_.budget && !ctx_.budget->admit(static_cast<int>(kLinesBarW) * kLnBarH, true)) return;
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

bool LinesPage::paint_verdict(const std::optional<adc::Reading> &r, uint32_t now_ms) {
    const Surface &c = ctx_.content;
    PaintBudget *const bg = ctx_.budget;
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
    uint32_t sig = pg::mix_str(pg::mix_str(2166136261u, v.main.view()), v.extra.view());
    sig = sig * 31 + static_cast<uint32_t>(v.tone) + 1;
    if (!painted_ || sig != vsig_) {   // the chips row; the Rp cell below repaints on its own char diff
        if (bg && !bg->admit(static_cast<int>(c.w) * kit::kChipH, true)) return false;
        vsig_ = sig;
        const ChipStyle ts = tone_chip(v.tone);
        const int16_t y = static_cast<int16_t>(kVerdictY + 2);
        auto draw_next = [&](int16_t x, std::string_view text, uint16_t fill, uint16_t ink) {
            kit::erase_chips(c, x, y, 6, kBg);
            return static_cast<int16_t>(x + 6 + kit::draw_chip(c, static_cast<int16_t>(x + 6), y, text, fill, ink));
        };
        int16_t x = draw_next(0, v.main.view(), ts.fill, ts.ink);
        const std::string_view ex = v.extra.view();
        if (!ex.empty()) {   // the detail: a voltage takes the volt spot, anything else the quiet bg2 chip
            const bool volts = ex.back() == 'V';
            x = draw_next(x, ex, volts ? kSpotVolt : kBg2, volts ? kSpotInk : kInk);
        }
        if (x < c.w) kit::erase_chips(c, x, y, static_cast<int16_t>(c.w - x), kBg);   // a shorter row must not leave the old tail
        painted_ = true;
    }
    if (!pg::put_field(pv_[0], bg, rp.view(), r ? kText : kFaint)) return false;
    const DpDmWord dd = dpdm_word(ctx_.fc_lines);   // the pair itself: a PD summary stays in the chips
    if (!pg::put_field(pv_[1], bg, dd.text, dd.dim ? kFaint : kText)) return false;
    StrBuf<16> vref;
    if (r) {
        vref.put_fixed(r->aux_mv, 2).put("V");
        if (r->mcu.valid) {
            StrBuf<24> t;
            mcu_text(t, true, r->mcu.deci_c);
            vref.put(" ").put(t.view());
        }
    } else {
        vref.put("--");
    }
    return pg::put_field(pv_[2], bg, vref.view(), r ? kText : kFaint);
}

void LinesPage::update(uint32_t now_ms) {
    PaintBudget *const bg = ctx_.budget;
    if (bg && bg->more) return;
    const std::optional<adc::Reading> &r = ctx_.analog;
    const int32_t mv[4] = {r ? r->dp_mv : 0, r ? r->dm_mv : 0, r ? r->cc1_mv : 0, r ? r->cc2_mv : 0};
    StrBuf<8> s;
    for (int i = 0; i < 4; i++) {
        pg::put_unit_or_dash(s, r.has_value(), mv[i], 2, 'V');
        if (!pg::put_field(v_[i], bg, s.view(), r && mv[i] >= kLnFaintMv ? kText : kFaint)) return;   // a line with nothing on it reads faint
        paint_bar(i, mv[i], r.has_value());
        if (bg && bg->more) return;
    }
    paint_verdict(r, now_ms);
}

bool LinesPage::on_event(Event) {
    return false;   // view-only in the ring
}

Strip LinesPage::labels() const {
    return base_labels();
}

void LinesPage::invalidate() {
    static_step_ = 0;
    painted_ = false;
    vsig_ = 0;
    for (int &p : px_) p = -1;
    pg::invalidate_fields(v_, 4);
    pg::invalidate_fields(pv_, 3);
}

// ---------------------------------------------------------------------------------------
// PD trigger submenu: a step wizard (OFFER, VOLTAGE, CURRENT, CONFIRM, then the LIVE card). The
// step logic is k2::Wizard (ui_wizard.hpp, host-tested); this part only draws it.
// ---------------------------------------------------------------------------------------

namespace {
// The key/value tables of the wizard: 20 px rows, S12 keys, S16 values (the same table as the FC trigger).
constexpr kit::KvLayout kPtKv{kit::kBodyY, 20, 6, &kLabelFont, nullptr, 64, 18, Align::Left};
constexpr kit::KvLayout kPtLiveKv{kit::kBodyY + kit::kAlertH + 8, 20, 6, &kLabelFont, nullptr, 84, 16, Align::Left};
constexpr int16_t kPtBigY = 112, kPtRangeY = 162, kPtRangeX = 10, kPtRangeW = 188;   // Voltage / Current: big value, range bar
constexpr int16_t kPtSendY = 120, kPtSendH = 72;                                     // Confirm: the SEND cell
constexpr uint8_t kNoStep = 0xFF;
int16_t pt_text_y(const kit::KvLayout &l, int row) {
    return static_cast<int16_t>(kit::kv_row_y(l, row) + kit::row_text_y(l.pitch, 16));
}
}  // namespace

void PdTriggerPage::draw_static() {
    reset_paint();
    if (!ctx_.blank) pg::clear_body(ctx_.content, kit::kBelowBar);
    paint_bar();
}

void PdTriggerPage::reset_paint() {
    rsig_ = bsig_ = barsig_ = tsig_ = vsig_ = 0;
    foot_.invalidate();
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
    struct PtBar { const char *title; uint8_t step; };
    static constexpr PtBar kBar[5] = {
        {K2_P("PD offer"), 1}, {K2_P("PD voltage"), 2}, {K2_P("PD current"), 2}, {K2_P("PD confirm"), 3}, {K2_P("PD live"), 0}
    };
    const char *title = refused ? K2_P("PD trigger") : kBar[static_cast<unsigned>(st)].title;
    const int step = refused ? 0 : kBar[static_cast<unsigned>(st)].step;
    (void)title; (void)step;
    bar_valid_ = true;
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
    if (blocked_) K2LOG("[PD] trigger refused: inline (%s)\n", link.inline_mode() ? K2_P("link inline") : pd::trigger_refusal_name(fusb302::trigger_refusal()));
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
        if (!ctx_.blank) pg::clear_body(ctx_.content, kit::kBodyY);   // below the progress rule
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
    ctx_.rows.draw(now_ms, ctx_.budget);
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
    const uint32_t mw = math::mw_of(mw_v, volt ? cl : wiz_.ma);
    const uint16_t col = volt ? kVolt : kAmp;
    uint32_t sig = mix_u32(mix_u32(mix_u32(mix_u32(2166136261u, volt ? 1u : 2u), wiz_.cursor), cap), volt ? 0u : mw_v);
    sig = mix_u32(sig, hi) | 1u;
    StrBuf<12> s;
    if (sig != bsig_) {
        bsig_ = sig;
        pg::clear_body(c, kit::kBodyY);
        static const char *const kVoltKeys[] = {K2_P("Offer"), K2_P("Max amps"), K2_P("Max power")};
        static const char *const kAmpKeys[] = {K2_P("Offer"), K2_P("Voltage"), K2_P("Power")};
        kit::kv_static(c, kPtKv, volt ? kVoltKeys : kAmpKeys, 3);
        pdo_line(d, line_);
        kit::kv_draw_text(c, kPtKv, 0, line_.text.view());
        pg::put_exact_unit(s, volt ? d.max_ma : mw_v, volt ? 'A' : 'V');
        kit::kv_draw_text(c, kPtKv, 1, s.view(), col == kVolt ? kAmp : kVolt);
        f_[0].init(c, kSmall, kPtKv.value_x, pt_text_y(kPtKv, 2), 8, Align::Left, kWatt, kBg);   // power_mw
        constexpr uint8_t kNumChars = 5;
        const int16_t x0 = static_cast<int16_t>((c.w - (kNumChars * kBig.adv + 2 + kMid.adv)) / 2);
        f_[1].init(c, kBig, x0, kPtBigY, kNumChars, Align::Right, col, kBg);
        kit::draw_big_unit(c, x0, kPtBigY, kNumChars, volt ? "V" : "A", col);
        range_.init(c, kPtRangeX, kPtRangeY, kPtRangeW);
        const char u = volt ? 'V' : 'A';
        s.clear();
        pg::put_exact_unit(s, lo, u);
        const int16_t ly = static_cast<int16_t>(kPtRangeY + kit::ProgressBar::kH + 5);
        gfx_text(c, kLabelFont, kPtRangeX, ly, s.view(), kDim, kBg);
        s.clear();
        pg::put_exact_unit(s, hi, u);
        gfx_text(c, kLabelFont, static_cast<int16_t>(kPtRangeX + kPtRangeW - static_cast<int>(s.view().size()) * kLabelFont.adv), ly, s.view(), kDim, kBg);
    }
    s.clear();
    s.put_fixed(static_cast<int32_t>(mw), 1).put_char('W');
    f_[0].draw(s.view());
    s.clear();
    s.put_fixed(static_cast<int32_t>(value), 2);
    f_[1].draw(s.view());
    range_.set(static_cast<int>(value > lo ? value - lo : 0), static_cast<int>(hi > lo ? hi - lo : 0));
}

// CONFIRM: the request as a four-row table (offer, volts, amps, power against what the source offers) and the SEND cell. The
// cell turns into CONFIRM with a draining bar after the first OK above 5 V. A plan the source would refuse shows an alert band
// and the reason instead.
void PdTriggerPage::paint_confirm(uint32_t now_ms) {
    const Surface &c = ctx_.content;
    WizSummary s;
    const bool ok = wiz_summary(*caps_, wiz_.choice(*caps_), ctx_.cap_mv, s);
    uint32_t sig = mix_u32(mix_u32(mix_u32(2166136261u, ok ? 1u : 0u), static_cast<uint32_t>(s.err)), s.needs_confirm ? 1u : 0u);
    sig = pg::mix_str(pg::mix_str(pg::mix_str(pg::mix_str(pg::mix_str(sig, s.volts.view()), s.amps.view()), s.watts.view()), s.offer_w.view()), s.kind.view());
    sig = mix_u32(sig, wiz_.cursor) | 1u;
    if (sig != bsig_) {
        bsig_ = sig;
        pg::clear_body(c, kit::kBodyY);
        send_.invalidate();
        if (ok && s.err != pd::Err::Ok) {   // the plan refused it: say so instead of a summary
            kit::draw_alert_band(c, kit::kBodyY, true, K2_P("NOT SENDABLE"));
            gfx_text(c, kSmall, 6, static_cast<int16_t>(kit::kBodyY + kit::kAlertH + 10), pd::err_name(s.err), kText, kBg);
        } else if (ok) {
            static const char *const kKeys[] = {K2_P("Offer"), K2_P("Voltage"), K2_P("Current"), K2_P("Power")};
            kit::kv_static(c, kPtKv, kKeys, 4);
            pdo_line(caps_->pdos[wiz_.cursor], line_);
            kit::kv_draw_text(c, kPtKv, 0, line_.text.view());
            kit::kv_draw_text(c, kPtKv, 1, s.volts.view(), kVolt);
            kit::kv_draw_text(c, kPtKv, 2, s.amps.view(), kAmp);
            kit::kv_draw_text(c, kPtKv, 3, s.watts.view(), kWatt);   // power_mw
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
        sub.put(K2_P("Send again: ")).put(s.volts.view()).put_char(' ').put(s.amps.view());
        const uint32_t used = armed_at_ && now_ms > armed_at_ ? now_ms - armed_at_ : 0;
        const int left = used >= Ui::kConfirmWindowMs ? 0 : static_cast<int>(1000u - used * 1000u / Ui::kConfirmWindowMs);
        send_.draw_armed(K2_P("CONFIRM"), sub.view(), left);
    } else {
        send_.draw_idle("SEND", s.needs_confirm ? K2_P("Above 5V: check your load") : "");
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
    const uint32_t sig = pg::mix_str(pg::mix_str(mix_u32(2166136261u, static_cast<uint32_t>(ls)), s.volts.view()), s.amps.view()) | 1u;
    if (sig != bsig_) {
        bsig_ = sig;
        vsig_ = tsig_ = 0;
        pg::clear_body(c, kit::kBodyY);
        static const char *const kKeys[] = {K2_P("Requested V"), K2_P("Measured V"), K2_P("Requested A"), K2_P("Measured A"), K2_P("Resets")};
        kit::kv_static(c, kPtLiveKv, kKeys, 5);
        kit::kv_draw_text(c, kPtLiveKv, 0, s.volts.view(), kVolt);
        kit::kv_draw_text(c, kPtLiveKv, 2, s.amps.view(), kAmp);
        f_[0].init(c, kSmall, kPtLiveKv.value_x, pt_text_y(kPtLiveKv, 1), 12, Align::Left, kVolt, kBg);
        f_[1].init(c, kSmall, kPtLiveKv.value_x, pt_text_y(kPtLiveKv, 3), 12, Align::Left, kAmp, kBg);
    }
    if (vsig_ != bsig_) {
        vsig_ = bsig_;
        const kit::Verdict v = good ? kit::Verdict::Pass : bad ? kit::Verdict::Fail :
                               (ls == LiveState::Dropping || ls == LiveState::Released) ? kit::Verdict::None : kit::Verdict::Running;
        kit::draw_verdict(c, 0, kit::kBodyY, c.w, kit::kAlertH, v, live_name(ls), kBg);
    }
    StrBuf<12> buf;
    pg::put_unit_or_dash(buf, on, ctx_.pm.vbus_mv, 2, 'V', true);
    pg::put_field(f_[0], nullptr, buf.view(), on ? kVolt : kDim);
    pg::put_unit_or_dash(buf, on, ctx_.pm.current_ma, 2, 'A', true);
    pg::put_field(f_[1], nullptr, buf.view(), on ? kAmp : kDim);
    const uint32_t rs = 1u + st.hard_resets;
    if (rs != tsig_) {
        tsig_ = rs;
        buf.clear();
        buf.put_u32(st.hard_resets);
        kit::kv_draw_text(c, kPtLiveKv, 4, buf.view(), st.hard_resets ? kCaution : kDim);
    }
}

void PdTriggerPage::paint_footer() {
    const pd::SinkStatus &st = fusb302::sink_status();
    StrBuf<32> msg;
    FooterTone tone = FooterTone::Info;
    if (blocked_ && wiz_.step == WizStep::Offer) {
        msg.put(K2_P("Other port busy: unplug"));
        tone = FooterTone::Error;
    } else if (wiz_.step == WizStep::Confirm && send_err_ != pd::Err::Ok) {
        msg.put(K2_P("Refused: ")).put(pd::err_name(send_err_));
        tone = FooterTone::Error;
    } else if (wiz_.step == WizStep::Offer && !Wizard::offer_available(*caps_, wiz_.cursor, ctx_.cap_mv)) {
        const bool over = pd::plan_request(*caps_, pd::Choice{static_cast<uint8_t>(wiz_.cursor + 1), 0, 0}, ctx_.cap_mv, wiz_.cursor >= 7).err == pd::Err::OverCap;
        if (over) msg.put(K2_P("Over ")).put_u32(ctx_.cap_mv / 1000).put(K2_P("V limit: pick other"));
        else msg.put(K2_P("Not requestable: pick other"));
        tone = FooterTone::Caution;
    } else if (wiz_.step == WizStep::Voltage && std::min<uint32_t>(caps_->pdos[wiz_.cursor].max_mv, std::max<uint32_t>(ctx_.cap_mv, pd::kVsafe5vMv)) < caps_->pdos[wiz_.cursor].max_mv) {
        const uint32_t vmax = std::max<uint32_t>(ctx_.cap_mv, pd::kVsafe5vMv);
        msg.put(K2_P("Capped at "));
        pg::put_exact_unit(msg, vmax, 'V');
        msg.put(K2_P(" by monitor"));
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
    if (tone == FooterTone::Caution || tone == FooterTone::Error)
        draw_toast(ctx_.content, tone, msg.view());
}

void PdTriggerPage::paint_refusal() {
    const Surface &c = ctx_.content;
    const SetupView v = setup_view(Tool::PdTrigger, refusal_state());
    uint32_t sig = pg::mix_str(2166136261u, v.footer);
    for (int k = 0; k < v.n; k++) sig = sig * 3 + (v.met[k] ? 1u : 0u);
    sig |= 1u;
    if (sig == rsig_) return;
    rsig_ = sig;
    kit::draw_alert_band(c, kit::kBodyY, true, K2_P("NOT READY"));
    const int16_t body_y = static_cast<int16_t>(kit::kBodyY + kit::kAlertH + kit::kAlertStripeH);
    gfx_fill(c, 0, body_y, c.w, static_cast<int16_t>(kFooterY - body_y), kBg);
    for (int k = 0; k < v.n; k++) {
        const int16_t y = static_cast<int16_t>(body_y + 8 + k * 22);
        draw_mark(c, kit::kMarkX, static_cast<int16_t>(y + 2), 12, v.met[k], v.met[k] ? kOk : kError, kBg);
        gfx_text(c, kSmall, kit::kTextX, y, v.check[k], v.met[k] ? kDim : kText, kBg);
    }
    draw_toast(c, FooterTone::Error, v.footer);
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

Strip PdTriggerPage::labels() const {
    if (refusal_ != Refusal::None) return base_labels();
    static constexpr Strip kStrips[5] = {
        {{sl::Back, sl::Up, sl::Next, sl::Dn}},
        {{sl::Back, sl::Dec1V, sl::Next, sl::Inc1V}},
        {{sl::Back, sl::Dec05A, sl::Next, sl::Inc05A}},
        {{sl::Back, sl::None, sl::Send, sl::None}},
        {{sl::Back, sl::None, sl::None, sl::None}},
    };
    Strip l = kStrips[static_cast<unsigned>(wiz_.step)];
    if (wiz_.step == WizStep::Offer && blocked_) l[Button::Ok] = sl::None;
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
    static constexpr uint16_t kCrCols[4] = {kOk, kCaution, kDim, kError};
    const unsigned i = static_cast<unsigned>(t);
    return i < 4 ? kCrCols[i] : kDim;
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
    for (const CrCard &cd : v.step) sig = pg::mix_str(sig * 7, cd.label);
    if (sig == csig_) return;
    csig_ = sig;
    const Surface &c = ctx_.content;
    if (!ctx_.blank) pg::clear_body(c);   // body between the bars (a blank screen is kBg)
    rsig_ = 0;
    pg::invalidate_fields(val_, 2);
    if (v.meter_missing) {
        kit::draw_alert_band(c, kit::kBodyY, true, K2_P("METER MISSING"));
        gfx_text(c, kSmall, kit::kMarkX, kit::kBodyY + kit::kAlertH + 10, K2_P("Cable R needs INA2xx"), kText, kBg);
        gfx_text(c, kSmall, kit::kMarkX, kit::kBodyY + kit::kAlertH + 30, K2_P("Diag shows the chip"), kDim, kBg);
        return;
    }
    const char *const keys[kCrRows] = {v.step[0].label, v.step[1].label, "Port"};
    kit::kv_static(c, kCrKv, keys, kCrRows);
    // The ports are fixed by the hardware: the load plugs into the top port (the charger into the bottom one), whatever the
    // screen rotation.
    kit::kv_draw_text(c, kCrKv, 2, K2_P("load on top"), kDim);
    kit::kv_cells(c, kCrKv, val_, 2);
}

void CableRPage::paint_result(const CrView &v) {
    uint32_t sig = v.has_result ? 1u : 0u;
    if (v.has_result) {
        sig = pg::mix_str(pg::mix_str(sig, v.number.view()), v.detail.view()) * 31 + static_cast<uint32_t>(v.grade);
        sig = pg::mix_str(sig, v.unit);
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
    const int16_t by = kCrResY + 12;
    if (v.grade == cable_r::Grade::Fair) {
        gfx_fill(c, kCrBadgeX, by, kCrBadgeW, kCrBadgeH, kCaution);
        gfx_text(c, kSmall, static_cast<int16_t>(kCrBadgeX + (kCrBadgeW - text_width(kSmall, v.grade_pill)) / 2),
                 static_cast<int16_t>(by + (kCrBadgeH - kSmall.height) / 2), v.grade_pill, kBg, kCaution);
    } else {
        kit::draw_verdict(c, kCrBadgeX, by, kCrBadgeW, kCrBadgeH,
                          v.grade == cable_r::Grade::Poor ? kit::Verdict::Fail : kit::Verdict::Pass, v.grade_pill, kBg);
    }
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
        for (int i = 0; i < 2; i++) pg::put_field(val_[i], nullptr, v.step[i].value.view(), cr_value_color(v.step[i].tone));
        paint_result(v);
    }
    if (cleared_until_ && static_cast<int32_t>(ctx_.now - cleared_until_) < 0 && !v.has_result) return;
    cleared_until_ = 0;
    const std::string_view foot = v.foot.view();
    const CrTone tone = v.foot_tone;
    const FooterTone ft = footer_tone_of(cr_color(tone));
    if (ft == FooterTone::Caution || ft == FooterTone::Error) {
        ctx_.toast.show(ft, foot, ctx_.now);
    } else {
        if (ctx_.toast.active(ctx_.now)) {
            ctx_.toast.clear();
            ctx_.toast.drop_erase();
        }
        foot_.paint_budgeted(1, ctx_.budget, static_cast<int>(ctx_.content.w) * 20, [&] {
            gfx_fill(ctx_.content, 0, static_cast<int16_t>(ctx_.content.h - 20), ctx_.content.w, 20, kBg);
        });
    }
}

bool CableRPage::on_event(Event e) {
    if (e.press != Press::Short) return false;
    if (e.button == Button::Ok) {
        ctx_.cable.reset();
        ctx_.kelvin.reset();
        cleared_until_ = ctx_.now + 1500;
        ctx_.toast.show(FooterTone::Info, K2_P("Cleared, measuring again"), ctx_.now, 1500);
        draw_toast(ctx_.content, FooterTone::Info, ctx_.toast.view());
        ctx_.toast.drawn = true;
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

Strip CableRPage::labels() const {
    return {{sl::Back, sl::Dim, sl::Reset, sl::Mode}};
}

void CableRPage::invalidate() {
    pg::invalidate_fields(val_, 2);
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
constexpr int16_t kFcSparkX = 6, kFcSparkY = 110, kFcSparkW = 196, kFcSparkH = 108;   // Live: the VBUS trace under the table
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
    bsig_ = barsig_ = tsig_ = ysig_ = 0;
    foot_.invalidate();
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
    const char *title = scan ? K2_P("FC scan") : retarget ? K2_P("Retarget") : K2_P("FC trigger");
    const int step = scan || stage == trig::Stage::Live ? 0 : stage == trig::Stage::Pick ? 1 : stage == trig::Stage::Edit ? 2 : 3;
    (void)title; (void)step;
    bar_valid_ = true;
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

// The protocol list: kit RowList rows (focus fill and bar), the name and what the entry takes at the right. Picking a row
// sends nothing; the list repaints only the rows that changed.
void TriggerPage::row_fn(void *self, int index, bool, kit::RowSpec &out) {
    (void)self;
    out = kit::RowSpec{};
    if (index < 0 || index >= trig::kEntries) return;
    const trig::Entry &e = trig::entry(index);
    out.text = e.name;
    out.tail = e.kind == trig::Kind::Scan ? K2_P("HV sweep") :
        e.kind == trig::Kind::Range ? K2_P("adjustable") : e.kind == trig::Kind::Levels ? K2_P("levels") : K2_P("5V only");
}

// The Offered cell of the table: what the protocol takes.
static void fc_offered_text(const trig::Entry &e, StrBufBase &s) {
    if (e.kind == trig::Kind::Range) {
        s.put_fixed(e.lo, 1).put("-").put_fixed(e.hi, 1).put_char('V');
    } else if (e.kind == trig::Kind::Levels) {
        for (int i = 0; i < e.n; ++i) {
            if (i) s.put_char('/');
            s.put_u32(uint32_t(e.mv[i] / 1000));
        }
        s.put_char('V');
    } else if (e.kind == trig::Kind::Scan) {
        s.put(K2_P("HV sweep"));
    } else {
        s.put(K2_P("5V only"));
    }
}

// The release monitor replaces the body while the measured release runs: an AlertBand (amber while it measures, red once
// VBUS did not settle) over the measured VBUS. Nothing else on the page reacts until it is Safe.
void TriggerPage::paint_release(bool fresh) {
    const Surface &c = ctx_.content;
    if (fresh) {
        const bool unsafe = ctx_.ui->fc_release().state() == trig::Release::State::Unsafe;
        kit::draw_alert_band(c, kit::kBodyY, unsafe, unsafe ? K2_P("UNSAFE VBUS") : K2_P("RELEASING"));
        static const char *const labels[] = {K2_P("Target"), K2_P("Measured")};
        kit::kv_static(c, kFcRelKv, labels, 2);
        const int16_t y0 = static_cast<int16_t>(kFcRelKv.y0 + kit::row_text_y(kFcRelKv.pitch, 16));
        kit::kv_draw_text(c, kFcRelKv, 0, "5V", kVolt);
        vbus_v_.init(c, kSmall, kFcRelKv.value_x, static_cast<int16_t>(y0 + kFcRelKv.pitch), 8, Align::Left, kVolt, kBg);
    }
    StrBuf<12> s;
    pg::put_unit_or_dash(s, ctx_.pm_online, ctx_.pm.vbus_mv, 2, 'V');
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
            else pg::clear_body(c, kit::kBelowBar);
        }
        bsig_ = sig;
        foot_.invalidate();
        tsig_ = 0;
        send_.invalidate();
        if (stage == trig::Stage::Live) ctx_.toast.clear();
    }
    StrBuf<32> s;
    if (cleanup) {
        paint_release(fresh);
    } else if (stage == trig::Stage::Pick) {
        if (fresh) {
            ctx_.rows.init(c, kit::kBodyY, row_fn, this);
            ctx_.rows.invalidate();
        }
        ctx_.rows.set_count(trig::kEntries);
        ctx_.rows.select(model_.sel());
        ctx_.rows.draw(now_ms, ctx_.budget);
    } else {
        // The key/value table: Edit and Review show the protocol, what it offers, the measured VBUS and the held voltage
        // (retarget) or the monitor cap; Live shows the state, the protocol, the request and the measurement.
        static const char *const kLiveKeys[] = {K2_P("State"), K2_P("Protocol"), K2_P("Requested"), K2_P("Measured")};
        static const char *const kHeldKeys[] = {K2_P("Protocol"), K2_P("Offered"), K2_P("Measured"), "Held"};
        static const char *const kCapKeys[] = {K2_P("Protocol"), K2_P("Offered"), K2_P("Measured"), "Cap"};
        if (fresh) {
            kit::kv_static(c, kFcKv, live ? kLiveKeys : retarget ? kHeldKeys : kCapKeys, 4);
            kit::kv_draw_text(c, kFcKv, live ? 1 : 0, e.name);
            vbus_v_.init(c, kSmall, kFcKv.value_x, fc_row_text_y(live ? 3 : 2), 8, Align::Left, kVolt, kBg);
            if (live) {
                state_v_.init(c, kSmall, kFcKv.value_x, fc_row_text_y(0), 12, Align::Left, kOk, kBg);
                target_v_.init(c, kSmall, kFcKv.value_x, fc_row_text_y(2), 8, Align::Left, kVolt, kBg);
            } else {
                fc_offered_text(e, s);
                kit::kv_draw_text(c, kFcKv, 1, s.view(), e.kind == trig::Kind::Scan ? kText : kVolt);
                if (retarget) {
                    held_v_.init(c, kSmall, kFcKv.value_x, fc_row_text_y(3), 8, Align::Left, kVolt, kBg);
                } else {
                    s.clear();
                    s.put_fixed(power::max_vbus_mv(), 1).put_char('V');
                    kit::kv_draw_text(c, kFcKv, 3, s.view(), kVolt);
                }
                // The draft: the big number with its unit, or the sweep word.
                const int16_t unit_y = static_cast<int16_t>(kFcBigY + kBig.ascent - kMid.ascent);
                if (scan_sel) {
                    gfx_text(c, kMid, 6, unit_y, "SCAN", kText, kBg);
                } else {
                    constexpr uint8_t kNumChars = 5;
                    target_v_.init(c, kBig, 6, kFcBigY, kNumChars, Align::Right, kVolt, kBg);
                    kit::draw_big_unit(c, 6, kFcBigY, kNumChars, "V", kVolt);
                }
                if (stage == trig::Stage::Review) send_.init(c, kFcSendY, kFcSendH);
            }
        }
        s.clear();
        if (live) {
            const bool waiting = run() == trig::Run::Negotiating;
            pg::put_field(state_v_, nullptr, waiting ? K2_P("Requesting") : K2_P("Holding"), waiting ? kCaution : kOk);
            s.put_fixed(model_.target_mv(), 2).put_char('V');
            target_v_.draw(s.view());
        } else if (!scan_sel) {
            s.put_fixed(model_.target_mv(), 2);
            target_v_.draw(s.view());
        }
        pg::put_unit_or_dash(s, ctx_.pm_online, ctx_.pm.vbus_mv, 2, 'V');
        vbus_v_.draw(s.view());
        if (retarget) {
            s.clear();
            s.put_fixed(model_.held_mv(), 2).put_char('V');
            held_v_.draw(s.view());
        }
        if (live && tsig_ != trace_ms_ + 1) { tsig_ = trace_ms_ + 1; draw_trace(); }
        if (stage == trig::Stage::Review) {
            if (model_.armed()) {
                s.clear();
                s.put(K2_P("Send again: "));
                if (scan_sel) s.put("SCAN"); else s.put_fixed(model_.target_mv(), 2).put_char('V');
                send_.draw_armed(K2_P("CONFIRM"), s.view(), static_cast<int>(model_.arm_left_permille(ctx_.now)));
            } else {
                send_.draw_idle("SEND", scan_sel ? K2_P("SCAN requests HV") : model_.needs_confirm() ? K2_P("Above 5V: other port empty") :
                                                                                           K2_P("Other port must be empty"));
            }
        }
    }
    if (ctx_.budget && ctx_.budget->more) return;
    // Footer: facts only. The Edit hint says how far one step moves.
    const char *footer = nullptr;
    if (cleanup) {
        footer = release == trig::Release::State::Unsafe ? K2_P("Unsafe VBUS: unplug source") : K2_P("Releasing; measuring VBUS");
    } else if (refused_ != fc::Fail::None) {
        footer = trig::fail_long(refused_);
    } else if (!model_.available()) {
        footer = K2_P("No voltage within monitor cap");
    } else if (stage == trig::Stage::Pick) {
        footer = release == trig::Release::State::Safe ? K2_P("5V verified. Pick protocol") : K2_P("Pick a protocol");
    } else if (stage == trig::Stage::Edit) {
        if (e.kind == trig::Kind::Range) {
            s.clear();
            s.put(K2_P("Step ")).put_u32(e.step).put(K2_P("mV, hold ")).put_u32(e.step < 1000 ? 1000 : e.step).put("mV");
            footer = s.c_str();
        } else {
            footer = K2_P("Choose offered voltage");
        }
    } else if (stage == trig::Stage::Review) {
        footer = model_.needs_confirm() ? (model_.armed() ? K2_P("Above 5V! Send again") : K2_P("Above 5V: send twice")) : K2_P("Ready to send");
    } else {
        footer = run() == trig::Run::Negotiating ? K2_P("Requesting the voltage") : K2_P("VBUS 10s trace, 0-20V");
    }
    const bool unsafe = cleanup && release == trig::Release::State::Unsafe;
    const bool error = unsafe || refused_ != fc::Fail::None || !model_.available();
    const bool caution = cleanup || (model_.needs_confirm() && (stage == trig::Stage::Review || live));
    if (error || (caution && stage != trig::Stage::Live))
        ctx_.toast.show(error ? FooterTone::Error : FooterTone::Caution, footer, ctx_.now);
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
        case fc::Outcome::Rejected: v = kit::Verdict::Fail; tail = K2_P("no reply"); break;
        case fc::Outcome::Skipped:  v = kit::Verdict::Skip; tail = K2_P("skipped"); break;
        case fc::Outcome::None:     v = running ? kit::Verdict::Running : kit::Verdict::Pending; break;
        }
        StrBuf<8> volts;
        if (o == fc::Outcome::Accepted) { volts.put_u32(static_cast<uint32_t>((res_[k].mv + 500) / 1000)).put_char('V'); tail = volts.c_str(); }
        const uint32_t ts = pg::mix_str(2166136261u, tail) * 31u + static_cast<uint32_t>(v) + 1u;
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
            label.put(K2_P("SCAN ")).put_u32(static_cast<uint32_t>(scan_step_ + 1)).put(" OF ").put_u32(fc::kScanSteps);
        } else if (acc > 0) {
            sv = kit::Verdict::Pass;
            label.put_u32(static_cast<uint32_t>(acc)).put(" OF ").put_u32(fc::kScanSteps).put(K2_P("  MAX "));
            trig::put_volts(label, maxmv);
        } else {
            label.put(K2_P("NONE ANSWERED"));
        }
        kit::draw_verdict(c, 0, kFcSumY, c.w, kFcSumH, sv, label.view(), kBg);
    }
    const FooterTone ft = ctx_.ui->fc_release().busy() ? FooterTone::Caution
                        : (!scanning_ && acc <= 0)     ? FooterTone::Caution
                                                       : FooterTone::Info;
    const char *fm = ctx_.ui->fc_release().busy() ? K2_P("Releasing; measuring VBUS")
                   : scanning_                    ? K2_P("Scanning...")
                   : acc > 0                      ? K2_P("Scan done")
                                                  : K2_P("Scan done: none answered");
    if (ft != FooterTone::Info) ctx_.toast.show(ft, fm, ctx_.now);
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
        if (!blank) pg::clear_body(ctx_.content, kit::kBelowBar);
        bsig_ = tsig_ = ysig_ = 0;
        foot_.invalidate();
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

Strip TriggerPage::labels() const {
    Strip l = base_labels();
    const trig::Run r = run();
    l[Button::Minus] = l[Button::Plus] = sl::None;
    if (ctx_.ui->fc_release().busy()) {
        l[Button::Ok] = sl::None;
        return l;
    }
    if (r != trig::Run::Idle || model_.retargeting())
        l[Button::Back] = sl::Stop;   // any BACK press stops; one word
    l[Button::Ok] = trig::ok_label(model_, r);
    if (r != trig::Run::Scan && r != trig::Run::Negotiating) {
        l[Button::Plus] = trig::plus_label(model_);
        l[Button::Minus] = trig::minus_label(model_);
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
    bsig_ = barsig_ = tsig_ = ysig_ = 0;
    foot_.invalidate();
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

Strip setting_labels(Strip labels, sl::Id action, int at, int count) {
    labels = list_strip(labels, at, count);
    labels[Button::Ok] = action;
    return labels;
}

enum class SettingRow : uint8_t {
    UsbLink,
    Autorotate,
    RotateButtons,
#if K2_SCREENSHOT
    Screenshot,
#endif
    Smoothing,
    IdleWake,
    TestLevel,
    LegacyUnits,
    SweepDepth,
    LongBack,
    HomeBoxes,
    Display,
    SystemState,
    Memory,
    BootInfo,
    Count
};
constexpr int kSettingsRows = static_cast<int>(SettingRow::Count);
// Checked in both builds (K2_SCREENSHOT 0 and 1): the page's public constants must follow the enum.
static_assert(kSettingsRows == SettingsPage::kRows);
static_assert(static_cast<int>(SettingRow::LegacyUnits) == SettingsPage::kUnitsRow);
static_assert(static_cast<int>(SettingRow::SweepDepth) == SettingsPage::kSweepRow);
static_assert(static_cast<int>(SettingRow::HomeBoxes) == SettingsPage::kHomeBoxesRow);
static_assert(static_cast<int>(SettingRow::Display) == SettingsPage::kDisplayRow);
static_assert(static_cast<int>(SettingRow::SystemState) == SettingsPage::kSystemRow);
static_assert(static_cast<int>(SettingRow::Memory) == SettingsPage::kMemoryRow);
static_assert(static_cast<int>(SettingRow::BootInfo) == SettingsPage::kBootRow);
}

void SettingsPage::enter() {
    sel_ = std::clamp(sel_, 0, kSettingsRows - 1);
    const auto stored = uiprefs::stored();   // one volume read per visit; row_fn paints from the copy
    ct_level_ = stored.ct_level;
    sweep_depth_ = stored.sweep_depth;
    ctx_.rows.init(ctx_.content, kListY, row_fn, this);
    ctx_.rows.set_count(kSettingsRows);
    ctx_.rows.select(sel_);
    ctx_.rows.invalidate();
    static_step_ = 0;
}

void SettingsPage::draw_static() {
    PaintBudget *const bg = ctx_.budget;
    if (static_step_ == 0) {
        if (bg && !bg->admit(140 * kBarH, true)) return;
        title_bar();
        ctx_.rows.init(ctx_.content, kListY, row_fn, this);
        ctx_.rows.set_count(kSettingsRows);
        ctx_.rows.select(sel_);
        foot_.invalidate();
        id_.invalidate();
        static_step_ = 1;
    }
    if (static_step_ == 1) {
        const int16_t end = kit::list_end(kListY);
        if (!ctx_.blank && end < ctx_.content.h) {
            const int cost = static_cast<int>(ctx_.content.w) * (ctx_.content.h - end);
            if (bg && !bg->admit(cost, true, true)) return;
            gfx_fill(ctx_.content, 0, end, ctx_.content.w, static_cast<int16_t>(ctx_.content.h - end), kBg);
        }
        static_step_ = 2;
    }
}

// The K2's identity: the version in font 16, then "up 1h05m  USB HID+WebUSB" in font 12 (minutes, so it repaints once a
// minute; the mode is usbcfg::describe of this boot's setting),
// and the dashed line the place pages close their identity with. y22..65, each pixel once.
void SettingsPage::paint_identity() {
    const uint32_t up_min = ctx_.now / 60000u;
    const usbcfg::Setting usb = usbcfg::current;
    const uint32_t usb_mode = usb.hid + 2u * usb.drive + 4u * usb.net + 8u * usb.webusb;
    id_.paint_budgeted(mix_u32(mix_u32(mix_init(), up_min), usb_mode), ctx_.budget,
                        static_cast<int>(ctx_.content.w) * (kListY - kit::kBelowBar), [&] {
        const Surface &c = ctx_.content;
        StrBuf<32> line;
        line.put("up ");
        if (up_min >= 60) {
            line.put_u32(up_min / 60).put_char('h');
            if (up_min % 60 < 10) line.put_char('0');
        }
        line.put_u32(up_min % 60).put_char('m');
        line.put(K2_P("  USB ")).put(usbcfg::describe(usb));   // this boot's mode, the words the USB Link note uses
        const kit::TextRun runs[2] = {{8, kIdHeadY, &kSmall, std::string_view(kFwVersion), kText},
                                      {8, kIdLineY, &kLabelFont, line.view(), kDim}};
        kit::paint_texts(c, {0, kit::kBelowBar, c.w, static_cast<int16_t>(kIdDashY - kit::kBelowBar)}, runs, 2, kBg);
        kit::draw_dashed(c, 0, c.w, kIdDashY, kInk3, kBg);
        gfx_fill(c, 0, static_cast<int16_t>(kIdDashY + 1), c.w, static_cast<int16_t>(kListY - kIdDashY - 1), kBg);
    });
}

// Boot info: the K2B1 record (the boot screen's QR) of this boot, shared as QR codes.
bool SettingsPage::share_boot_info() {
    std::span<std::uint8_t> buf = ctx_.ui->share_buffer();
    if (buf.size() < bootinfo::kQrMax + 1) return false;
    bootinfo::Snapshot snap;
    bootinfo::collect(snap);
    const std::size_t len = bootinfo::format(snap, {reinterpret_cast<char *>(buf.data()), bootinfo::kQrMax + 1});
    return ctx_.ui->open_share(qrshare::Type::BootInfo, qrshare::kSchemaBootInfo, "Boot info", len, ctx_.now);   // k2-plain: share title is memcpy'd
}

// Row i: the setting name, its current value at the right (green when ON; a chevron on the row that only
// opens a page) and, on the focused row, one dim line saying what the value means. Everything is read live from the
// settings, so the rows keep no value text of their own.
void SettingsPage::row_fn(void *ctx, int i, bool, kit::RowSpec &out) {
    static constexpr const char *kTitles[kSettingsRows] = {
        K2_P("USB Link"), K2_P("Autorotate"), K2_P("Rotate buttons"),
#if K2_SCREENSHOT
        K2_P("Screenshot"),
#endif
        K2_P("Smoothing"), K2_P("Wake port"), K2_P("Test level"), K2_P("Huawei/Samsung"), K2_P("Sweep depth"), K2_P("Long Back"), K2_P("Home boxes"), K2_P("Display"), K2_P("System state"), K2_P("Memory"), K2_P("Boot info")};
    static constexpr const char *kPresets[smoothing::kPresetCount] = {"Off", "Low", K2_P("Medium"), "High"};
    static constexpr const char *kPresetNotes[smoothing::kPresetCount] = {K2_P("Raw readings"), K2_P("Light calming"), K2_P("Balanced"), K2_P("Steady, slower")};
    // charger_test::Level order (Medium 0, Light 1, Deep 2): the charger test's default level and what it runs.
    static constexpr const char *kLevels[charger_test::kLevelCount] = {K2_P("Medium"), K2_P("Light"), "Deep"};
    static constexpr const char *kLevelNotes[charger_test::kLevelCount] = {
        K2_P("Tests each advertised voltage"), K2_P("Checks what charger offers"),
#if K2_PDSPEC
        K2_P("Sweeps, accuracy & PD spec")
#else
        K2_P("Sweeps & voltage accuracy")
#endif
    };
    // ctsweep::Depth order (100 mV 0, 200 mV 1, Phone 2, 20 mV 3): the deep sweeps' step.
    static constexpr const char *kDepthNotes[ctsweep::kDepthCount] = {K2_P("Every 100 mV, PPS and AVS"), K2_P("Every 200 mV, quickest"), K2_P("100 mV, 20 mV in phone zones"), K2_P("Every 20 mV on PPS, slowest")};
    static_assert(all_fit_p(kDepthNotes, kLabelAdv, kit::kNoteMaxPx), "a Sweep depth note is wider than the row note");
    const auto *self = static_cast<const SettingsPage *>(ctx);
    out.text = kTitles[i];
    auto set_toggle = [&](bool on, const char *on_note, const char *off_note) {
        out.tail = on ? "ON" : "OFF";
        out.tail_color = on ? kOk : kDim;
        out.note = on ? on_note : off_note;
    };
    switch (static_cast<SettingRow>(i)) {
    case SettingRow::UsbLink:
        out.chevron = true;
        // The truth the USB Link page shows: this boot's mode against the stored one (its "Replug to apply" footer).
        self->usb_note_ = usbcfg::link_note(usbcfg::current, usbcfg::stored());
        out.note = self->usb_note_.view();
        break;
    case SettingRow::Autorotate:
        out.tail = self->ctx_.ui->rotation_locked() ? K2_P("Locked") : "Auto";
        out.note = self->ctx_.ui->rotation_locked() ? K2_P("Fixed orientation") : K2_P("Follows gravity");
        break;
    case SettingRow::RotateButtons:
        set_toggle(self->ctx_.ui->rotate_buttons(), K2_P("Follow the display"), K2_P("Physical keys"));
        break;
#if K2_SCREENSHOT
    case SettingRow::Screenshot:
        set_toggle(uiprefs::screenshot_enabled, K2_P("Remote and web capture"), K2_P("No capture"));
        break;
#endif
    case SettingRow::Smoothing: {
        const auto p = static_cast<unsigned>(power::smoothing_preset());
        out.tail = kPresets[p < smoothing::kPresetCount ? p : 0];
        out.note = kPresetNotes[p < smoothing::kPresetCount ? p : 0];
        break;
    }
    case SettingRow::IdleWake:   // the opt-in exception to "no Rd at idle": a sleeping source's port is asked with our Rd
        set_toggle(fusb302::idle_wake(), K2_P("Idle Rd on a sleeping port"), K2_P("Idle stays passive"));
        break;
    case SettingRow::TestLevel: {
        const unsigned l = self->ct_level_ < charger_test::kLevelCount ? self->ct_level_ : 0;
        out.tail = kLevels[l];
        out.note = kLevelNotes[l];
        break;
    }
    case SettingRow::LegacyUnits:   // FCP, SCP and AFC: slow to run and rarely answered, so off until asked for
        set_toggle(uiprefs::legacy_units_shown, K2_P("FCP, SCP, AFC in the test"), K2_P("FCP, SCP, AFC hidden"));
        break;
    case SettingRow::SweepDepth: {
        const unsigned d = self->sweep_depth_ < ctsweep::kDepthCount ? self->sweep_depth_ : 1;
        out.tail = ctsweep::depth_name(ctsweep::Depth(d));
        out.note = kDepthNotes[d];
        break;
    }
    case SettingRow::LongBack:   // the ring view's long Back target (ring::view_key)
        out.tail = ring::name(ring::sanitize_target(self->ctx_.keep.long_back));
        out.note = K2_P("Where Long Back jumps");
        break;
    case SettingRow::HomeBoxes:
        out.chevron = true;
        out.note = K2_P("Configure 3 Home info boxes");
        break;
    case SettingRow::Display:
        out.tail = {self->display_tail_, uidisp::format_summary(self->display_tail_, idle_hw::config())};
        out.note = K2_P("Brightness, dim, screen off");
        break;
    case SettingRow::SystemState:
        out.chevron = true;
        out.note = K2_P("meter, USB, FUSB302, MCU");
        break;
    case SettingRow::Memory:
        out.chevron = true;
        out.note = K2_P("stacks, RAM, flash, files");
        break;
    default:   // SettingRow::BootInfo
        out.chevron = true;
        out.note = K2_P("this boot's log as a QR code");
        break;
    }
}

void SettingsPage::update(uint32_t) {
    PaintBudget *const bg = ctx_.budget;
    title_badge(false);
    if (bg && bg->more) return;
    paint_identity();
    if (bg && bg->more) return;
    ctx_.rows.select(sel_);
    ctx_.rows.draw(ctx_.now, bg);
    if (bg && bg->more) return;
}

bool SettingsPage::on_event(Event e) {
    if (e.press == Press::Short && (e.button == Button::Plus || e.button == Button::Minus)) {
        sel_ = std::clamp(sel_ + (e.button == Button::Plus ? 1 : -1), 0, kSettingsRows - 1);
        update(ctx_.now);
        return true;
    }
    if (e.press == Press::Short && e.button == Button::Ok) {
        const auto row = static_cast<SettingRow>(sel_);
        if (row == SettingRow::Smoothing || row == SettingRow::IdleWake || row == SettingRow::TestLevel ||
            row == SettingRow::LegacyUnits || row == SettingRow::SweepDepth
#if K2_SCREENSHOT
            || row == SettingRow::Screenshot
#endif
        ) {
            uiprefs::Prefs p = uiprefs::stored();
            switch (row) {
            case SettingRow::Smoothing:
                p.smoothing = smoothing::preset_from_index((static_cast<unsigned>(power::smoothing_preset()) + 1) % smoothing::kPresetCount);
                power::set_smoothing(p.smoothing);
                break;
            case SettingRow::IdleWake:
                p.idle_wake = !fusb302::idle_wake();
                fusb302::set_idle_wake(p.idle_wake);
                break;
            case SettingRow::TestLevel:
                p.ct_level = static_cast<uint8_t>(charger_test::next_level(charger_test::Level(ct_level_ < charger_test::kLevelCount ? ct_level_ : 0)));
                ct_level_ = p.ct_level;
                if (!ctx_.ui->charger_test_busy()) ctx_.ui->charger_test_set_level(p.ct_level);
                break;
            case SettingRow::LegacyUnits:
                p.legacy_units = !uiprefs::legacy_units_shown;
                uiprefs::legacy_units_shown = p.legacy_units;
                ctx_.ui->charger_test_legacy_changed();
                break;
            case SettingRow::SweepDepth:
                p.sweep_depth = static_cast<uint8_t>(ctsweep::next_depth(ctsweep::Depth(sweep_depth_ < ctsweep::kDepthCount ? sweep_depth_ : 1)));
                sweep_depth_ = p.sweep_depth;
                if (!ctx_.ui->charger_test_busy()) ctx_.ui->charger_test_set_sweep_depth(p.sweep_depth);
                break;
#if K2_SCREENSHOT
            default:
                p.screenshot = !uiprefs::screenshot_enabled;
                uiprefs::screenshot_enabled = p.screenshot;
                break;
#endif
            }
            uiprefs::set(p);
            update(ctx_.now);
            return true;
        }
        switch (row) {
        case SettingRow::LongBack:   // Home -> Lines -> ... -> K2 -> Home; remembered
            ctx_.ui->set_long_back(static_cast<uint8_t>(ring::next(ring::sanitize_target(ctx_.keep.long_back))));
            update(ctx_.now);
            break;
        case SettingRow::RotateButtons:
            ctx_.ui->set_rotate_buttons(!ctx_.ui->rotate_buttons());
            update(ctx_.now);
            break;
        case SettingRow::HomeBoxes:
            ctx_.ui->push_menu(Menu::HomeBoxes, ctx_.now);
            break;
        case SettingRow::Display:
            ctx_.ui->push_menu(Menu::Display, ctx_.now);
            break;
        case SettingRow::SystemState:
            ctx_.ui->push_menu(Menu::Diag, ctx_.now);
            break;
        case SettingRow::Memory:
            ctx_.ui->push_menu(Menu::Memory, ctx_.now);
            break;
        case SettingRow::BootInfo:
            share_boot_info();
            break;
        default:
            ctx_.ui->push_menu(sel_ == 0 ? Menu::UsbLink : Menu::Autorotate, ctx_.now);
            break;
        }
        return true;
    }
    return false;
}

Strip SettingsPage::labels() const {
    const auto row = static_cast<SettingRow>(sel_);
    const bool opens = row < SettingRow::RotateButtons || row >= SettingRow::HomeBoxes;   // the chevron rows
    const bool cycles = row == SettingRow::Smoothing || row == SettingRow::TestLevel || row == SettingRow::SweepDepth || row == SettingRow::LongBack;
    return setting_labels(base_labels(), opens ? sl::Open : cycles ? sl::Cycle : sl::Toggle, sel_, kSettingsRows);
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
    static constexpr const char *kNames[5] = {"Auto", K2_P("Manual 0 deg"), K2_P("Manual 90 deg"), K2_P("Manual 180 deg"), K2_P("Manual 270 deg")};
    const auto *self = static_cast<const AutorotatePage *>(ctx);
    out.marker = kit::Marker::Radio;
    out.state = rotation_choice(*self->ctx_.ui) == i ? 1 : 0;
    out.text = kNames[i];
    out.note = i == 0 ? K2_P("Follows gravity") : K2_P("Fixed orientation");
}

void AutorotatePage::update(uint32_t) {
    title_badge(false);
    ctx_.rows.select(sel_);
    ctx_.rows.draw(ctx_.now, ctx_.budget);
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

Strip AutorotatePage::labels() const { return setting_labels(base_labels(), sl::Apply, sel_, 5); }

// ---------------------------------------------------------------------------------------
// Home boxes submenu (configure the 3 Home info boxes: static metric, Rotating, or Off)
// ---------------------------------------------------------------------------------------

void HomeBoxesPage::enter() {
    view_ = View::List;
    sel_ = 0;
    ctx_.rows.set_count(kRows);
    ctx_.rows.select(0);
}

void HomeBoxesPage::show(View v) {
    view_ = v;
    if (v == View::PickBox) {
        pick_ = static_cast<int>(ctx_.keep.home_boxes[static_cast<std::size_t>(sel_)]);
    }
    if (!ctx_.blank) pg::clear_body(ctx_.content, kit::kBelowBar);
    title_bar();
    ctx_.rows.set_count(count());
    ctx_.rows.select(cur());
    invalidate();
    update(ctx_.now);
}

void HomeBoxesPage::draw_static() {
    title_bar();
    ctx_.rows.init(ctx_.content, kit::kBodyY, row_fn, this);
    ctx_.rows.set_count(count());
    ctx_.rows.select(cur());
}

void HomeBoxesPage::row_fn(void *ctx, int i, bool focused, kit::RowSpec &out) {
    static constexpr const char *kTitles[kRows] = {K2_P("Box 1"), K2_P("Box 2"), K2_P("Box 3"), K2_P("Reset defaults")};
    const auto *self = static_cast<const HomeBoxesPage *>(ctx);
    if (self->view_ == View::PickBox) {
        const auto k = static_cast<home::BoxKind>(i);
        out.marker = kit::Marker::Radio;
        out.state = self->ctx_.keep.home_boxes[static_cast<std::size_t>(self->sel_)] == k ? 1 : 0;
        out.text = home::box_short_name(k);
        if (focused) out.note = home::box_note(k);
        return;
    }
    out.text = kTitles[i];
    if (i < home::kBoxCount) {
        const home::BoxKind b = self->ctx_.keep.home_boxes[static_cast<std::size_t>(i)];
        out.tail = home::box_short_name(b);
        out.tail_color = b == home::BoxKind::Off ? kDim : kAccent;
        out.note = home::box_note(b);
    } else {
        out.note = K2_P("PD Req, Energy, Rotating");
    }
}

void HomeBoxesPage::update(uint32_t) {
    title_badge(false);
    ctx_.rows.select(cur());
    ctx_.rows.draw(ctx_.now, ctx_.budget);
}

bool HomeBoxesPage::on_event(Event e) {
    if (e.press != Press::Short) return false;
    if (e.button == Button::Plus || e.button == Button::Minus) {
        cur() = std::clamp(cur() + (e.button == Button::Plus ? 1 : -1), 0, count() - 1);
        update(ctx_.now);
        return true;
    }
    if (e.button == Button::Ok) {
        auto boxes = ctx_.keep.home_boxes;
        if (view_ == View::List) {
            if (sel_ < home::kBoxCount) {
                show(View::PickBox);
                return true;
            }
            boxes = home::kDefaultBoxes;
            ctx_.ui->set_home_boxes(boxes);
            ctx_.rows.invalidate();
            update(ctx_.now);
            return true;
        }
        boxes[static_cast<std::size_t>(sel_)] = static_cast<home::BoxKind>(pick_);
        ctx_.ui->set_home_boxes(boxes);
        show(View::List);
        return true;
    }
    if (e.button == Button::Back && view_ != View::List) {
        show(View::List);
        return true;
    }
    return false;
}

Strip HomeBoxesPage::labels() const {
    Strip l = list_strip(base_labels(), view_ == View::List ? sel_ : pick_, count());
    l[Button::Ok] = view_ == View::List ? (sel_ < home::kBoxCount ? sl::Pick : sl::Reset) : sl::Select;
    return l;
}

// ---------------------------------------------------------------------------------------
// Display submenu (backlight level, dim and screen-off timeouts)
// ---------------------------------------------------------------------------------------

void DisplayPage::enter() {
    view_ = View::List;
    sel_ = 0;
}

// A view change repaints the body: the row count and the title differ between the list and the pick lists.
void DisplayPage::show(View v) {
    view_ = v;
    if (v == View::PickDim || v == View::PickOff) {
        const idle::Config &c = idle_hw::config();
        const int at = v == View::PickDim ? uidisp::index_of(uidisp::kDimAfter, c.dim_after_s) : uidisp::index_of(uidisp::kOffAfter, c.off_after_s);
        pick_ = at < 0 ? 0 : at;   // a value set from the remote and not in the list starts on the first option
        title_ = v == View::PickDim ? rail::kDim : rail::kOff;
    } else {
        title_ = rail::kDisplay;
    }
    if (!ctx_.blank) pg::clear_body(ctx_.content, kit::kBelowBar);
    title_bar();
    ctx_.rows.set_count(count());
    ctx_.rows.select(cur());
    invalidate();
    update(ctx_.now);
}

void DisplayPage::draw_static() {
    title_bar();
    ctx_.rows.init(ctx_.content, kit::kBodyY, row_fn, this);
    ctx_.rows.set_count(count());
    ctx_.rows.select(cur());
    foot_.invalidate();
}

void DisplayPage::row_fn(void *ctx, int i, bool, kit::RowSpec &out) {
    static constexpr const char *kTitles[3] = {K2_P("Brightness"), K2_P("Dim after"), K2_P("Screen off")};
    const auto *self = static_cast<const DisplayPage *>(ctx);
    const idle::Config &c = idle_hw::config();
    if (self->view_ >= View::PickDim) {   // a pick list: the radio sits on the stored value
        const bool dim = self->view_ == View::PickDim;
        out.marker = kit::Marker::Radio;
        out.state = (dim ? c.dim_after_s == uidisp::kDimAfter[static_cast<std::size_t>(i)]
                         : c.off_after_s == uidisp::kOffAfter[static_cast<std::size_t>(i)]) ? 1 : 0;
        out.text = dim ? uidisp::kDimNames[static_cast<std::size_t>(i)] : uidisp::kOffNames[static_cast<std::size_t>(i)];
        return;
    }
    char *t = self->tail_[i];
    const std::span<char> buf(t, sizeof self->tail_[i]);
    out.text = kTitles[i];
    if (i == 0) {
        out.tail = {t, uidisp::format_percent(buf, c.brightness)};
        out.tail_color = self->view_ == View::Adjust ? kFocus : kDim;
        out.note = self->view_ == View::Adjust ? K2_P("Changes live") : K2_P("Backlight level");
    } else {
        out.tail = {t, uidisp::format_span(buf, i == 1 ? c.dim_after_s : c.off_after_s, false)};
        out.note = i == 1 ? K2_P("Dims the backlight") : K2_P("Only with nothing plugged in");
    }
}

void DisplayPage::update(uint32_t) {
    title_badge(false);
    ctx_.rows.select(cur());
    ctx_.rows.draw(ctx_.now, ctx_.budget);
}

bool DisplayPage::on_event(Event e) {
    if (e.press != Press::Short) return false;
    if (view_ == View::Adjust) {
        if (e.button == Button::Plus || e.button == Button::Minus) {
            idle::Config c = idle_hw::config();
            c.brightness = uidisp::step_brightness(c.brightness, e.button == Button::Plus ? 1 : -1);
            idle_hw::set_config(c);
            update(ctx_.now);
            return true;
        }
        if (e.button == Button::Ok || e.button == Button::Back) {
            show(View::List);
            return true;
        }
        return false;
    }
    if (e.button == Button::Plus || e.button == Button::Minus) {
        cur() = std::clamp(cur() + (e.button == Button::Plus ? 1 : -1), 0, count() - 1);
        update(ctx_.now);
        return true;
    }
    if (e.button == Button::Ok) {
        if (view_ == View::List) {
            show(sel_ == 0 ? View::Adjust : sel_ == 1 ? View::PickDim : View::PickOff);
            return true;
        }
        idle::Config c = idle_hw::config();
        if (view_ == View::PickDim) c.dim_after_s = uidisp::kDimAfter[static_cast<std::size_t>(pick_)];
        else c.off_after_s = uidisp::kOffAfter[static_cast<std::size_t>(pick_)];
        idle_hw::set_config(c);
        show(View::List);
        return true;
    }
    if (e.button == Button::Back && view_ != View::List) {   // out of a pick list, not out of the page
        show(View::List);
        return true;
    }
    return false;
}

Strip DisplayPage::labels() const {
    Strip l = base_labels();
    if (view_ == View::Adjust) {
        const uint8_t b = idle_hw::config().brightness;
        l[Button::Minus] = uidisp::can_step(b, -1) ? sl::Dec : sl::DecDim;
        l[Button::Plus] = uidisp::can_step(b, 1) ? sl::Inc : sl::IncDim;
        l[Button::Ok] = sl::Done;
        return l;
    }
    l = list_strip(l, view_ >= View::PickDim ? pick_ : sel_, count());
    l[Button::Ok] = view_ == View::List ? (sel_ == 0 ? sl::Set : sl::Pick) : sl::Select;
    return l;
}

// ---------------------------------------------------------------------------------------
// Ring position 7 TOOLS: triggers (Request voltage, Fast charge) and active bench utilities
// ---------------------------------------------------------------------------------------


int ToolsPage::rows() const { return bench::tools().n + 1; }   // the tools, then the PD limiter

void ToolsPage::enter() {
    view_ = View::List;
    title_ = rail::kTools;
    sel_ = std::clamp(sel_, 0, rows() - 1);
    seen_ = fusb302::limit_status().state;   // a give-up while elsewhere shows on the Charger slice, not as a late result
}

pd::LimTech &ToolsPage::cur_tech() {
    const View tv = in_sub_pick() ? tech_view_ : view_;
    return tv == View::EditFixed ? draft_.fixed : tv == View::EditPps ? draft_.pps : draft_.avs;
}
const pd::LimTech &ToolsPage::cur_tech() const {
    const View tv = in_sub_pick() ? tech_view_ : view_;
    return tv == View::EditFixed ? draft_.fixed : tv == View::EditPps ? draft_.pps : draft_.avs;
}

uint16_t ToolsPage::tech_max_ma() const {
    const View tv = in_sub_pick() ? tech_view_ : view_;
    const uint16_t m = tv == View::EditFixed ? fixed_max_ma_ : tv == View::EditPps ? pps_max_ma_ : avs_max_ma_;
    return m >= pd::kLimMinFixedMa ? std::min<uint16_t>(m, pd::kLimMaxMa) : pd::kLimMaxMa;
}

// The A list for the voltage in play: Fixed takes that voltage's offer, PPS the APDOs covering it, SPR AVS its 15 V or
// 20 V current field; no caps (or no match)
// keeps the tech's ceiling. A stored current above the offer reads as Max.
void ToolsPage::open_amps() {
    const uint16_t mv = va_ ? pend_mv_ : cur_tech().max_mv;
    uint16_t offer = 0;
    const pd::SourceCaps &c = held::shown_caps();
    if (has_caps_ && c.valid) {
        const std::span<const pd::Pdo> p(c.pdos.data(), std::min<std::size_t>(c.num_pdos, c.pdos.size()));
        offer = tech_view_ == View::EditFixed ? pd::lim_fixed_offer_ma(p, mv)
              : tech_view_ == View::EditPps   ? pd::lim_pps_offer_ma(p, mv)
                                              : pd::lim_avs_offer_ma(p, mv);
    }
    amp_offer_ma_ = offer ? std::min(offer, pd::kLimMaxMa) : tech_max_ma();
    const uint16_t ma = cur_tech().max_ma;
    amp_pick_ma_ = ma > amp_offer_ma_ ? 0 : ma;
}

// A view change repaints the body: row count and title differ across List, PickLimit, tech editors and pick lists.
void ToolsPage::show(View v) {
    const View was = view_;
    if (was >= View::EditFixed && was <= View::EditAvs) tech_view_ = was;
    view_ = v;
    if (v == View::PickLimit) {
        build_picks();
        if (was == View::List) {
            const pd::LimStatus &s = fusb302::limit_status();
            draft_ = s.state != pd::LimState::Off ? s.cfg : pd::LimConfig{};
            pick_ = s.state != pd::LimState::Off ? 1 : 0;
            if (!has_caps_) ctx_.toast.show(FooterTone::Info, text::tools::kLimToastNoOffers, ctx_.now);
        }
        title_ = rail::kLimit;
        drop_confirm();
    } else if (in_tech()) {
        if (was == View::PickLimit) edit_idx_ = 0;
        va_ = false;
        title_ = rail::kLimit;
        drop_confirm();
    } else if (in_sub_pick()) {
        if (v == View::PickMode) {
            sub_pick_ = cur_tech().enabled ? 0 : 1;
        } else {
            if (v == View::PickMaxA) open_amps();
            const uint16_t cur_v = v == View::PickMaxV ? (va_ ? pend_mv_ : cur_tech().max_mv)
                                 : amp_pick_ma_;
            sub_pick_ = 0;
            const int n = sub_count();
            for (int i = 0; i < n; ++i) {
                if (sub_val(i) == cur_v) { sub_pick_ = i; break; }
            }
        }
        title_ = rail::kLimit;
    } else {
        title_ = rail::kTools;
        drop_confirm();
    }
    if (!ctx_.blank) pg::clear_body(ctx_.content, kit::kBelowBar);
    title_bar();
    ctx_.rows.set_count(count());
    ctx_.rows.select(cur());
    invalidate();
    update(ctx_.now);
}

// Extract Fixed voltages and PPS/SPR-AVS ceilings from held/live caps for the per-tech picker steps.
void ToolsPage::build_picks() {
    const pd::SourceCaps &c = held::shown_caps();
    has_caps_ = c.valid;
    nfixed_ = 0;
    fixed_max_ma_ = 0;
    has_pps_ = false; pps_min_mv_ = 0; pps_max_mv_ = 0; pps_max_ma_ = 0;
    has_avs_ = false; avs_max_ma_ = 0;
    if (!c.valid) return;
    const std::size_t n = std::min<std::size_t>(c.num_pdos, c.pdos.size());
    for (std::size_t i = 0; i < n; ++i) {
        const pd::Pdo &p = c.pdos[i];
        if (p.type == pd::PdoType::Fixed && p.max_mv >= 5000 && p.max_mv <= pd::kLimMaxMv) {
            fixed_max_ma_ = std::max(fixed_max_ma_, p.max_ma);
            bool dup = false;
            for (uint8_t j = 0; j < nfixed_; ++j) dup |= fixed_mvs_[j] == p.max_mv;
            if (!dup && nfixed_ < pd::kMaxPdos) fixed_mvs_[nfixed_++] = p.max_mv;
        } else if (p.type == pd::PdoType::Pps) {
            has_pps_ = true;
            pps_min_mv_ = pps_min_mv_ == 0 ? p.min_mv : std::min(pps_min_mv_, p.min_mv);
            pps_max_mv_ = std::max(pps_max_mv_, p.max_mv);
            pps_max_ma_ = std::max(pps_max_ma_, p.max_ma);
        } else if (p.type == pd::PdoType::Avs && (p.raw >> 28) == 0xE) {
            has_avs_ = true;
            const uint16_t m15 = static_cast<uint16_t>(((p.raw >> 10) & 0x3FFu) * 10u);
            avs_max_ma_ = std::max(avs_max_ma_, std::max(m15, p.max_ma));
        }
    }
    for (uint8_t i = 1; i < nfixed_; ++i)
        for (uint8_t j = i; j > 0 && fixed_mvs_[j] < fixed_mvs_[j - 1]; --j) std::swap(fixed_mvs_[j], fixed_mvs_[j - 1]);
}

namespace {
inline constexpr uint16_t kDefaultFxMvs[] = {5000, 9000, 12000, 15000, 20000};}  // namespace

void ToolsPage::pps_bounds(uint16_t &lo, uint16_t &hi) const {
    const uint16_t lo_raw = pps_min_mv_ >= 3000 ? pps_min_mv_ : 5000;
    lo = static_cast<uint16_t>(((lo_raw + 999u) / 1000u) * 1000u);
    hi = pps_max_mv_ >= lo ? static_cast<uint16_t>((pps_max_mv_ / 1000u) * 1000u) : 21000;
}

int ToolsPage::sub_count() const {
    if (view_ == View::PickMode) return 2;
    if (view_ == View::PickMaxV) {
        if (tech_view_ == View::EditFixed) return 1 + (nfixed_ > 0 ? int(nfixed_) : int(std::size(kDefaultFxMvs)));
        if (tech_view_ == View::EditPps) {
            uint16_t lo, hi;
            pps_bounds(lo, hi);
            return 1 + int((hi - lo) / pd::kLimPpsStepMv) + 1;
        }
        return 2;   // SPR AVS: Max, 15V
    }
    return pd::lim_amp_count(amp_offer_ma_);
}

uint16_t ToolsPage::sub_val(int index) const {
    if (index <= 0) return 0;
    if (view_ == View::PickMaxV) {
        if (tech_view_ == View::EditFixed) {
            const uint16_t *vals = nfixed_ > 0 ? fixed_mvs_ : kDefaultFxMvs;
            return vals[index - 1];
        }
        if (tech_view_ == View::EditPps) {
            uint16_t lo, hi;
            pps_bounds(lo, hi);
            return static_cast<uint16_t>(lo + (index - 1) * pd::kLimPpsStepMv);
        }
        return 15000;
    }
    return pd::lim_amp_at(amp_offer_ma_, index);
}

void ToolsPage::fmt_tech_summary(const pd::LimTech &t, StrBufBase &out) const {
    out.clear();
    if (!t.enabled) { out.put(text::tools::kLimOff); return; }
    if (t.max_mv == 0 && t.max_ma == 0) { out.put(text::tools::kLimMax); return; }
    if (t.max_mv) {
        pg::put_exact_unit(out, t.max_mv, 'V');
        if (t.max_ma) out.put_char(' ');
    }
    if (t.max_ma) pg::put_exact_unit(out, t.max_ma, 'A');
}

void ToolsPage::draw_static() {
    title_bar();
    ctx_.rows.init(ctx_.content, kit::kBodyY, &ToolsPage::row_fn, this);
    ctx_.rows.set_count(count());
    ctx_.rows.select(cur());
}

void ToolsPage::tech_row(int index, bool focused, kit::RowSpec &out) const {
    namespace tx = text::tools;
    struct Meta { const char *text, *note; };
    static constexpr Meta kMeta[3] = {
        {tx::kLimRowMode, tx::kLimNoteMode},
        {tx::kLimRowMaxV, tx::kLimNoteStepV},
        {tx::kLimRowMaxA, tx::kLimNoteStepA},
    };
    const Meta &m = kMeta[index];
    const pd::LimTech &t = cur_tech();
    tail_.clear();
    out.text = m.text;
    if (focused) out.note = m.note;
    if (index == 0) {
        out.tail = t.enabled ? tx::kLimOn : tx::kLimOff;
        out.tail_color = t.enabled ? kOk : kCaution;
        return;
    }
    const uint16_t val = index == 1 ? t.max_mv : t.max_ma;
    if (val == 0) tail_.put(tx::kLimMax);
    else pg::put_exact_unit(tail_, val, index == 1 ? 'V' : 'A');
    out.tail = tail_.view();
    out.tail_color = val ? kCaution : kDim;
}

void ToolsPage::sub_pick_row(int index, bool focused, kit::RowSpec &out) const {
    namespace tx = text::tools;
    const pd::LimTech &t = cur_tech();
    out.marker = kit::Marker::Radio;
    if (view_ == View::PickMode) {
        const bool en = index == 0;
        out.state = t.enabled == en ? 1 : 0;
        out.text = en ? tx::kLimOn : tx::kLimOff;
        if (focused) out.note = tx::kLimNoteMode;
        return;
    }
    const bool is_v = view_ == View::PickMaxV;
    const uint16_t v = sub_val(index);
    const uint16_t cur_v = is_v ? t.max_mv : amp_pick_ma_;
    out.state = cur_v == v ? 1 : 0;
    tail_.clear();
    if (v == 0) tail_.put(tx::kLimMax);
    else pg::put_exact_unit(tail_, v, is_v ? 'V' : 'A');
    out.text = tail_.view();
    if (focused && !is_v && va_) {   // the voltage this current goes with
        note_.clear();
        note_.put(tx::kLimNoteAmpsAt);
        if (pend_mv_) pg::put_exact_unit(note_, pend_mv_, 'V');
        else note_.put(tx::kLimNoteAnyV);
        out.note = note_.view();
    }
}

void ToolsPage::limit_row(int index, bool focused, kit::RowSpec &out) const {
    namespace tx = text::tools;
    const pd::LimStatus &s = fusb302::limit_status();
    const bool armed = s.state != pd::LimState::Off;
    if (view_ == View::List) {
        out.text = tx::kLimName;
        tail_.clear();
        if (armed) {
            char lb[pd::kLimLabelMax + 1]{};
            const std::size_t ln = pd::lim_label(s.cfg, lb);
            tail_.put(std::string_view{lb, ln});
            out.tail_color = kCaution;
        } else {
            tail_.put(tx::kLimOff);
        }
        out.tail = tail_.view();
        if (focused) {
            if (armed) {
                note_.clear();
                note_.put(tx::kLimNoteAbove).put(tail_.view());
                out.note = note_.view();
            } else {
                out.note = tx::kLimNoteOff;
            }
        }
        return;
    }
    if (in_sub_pick()) {
        sub_pick_row(index, focused, out);
        return;
    }
    if (in_tech()) {
        tech_row(index, focused, out);
        return;
    }
    if (index == 0) {
        out.marker = kit::Marker::Radio;
        out.state = armed ? 0 : 1;
        out.text = tx::kLimOff;
        if (focused) out.note = tx::kLimNoteNone;
        return;
    }
    if (index >= 1 && index <= 3) {
        struct Meta { const char *text, *note; };
        static constexpr Meta kMeta[3] = {
            {tx::kLimFixed, nullptr},   // pd::lim_fixed_note
            {tx::kLimPps,   tx::kLimNotePps},
            {tx::kLimAvs,   tx::kLimNoteAvs},
        };
        const int k = index - 1;
        out.text = kMeta[k].text;
        const bool avail = k == 0 || !has_caps_ || (k == 1 ? has_pps_ : has_avs_);
        if (!avail) {
            out.text_color = kInk3;
            out.tail = "none";
            out.tail_color = kInk3;
            if (focused) out.note = tx::kLimNoteNoOffer;
            return;
        }
        const pd::LimTech *techs[3] = {&draft_.fixed, &draft_.pps, &draft_.avs};
        const pd::LimTech &t = *techs[k];
        fmt_tech_summary(t, tail_);
        out.tail = tail_.view();
        out.tail_color = !t.enabled || t.max_mv || t.max_ma ? kCaution : (held::showing_held() ? kStale : kDim);
        if (held::showing_held() && has_caps_) out.text_color = kStale;
        out.chevron = true;
        if (focused && k == 0) {   // the Fixed range follows the limit picked
            char b[pd::kLimNoteMax + 1];
            note_.clear();
            note_.put(std::string_view(b, pd::lim_fixed_note(t, b)));
            out.note = note_.view();
        } else if (focused) {
            out.note = kMeta[k].note;
        }
        return;
    }
    if (index == 4) {
        out.text = tx::kLimApply;
        if (draft_.limit_mw) {
            tail_.clear();
            tail_.put_u32(pd::lim_watts(draft_.limit_mw)).put_char('W');
            out.tail = tail_.view();
            out.tail_color = kCaution;
        }
        if (focused) out.note = tx::kLimNoteApply;
        return;
    }
    out.text = tx::kLimRowReset;
    if (focused) out.note = tx::kLimNoteReset;
}

void ToolsPage::row_fn(void *ctx, int index, bool focused, kit::RowSpec &out) {
    auto *self = static_cast<ToolsPage *>(ctx);
    const bench::Acts b = bench::tools();
    if (self->view_ != View::List || index == b.n) {
        self->limit_row(index, focused, out);
        return;
    }
    if (index < 0 || index >= b.n) return;
    const bench::Act &a = b.a[index];
    out.text = a.name;
    const SetupState ss = setup_state(self->ctx_);
    const Readiness rd = a.kind == bench::ActKind::Tool ? tool_readiness(a.arg, ss, self->ctx_.pm_online) : Readiness{};
    if (rd.level == ToolLevel::Blocked) {
        out.text_color = kInk3;
        out.tail = rd.tag;
        out.tail_color = kInk3;
        out.chevron = false;
    } else {
        if (rd.level == ToolLevel::Caution) {
            out.tail = rd.tag;
            out.tail_color = kCaution;
        }
        out.chevron = rd.level != ToolLevel::Caution;
    }
    if (focused) {
        out.note = (rd.level != ToolLevel::Ready && rd.reason && rd.reason[0]) ? rd.reason : bench::act_hint(a);
    }
}

// The limiter ended on its own (it gave up): say why once, if this page is on screen. A user's off or a detach says nothing.
void ToolsPage::limit_edge() {
    namespace tx = text::tools;
    const pd::LimStatus &s = fusb302::limit_status();
    if (seen_ != pd::LimState::Off && s.state == pd::LimState::Off) {
        const char *m = nullptr;
        switch (s.why) {
        case pd::LimWhy::Rate: m = tx::kLimToastRate; break;
        case pd::LimWhy::ChargerResets: m = tx::kLimToastChargerResets; break;
        case pd::LimWhy::SinkResets: m = tx::kLimToastSinkResets; break;
        case pd::LimWhy::SinkIgnores: m = tx::kLimToastSinkIgnores; break;
        case pd::LimWhy::Epr: m = tx::kLimToastEpr; break;
        case pd::LimWhy::NoFit: m = tx::kLimToastNoFit; break;
        case pd::LimWhy::ChargerRefuses: m = tx::kLimToastChargerRefuses; break;
        default: break;
        }
        if (m) ctx_.toast.show(FooterTone::Caution, m, ctx_.now);
    }
    seen_ = s.state;
}

void ToolsPage::update(uint32_t) {
    title_badge(false);
    limit_edge();
    if (view_ == View::PickLimit) build_picks();
    cur() = std::clamp(cur(), 0, count() - 1);
    ctx_.rows.set_count(count());
    ctx_.rows.select(cur());
    ctx_.rows.draw(ctx_.now, ctx_.budget);
}

void ToolsPage::drop_confirm() {
    if (confirm_until_ != 0 && ctx_.toast.view() == text::tools::kLimToastConfirm) ctx_.toast.clear();
    confirm_until_ = 0;
}

void ToolsPage::ask_confirm(uint32_t key) {
    confirm_key_ = key;
    confirm_until_ = ctx_.now + Ui::kConfirmWindowMs;
    if (confirm_until_ == 0) confirm_until_ = 1;
    ctx_.toast.show(FooterTone::Caution, text::tools::kLimToastConfirm, ctx_.now, Ui::kConfirmWindowMs);
}

namespace {
// A limiter result toast, indexed by the result enum; no text = no toast, dwell 0 = the default.
struct LimToast {
    const char *msg;
    FooterTone tone;
    uint16_t dwell_ms;
};
void show_lim_toast(Toast &toast, const LimToast &t, uint32_t now) {
    if (t.msg) toast.show(t.tone, t.msg, now, t.dwell_ms ? t.dwell_ms : kToastDefaultMs);
}
}  // namespace

bool ToolsPage::apply_limit(bool off) {
    namespace tx = text::tools;
    const uint32_t key = off ? 0u : 1u;
    if (!off) {
        if (!draft_.fixed.enabled && !draft_.pps.enabled && !draft_.avs.enabled) {
            ctx_.toast.show(FooterTone::Caution, tx::kLimToastKeepOne, ctx_.now);
            return true;
        }
        if (!pd::lim_config_active(draft_)) {
            ctx_.toast.show(FooterTone::Caution, tx::kLimToastNoLimit, ctx_.now);
            return true;
        }
        const pd::SourceCaps &c = held::shown_caps();   // refused before any reset: the list could not carry it
        if (c.valid && !pd::lim_fits({c.pdos.data(), std::min<size_t>(c.num_pdos, c.pdos.size())}, draft_)) {
            ctx_.toast.show(FooterTone::Caution, tx::kLimToastNoFit, ctx_.now);
            return true;
        }
        const pd::LimStatus &s = fusb302::limit_status();
        if (s.state != pd::LimState::Off && s.cfg == draft_) return false;
    }
    const bool confirmed = confirmed_for(key);
    drop_confirm();
    const Ui::LimitSet ls = ctx_.ui->pd_limit_set(off ? pd::LimConfig{} : draft_, off, confirmed, ctx_.now);
    if (ls == Ui::LimitSet::NeedConfirm) { ask_confirm(key); return true; }
    static constexpr LimToast kSetToast[] = {
        {nullptr, FooterTone::Info, 0},                   // Done
        {tx::kLimToastResetting, FooterTone::Info, 2500}, // DoneReset
        {nullptr, FooterTone::Info, 0},                   // NeedConfirm
        {tx::kLimToastNoReset, FooterTone::Caution, 0},   // NoReset
        {tx::kLimToastBusy, FooterTone::Caution, 0},      // Busy
        {tx::kLimToastBadLimit, FooterTone::Error, 0},    // BadLimit
    };
    static_assert(std::size(kSetToast) == static_cast<size_t>(Ui::LimitSet::BadLimit) + 1);
    show_lim_toast(ctx_.toast, kSetToast[static_cast<unsigned>(ls)], ctx_.now);
    return false;
}

bool ToolsPage::on_event(Event e) {
    if (e.press != Press::Short) return false;
    namespace tx = text::tools;
    if (e.button == Button::Plus || e.button == Button::Minus) {
        cur() = std::clamp(cur() + (e.button == Button::Plus ? 1 : -1), 0, count() - 1);
        drop_confirm();   // a moved selection is another question
        update(ctx_.now);
        return true;
    }
    if (e.button == Button::Ok) {
        const bench::Acts b = bench::tools();
        if (view_ == View::List) {
            if (sel_ >= b.n) show(View::PickLimit);
            else open_bench_act(*ctx_.ui, b.a[sel_], ctx_.now);
            return true;
        }
        if (view_ == View::PickLimit) {
            if (pick_ == 0 || pick_ == 4) {
                if (apply_limit(pick_ == 0)) update(ctx_.now);
                else show(View::List);
            } else if (pick_ >= 1 && pick_ <= 3) {
                const bool ok[3] = {true, !has_caps_ || has_pps_, !has_caps_ || has_avs_};
                if (ok[pick_ - 1]) show(static_cast<View>(static_cast<int>(View::EditFixed) + pick_ - 1));
            } else if (!has_caps_ && pick_ == 5) {
                constexpr uint32_t kResetKey = 0xFFFFFFFFu;
                const bool confirmed = confirmed_for(kResetKey);
                drop_confirm();
                const Ui::LinkResetResult lr = ctx_.ui->link_reset(confirmed, ctx_.now);
                if (lr == Ui::LinkResetResult::NeedConfirm) {
                    ask_confirm(kResetKey);
                } else {
                    static constexpr LimToast kLrToast[] = {
                        {tx::kLimToastRescan, FooterTone::Info, 2500},        // Cycled
                        {tx::kLimToastResetting, FooterTone::Info, 2500},     // HardReset
                        {nullptr, FooterTone::Info, 0},                       // NeedConfirm
                        {tx::kLimToastSelfPowered, FooterTone::Caution, 0},   // SelfPowered
                        {tx::kLimToastRetry, FooterTone::Caution, 1500},      // Busy
                        {tx::kLimToastNoLink, FooterTone::Info, 1500},        // NoLink
                    };
                    static_assert(std::size(kLrToast) == static_cast<size_t>(Ui::LinkResetResult::NoLink) + 1);
                    show_lim_toast(ctx_.toast, kLrToast[static_cast<unsigned>(lr)], ctx_.now);
                }
                update(ctx_.now);
            }
            return true;
        }
        if (in_tech()) {
            show(edit_idx_ == 0 ? View::PickMode : edit_idx_ == 1 ? View::PickMaxV : View::PickMaxA);
            return true;
        }
        // In PickMode / PickMaxV / PickMaxA: commit selection and return to the tech view.
        if (view_ == View::PickMaxV) {   // on to this voltage's current; both commit there
            pend_mv_ = sub_val(sub_pick_);
            va_ = true;
            show(View::PickMaxA);
            return true;
        }
        if (view_ == View::PickMode) cur_tech().enabled = (sub_pick_ == 0);
        else {
            if (va_) cur_tech().max_mv = pend_mv_;
            cur_tech().max_ma = sub_val(sub_pick_);
        }
        draft_.limit_mw = 0;
        show(tech_view_);
        return true;
    }
    if (e.button == Button::Back && view_ != View::List) {   // step back to tech view, PickLimit, or List, never out of Tools
        if (view_ == View::PickMaxA && va_) {   // back to the voltages, nothing committed
            show(View::PickMaxV);
            va_ = false;
            return true;
        }
        show(in_sub_pick() ? tech_view_ : in_tech() ? View::PickLimit : View::List);
        return true;
    }
    return false;
}

Strip ToolsPage::labels() const {
    const int at = view_ == View::List ? sel_ : view_ == View::PickLimit ? pick_ : in_tech() ? edit_idx_ : sub_pick_;
    Strip l = list_strip(base_labels(), at, count());
    if (view_ == View::List) {
        const bench::Acts b = bench::tools();
        bool blocked = false;
        if (sel_ >= 0 && sel_ < b.n && b.a[sel_].kind == bench::ActKind::Tool)
            blocked = tool_readiness(b.a[sel_].arg, setup_state(ctx_), ctx_.pm_online).level == ToolLevel::Blocked;
        l[Button::Ok] = sel_ >= b.n ? sl::Pick : sl::dim_if(sl::Open, blocked);   // a blocked tool's verb greys with its row
    } else if (view_ == View::PickLimit) {
        const bool offered = pick_ == 1 || !has_caps_ || (pick_ == 2 ? has_pps_ : has_avs_);
        l[Button::Ok] = (pick_ >= 1 && pick_ <= 3) ? sl::dim_if(sl::Open, !offered) : sl::Select;
    } else if (in_tech()) {
        l[Button::Ok] = sl::Pick;
    } else {
        l[Button::Ok] = sl::Select;
    }
    return l;
}

// ---------------------------------------------------------------------------------------
// USB link submenu (what enumerates at the next boot)
// ---------------------------------------------------------------------------------------

namespace {
constexpr int kUlRows = usbcfg::rows();   // 2 without the network function (K2_NET=0) and the WebUSB transport (K2_WEBUSB=0)
constexpr usbcfg::Row ul_row(int i) { return usbcfg::row_at(i); }
struct UlMeta { const char *name, *hint; };
constexpr UlMeta kUlMeta[4] = {
    {"HID", K2_P("logs and remote")},
    {K2_P("Drive"), K2_P("read-only files")},
    {K2_P("Network"), K2_P("web 192.168.7.1")},
    {K2_P("WebUSB"), K2_P("web + remote")},
};
bool ul_on(const usbcfg::Setting &s, usbcfg::Row r) {
    switch (r) {
        case usbcfg::Row::Hid: return s.hid;
        case usbcfg::Row::Drive: return s.drive;
        case usbcfg::Row::Net: return s.net;
        case usbcfg::Row::Webusb: return s.webusb;
    }
    return false;
}
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

// One row per USB function: the name, ON / OFF at the right and, focused, what it carries.
void UsbLinkPage::row_fn(void *ctx, int i, bool, kit::RowSpec &out) {
    const auto *self = static_cast<const UsbLinkPage *>(ctx);
    const usbcfg::Row r = ul_row(i);
    const UlMeta &m = kUlMeta[static_cast<unsigned>(r)];
    const bool on = ul_on(self->pending_, r);
    out.text = m.name;
    out.tail = on ? "ON" : "OFF";
    out.tail_color = on ? kOk : kDim;
    out.note = usbcfg::same_mode(self->pending_, usbcfg::current) ? m.hint : K2_P("Replug to apply");
}

void UsbLinkPage::update(uint32_t) {
    title_badge(false);
    ctx_.rows.select(sel_);
    ctx_.rows.draw(ctx_.now, ctx_.budget);
}

bool UsbLinkPage::on_event(Event e) {
    if (e.press != Press::Short) return false;
    if (e.button == Button::Plus || e.button == Button::Minus) {
        sel_ = std::clamp(sel_ + (e.button == Button::Plus ? 1 : -1), 0, kUlRows - 1);   // stops at the ends, as every list
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

Strip UsbLinkPage::labels() const {
    Strip l = list_strip(base_labels(), sel_, kUlRows);
    l[Button::Ok] = sl::Toggle;
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
    struct PaintWork {
        union {
            std::uint8_t temp[qrtest::kWsBufLen];
            std::uint16_t line[qrview::kPanelPx];
        };
        std::uint8_t code[qrtest::kWsBufLen];
    };
    static_assert(sizeof(PaintWork) <= scratch::kSize, "qrtest paint fits the scratch arena");
    scratch::Lease lease("qrtest");   // k2-plain: scratch owner tag
    auto &pw = scratch::as<PaintWork>();
    link_len_ = qrtest::build_link(version_, px_, std::span<char>(reinterpret_cast<char*>(pw.temp), qrtest::kWsBufLen));
    n_ = link_len_ != 0 && version_ >= qrtest::kVersionMin && version_ <= qrtest::kMaxShownVersion
             ? qr::encode_placed(pw.code, pw.temp, link_len_, version_)
             : 0;
    lcd::fill(qrview::kWhite);   // the whole panel, button strip included (erase pass: to the background)
    if (n_ != 0)
        qrview::draw_code_raw_in(qrview::Area{}, pw.code, n_, 0, px_, pw.line, quiet_, px_);
    painted_ = true;
}

bool QrTestPage::on_event(Event) {
    return false;   // every button is declined: a short BACK pops the page (the Ui does it)
}

Strip QrTestPage::labels() const {
    Strip l = base_labels();
    l[Button::Ok] = l[Button::Minus] = l[Button::Plus] = sl::None;
    return l;
}

}  // namespace k2
