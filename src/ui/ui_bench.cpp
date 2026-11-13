// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The Bench (Fieldbook, CONCEPT 7): seven places on one page, each a row with what is known about it, and a place's actions on
// the same page. The tables and the row texts are pure (bench:: in ui_tools.cpp); this file reads the drivers and paints.
#include <cstring>
#include <string_view>
#include "k2/battery/battery_hw.hpp"
#include "k2/core/version.hpp"
#include "k2/drivers/fusb302.hpp"
#include "k2/drivers/usbcfg.hpp"
#include "k2/fastcharge/fastcharge_classify.hpp"
#include "k2/pd/diag_live.hpp"
#include "k2/pd/link.hpp"
#include "k2/pd/pd_source.hpp"
#include "k2/pd/pdtrace.hpp"
#include "k2/ui/battery_view.hpp"
#include "k2/ui/home_model.hpp"
#include "k2/ui/text/tools.hpp"
#include "k2/ui/ui.hpp"
#include "k2/ui/ui_pages.hpp"
#include "k2/ui/ui_style.hpp"
#include "k2/ui/ui_widgets.hpp"

namespace k2 {

namespace {
using bench::Place;
constexpr Icon kPlaceIcons[bench::kPlaces] = {Icon::Meter, Icon::Plug, Icon::Cable, Icon::Phone, Icon::Battery, Icon::Report,
                                              Icon::Chip};
static_assert(ToolsPage::kBenchEnd <= kit::kListBottom, "the seven places end above the strip the Ui lays before the footer");
static_assert(ToolsPage::kNameDy + 16 <= ToolsPage::kStatusDy, "a row's name and status line do not overlap");
static_assert(ToolsPage::kStatusDy + 12 <= ToolsPage::kRowPitch, "a row's status line stays inside the row");
static_assert(ToolsPage::kTextX + bench::kStatusChars * 6 <= ToolsPage::kIconX, "a status line ends before the icon");

// A fast-charge verdict worth naming on the Charger row (PD speaks for itself through its capabilities).
const char *fc_word(fastcharge::Protocol p) {
    using P = fastcharge::Protocol;
    switch (p) {
    case P::Pending: case P::NoInput: case P::NoQuick: case P::UsbData: case P::PdLikely: case P::Pd: return nullptr;
    default: return fastcharge::short_name(p);
    }
}

uint32_t mix(uint32_t h, uint32_t v) { return (h ^ v) * 16777619u; }
}  // namespace

const char *ToolsPage::tool_name(int i) {
    static_assert(text::tools::kSlots == kToolSlots);
    return text::tools::kNames[i < 0 || i >= kTools ? 0 : i];   // the hub's copy: include/k2/ui/text/tools.hpp
}

int ToolsPage::selected() const {
    const int s = ctx_.keep.bench_sel;
    return s < 0 ? 0 : s >= bench::kPlaces ? bench::kPlaces - 1 : s;
}

bench::Facts ToolsPage::facts() const {
    bench::Facts f;
    const power::Metrics &pm = ctx_.pm;
    f.vbus = ctx_.pm_online && pm.vbus_mv >= 1000;
    f.mv = pm.vbus_mv;
    f.ma = pm.current_ma;
    f.mw = pm.power_mw;
    const pd::Link &link = ctx_.ui->link();
    const bool pd_link = link.attached() && !link.waiting();
    const pd::SourceCaps &caps = pd::active_caps();
    if (pd_link && caps.valid && caps.num_pdos > 0) {
        f.caps = true;
        f.spec_rev = caps.spec_rev;
        f.offers = caps.num_pdos;
        for (int i = 0; i < caps.num_pdos && i < static_cast<int>(pd::kMaxPdos); i++)
            f.max_mw = caps.pdos[i].max_mw > f.max_mw ? caps.pdos[i].max_mw : f.max_mw;
    }
    if (f.vbus) f.fc = fc_word(ctx_.fc);
    const pd::Emarker &em = fusb302::emarker();
    if (em.valid) {
        f.cable = true;
        f.cable_a = em.max_current_a;
        f.cable_v = em.max_voltage_v;
        f.cable_w = em.max_power_w;
    }
    const pd::Contract &ctr = fusb302::contract();
    if (pd_link && ctr.valid && ctr.mv) {
        f.contract = true;
        f.ctr_mv = ctr.mv;
        f.ctr_ma = ctr.op_ma;
    }
    const home::Mode m = !ctx_.pm_online ? home::Mode::NoMeter
                       : pm.vbus_mv >= 1000 ? home::Mode::Live
                       : ctx_.stale() ? home::Mode::Stale : home::Mode::Idle;
    home::DiagText dt;
    if (home::format_diag(m, pd::diag::live::report(), dt)) f.diag.put(dt.value.view());
    for (std::size_t i = 0; i < pdtrace::kCapSlots; i++)
        f.captures = static_cast<uint8_t>(f.captures + (pdtrace::capture_info(i).trigger != pdtrace::Trigger::None));
    f.version = kFwVersion;
    f.hid = usbcfg::current.hid;
    const battery::Recorder &rec = battery::hw::service().rec();
    if (rec.state() != battery::State::Idle) {
        battery::Record r;
        rec.to_record(r);
        f.batt = true;
        battery::view::bench_status(battery::view::shown(r, true), f.batt_status);
    }
    return f;
}

void ToolsPage::draw_static() {
    title_bar();
    // paint_bench lays the whole body from the first update
    for (int i = 0; i < bench::kPlaces; i++)
        status_[i].init(ctx_.content, kLabelFont, kTextX, static_cast<int16_t>(kRowY0 + i * kRowPitch + kStatusDy),
                        bench::kStatusChars, Align::Left, kInk2, kBg);
}

// One Bench row, every pixel of it but the status line (a Field, painted after this): the rail and the node (solid = something
// known, hollow = nothing), the place name (font 16) and the place icon, on the row's ground (the focus fill, or the Classic bar).
void ToolsPage::paint_row(int i, bool known, bool focus) {
    const Surface &s = ctx_.content;
    const int16_t y = static_cast<int16_t>(kRowY0 + i * kRowPitch);
    const bool bar = focus && kFocusBar != 0;
    const uint16_t bg = focus ? kFocusFill : kBg;
    const uint16_t ink = focus ? kOnFocus : kInk, dim = focus ? kOnFocusDim : kInk2, line = focus ? kOnFocus : kAccent;
    const std::string_view name = bench::place_name(static_cast<Place>(i));
    const int16_t node_y = static_cast<int16_t>(y + kNodeDy);
    const int16_t below = static_cast<int16_t>(kRowPitch - kNodeDy - kNodeSz);
    kit::Box holes[6];
    int n = 0;
    holes[n++] = {kTextX, static_cast<int16_t>(y + kNameDy), static_cast<int16_t>(name.size() * kSmall.adv), kSmall.height};
    holes[n++] = {kTextX, static_cast<int16_t>(y + kStatusDy), status_[i].width_px(), kLabelFont.height};
    holes[n++] = {kIconX, static_cast<int16_t>(y + kIconDy), kIconSize, kIconSize};
    holes[n++] = {kNodeX, node_y, kNodeSz, kNodeSz};
    if (i > 0) holes[n++] = {kRailX, y, kRailW, kNodeDy};
    if (i + 1 < bench::kPlaces) holes[n++] = {kRailX, static_cast<int16_t>(node_y + kNodeSz), kRailW, below};
    const int16_t x0 = bar ? 3 : 0;
    if (bar) gfx_fill(s, 0, y, 3, kRowPitch, kFocusBar);
    kit::fill_except(s, {x0, y, static_cast<int16_t>(s.w - x0), kRowPitch}, holes, n, bg);
    if (i > 0) gfx_fill(s, kRailX, y, kRailW, kNodeDy, line);
    if (i + 1 < bench::kPlaces) gfx_fill(s, kRailX, static_cast<int16_t>(node_y + kNodeSz), kRailW, below, line);
    if (known) {
        gfx_fill(s, kNodeX, node_y, kNodeSz, kNodeSz, line);
    } else {   // a 2 px frame round the row's ground
        gfx_fill(s, kNodeX, node_y, kNodeSz, 2, line);
        gfx_fill(s, kNodeX, static_cast<int16_t>(node_y + 2), 2, kNodeSz - 4, line);
        gfx_fill(s, kNodeX + 2, static_cast<int16_t>(node_y + 2), kNodeSz - 4, kNodeSz - 4, bg);
        gfx_fill(s, kNodeX + kNodeSz - 2, static_cast<int16_t>(node_y + 2), 2, kNodeSz - 4, line);
        gfx_fill(s, kNodeX, static_cast<int16_t>(node_y + kNodeSz - 2), kNodeSz, 2, line);
    }
    gfx_text(s, kSmall, kTextX, static_cast<int16_t>(y + kNameDy), name, ink, bg);
    draw_icon(s, kIconX, static_cast<int16_t>(y + kIconDy), kPlaceIcons[i], focus ? ink : dim, bg);
    status_[i].set_colors(dim, bg);
    status_[i].invalidate();
}

// A row repaints whole only when its node or its focus changes; a live status (the Meter's readings, the watts a device gets)
// rewrites just the characters that changed.
void ToolsPage::paint_bench() {
    PaintBudget *const bg = ctx_.budget;
    const bench::Facts f = facts();
    const int sel = selected();
    for (int i = 0; i < bench::kPlaces; i++) {
        const bench::Row r = bench::row(static_cast<Place>(i), f);
        const uint32_t h = mix(mix(2166136261u, static_cast<uint32_t>(r.known) | (i == sel ? 2u : 0u)), ctx_.rotation & 3u) | 1u;
        if (h != sig_[i]) {   // 0 means "repaint"
            if (bg && !bg->admit(static_cast<int>(ctx_.content.w) * kRowPitch, true)) return;
            sig_[i] = h;
            paint_row(i, r.known, i == sel);
        } else if (bg && !status_[i].valid() && !bg->admit(status_[i].box_px(), true)) {
            return;
        }
        status_[i].draw(r.status.view());
    }
    if (!tail_) {   // the strip between the last row and the one the Ui lays above the footer
        if (bg && !bg->admit(static_cast<int>(ctx_.content.w) * (kit::kListBottom - kBenchEnd), true)) return;
        gfx_fill(ctx_.content, 0, kBenchEnd, ctx_.content.w, static_cast<int16_t>(kit::kListBottom - kBenchEnd), kBg);
        tail_ = true;
    }
    StrBuf<28> line;
    bench::fact(f, line);
    draw_footer_budgeted(ctx_.content, FooterTone::Info, line.view(), foot_, bg);
}

void ToolsPage::update(uint32_t) {
    title_badge(false);
    paint_bench();
}

Icon place_icon(Place p) { return kPlaceIcons[static_cast<int>(p) < bench::kPlaces ? static_cast<int>(p) : 0]; }

// A tool opens its menu over the page OK was pressed on (the place page stays under it, BACK comes back to it); a page
// replaces everything (Test charger, Settings).
void open_bench_act(Ui &ui, const bench::Act &a, uint32_t now_ms) {
    switch (a.kind) {
    case bench::ActKind::Tool: ui.push_menu(ToolsPage::tool_menu(a.arg), now_ms); break;
    case bench::ActKind::Page: ui.goto_page(a.arg, now_ms); break;
    case bench::ActKind::Menu: ui.push_menu(static_cast<Menu>(a.arg), now_ms); break;
    }
}

bool ToolsPage::on_event(Event e) {
    if (e.press != Press::Short) return false;   // a long BACK goes to the Meter (the Ui)
    Ui &ui = *ctx_.ui;
    const int sel = selected();
    if (e.button == Button::Plus || e.button == Button::Minus) {
        const int to = sel + (e.button == Button::Plus ? 1 : -1);
        if (to >= 0 && to < bench::kPlaces) ctx_.keep.bench_sel = to;
        update(ctx_.now);
        return true;
    }
    if (e.button == Button::Ok) {
        if (sel == static_cast<int>(Place::Meter)) ui.show_face(ctx_.now);
        else ui.open_place(static_cast<Place>(sel), ctx_.now);   // may replace this page: return at once
        return true;
    }
    if (e.button == Button::Back) {
        ui.show_face(ctx_.now);
        return true;
    }
    return false;
}

PanelLabels ToolsPage::labels() const {
    PanelLabels l = list_labels(base_labels());   // short [+]/[-] move the focus
    l.b[static_cast<int>(Button::Ok)] = {"Open", "", "", false};
    return l;
}

void ToolsPage::invalidate() {
    foot_.invalidate();
    for (uint32_t &s : sig_) s = 0;
    for (Field &f : status_) f.invalidate();
    tail_ = false;
}

}  // namespace k2
