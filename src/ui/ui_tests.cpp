// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Tests 5-page carousel: see k2/ui/ui_tests.hpp.
#include "k2/ui/ui_tests.hpp"

#include <algorithm>
#include <cstring>
#include <string_view>

#include "k2/battery/battery_hw.hpp"
#include "k2/core/build_config.h"
#include "k2/drivers/fusb302.hpp"
#include "k2/fastcharge/fastcharge_classify.hpp"
#include "k2/pd/pd_source.hpp"
#include "k2/ui/trunc.hpp"
#include "k2/ui/ui.hpp"
#include "k2/ui/ui_style.hpp"
#include "k2/ui/ui_widgets.hpp"

namespace k2 {

namespace {

static_assert(TestsPage::kOverviewEnd <= kFooterY, "Overview rows must end above the footer");
static_assert(TestsPage::kNameDy + 16 <= TestsPage::kSubDy, "Overview name and summary must not overlap");
static_assert(TestsPage::kSubDy + 12 <= TestsPage::kRowPitch, "Overview summary must stay inside the row");
static_assert(TestsPage::kNameX + 7 * 8 <= TestsPage::kMetricX, "Overview name must end before the right metric");
static_assert(TestsPage::kMetricX + tests_view::kMetricChars * 8 <= 224, "Overview metric must fit canvas width");
static_assert(TestsPage::kSubX + tests_view::kSubChars * 6 <= 224, "Overview summary must fit canvas width");

constexpr const char *kOverviewNames[TestsPage::kOverviewRows] = {"Charger", "Cable", "Battery", "Device"};
constexpr Icon kOverviewIcons[TestsPage::kOverviewRows] = {Icon::Plug, Icon::Cable, Icon::Battery, Icon::Phone};

const char *fc_word(fastcharge::Protocol p) {
    using P = fastcharge::Protocol;
    switch (p) {
    case P::Pending: case P::NoInput: case P::NoQuick: case P::UsbData: case P::PdLikely: case P::Pd: return nullptr;
    default: return fastcharge::short_name(p);
    }
}

void put_volts_short(StrBufBase &s, int32_t mv) {
    if (mv % 1000 == 0) s.put_u32(static_cast<uint32_t>(mv / 1000));
    else s.put_fixed(mv, mv % 100 == 0 ? 1 : 2);
    s.put_char('V');
}

void put_amps_short(StrBufBase &s, int32_t ma) {
    if (ma % 1000 == 0) s.put_u32(static_cast<uint32_t>(ma / 1000));
    else s.put_fixed(ma, ma % 100 == 0 ? 1 : 2);
    s.put_char('A');
}

void append_if_fits(StrBuf<tests_view::kSubChars + 1> &dst, std::string_view part) {
    if (dst.view().size() + part.size() <= tests_view::kSubChars) dst.put(part);
}

using K = bench::ActKind;
constexpr uint8_t page_arg(Page p) { return static_cast<uint8_t>(p); }

// Static test lists for tabs 1 (Charger), 2 (Cable), 4 (Device).
// Tool slots (ui_tools.hpp): 0 FC trigger, 1 PD trigger, 2 Spoof, 3 Cable R, 9 Report cards, 11 Twins, 12 Loop probe, 13 PD Stress.
constexpr const char *kNoteInterrogate = "identify or view device";
constexpr const char *kNoteTestCharger = "grades every offer";
static_assert(fits_px(kLabelAdv, kNoteInterrogate, kit::kNoteMaxPx), "Interrogate note fits note box");
static_assert(fits_px(kLabelAdv, kNoteTestCharger, kit::kNoteMaxPx), "Test charger note fits note box");

}  // namespace

namespace tests_view {

void format_charger(const pd::SourceCaps &caps, bool pd_link, fastcharge::Protocol fc, bool vbus,
                    StrBuf<kMetricChars + 1> &metric, StrBuf<kSubChars + 1> &line) {
    metric.clear();
    line.clear();
    if (pd_link && caps.valid && caps.num_pdos > 0) {
        uint32_t max_mw = 0;
        bool has_pps = false, has_avs = false;
        uint16_t fixed_v[pd::kMaxPdos] = {};
        int n_fixed = 0;
        for (int i = 0; i < caps.num_pdos && i < static_cast<int>(pd::kMaxPdos); i++) {
            const pd::Pdo &p = caps.pdos[i];
            if (p.max_mw > max_mw) max_mw = p.max_mw;
            if (p.type == pd::PdoType::Pps) has_pps = true;
            else if (p.type == pd::PdoType::Avs) has_avs = true;
            else if (p.type == pd::PdoType::Fixed && p.max_mv >= 1000) {
                const uint16_t v = static_cast<uint16_t>((p.max_mv + 500u) / 1000u);
                bool dup = false;
                for (int j = 0; j < n_fixed; j++) if (fixed_v[j] == v) { dup = true; break; }
                if (!dup) fixed_v[n_fixed++] = v;
            }
        }
        if (max_mw >= 500) metric.put_u32((max_mw + 500u) / 1000u).put_char('W');
        line.put(caps.epr ? "PD3.1 " : caps.spec_rev >= 2 ? "PD3.0 " : caps.spec_rev == 1 ? "PD2.0 " : "PD ");
        const char *suffix = (has_pps && has_avs) ? " + PPS/AVS"
                           : has_pps ? " + PPS"
                           : has_avs ? " + AVS" : "";
        const std::size_t suffix_len = std::strlen(suffix);
        if (n_fixed > 0) {
            bool any = false;
            for (int i = 0; i < n_fixed; i++) {
                StrBuf<8> tok;
                if (any) tok.put_char('/');
                tok.put_u32(fixed_v[i]);
                if (line.view().size() + tok.view().size() + 1 + suffix_len > kSubChars) break;
                line.put(tok.view());
                any = true;
            }
            if (any) line.put_char('V');
        } else {
            line.put_u32(caps.num_pdos).put(caps.num_pdos == 1 ? " PDO" : " PDOs");
        }
        append_if_fits(line, suffix);
        return;
    }
    if (vbus) {
        if (const char *w = fc_word(fc)) { line.put(w); return; }
        line.put("5V source");
        return;
    }
    line.put("No charger");
}

void format_cable(const pd::Emarker &em, StrBuf<kMetricChars + 1> &metric, StrBuf<kSubChars + 1> &line) {
    metric.clear();
    line.clear();
    if (!em.valid) {
        line.put("Not read");
        return;
    }
    if (em.max_power_w) metric.put_u32(em.max_power_w).put_char('W');
    else if (em.max_current_a) metric.put_u32(em.max_current_a).put_char('A');
    line.put_u32(em.max_current_a).put("A ").put_u32(em.max_voltage_v).put_char('V');
    if (em.epr_capable) {
        append_if_fits(line, " EPR");
    } else if (em.speed_str && em.speed_str[0]) {
        StrBuf<24> sp;
        sp.put(", ").put(em.speed_str);
        append_if_fits(line, sp.view());
    }
}

void format_battery(const battery::view::Shown &shown, std::string_view status,
                    StrBuf<kMetricChars + 1> &metric, StrBuf<kSubChars + 1> &line) {
    metric.clear();
    line.clear();
    if (shown.any && shown.eff != battery::kNoRatio) metric.put(battery::view::pct(shown.eff).view());
    if (!status.empty()) line.put(status.substr(0, kSubChars));
    else line.put("No run");
}

void format_device(bool id_cur, const pd::Identity &id, const pd::ExtInfo &ext, uint32_t rev,
                   const pd::Contract &ctr, bool pd_link,
                   StrBuf<kMetricChars + 1> &metric, StrBuf<kSubChars + 1> &line) {
    metric.clear();
    line.clear();
    const int soc = (id_cur && ext.battery_status.valid && ext.battery_status.present)
                        ? pd::battery_soc_pct(ext.battery, ext.battery_status)
                        : -1;
    if (soc >= 0) {
        metric.put_u32(static_cast<uint32_t>(soc)).put_char('%');
    } else if (pd_link && ctr.valid && ctr.mv && ctr.op_ma) {
        const uint32_t mw = (static_cast<uint32_t>(ctr.mv) * static_cast<uint32_t>(ctr.op_ma)) / 1000u;
        if (mw >= 500) metric.put_u32((mw + 500u) / 1000u).put_char('W');
    }
    if (id_cur) {
        if (id.valid) {
            if (id.model && id.model[0]) append_if_fits(line, id.model);
            else if (id.vendor && id.vendor[0]) append_if_fits(line, id.vendor);
            else if (id.vid) {
                StrBuf<12> vid;
                vid.put("VID ");
                static constexpr char kHex[] = "0123456789ABCDEF";
                for (int sh = 12; sh >= 0; sh -= 4) vid.put_char(kHex[(id.vid >> sh) & 0xFu]);
                append_if_fits(line, vid.view());
            }
        }
        if (rev != 0) {
            StrBuf<12> r;
            if (!line.view().empty()) r.put(" ");
            r.put("PD").put_u32((rev >> 28) & 0xFu).put_char('.').put_u32((rev >> 24) & 0xFu);
            append_if_fits(line, r.view());
        }
        if (soc >= 0) {
            StrBuf<10> s;
            if (!line.view().empty()) s.put(" ");
            s.put_u32(static_cast<uint32_t>(soc)).put("%");
            append_if_fits(line, s.view());
        }
        if (!line.view().empty()) return;
    }
    if (pd_link && ctr.valid && ctr.mv) {
        line.put("Asks ");
        put_volts_short(line, ctr.mv);
        if (ctr.op_ma) {
            line.put_char(' ');
            put_amps_short(line, ctr.op_ma);
        }
        return;
    }
    line.put("None inline");
}

}  // namespace tests_view

// ---- TestsPage ------------------------------------------------------------------------------------------------------------

TestsPage::Items TestsPage::tab_items(TestTab t) {
    static constexpr Item kCharger[] = {
        {{"Test charger", K::Page, page_arg(Page::ChargerTest), kNoteTestCharger}, ItemKind::Act},
        {{"Request voltage", K::Tool, 1, nullptr}, ItemKind::Act},
        {{"Fast charge", K::Tool, 0, nullptr}, ItemKind::Act},
        {{"Report cards", K::Tool, 9, nullptr}, ItemKind::Act},
#if K2_PDSTRESS
        {{"Stress test", K::Tool, 13, nullptr}, ItemKind::Act},
#endif
    };
    static constexpr Item kCable[] = {
        {{"Spoof as...", K::Tool, 2, nullptr}, ItemKind::Act},
        {{"Resistance", K::Tool, 3, nullptr}, ItemKind::Act},
#if K2_LOOPPROBE
        {{"Loop probe", K::Tool, 12, nullptr}, ItemKind::Act},
#endif
    };
    static constexpr Item kDevice[] = {
        {{"Interrogate", K::Menu, 0, kNoteInterrogate}, ItemKind::DevicePlace},
#if K2_TWIN
        {{"Twins", K::Tool, 11, nullptr}, ItemKind::Act},
#endif
    };
    switch (t) {
    case TestTab::Charger: return {kCharger, static_cast<int>(std::size(kCharger))};
    case TestTab::Cable: return {kCable, static_cast<int>(std::size(kCable))};
    case TestTab::Device: return {kDevice, static_cast<int>(std::size(kDevice))};
    default: return {nullptr, 0};
    }
}

TestsPage::TestsPage(UiContext &c, TestTab initial)
    : PageBase(c, Page::Count, "Tests", Icon::Bolt),
      tab_(initial == TestTab::Battery || initial >= TestTab::Count ? TestTab::Overview : initial) {
    bar_icon_ = true;
    apply_header();
    for (int i = 0; i < kOverviewRows; i++) {
        metric_[i].init(ctx_.content, kSmall, kMetricX, static_cast<int16_t>(kRowY0 + i * kRowPitch + kNameDy),
                        tests_view::kMetricChars, Align::Right, kInk, kBg);
        sub_[i].init(ctx_.content, kLabelFont, kSubX, static_cast<int16_t>(kRowY0 + i * kRowPitch + kSubDy),
                     tests_view::kSubChars, Align::Left, kInk2, kBg);
    }
}

void TestsPage::apply_header() {
    switch (tab_) {
    case TestTab::Charger: title_ = "Charger"; icon_ = Icon::Plug; break;
    case TestTab::Cable: title_ = "Cable"; icon_ = Icon::Cable; break;
    case TestTab::Device: title_ = "Device"; icon_ = Icon::Phone; break;
    default: title_ = "Tests"; icon_ = Icon::Bolt; break;
    }
}

int TestsPage::count() const {
    if (tab_ == TestTab::Overview) return kOverviewRows;
    return tab_items(tab_).n;
}

ListState TestsPage::list_state() const {
    if (tab_ == TestTab::Overview) return {kOverviewRows, kOverviewRows, 0};
    return ctx_.rows.state();
}

void TestsPage::enter() {
    const int idx = static_cast<int>(tab_);
    const int n = count();
    sel_ = n > 0 ? std::clamp(static_cast<int>(ctx_.keep.tests_sel[idx]), 0, n - 1) : 0;
}

void TestsPage::leave() {
    ctx_.keep.tests_sel[static_cast<int>(tab_)] = static_cast<uint8_t>(sel_);
}

void TestsPage::set_tab(TestTab t, uint32_t now_ms) {
    if (t == TestTab::Battery || t >= TestTab::Count || t == tab_) return;
    ctx_.keep.tests_sel[static_cast<int>(tab_)] = static_cast<uint8_t>(sel_);
    tab_ = t;
    apply_header();
    const int n = count();
    sel_ = n > 0 ? std::clamp(static_cast<int>(ctx_.keep.tests_sel[static_cast<int>(tab_)]), 0, n - 1) : 0;
    ctx_.rows.detach();
    pg::clear_body(ctx_.content, kBarH);
    draw_static();
    update(now_ms);
}

void TestsPage::draw_static() {
    apply_header();
    title_bar();
    if (tab_ == TestTab::Overview) {
        if (!ctx_.blank) pg::clear_body(ctx_.content, kBarH);
        for (int i = 0; i < kOverviewRows; i++) {
            metric_[i].init(ctx_.content, kSmall, kMetricX, static_cast<int16_t>(kRowY0 + i * kRowPitch + kNameDy),
                            tests_view::kMetricChars, Align::Right, kInk, kBg);
            sub_[i].init(ctx_.content, kLabelFont, kSubX, static_cast<int16_t>(kRowY0 + i * kRowPitch + kSubDy),
                         tests_view::kSubChars, Align::Left, kInk2, kBg);
        }
    } else {
        if (!ctx_.blank) pg::clear_body(ctx_.content, kBarH);
        ctx_.rows.init(ctx_.content, kit::kBodyY, &TestsPage::row_fn, this);
        ctx_.rows.set_count(count());
        ctx_.rows.select(sel_);
        kit::fill_list_gap(ctx_.content, kit::kBodyY);
    }
    invalidate();
}

void TestsPage::invalidate() {
    bar_valid_ = false;
    foot_.invalidate();
    for (uint32_t &s : row_sig_) s = 0;
    for (int i = 0; i < kOverviewRows; i++) {
        metric_[i].invalidate();
        sub_[i].invalidate();
    }
    ctx_.rows.invalidate();
}

void TestsPage::paint_overview_row(int i, bool focus, std::string_view metric, std::string_view sub) {
    const Surface &s = ctx_.content;
    const int16_t y = static_cast<int16_t>(kRowY0 + i * kRowPitch);
    const bool bar = focus && kFocusBar != 0;
    const uint16_t bg = focus ? kFocusFill : kBg;
    const uint16_t ink = focus ? kOnFocus : kInk;
    const uint16_t dim = focus ? kOnFocusDim : kInk2;
    const std::string_view name = kOverviewNames[i];
    const uint32_t sig = pg::mix_str(pg::mix_str(mix_u32(mix_init(), static_cast<uint32_t>(focus)), metric), sub);
    if (sig != row_sig_[i]) {
        kit::Box holes[4];
        int n = 0;
        holes[n++] = {kIconX, static_cast<int16_t>(y + kIconDy), kIconSize, kIconSize};
        holes[n++] = {kNameX, static_cast<int16_t>(y + kNameDy), static_cast<int16_t>(name.size() * kSmall.adv), kSmall.height};
        holes[n++] = {kMetricX, static_cast<int16_t>(y + kNameDy), metric_[i].width_px(), kSmall.height};
        holes[n++] = {kSubX, static_cast<int16_t>(y + kSubDy), sub_[i].width_px(), kLabelFont.height};
        const int16_t x0 = bar ? 3 : 0;
        if (bar) gfx_fill(s, 0, y, 3, kRowPitch, kFocusBar);
        kit::fill_except(s, {x0, y, static_cast<int16_t>(s.w - x0), kRowPitch}, holes, n, bg);
        draw_icon(s, kIconX, static_cast<int16_t>(y + kIconDy), kOverviewIcons[i], focus ? ink : dim, bg);
        gfx_text(s, kSmall, kNameX, static_cast<int16_t>(y + kNameDy), name, ink, bg);
        metric_[i].set_colors(ink, bg);
        metric_[i].invalidate();
        sub_[i].set_colors(dim, bg);
        sub_[i].invalidate();
        row_sig_[i] = sig;
    }
    pg::put_field(metric_[i], ctx_.budget, metric, ink, bg);
    pg::put_field(sub_[i], ctx_.budget, sub, dim, bg);
}

void TestsPage::paint_overview() {
    const bool vbus = ctx_.pm_online && ctx_.pm.vbus_mv >= 1000;
    const pd::Link &link = ctx_.ui->link();
    const bool pd_link = link.attached() && !link.waiting();

    StrBuf<tests_view::kMetricChars + 1> m;
    StrBuf<tests_view::kSubChars + 1> l;

    // Row 0: Charger
    tests_view::format_charger(pd::active_caps(), pd_link, ctx_.fc, vbus, m, l);
    paint_overview_row(0, sel_ == 0, m.view(), l.view());

    // Row 1: Cable
    tests_view::format_cable(fusb302::emarker(), m, l);
    paint_overview_row(1, sel_ == 1, m.view(), l.view());

    // Row 2: Battery
    battery::view::Shown batt_shown{};
    StrBuf<bench::kStatusChars + 1> batt_status;
    const battery::Recorder &rec = battery::hw::service().rec();
    if (rec.state() != battery::State::Idle) {
        battery::Record r;
        rec.to_record(r);
        batt_shown = battery::view::shown(r, true);
        battery::view::bench_status(batt_shown, batt_status);
    }
    tests_view::format_battery(batt_shown, batt_status.view(), m, l);
    paint_overview_row(2, sel_ == 2, m.view(), l.view());

    // Row 3: Device
    const bool id_cur = fusb302::identify_current();
    tests_view::format_device(id_cur, fusb302::identify_identity(), fusb302::identify_ext(),
                              id_cur ? fusb302::identify_status().revision : 0u,
                              fusb302::contract(), pd_link, m, l);
    paint_overview_row(3, sel_ == 3, m.view(), l.view());

    draw_footer_cached(ctx_.content, FooterTone::Info, "Select domain to test", foot_);
}

void TestsPage::row_fn(void *self, int index, bool focused, kit::RowSpec &out) {
    auto *p = static_cast<TestsPage *>(self);
    out = kit::RowSpec{};
    p->tail_.clear();
    const Items items = tab_items(p->tab_);
    if (index < 0 || index >= items.n) return;
    const Item &it = items.a[index];
    out.text = it.act.name;
    out.chevron = true;
    const char *note = bench::act_hint(it.act);
    if (it.kind == ItemKind::Act && it.act.kind == bench::ActKind::Tool) {
        const Readiness r = tool_readiness(it.act.arg, setup_state(p->ctx_), p->ctx_.pm_online);
        if (r.level != ToolLevel::Ready) {
            p->tail_.put(r.tag);
            out.tail = p->tail_.view();
            if (r.level == ToolLevel::Blocked) {
                out.text_color = kInk3;
                out.tail_color = kInk3;
            } else {
                out.tail_color = kCaution;
            }
            out.chevron = false;
        }
    }
    if (focused && note && note[0]) out.note = note;
}

void TestsPage::update_list(uint32_t now_ms) {
    const Items items = tab_items(tab_);
    sel_ = items.n > 0 ? std::clamp(sel_, 0, items.n - 1) : 0;
    ctx_.rows.set_count(items.n);
    ctx_.rows.select(sel_);
    ctx_.rows.draw(now_ms, ctx_.budget);

    FooterTone tone = FooterTone::Info;
    const char *msg = "";
    if (sel_ >= 0 && sel_ < items.n) {
        const Item &it = items.a[sel_];
        if (it.kind == ItemKind::Act && it.act.kind == bench::ActKind::Tool) {
            const Readiness r = tool_readiness(it.act.arg, setup_state(ctx_), ctx_.pm_online);
            if (r.level != ToolLevel::Ready && r.reason && r.reason[0]) {
                tone = r.level == ToolLevel::Blocked ? FooterTone::Error : FooterTone::Caution;
                msg = r.reason;
            }
        }
        if (!msg[0]) msg = bench::act_hint(it.act);
    }
    draw_footer_cached(ctx_.content, tone, msg, foot_);
}

void TestsPage::update(uint32_t now_ms) {
    title_pager(static_cast<int>(tab_), static_cast<int>(TestTab::Count));
    if (tab_ == TestTab::Overview) paint_overview();
    else update_list(now_ms);
}

bool TestsPage::on_event(Event e) {
    Ui &ui = *ctx_.ui;
    if (e.press == Press::Long) {
        if (e.button == Button::Minus || e.button == Button::Plus) {
            const int cur = static_cast<int>(tab_);
            const int n = static_cast<int>(TestTab::Count);
            const int next = (cur + n + (e.button == Button::Plus ? 1 : -1)) % n;
            ui.switch_tests_tab(static_cast<TestTab>(next), ctx_.now);
            return true;
        }
        return false;
    }
    if (e.press != Press::Short) return false;
    const int n = count();
    if (e.button == Button::Plus || e.button == Button::Minus) {
        if (n > 0) sel_ = std::clamp(sel_ + (e.button == Button::Plus ? 1 : -1), 0, n - 1);
        update(ctx_.now);
        return true;
    }
    if (e.button == Button::Ok) {
        if (tab_ == TestTab::Overview) {
            ui.switch_tests_tab(static_cast<TestTab>(sel_ + 1), ctx_.now);
            return true;
        }
        const Items items = tab_items(tab_);
        if (sel_ >= 0 && sel_ < items.n) {
            const Item &it = items.a[sel_];
            if (it.kind == ItemKind::DevicePlace) ui.open_place(bench::Place::Device, ctx_.now);
            else open_bench_act(ui, it.act, ctx_.now);
        }
        return true;
    }
    if (e.button == Button::Back) {
        if (tab_ == TestTab::Overview) return false;   // Ui pops Menu::Tests back to Bench
        ui.switch_tests_tab(TestTab::Overview, ctx_.now);
        return true;
    }
    return false;
}

Strip TestsPage::labels() const {
    Strip l = base_labels();
    l[Button::Minus] = sl::UpPrev;
    l[Button::Plus] = sl::DnNext;
    l[Button::Ok] = sl::Open;
    return l;
}

}  // namespace k2
