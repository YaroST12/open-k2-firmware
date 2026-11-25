// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#include "k2/ui/ui.hpp"
#include "k2/battery/battery_hw.hpp"
#include "k2/core/gap_meter.hpp"
#include "k2/core/held.hpp"
#include "k2/core/lim_comeback.hpp"
#include "k2/core/log.hpp"
#include "k2/core/loop_guard.hpp"
#include "k2/core/paint_meter.hpp"
#include "k2/core/rescan.hpp"
#include "k2/core/strpack.hpp"
#include "k2/core/survey.hpp"
#include "k2/fastcharge/dpsniff.hpp"
#include "k2/fastcharge/fctrig.hpp"
#include "k2/fastcharge/svooc_trig.hpp"
#include "k2/drivers/fusb302.hpp"
#include "k2/drivers/lcd.hpp"
#include "k2/pd/emk_spoof.hpp"
#include "k2/pd/link.hpp"
#include "k2/pd/pd_emarker_plan.hpp"
#include "k2/pd/pd_source.hpp"
#include "k2/pd/power_source.hpp"
#include "k2/drivers/power_calc.hpp"
#include "k2/drivers/systick.hpp"
#include "k2/drivers/usb.hpp"
#include "k2/drivers/buttons.hpp"
#if K2_LOOPPROBE
#include "k2/loopprobe/loop_probe_hw.hpp"
#endif
#include "k2/ui/kit.hpp"
#include "k2/ui/readout_fmt.hpp"
#include "k2/ui/text.hpp"
#include "k2/ui/ui_calc.hpp"
#include "k2/ui/uiprefs.hpp"
#include "k2/ui/ui_style.hpp"
#include "k2/ui/ui_widgets.hpp"
#include "k2/ui/ui_viewport.hpp"

namespace k2 {

namespace {
uint32_t now_us() { return systick::micros(); }
}

// ---------------------------------------------------------------------------------------
// Setup and paging
// ---------------------------------------------------------------------------------------

Ui::Ui() : charger_test_(ctx_)
#if K2_PDSTRESS
           , pdstress_(ctx_, charger_test_)
#endif
#if K2_PDSPEC
           , pdspec_(ctx_)
#endif
{
    ctx_.ui = this;
    pages_.push<HomePage>(ctx_);   // the arena is never empty: page_ starts on Home
}

// The rotation page, built on an empty arena (set_page has popped the old one).
PageIf *Ui::build_base(Page p) {
    switch (p) {
    case Page::Detail: return pages_.push<DetailPage>(ctx_);
    case Page::ChargerTest: return pages_.push<ChargerTestPage>(ctx_, charger_test_);
    case Page::Lines: return pages_.push<LinesPage>(ctx_);
    case Page::Settings: return pages_.push<SettingsPage>(ctx_);
    case Page::Graphs: return pages_.push<GraphsPage>(ctx_);
    case Page::Ripple: return pages_.push<RipplePage>(ctx_);
    default: return pages_.push<HomePage>(ctx_);
    }
}

PageIf *Ui::build_menu(Menu m) {
    switch (m) {
    case Menu::Trigger: return pages_.push<TriggerPage>(ctx_);
    case Menu::Emarker: return pages_.push<EmarkerPage>(ctx_);
    case Menu::Connection: {
        ConnectionPage *p = pages_.push<ConnectionPage>(ctx_);
        if (p) {
            const conn::Tab t = conn_lock_ >= 0 ? static_cast<conn::Tab>(conn_lock_) : conn::Tab::Source;
            if (conn_overview_next_) {
                p->set_overview(t);
            } else {
                p->lock(t);
                if (conn_actions_next_) p->set_actions_mode(true);
            }
        }
        conn_lock_ = -1;
        conn_actions_next_ = false;
        conn_overview_next_ = false;
        return p;
    }
    case Menu::Diag: return pages_.push<DiagPage>(ctx_);
    case Menu::CableR: return pages_.push<CableRPage>(ctx_);
    case Menu::BlackBox: return pages_.push<BlackBoxPage>(ctx_);
    case Menu::Spoof: return pages_.push<SpoofPage>(ctx_);
    case Menu::Cards: return pages_.push<CardsPage>(ctx_);
    case Menu::Share: return pages_.push<SharePage>(ctx_, share_rec_);
    case Menu::UsbLink: return pages_.push<UsbLinkPage>(ctx_);
    case Menu::Autorotate: return pages_.push<AutorotatePage>(ctx_);
    case Menu::QrTest: return pages_.push<QrTestPage>(ctx_);
#if K2_TWIN
    case Menu::Twin: return pages_.push<TwinPage>(ctx_);
#endif
#if K2_LOOPPROBE
    case Menu::LoopProbe: return pages_.push<LoopProbePage>(ctx_);
#endif
#if K2_PDSTRESS
    case Menu::PdStress: return pages_.push<PdStressPage>(ctx_);
#endif
    case Menu::Diagnosis: {
        DiagnosisPage *p = pages_.push<DiagnosisPage>(ctx_);
        if (p && place_next_) p->as_place();   // the Log place, before enter() and the first paint
        return p;
    }
    case Menu::Battery: return pages_.push<BatteryPage>(ctx_);
    case Menu::Memory: return pages_.push<MemoryPage>(ctx_);
    case Menu::Display: return pages_.push<DisplayPage>(ctx_);
    case Menu::HomeBoxes: return pages_.push<HomeBoxesPage>(ctx_);
    case Menu::Tools: return pages_.push<ToolsPage>(ctx_);
    default: return pages_.push<PdTriggerPage>(ctx_);
    }
}

ListState Ui::list_state() {
    if (depth_) {
        switch (menu()) {
        case Menu::Trigger: return top_as<TriggerPage>().list_state();
        case Menu::Emarker: return top_as<EmarkerPage>().list_state();
        case Menu::Connection: return top_as<ConnectionPage>().list_state();
        case Menu::Diag: return top_as<DiagPage>().list_state();
        case Menu::PdTrigger: return top_as<PdTriggerPage>().list_state();
        case Menu::BlackBox: return top_as<BlackBoxPage>().list_state();
        case Menu::Cards: return top_as<CardsPage>().list_state();
        case Menu::Autorotate: return top_as<AutorotatePage>().list_state();
#if K2_TWIN
        case Menu::Twin: return top_as<TwinPage>().list_state();
#endif
        case Menu::Diagnosis: return top_as<DiagnosisPage>().list_state();
        case Menu::Battery: return top_as<BatteryPage>().list_state();
        case Menu::Memory: return top_as<MemoryPage>().list_state();
        case Menu::Display: return top_as<DisplayPage>().list_state();
        case Menu::HomeBoxes: return top_as<HomeBoxesPage>().list_state();
        case Menu::Tools: return top_as<ToolsPage>().list_state();
        default: return ListState{};
        }
    }
    switch (page_) {
    case Page::ChargerTest: return top_as<ChargerTestPage>().list_state();
    case Page::Lines: return top_as<LinesPage>().list_state();
    case Page::Settings: return top_as<SettingsPage>().list_state();
    default: return ListState{};
    }
}

void Ui::sync_surface() {
    if (uses_opp_rail()) {
        ctx_.content = {0, 0, kTwinContentW, kTwinContentH};
        ctx_.screen = surface_of(layout_.twin_content);
    } else {
        ctx_.content = {0, 0, 224, 240};
        ctx_.screen = surface_of(layout_.content);
    }
}

void Ui::init(uint32_t now_ms, bool cancel_resume) {
    lcd::set_rotation(orient_.rotation);
    layout_ = compute_layout(orient_.edge, kSlotCenters, kSlotLens, kPanelThickness);
    sync_surface();
    panel_ = surface_of(layout_.panel);
    ctx_.meter.reset(now_ms);
    current_filter_.reset();

    booting_ = true;
    lcd::fill(kBg);
    ring_landing_ = true;   // boots in the ring view, on Home
    set_page(Page::Home, now_ms);
    ring_landing_ = false;
    ctx_.cables.load();   // the remembered cable selection, before boot_restore's boot-time arming reads it
    if (charger_test_.boot_restore(cancel_resume)) set_page(Page::ChargerTest, now_ms);   // reboot-resume: "Resume run?" (or by itself)
#if K2_PDSTRESS
    else pdstress_.boot_restore(cancel_resume, now_ms);   // a PD Stress journal resumes by itself once VBUS is steady
#endif
    booting_ = false;
#if K2_LOOPPROBE
    // A loop probe record still marked in progress: the K2 restarted inside its CC step. Close it (CC Interrupted / Rebooted);
    // Tools -> Loop probe and `k2_remote loop-probe` report it. The probe itself never resumes.
    if (const auto step = loopprobe::boot_check(loopprobe::hw::store()))
        K2LOG("[LPRB] boot: closed the run interrupted in %s\n", loopprobe::step_name(*step));
#endif
    K2LOG("[ROT] Settings > Autorotate: auto or manual\n");
}

void Ui::suppress_flipped_arrows() {
    ctx_.keep.trigger.invalidate_confirmation();
    suppress_rotated_press_[static_cast<int>(Button::Minus)] = down_[static_cast<int>(Button::Minus)];
    suppress_rotated_press_[static_cast<int>(Button::Plus)] = down_[static_cast<int>(Button::Plus)];
}

void Ui::set_rotation(uint8_t rotation, uint32_t now_ms) {
    rotation &= 3;
    // orient_ is only ever assigned from orientation_for() (and kDefaultOrientation ==
    // orientation_for(0)), so the edge always matches the rotation: a plain compare suffices and
    // the button strip stays attached to the physical button edge in every rotation.
    if (rotation == orient_.rotation) return;
    charger_test_.invalidate_confirmation();
    if (rotate_buttons_ && (rotation >= 2) != (orient_.rotation >= 2)) suppress_flipped_arrows();
    orient_ = orientation_for(rotation);
    ctx_.rotation = rotation;
    layout_ = compute_layout(orient_.edge, kSlotCenters, kSlotLens, kPanelThickness);
    sync_surface();
    panel_ = surface_of(layout_.panel);
    BudgetScope scope(*this);
    lcd::set_rotation(rotation);
    start_full_clear();
    hold_shown_ = false;
    redraw(now_ms);
    K2LOG("[ROT] rotation=%u mode=%s\n", unsigned(rotation), rotation_lock_ ? "lock" : "auto");
}

void Ui::set_rotate_buttons(bool enabled) {
    if (rotate_buttons_ == enabled) return;
    if (orient_.rotation >= 2) {
        charger_test_.invalidate_confirmation();
        suppress_flipped_arrows();
    }
    rotate_buttons_ = enabled;
    sync_panel();
    store_orientation(orient_.rotation);
}

void Ui::restore_orientation(bool manual, uint8_t rotation, bool rotate_buttons) {
    rotate_buttons_ = rotate_buttons;
    rotation_lock_ = manual;
    if (!manual) return;
    rotation &= 3;
    orient_ = orientation_for(rotation);   // init() sets the panel, layout and first frame from these
    ctx_.rotation = rotation;
}

// Keeps the other stored fields; a write only when something changed (the gesture and the pages may repeat a value).
void Ui::store_orientation(uint8_t rotation) {
    uiprefs::Prefs p = uiprefs::stored();
    const uiprefs::Prefs before = p;
    p.rotation_manual = rotation_lock_;
    p.rotation = rotation_lock_ ? static_cast<uint8_t>(rotation & 3) : 0;   // the value means nothing in auto: keep the file canonical
    p.rotate_buttons = rotate_buttons_;
    if (p != before) uiprefs::set(p);
}

void Ui::set_home_hero(home::Qty q) {
    ctx_.keep.home_hero = q;
    K2LOG("[UI] home hero=%u\n", unsigned(home::qty_byte(q)));
    current().invalidate();   // Home paints its new layout progressively via tick()
    paint_pending_ = true;
    uiprefs::Prefs p = uiprefs::stored();
    const uiprefs::Prefs before = p;
    p.home_hero = home::qty_byte(q);
    if (p != before) uiprefs::set(p);
}

void Ui::set_home_boxes(const std::array<home::BoxKind, home::kBoxCount> &boxes) {
    ctx_.keep.home_boxes = boxes;
    ctx_.keep.boxes.reset();
    uiprefs::Prefs p = uiprefs::stored();
    const uiprefs::Prefs before = p;
    for (int i = 0; i < home::kBoxCount; i++) p.home_boxes[i] = static_cast<uint8_t>(boxes[i]);
    if (p != before) uiprefs::set(p);
}

void Ui::set_long_back(uint8_t pos) {
    ctx_.keep.long_back = static_cast<uint8_t>(ring::sanitize_target(pos));
    K2LOG("[UI] long back=%u\n", unsigned(ctx_.keep.long_back));
    uiprefs::Prefs p = uiprefs::stored();
    const uiprefs::Prefs before = p;
    p.long_back = ctx_.keep.long_back;
    if (p != before) uiprefs::set(p);
}

void Ui::feed_gravity(std::optional<orientation::Gravity> gravity, uint32_t now_ms) {
    const auto changed = gravity_.update(gravity, now_ms);
    if (!rotation_lock_ && changed) set_rotation(*changed, now_ms);
}

void Ui::configure_rotation(bool automatic, uint8_t rotation) {
    rotation_lock_ = !automatic;
    rotation_steps_ = 0;
    rotation_notice_until_ = 0;
    notice_ = nullptr;
    rotation_notice_drawn_ = false;
    requested_rotation_ = automatic ? gravity_.rotation() : static_cast<uint8_t>(rotation & 3);
    K2LOG("[ROT] settings=%s degrees=%u\n", automatic ? "auto" : K2_P("manual"), unsigned(requested_rotation_) * 90);
    store_orientation(requested_rotation_);
}

void Ui::rotation_gesture(uint32_t now_ms) {
    if (!rotation_lock_) {
        rotation_lock_ = true;
        rotation_steps_ = 0;
    } else if (++rotation_steps_ < 4) {
        set_rotation(static_cast<uint8_t>(rotation() + 1), now_ms);
    } else {
        rotation_lock_ = false;
        set_rotation(gravity_.rotation(), now_ms);
    }
    notice_ = nullptr;
    rotation_notice_until_ = now_ms + 5000;
    rotation_notice_drawn_ = false;
    draw_rotation_notice(now_ms);
    K2LOG("[ROT] %s %u; hold +/- together to cycle\n", rotation_lock_ ? "LOCK" : "AUTO", unsigned(rotation()));
    store_orientation(rotation());   // the chord is the same setting as Settings > Autorotate: remembered the same way
}

void Ui::draw_rotation_notice(uint32_t now_ms) {
    if (!rotation_notice_until_ ||
        static_cast<int32_t>(now_ms - rotation_notice_until_) >= 0) return;
    ContentViewport viewport(active_dest(), ctx_.content.w, ctx_.content.h);
    StrBuf<24> notice;
    if (notice_) notice.put(notice_);   // a refusal note (refuse_menu) shares the toast and its timer
    else if (rotation_lock_) notice.put(K2_P("Manual ")).put_u32(rotation() * 90).put(" deg");
    else notice.put(K2_P("Autorotate: Auto"));
    if (notice_ == kCalibrationNotice && page_ == Page::Home) return;
    draw_toast(ctx_.content, notice_ == kCalibrationNotice ? FooterTone::Caution
                         : notice_ ? FooterTone::Error : FooterTone::Info, notice.view());
    rotation_notice_drawn_ = true;
}

// A pill that ended or shrank left its pixels over the page, which has no framebuffer to restore them from: wipe the band and
// repaint the page (static chrome included) through the normal paint pipeline, then the live toast goes on top again.
bool Ui::restore_under_toast() {
    ctx_.toast.drop_erase();
    ctx_.toast.drawn = false;
    if (depth_ == 0 && page_ == Page::ChargerTest) {   // its grid keeps the band clean itself
        erase_toast(ctx_.content);
        return false;
    }
    toast_clear_pending_ = true;
    static_pending_ = true;
    ctx_.keep_status = true;
    current().invalidate_for_capture();
    paint_pending_ = true;
    return true;
}

void Ui::draw_toast_overlay(uint32_t now_ms) {
    if (ctx_.toast.erase_pending) {
        if (restore_under_toast()) return;   // the repaint raises the toast again once the page is back
    }
    if (!ctx_.toast.active(now_ms)) return;
    uint16_t first = 0, last = 0;
    if (ctx_.toast.drawn && !ContentViewport::panel_band(first, last)) return;
    draw_toast(ctx_.content, ctx_.toast.tone, ctx_.toast.view());
    ctx_.toast.drawn = true;
}

// Opaque pages: the full paint (draw_static + update with ctx_.blank false) writes every content pixel, panel strip aside
// (draw_panel owns it). A page that is not listed is erased before it paints. The pixel-exact walk of every switch in
// tests/test_ui_capture.cpp proves each entry: it compares the switch against a clean paint, in all four rotations.
bool Ui::page_opaque(Page p, bool entering) const {
    switch (p) {
    case Page::Home: case Page::Detail: case Page::Settings: case Page::Graphs: return true;
    case Page::ChargerTest: return charger_test_.paints_opaque(entering);   // the stage decides what its paint covers
    default: return false;
    }
}

bool Ui::menu_opaque(Menu m) {
    switch (m) {
    case Menu::Diag: case Menu::Memory: return true;
    default: return false;   // the rest wipe their body on a live screen (no gain over the erase), or paint by mode, or hold data
    }
}

void Ui::set_page(Page p, uint32_t now_ms) {
    // Every way out of a page or submenu lands here (paging, BACK, the remote): a trigger never survives.
    const uint32_t t0 = now_us();
    PageIf *const outgoing = &current();   // the top menu when one is open; its ink is what is on screen
    while (depth_ > 0) {                   // menu stack_[i] lives at arena index i + 1
        PageIf *const menu_page = pages_.at(depth_);
        --depth_;
        menu_page->leave();
    }
    const bool changed = p != page_;
    if (changed) pages_.at(0)->leave();
    page_ = p;
    ring_view_ = ring_landing_;
    kit::set_browse(ring_view_);
    if (p != Page::Home && notice_ == kCalibrationNotice && rotation_notice_until_ == 0)
        rotation_notice_until_ = now_ms + 5000;   // one boot warning, deferred until a page with a footer is visible
    const bool blank = clear_for_incoming(now_ms, *outgoing, page_opaque(p, true));   // the outgoing pages are still alive for a replay
    pages_.pop_to(changed ? 0 : 1);        // the menus go; so does the rotation page unless it is the one asked for
    if (changed) build_base(p);
    if (p == Page::ChargerTest || p == Page::Settings) pages_.at(0)->enter();
    paint_incoming(now_ms, t0, blank);
}

bool Ui::port_owner_busy() const {
    return ports_busy() || fusb302::listen_active() || fctrig::status().state != fc::State::Idle || fctrig::scan_running() ||
           fctrig::ufcs_exiting() || fctrig::svooc_running() || dpsniff::armed();
}

pd::SvWhy Ui::auto_read_block() const {
    if (link_reset_inline()) return pd::SvWhy::Inline;
    if (port_owner_busy() || busy_why() != BusyWhy::None || fusb302::busy_why() != pd::BusyWhy::None || emk_spoof::active() ||
        fusb302::ra_active())
        return pd::SvWhy::ChargerTest;
    return pd::SvWhy::None;
}

pd::LimRefusal Ui::pd_limit_arm(const pd::LimConfig &cfg, bool seed) const {
    pd::LimFacts f;
    f.busy = port_owner_busy();
    return fusb302::limit_arm(cfg, f, seed);
}

pd::LimRefusal Ui::pd_limit_arm(uint32_t limit_mw, bool seed) const {
    return pd_limit_arm(pd::lim_config_watts(limit_mw), seed);
}

bool Ui::pd_limit_link_live() const {
    return link_reset_inline() && fusb302::far_sink_seen();   // certain evidence only: a Request from a sink behind the K2
}

Ui::LimitSet Ui::pd_limit_set(const pd::LimConfig &cfg, bool off, bool confirmed, uint32_t now_ms) {
    if (off) lim_comeback_scrub_ = false;
    // Off undoes a forged list the sink still holds (a give-up's included); with none out there is nothing to reset.
    const bool held = off && fusb302::limit_holds_sink();   // our list on the wire: inline, whatever the latch says
    const bool undo = !off || held;
    if (off && !undo && !fusb302::limit_armed()) return LimitSet::Done;
    if (!off) {   // the gate before the reset: a refused limit never costs a link reset
        pd::LimFacts f;
        f.busy = port_owner_busy();
        switch (pd::lim_gate(f, cfg)) {
        case pd::LimRefusal::None: break;
        case pd::LimRefusal::BadLimit: return LimitSet::BadLimit;
        case pd::LimRefusal::Busy: return LimitSet::Busy;
        }
    }
    bool seed = true;
    LimitSet res = LimitSet::Done;
    // a contract joined mid-way (no Request heard since boot or attach): nothing to seed from, the reset makes the charger offer
    const bool joined = !off && link_reset_inline() &&
                        (fusb302::contract().valid || (ctx_.pm_online && ctx_.vbus_fast_mv > pd::kVSafe5MaxMv));
    if (undo && (pd_limit_link_live() || held || joined)) {
        const bool bus_pwr = fusb302::self_powered() || power_source::bus_powered();
        if (confirmed && bus_pwr && !off && !limcomeback::save(cfg)) {
            res = LimitSet::NoReset;   // never cut VBUS unless comeback persisted: arm seeded and ask to replug
        } else {
            if (confirmed && bus_pwr && off) limcomeback::clear();
            switch (link_reset(confirmed, now_ms, /*allow_bus_powered=*/true, held)) {
            case LinkResetResult::HardReset:
            case LinkResetResult::Cycled: seed = false; res = LimitSet::DoneReset; break;   // the contract the sniffer holds is ending
            case LinkResetResult::NeedConfirm: return LimitSet::NeedConfirm;
            case LinkResetResult::Busy:
                res = LimitSet::NoReset;
                if (!off) {
                    if (bus_pwr) limcomeback::clear();
                    return LimitSet::Busy;
                }
                break;
            case LinkResetResult::SelfPowered:   // non-bypassable or fallback
            case LinkResetResult::NoLink:
                res = LimitSet::NoReset;
                break;
            }
            if (bus_pwr && res != LimitSet::DoneReset) limcomeback::clear();
            if (!off && bus_pwr && res == LimitSet::DoneReset) {
                lim_comeback_scrub_ = true;
                lim_comeback_scrub_at_ms_ = now_ms + 5000;
            }
        }
    }
    if (off) {
        limcomeback::clear();
        fusb302::limit_disarm();
        return res;
    }
    switch (pd_limit_arm(cfg, seed)) {
    case pd::LimRefusal::None: return res;
    case pd::LimRefusal::BadLimit: return LimitSet::BadLimit;
    case pd::LimRefusal::Busy: return LimitSet::Busy;
    }
    return LimitSet::Busy;
}

Ui::LimitSet Ui::pd_limit_shell_arm(const pd::LimConfig &cfg, uint32_t now_ms) {
    if (!fusb302::limit_seed_ready()) return pd_limit_set(cfg, false, true, now_ms);
    switch (pd_limit_arm(cfg)) {
    case pd::LimRefusal::None: return LimitSet::Done;
    case pd::LimRefusal::BadLimit: return LimitSet::BadLimit;
    case pd::LimRefusal::Busy: return LimitSet::Busy;
    }
    return LimitSet::Busy;
}

// Once per give-up. Bus-powered the reset would reboot the K2 and hide the reason: the sink keeps the list until Off.
void Ui::lim_refused_poll() {
    const pd::LimStatus &s = fusb302::limit_status();
    const bool refused = s.state == pd::LimState::Off && s.why == pd::LimWhy::ChargerRefuses;
    if (refused && !lim_refused_ && fusb302::limit_holds_sink() && !fusb302::self_powered() && !power_source::bus_powered())
        pd_limit_set(pd::LimConfig{}, true, true, ctx_.now);
    lim_refused_ = refused;
}

Ui::LimitSet Ui::pd_limit_set(uint32_t limit_mw, bool confirmed, uint32_t now_ms) {
    return pd_limit_set(limit_mw == 0 ? pd::LimConfig{} : pd::lim_config_watts(limit_mw), limit_mw == 0, confirmed, now_ms);
}

bool Ui::twin_live_busy() const {
#if K2_TWIN
    return twin_hw::live_running() || twin_hw::inline_running();   // a proof or an inline interrogation
#else
    return fusb302::interrogate_running();
#endif
}

#if K2_TWIN
twin_hw::LiveCtx Ui::twin_ctx(uint32_t now_ms) const {
    twin_hw::LiveCtx c;
    c.now = now_ms;
    c.vbus_fast_mv = ctx_.pm_online ? ctx_.vbus_fast_mv : -1;
    c.cap_mv = ctx_.cap_mv;
    c.busy = charger_test_busy() || fc_release_.busy() || loop_probe_busy();
    c.inline_link = link_.inline_mode() || ctx_.inline_mode;
    c.owners_busy = pdstress_active() || pdspec_active() || fctrig::svooc_running() || fctrig::scan_running();
    return c;
}
#endif

void Ui::cancel_twin() {
#if K2_TWIN
    twin_hw::live_cancel();
    twin_hw::inline_cancel();
#else
    fusb302::interrogate_abort();
#endif
}

void Ui::cancel_runs() {
    cancel_charger_test();
    cancel_twin();
#if K2_PDSTRESS
    pdstress_.cancel();
#endif
#if K2_PDSPEC
    pdspec_.cancel();
#endif
#if K2_LOOPPROBE
    if (LoopProbePage *probe = find<LoopProbePage>(Menu::LoopProbe)) probe->cancel(ctx_.now);
#endif
}

#if K2_LOOPPROBE
bool Ui::loop_probe_busy() const {
    const LoopProbePage *probe = find<LoopProbePage>(Menu::LoopProbe);
    return probe && probe->running();
}
#endif

// The hold that START's own PDO 1 request puts on the sink (Rd presented for a run) is not a foreign tool, and neither is the
// self-hold of a self-powered or bus-powered K2 (its own plain 5 V contract): that hold is the K2's supply, so releasing it
// would cut the K2's power and it never blocks. Neither is a finished re-read's own leftover PDO 1 contract inside its
// linger (fusb302::reread_own_hold): START takes that over like the self-hold (hardware 2026-11-10, b14c1b9).
// What keeps an identity (or a START) out, said as a footer line (at most 24 characters) that names the cause and the way out;
// nullptr = nothing does. An identity armed in Tools -> Spoof is not one of them either: START disarms whatever is armed and
// the pick takes over ("Off" arms nothing).
BusyWhy Ui::busy_why() const {
    const auto& sink = fusb302::sink_status();
    const auto fs = fctrig::status();
    if (charger_test_busy()) return BusyWhy::TestRunning;
    if (twin_live_busy()) return BusyWhy::TwinRun;
    if (pdstress_active()) return BusyWhy::PdStress;   // PD Stress: running or resuming
    if (pdspec_active()) return BusyWhy::PdSpec;
    if (loop_probe_busy()) return BusyWhy::LoopProbe;
    if (fc_release_.busy()) return BusyWhy::FcRelease;
    if (fctrig::scan_running() || fs.state == fc::State::Negotiating || fs.state == fc::State::Active ||
        fctrig::svooc_running())
        return BusyWhy::QcTrigger;
    if (pd::user_hold(sink) && !fusb302::rd_for_run_on() && !fusb302::reread_own_hold() && !fusb302::self_powered() && !power_source::bus_powered())
        return BusyWhy::PdHold;
    if (sink.epr_mode) return BusyWhy::Epr;
    if (sink.state != pd::SinkState::Idle && sink.state != pd::SinkState::Ready && sink.state != pd::SinkState::Failed)
        return BusyWhy::PdRequest;
    if (ctx_.em_scanning) return BusyWhy::EmScan;
    if (fusb302::identify_status().state == pd::IdState::Running) return BusyWhy::Identify;
    return BusyWhy::None;
}

const char *Ui::busy_reason() const {
    switch (busy_why()) {
    case BusyWhy::TestRunning: return K2_P("Test running: stop it");
    case BusyWhy::FcRelease: return K2_P("5V release: wait");
    case BusyWhy::QcTrigger: return K2_P("QC trigger: stop it");
    case BusyWhy::PdHold: return K2_P("PD hold: release it");
    case BusyWhy::Epr: return K2_P("EPR contract: exit it");
    case BusyWhy::PdRequest: return K2_P("PD request: wait");
    case BusyWhy::EmScan: return K2_P("E-marker scan: wait");
    case BusyWhy::Identify: return K2_P("Identify: wait");
    case BusyWhy::TwinRun: return K2_P("Twin proof: stop it");
#if K2_PDSTRESS
    case BusyWhy::PdStress: return pdstress_.busy_reason();   // "PD Stress: stop it" / "PD Stress: resuming"
#else
    case BusyWhy::PdStress: return K2_P("PD Stress: stop it");   // unreachable: pdstress_active() is false
#endif
    case BusyWhy::LoopProbe: return K2_P("Loop probe: stop it");
    case BusyWhy::PdSpec: return K2_P("PD Spec: stop it");
    case BusyWhy::None: break;
    }
    return nullptr;
}

// The session block of the remote (GET_STATE part 1). Every value is read from the helper the screen reads it through, so
// the host sees what the panel shows without a second copy of the logic. The list cursor is the shared RowList's, valid
// only on the pages that bind it (and, for the pages that fall back to a non-list view, only while their list is up).
Ui::Session Ui::session() {
    const auto clamp16 = [](uint32_t v) { return static_cast<uint16_t>(v > 0xFFFFu ? 0xFFFFu : v); };
    Session s;
    s.mah = ctx_.meter.mah();
    s.elapsed_s = ctx_.pm_online && ctx_.pm.vbus_mv >= 1000 ? ctx_.meter.elapsed_s(ctx_.now)
                : ctx_.stale() ? ctx_.last.elapsed_s : 0;
    const home::ProtoInfo p = proto_now();
    s.proto_kind = static_cast<uint8_t>(p.kind);
    s.limit_ma = clamp16(p.ma);
    s.max_mv = clamp16(p.max_mv);
    s.typec_ma = clamp16(fusb302::typec_ma());
    s.busy = busy_why();
    s.fc_release = fc_release_.busy();
    const conn::Facts f = passive_facts();
    s.topology = static_cast<uint8_t>(f.topo);
    s.wait_kind = static_cast<uint8_t>(f.wait);
    s.pd_none_why = static_cast<uint8_t>(link_.pd_none_why());
    s.open_cc_mv = f.open_cc_mv < 0 ? 0xFFFF : static_cast<uint16_t>(f.open_cc_mv > 0xFFFE ? 0xFFFE : f.open_cc_mv);
    s.depth = static_cast<uint8_t>(depth_);
    for (unsigned i = 0; i < depth_; i++) s.stack[i] = static_cast<uint8_t>(stack_[i]);
    bool rows = false;
    if (depth_) {
        switch (menu()) {
        case Menu::Trigger: case Menu::Emarker: case Menu::Cards: case Menu::PdTrigger: case Menu::Twin: case Menu::Diagnosis:
        case Menu::Battery:
            rows = list_state().count > 0;
            break;
        case Menu::Spoof: case Menu::UsbLink: case Menu::Autorotate: case Menu::BlackBox: case Menu::Display: case Menu::HomeBoxes: case Menu::Tools: rows = true; break;
        case Menu::Connection: {
            const ConnectionPage &cp = connection_page();
            s.sub = static_cast<uint8_t>(static_cast<uint8_t>(cp.tab()) & kSubTabMask);
            break;
        }
        default: break;
        }
    } else {
        rows = page_ == Page::Settings && !ring_view_;
    }
    if (depth_ && ring_view_) rows = false;   // the ring view moves between pages: no list is being scrolled
    if (rows) {
        const int n = ctx_.rows.count(), c = ctx_.rows.selected();
        s.count = static_cast<uint8_t>(n > 255 ? 255 : n < 0 ? 0 : n);
        s.sel = static_cast<uint8_t>(c < 0 ? 0 : c > 254 ? 254 : c);
    }
    s.home_window = static_cast<uint8_t>(ctx_.keep.home_window);
    s.flags = static_cast<uint8_t>((hold_shown_ ? kSessionHoldCard : 0) | (rotation_notice_until_ ? kSessionRotationNotice : 0) |
                                   (ctx_.em_scanning ? kSessionEmScanning : 0) | (ctx_.em_listen_until ? kSessionEmListening : 0) |
                                   (ctx_.em_quiet ? kSessionEmQuiet : 0) | (ctx_.id_no_device ? kSessionIdNoDevice : 0) |
                                   (ring_view_ ? kSessionRingView : 0));
    s.pc_link = static_cast<uint8_t>(pc_state_);
    return s;
}

// The loop lock's footer line (8 px a character in the 220 px footer band).
static_assert(fits_px_p(8, loopguard::kLockText, 220), "the loop lock line is wider than the footer band");

// PD Stress asks this one: the same owners as busy_reason() minus the ones a run causes itself (a held contract, the sink state, an
// EPR contract). Its own run is not "foreign".
const char *Ui::foreign_busy() const {
    const auto fs = fctrig::status();
    if (loopguard::locked()) return loopguard::kLockText;   // a run starts nothing above 5 V, a restore waits (k2/core/loop_guard.hpp)
    if (charger_test_busy()) return K2_P("Test running: stop it");
    if (pdspec_active()) return K2_P("PD Spec: stop it");   // PD Stress and the charger test ask this; a remote-started spec run asks nothing while it goes on
    if (loop_probe_busy()) return K2_P("Loop probe: stop it");
    if (fc_release_.busy()) return K2_P("5V release: wait");
    if (fctrig::scan_running() || fs.state == fc::State::Negotiating || fs.state == fc::State::Active ||
        fctrig::svooc_running())
        return K2_P("QC trigger: stop it");
    if (ctx_.em_scanning) return K2_P("E-marker scan: wait");
    if (fusb302::identify_status().state == pd::IdState::Running) return K2_P("Identify: wait");
    return nullptr;
}

void Ui::redraw(uint32_t now_ms) {
    rotation_notice_drawn_ = false;
    paint_incoming(now_ms, now_us(), true);   // the callers have just filled the screen
}

void Ui::erase_outgoing(uint32_t now_ms, PageIf &outgoing) {
    const Rect out_dest = ctx_.content.w == kTwinContentW ? layout_.twin_content : layout_.content;
    ContentViewport viewport(out_dest, ctx_.content.w, ctx_.content.h);
    rotation_notice_drawn_ = false;
    // Erase the outgoing page's ink by replaying its own paint with every pixel forced to
    // kBg: only the pixels it actually drew are rewritten, not the whole screen. The hold
    // card and the notices are part of the screen too, so they go through the same pass.
    lcd::set_erase(true);
    outgoing.invalidate();
    outgoing.sample();
    outgoing.draw_static();
    outgoing.update(now_ms);
    draw_release_notice();
    if (hold_shown_) gfx_fill(ctx_.content, static_cast<int16_t>((ctx_.content.w - 200) / 2), 70, 200, 84, kBg);
    draw_rotation_notice(now_ms);
    draw_toast_overlay(now_ms);
    lcd::set_erase(false);
}

void Ui::reset_paint_pipeline() {
    // Only invalidate the row cache here: set_rotation/repaint call start_full_clear() on the
    // same live page and must keep ctx_.rows's ctx_ binding and scroll offset intact.
    ctx_.rows.invalidate();
    footer_error_clear();   // a page switch or full clear: the incoming page's footer says again whether an error is showing
    paint_pending_ = false;
    paint_blank_ = false;
    clear_pending_ = false;
    clear_full_lcd_ = false;
    clear_y_ = clear_y1_ = 0;
    chrome_gaps_pending_ = false;
    toast_clear_pending_ = false;
    static_pending_ = false;
    ctx_.keep_status = false;
    panel_pending_ = false;
    opp_pending_ = false;
    timing_in_flight_ = false;
    panel_mask_ = 0;
    opp_mask_ = 0;
}

void Ui::start_full_clear() {
    reset_paint_pipeline();
    if (ctx_.toast.active(ctx_.now)) ctx_.toast.drawn = false;   // the full clear below erases the old pill
    clear_pending_ = true;
    clear_full_lcd_ = true;
    clear_y_ = 0;
    clear_y1_ = static_cast<int16_t>(lcd::kHeight);
}

// Clears what the incoming page must not find under it; false = nothing was cleared and the page paints over live ink.
bool Ui::clear_for_incoming(uint32_t now_ms, PageIf &outgoing, bool incoming_opaque, bool detach_rows) {
    const bool clear_toast = ctx_.toast.drawn || fc_release_.busy() || loopguard::locked()
#if K2_PDSTRESS
        || pdstress_.footer() != nullptr
#endif
#if K2_PDSPEC
        || pdspec_.footer() != nullptr
#endif
        ;
    const uint32_t content_px = static_cast<uint32_t>(ctx_.content.w) * static_cast<uint32_t>(ctx_.content.h);
    const int16_t next_w = uses_opp_rail() ? kTwinContentW : 224;
    reset_paint_pipeline();
    toast_clear_pending_ = clear_toast;
    ctx_.toast.clear();
    ctx_.toast.drop_erase();
    if (!incoming_opaque) {
        if (ctx_.content.w == next_w && full_px_ < content_px &&
            (!ctx_.budget || full_px_ <= static_cast<uint32_t>(PaintBudget::kDefaultPx))) {
            PaintBudget *const saved_budget = ctx_.budget;
            ctx_.budget = nullptr;   // small outgoing erase replays completely without slicing
            erase_outgoing(now_ms, outgoing);
            ctx_.budget = saved_budget;
            if (ctx_.budget) ctx_.budget->spent += static_cast<int>(full_px_);
        } else {
            clear_pending_ = true;
            clear_full_lcd_ = false;
            clear_y_ = 0;
            clear_y1_ = ctx_.content.h;
        }
    }
    if (detach_rows) ctx_.rows.detach();
    // Whatever the card and the notice drew is gone now (wiped, or painted over by the opaque page): redraw them on demand.
    rotation_notice_drawn_ = false;
    hold_shown_ = false;
    hold_px_ = hold_left_ = -1;
    return !incoming_opaque;
}

bool Ui::paint_more(uint32_t now_ms) {
    if (!paint_pending_ || hold_shown_ || paint_off_) return false;
    BudgetScope scope(*this);
    const uint32_t t0 = now_us();
    ctx_.now = now_ms;
    pump_paint(now_ms);
    const uint32_t dt = now_us() - t0;
    if (dt > field_max_us_) field_max_us_ = dt;
    if (timing_pending_) log_timing(now_ms);
    return true;
}

void Ui::paint_incoming(uint32_t now_ms, uint32_t t0, bool blank) {
    BudgetScope scope(*this);
    ring_settle();
    sync_surface();
    last_update_ms_ = now_ms;
    current().invalidate();   // the erase pass erased everything the field caches remember; a built page has drawn nothing
    current().sample();
    paint_blank_ = blank;
    chrome_gaps_pending_ = !blank && uses_opp_rail() && kit::kBodyY > 0;
    static_pending_ = true;
    panel_pending_ = true;
    panel_mask_ = 0x0Fu;
    opp_pending_ = uses_opp_rail();
    opp_mask_ = opp_pending_ ? 0x0Fu : 0u;
    for (uint32_t &s : opp_sig_) s = 0;
    paint_t0_us_ = t0;
    paint_px0_ = paint::meter.px_count();
    full_px_ = static_cast<uint32_t>(ctx_.content.w) * static_cast<uint32_t>(ctx_.content.h);
    timing_in_flight_ = true;
    paint_pending_ = true;
    pump_paint(now_ms);
}

// A menu that could not open (stack depth or arena full) is never silent: a log line and a footer notice on the page that stays.
void Ui::refuse_menu(Menu m, const char *why, uint32_t now_ms) {
    K2LOG("[UI] menu %u refused: %s (depth %u, arena %u/%u B)\n", unsigned(m), why, depth_, unsigned(pages_.used()),
          unsigned(kPageArenaBytes));
    notice_ = K2_P("Can't open that here");
    rotation_notice_until_ = now_ms + 3000;
    rotation_notice_drawn_ = false;
    draw_rotation_notice(now_ms);
}

bool Ui::push_menu(Menu m, uint32_t now_ms) {
    // The PD Stress page opens over its own run (a run the remote started shows there, and its BACK stops it); it only reads the session.
    if (ports_busy() && !(m == Menu::PdStress && pdstress_active())) {
        K2LOG("[TEST] menu refused: test/release in progress\n");
        return false;
    }
    if (m >= Menu::Count) return false;
    if (depth_ >= kMaxMenuDepth) {
        refuse_menu(m, K2_P("depth"), now_ms);
        return false;
    }
    const uint32_t t0 = now_us();
    PageIf *const outgoing = &current();   // stays alive under the new menu
    PageIf *const page = build_menu(m);
    if (!page) {                           // the arena is full (the static_asserts in ui.hpp keep this unreachable)
        refuse_menu(m, K2_P("arena"), now_ms);
        return false;
    }
    stack_[depth_++] = m;
    ring_view_ = ring_landing_;
    kit::set_browse(ring_view_);
    page->enter();
    const bool blank = clear_for_incoming(now_ms, *outgoing, menu_opaque(m));
    paint_incoming(now_ms, t0, blank);
    return true;
}

bool Ui::open_share(qrshare::Type type, std::uint8_t schema, std::string_view title, std::size_t len, uint32_t now_ms, std::span<const std::uint8_t> ext) {
    if (!share_rec_.begin(type, schema, title, len, ext)) {
        K2LOG("[SHARE] nothing to share (%u bytes)\n", unsigned(len));
        return false;
    }
    ctx_.now = now_ms;   // enter() times the first part from it
    if (!push_menu(Menu::Share, now_ms)) return false;
    K2LOG("[SHARE] type=%u %uB in %u part(s)\n", unsigned(type), unsigned(len), share_rec_.parts());
    return true;
}

bool Ui::pop_menu(uint32_t now_ms) { return depth_ > 0 && pop_to(depth_ - 1, now_ms); }

// Closes every menu above keep_depth in one paint (the top one's ink is what is on screen).
bool Ui::pop_to(unsigned keep_depth, uint32_t now_ms) {
    if (depth_ <= keep_depth) return false;
    const uint32_t t0 = now_us();
    PageIf *const outgoing = pages_.at(depth_);   // the top menu
    while (depth_ > keep_depth) pages_.at(depth_--)->leave();   // menu stack_[i] lives at arena index i + 1
    const bool incoming_opaque = depth_ > 0 ? menu_opaque(stack_[depth_ - 1]) : page_opaque(page_);   // the page below
    const bool blank = clear_for_incoming(now_ms, *outgoing, incoming_opaque);
    pages_.pop_to(depth_ + 1);                     // the page below is the one that was kept
    paint_incoming(now_ms, t0, blank);
    return true;
}

// ---------------------------------------------------------------------------------------
// Button label panel
// ---------------------------------------------------------------------------------------

void Ui::draw_panel() {
    for (unsigned i = 0; i < 4; i++) draw_slot(i);
}

void Ui::sync_panel() {
    for (unsigned i = 0; i < 4; i++)
        if (slot_sig(i) != slot_sig_[i]) draw_slot(i);
}

namespace {
struct OppCells {
    kit::OppCell c[4];
    StrBuf<12> txt[4];
};

void fmt_opp_v(StrBuf<12> &out, bool ok, int32_t mv) {
    if (!ok) { out.put(K2_P("--.-V")); return; }
    char buf[12];
    const std::size_t n = readout::fmt3(buf, mv < 0 ? 0 : mv, 'V');
    out.put({buf, n});
}
void fmt_opp_a(StrBuf<12> &out, bool ok, int32_t ma) {
    if (!ok) { out.put(K2_P("-.--A")); return; }
    char buf[12];
    const std::size_t n = readout::fmt3(buf, power::magnitude(ma), 'A');
    out.put({buf, n});
}
void fmt_opp_w(StrBuf<12> &out, bool ok, int32_t mw) {
    if (!ok) { out.put(K2_P("--.-W")); return; }
    char buf[12];
    const std::size_t n = readout::fmt3(buf, power::magnitude(mw), 'W');
    out.put({buf, n});
}
void fmt_opp_adc_v(StrBuf<12> &out, const std::optional<adc::Reading> &r, int32_t mv) {
    if (!r || mv < 0) { out.put(K2_P("-.--V")); return; }
    out.put_fixed(mv, 2).put_char('V');
}

OppCells build_opp_cells(const UiContext &ctx, const kit::OppRailSpec &spec, bool active, pclink::State link) {
    OppCells oc{};
    // Slot 0: uppercase MastheadCell; the names come from rail_title.hpp (at most 4 characters, checked at compile time)
    static_assert(rail::kTitleMax * 6 <= kOppSlotLen[0] - 4, "a rail name fits the title cell");
    for (char ch : spec.title) {
        if (oc.txt[0].view().size() >= rail::kTitleMax) break;
        if (ch >= 'a' && ch <= 'z') ch = static_cast<char>(ch - 'a' + 'A');
        oc.txt[0].put_char(ch);
    }
    oc.c[3].link = link;   // the PC-link icon ends the rail, after the readout
    oc.c[0].masthead = true;
    oc.c[0].active = active;
    oc.c[0].value = oc.txt[0].view();
    oc.c[0].value_color = active ? kSpotInk : kInk2;
    oc.c[0].dot_count = spec.dot_count;
    oc.c[0].dot_cur = spec.dot_cur;

    const bool stale = ctx.stale();
    const bool ok = ctx.pm_online || stale;
    const power::Metrics &pm = stale ? ctx.last.pm : ctx.pm;
    const uint16_t ink = stale ? kStale : kInk;

    switch (spec.telemetry) {
    case kit::OppTelemetry::Vaw:
        fmt_opp_v(oc.txt[1], ok, pm.vbus_mv);
        oc.c[1].spot = stale ? kStale : kSpotVolt;
        oc.c[1].value = oc.txt[1].view();
        oc.c[1].value_color = ink;

        fmt_opp_a(oc.txt[2], ok, pm.current_ma);
        oc.c[2].spot = stale ? kStale : kSpotAmp;
        oc.c[2].value = oc.txt[2].view();
        oc.c[2].value_color = ink;

        fmt_opp_w(oc.txt[3], ok, pm.power_mw);
        oc.c[3].spot = stale ? kStale : kWattFill;
        oc.c[3].value = oc.txt[3].view();
        oc.c[3].value_color = ink;
        break;

    case kit::OppTelemetry::PeakSagR: {
        fmt_opp_w(oc.txt[1], ctx.extremes.peak_mw() > 0, static_cast<int32_t>(ctx.extremes.peak_mw()));
        oc.c[1].tag = "PK ";
        oc.c[1].tag_color = kDim;
        oc.c[1].value = oc.txt[1].view();
        oc.c[1].value_color = stale ? kStale : kWattFill;

        const bool has_sag = ctx.extremes.has_sag();
        const uint32_t sag_mv = has_sag ? (ctx.extremes.sag_bp() * ctx.extremes.sag_level_mv() + 5000u) / 10000u : 0u;
        fmt_opp_v(oc.txt[2], has_sag, static_cast<int32_t>(sag_mv));
        oc.c[2].tag = "SAG ";
        oc.c[2].tag_color = kDim;
        oc.c[2].value = oc.txt[2].view();
        oc.c[2].value_color = stale ? kStale : kSpotVolt;

        if (const auto est = ctx.cable.estimate()) {
            char rbuf[10];
            const int rn = cable_r::fmt_mohm(std::span(rbuf), est->mohm);
            oc.txt[3].put({rbuf, static_cast<std::size_t>(rn > 0 ? rn : 0)});
        } else {
            oc.txt[3].put("--");
        }
        oc.c[3].tag = "R ";
        oc.c[3].tag_color = kDim;
        oc.c[3].value = oc.txt[3].view();
        oc.c[3].value_color = stale ? kStale : kInk2;
        break;
    }

    case kit::OppTelemetry::WattCcDp:
        fmt_opp_w(oc.txt[1], ok, pm.power_mw);
        oc.c[1].spot = stale ? kStale : kWattFill;
        oc.c[1].value = oc.txt[1].view();
        oc.c[1].value_color = ink;

        fmt_opp_adc_v(oc.txt[2], ctx.analog, ctx.analog ? ctx.analog->cc1_mv : -1);
        oc.c[2].tag = "CC1 ";
        oc.c[2].tag_color = kDim;
        oc.c[2].value = oc.txt[2].view();
        oc.c[2].value_color = ink;

        fmt_opp_adc_v(oc.txt[3], ctx.analog, ctx.analog ? ctx.analog->dp_mv : -1);
        oc.c[3].tag = "D+ ";
        oc.c[3].tag_color = kDim;
        oc.c[3].value = oc.txt[3].view();
        oc.c[3].value_color = ink;
        break;

    case kit::OppTelemetry::AmpCcDp:
        fmt_opp_a(oc.txt[1], ok, pm.current_ma);
        oc.c[1].spot = stale ? kStale : kSpotAmp;
        oc.c[1].value = oc.txt[1].view();
        oc.c[1].value_color = ink;

        fmt_opp_adc_v(oc.txt[2], ctx.analog, ctx.analog ? ctx.analog->cc1_mv : -1);
        oc.c[2].tag = "CC1 ";
        oc.c[2].tag_color = kDim;
        oc.c[2].value = oc.txt[2].view();
        oc.c[2].value_color = ink;

        fmt_opp_adc_v(oc.txt[3], ctx.analog, ctx.analog ? ctx.analog->dp_mv : -1);
        oc.c[3].tag = "D+ ";
        oc.c[3].tag_color = kDim;
        oc.c[3].value = oc.txt[3].view();
        oc.c[3].value_color = ink;
        break;
    }
    return oc;
}

uint32_t opp_cell_sig(PanelEdge opp_edge, int16_t pos, const kit::OppCell &c) {
    uint32_t h = mix_init();
    h = mix_u32(h, static_cast<uint32_t>(opp_edge) | (static_cast<uint32_t>(pos) << 4) |
                   (static_cast<uint32_t>(c.masthead) << 24) |
                   (static_cast<uint32_t>(c.dot_count) << 25) | (static_cast<uint32_t>(c.dot_cur) << 28) |
                   (static_cast<uint32_t>(c.active) << 31));
    h = mix_u32(h, static_cast<uint32_t>(c.spot) | (static_cast<uint32_t>(c.value_color) << 16));
    h = mix_u32(h, c.tag_color | (static_cast<uint32_t>(c.link) << 16));
    h = mix(h, c.tag);
    h = mix(h, c.value);
    return h | 1u;
}
}  // namespace

void Ui::draw_opp_slot(unsigned slot) {
    if (slot > 3 || !uses_opp_rail()) return;
    ContentViewport::Pause unscaled_rail;
    const PanelEdge opp_edge = opposite_edge(orient_.edge);
    const OppCells oc = build_opp_cells(ctx_, opp_spec(), !ring_view_, pc_state_);
    const TextDir dir = rail_text_dir(orient_.edge);
    const Rect sr = opp_slot_rect(layout_.opp_rail, opp_edge, dir, slot);   // slot 0 is the title in every rotation
    opp_sig_[slot] = opp_cell_sig(opp_edge, panel_is_column(opp_edge) ? sr.y : sr.x, oc.c[slot]);
    kit::draw_opp_slot(surface_of(sr), opp_edge, dir, slot, oc.c[slot]);
}

void Ui::draw_opp_rail() {
    if (!uses_opp_rail()) return;
    for (unsigned i = 0; i < 4; i++) draw_opp_slot(i);
}

void Ui::sync_opp_rail() {
    if (!uses_opp_rail()) return;
    ContentViewport::Pause unscaled_rail;
    const PanelEdge opp_edge = opposite_edge(orient_.edge);
    const OppCells oc = build_opp_cells(ctx_, opp_spec(), !ring_view_, pc_state_);
    const TextDir dir = rail_text_dir(orient_.edge);
    for (unsigned i = 0; i < 4; i++) {
        const Rect sr = opp_slot_rect(layout_.opp_rail, opp_edge, dir, i);
        const uint32_t sig = opp_cell_sig(opp_edge, panel_is_column(opp_edge) ? sr.y : sr.x, oc.c[i]);
        if (sig == opp_sig_[i]) continue;
        opp_sig_[i] = sig;
        kit::draw_opp_slot(surface_of(sr), opp_edge, dir, i, oc.c[i]);
    }
}

void Ui::update_lit(int k, bool was_held, uint32_t now_ms) {
    const bool is_held = held(k);
    bool lit = pressed_[k];
    if (is_held && !was_held) { lit = true; lit_since_[k] = now_ms; }
    else if (!is_held && lit && now_ms - lit_since_[k] >= kPressFlashMs) lit = false;
    if (lit == pressed_[k]) return;
    pressed_[k] = lit;
    for (unsigned i = 0; i < 4; i++)
        if (static_cast<int>(slot_button(orient_, i)) == k) draw_slot(i);
}

void Ui::set_pressed(Button b, bool down, uint32_t now_ms) {
    const int k = static_cast<int>(b);
    const bool was_held = held(k);
    if (down && !down_[k]) suppress_rotated_press_[k] = false;
    if (down && !was_held) suppress_chord_[k] = false;
    down_[k] = down;
    update_lit(k, was_held, now_ms);
    feed_chord(now_ms);
}

void Ui::feed_chord(uint32_t now_ms) {
    constexpr int ok = static_cast<int>(Button::Ok), back = static_cast<int>(Button::Back);
    chord_.keys(held(ok), held(back), now_ms);
    if (!chord_.active()) return;
    suppress_chord_[ok] = suppress_chord_[back] = true;   // neither key's own Short / Long / Hold, until pressed afresh
    ctx_.keep.trigger.invalidate_confirmation();
    charger_test_.invalidate_confirmation();
}

void Ui::set_remote_pressed(Button logical, bool down, uint32_t now_ms) {
    const int l = static_cast<int>(logical);
    if (down) {
        if (remote_key_[l] != 0xFF) return;   // already held
        // Back and OK never move; plus / minus light the key that shows their action.
        remote_key_[l] = static_cast<uint8_t>(oriented_button(logical, orient_.rotation, rotate_buttons_));
    } else if (remote_key_[l] == 0xFF) {
        return;
    }
    const int k = remote_key_[l];
    const bool was_held = held(k);
    if (down && !was_held) suppress_chord_[k] = false;
    remote_down_[k] = down;
    if (!down) remote_key_[l] = 0xFF;
    update_lit(k, was_held, now_ms);
    feed_chord(now_ms);
}

// Reboot HoldCard (design 6.5): shown from kHoldCardMs of the OK + Back chord, counts down to the kChordRebootMs
// reboot, and releasing either key before that removes it and repaints the page. Pixels in content coordinates.
void Ui::update_hold_card(uint32_t now_ms, RebootChord::Step step) {
    const uint32_t ms = chord_.held_ms(now_ms);
    if (step == RebootChord::Step::None) {
        if (hold_shown_) {   // cancelled: wipe the card (an opaque page just paints over it) and repaint the page
            const uint32_t t0 = now_us();
            const bool opaque = depth_ > 0 ? menu_opaque(menu()) : page_opaque(page_);
            const bool blank = clear_for_incoming(now_ms, current(), opaque, false);   // the replay sees hold_shown_ still set
            paint_incoming(now_ms, t0, blank);
        }
        return;
    }
    draw_hold_card(ms);
}

// Fieldbook: an orange card with a 2 px ink frame, the count in font 24 and "release to cancel" in font 12 centred on it,
// and an ink bar filling along its bottom edge towards the reboot. Square corners, like every Fieldbook block.
void Ui::draw_hold_card(uint32_t ms) {
    const Surface &c = ctx_.content;
    constexpr int16_t kY = 70, kW = 200, kH = 84, kF = 2;               // the card, its frame inside (fits 208 and 224)
    const int16_t kX = static_cast<int16_t>((c.w - kW) / 2);
    constexpr int16_t kIn = kW - 2 * kF, kBarH = 10, kBarY = kY + kH - kF - kBarH;
    const int16_t kCountX = static_cast<int16_t>(kX + (kW - 11 * 12) / 2);
    const int16_t kNoteX = static_cast<int16_t>(kX + (kW - 17 * 6) / 2);
    const uint32_t total = kChordRebootMs;
    const int left = hold_secs_left(ms, total);
    const int px = hold_fill_px(ms, total, kIn);
    hold_ms_ = ms;
    if (!hold_shown_) {
        hold_shown_ = true;
        hold_px_ = hold_left_ = -1;
        gfx_fill(c, kX, kY, kW, kF, kInk);
        gfx_fill(c, kX, kY + kH - kF, kW, kF, kInk);
        gfx_fill(c, kX, kY + kF, kF, kH - 2 * kF, kInk);
        gfx_fill(c, kX + kW - kF, kY + kF, kF, kH - 2 * kF, kInk);
        gfx_fill(c, kX + kF, kY + kF, kIn, kH - 2 * kF, kCaution);
        gfx_text(c, kLabelFont, kNoteX, 116, K2_P("release to cancel"), kSpotInk, kCaution);
    }
    if (left != hold_left_) {
        hold_left_ = left;
        char t[12] = "Reboot in ";   // k2-plain: char array
        t[10] = static_cast<char>('0' + left);
        t[11] = 0;
        gfx_text(c, kMid, kCountX, 84, t, kSpotInk, kCaution);
    }
    if (px != hold_px_) {
        hold_px_ = px;
        if (px > 0) gfx_fill(c, kX + kF, kBarY, static_cast<int16_t>(px), kBarH, kSpotInk);
    }
}

void Ui::show_rebooting() {
    ContentViewport viewport(active_dest(), ctx_.content.w, ctx_.content.h);
    gfx_text(ctx_.content, kMid, 14, 100, K2_P("Rebooting..."), kWarn, kBg);
}

// ---------------------------------------------------------------------------------------
// Input and periodic work
// ---------------------------------------------------------------------------------------

UiAction Ui::on_button(Button b, ButtonEvent ev, uint32_t now_ms) {
    BudgetScope scope(*this);
    const auto event = to_event(b, ev);
    if (!event) return UiAction::None;
    const bool both = down_[static_cast<int>(Button::Minus)] && down_[static_cast<int>(Button::Plus)];
    if ((b == Button::Minus || b == Button::Plus) && both && event->press == Press::Long) {
        ctx_.keep.trigger.invalidate_confirmation();
        charger_test_.invalidate_confirmation();
        if (!rotation_chord_) { rotation_chord_ = true; rotation_gesture(now_ms); }
        return UiAction::None;
    }
    if (rotation_chord_ && (b == Button::Minus || b == Button::Plus)) return UiAction::None;
    if (suppress_rotated_press_[static_cast<int>(b)] || suppress_chord_[static_cast<int>(b)]) return UiAction::None;
    Event mapped = *event;
    mapped.button = oriented_button(b, orient_.rotation, rotate_buttons_);
    return finish_button(dispatch_button(mapped, now_ms), now_ms);
}

UiAction Ui::on_remote_button(Button logical, ButtonEvent ev, uint32_t now_ms) {
    BudgetScope scope(*this);
    const auto event = to_event(logical, ev);
    if (!event) return UiAction::None;
    // Logical key: no orientation mapping, no rotation chord (a remote has no physical key pair to hold). OK and Back
    // never move, so the reboot chord's suppression is indexed the same for both.
    if ((logical == Button::Ok || logical == Button::Back) && suppress_chord_[static_cast<int>(logical)]) return UiAction::None;
    return finish_button(dispatch_button(*event, now_ms), now_ms);
}

UiAction Ui::finish_button(UiAction action, uint32_t now_ms) {
    if (requested_rotation_ < 4) {
        const uint8_t target = requested_rotation_;
        requested_rotation_ = 0xFF;
        set_rotation(target, now_ms);
    }
    return action;
}

UiAction Ui::dispatch_button(Event event, uint32_t now_ms) {
    ContentViewport viewport(active_dest(), ctx_.content.w, ctx_.content.h);
    static const char *const names[4] = {"BACK", "-", "OK", "+"};
    K2LOG("[KEY] [%s] %s page=%d\n", names[static_cast<int>(event.button)],
                event.press == Press::Long ? "long" : event.press == Press::Hold ? "hold" : K2_P("short"), static_cast<int>(page_index()));

    ctx_.now = now_ms;
    if (event.button == Button::Back && event.press == Press::Hold && menu() == Menu::Trigger)
        top_as<TriggerPage>().on_event({Button::Back, Press::Long});
    if (event.button == Button::Back && event.press == Press::Hold && !depth_ && page_ == Page::ChargerTest)
        charger_test_.cancel();
    if (ring_view_) {   // in the ring the keys only move; the page below sees none of them
        ring_key(event, now_ms);
        if (ctx_.toast.active(now_ms)) {
            ctx_.toast.drawn = false;
            draw_toast_overlay(now_ms);
            paint_pending_ = true;
        }
        return UiAction::None;
    }
    // An entered page sees everything first. A short BACK it declines pops its menu, or at the top level (the ring page itself)
    // leaves to the ring; a long BACK nobody takes leaves to the ring from any depth.
    const bool prep_was_pending = clear_pending_ || static_pending_;
    const Dispatch d = depth_ ? dispatch_modal(event, current()) : dispatch(event, current());
    const bool short_back = event.button == Button::Back && event.press == Press::Short;
    // The place pages are menus over Lines (depth 1 is their top level); a menu over any other page is one level down from it.
    const bool top_level = depth_ <= (page_ == Page::Lines || (depth_ == 1 && stack_[0] == Menu::Tools) ? 1 : 0);
    if (d.route == Route::Pop && ring::entered_back(Press::Short, top_level) == ring::Do::None) pop_menu(now_ms);
    else if (page_ == Page::ChargerTest && !depth_ && d.route == Route::Unhandled && short_back) open_place_actions(bench::Place::Charger, now_ms);   // up one level: the place's Actions
    else if (d.route == Route::Pop || d.global == Global::ToMeter || (d.route == Route::Unhandled && short_back && !depth_)) ring_leave(now_ms);
    else {
        if (prep_was_pending && (clear_pending_ || static_pending_)) {
            current().invalidate();
            paint_pending_ = true;
        }
        if (ctx_.budget && ctx_.budget->more) paint_pending_ = true;
        sync_panel();
        if (uses_opp_rail()) sync_opp_rail();
    }
    if (ctx_.toast.active(now_ms)) {
        ctx_.toast.drawn = false;
        draw_toast_overlay(now_ms);
        paint_pending_ = true;
    }
    return UiAction::None;
}

// ---- the page ring (ui_ring.hpp) -------------------------------------------------------------------------------------------

ring::Pos Ui::ring_position() const {
    const Menu first = depth_ ? stack_[0] : Menu::Count;
    const uint8_t tab = first == Menu::Connection ? static_cast<uint8_t>(connection_page().tab()) : 0;
    return ring::owner(page_, first, tab);
}

Strip Ui::strip_now() const {
    if (!ring_view_) return current().labels();
    return ring::view_strip(ring_position(), ring::sanitize_target(ctx_.keep.long_back));
}

kit::OppRailSpec Ui::opp_spec() const {
    kit::OppRailSpec s = current().opp_rail();
    if (ring_view_) {   // the position number replaces the page's dots; an entered page shows its own title
        s.title = ring::mast_name(ring_position());
        s.dot_count = 0;
        s.dot_cur = 0;
    }
    return s;
}

// The view-only pages have nothing to enter, so they are always in the ring view.
void Ui::ring_settle() {
    if (depth_ == 0 && (page_ == Page::Lines || page_ == Page::Detail)) ring_view_ = true;
    kit::set_browse(ring_view_);
}

bool Ui::ring_show(ring::Pos pos, uint32_t now_ms) {
    const ring::Target t = ring::target(pos);
    if (t.menu != Menu::Count && ports_busy()) return false;   // a menu would be refused: change nothing
    BudgetScope scope(*this);
    ring_landing_ = true;
    set_page(t.page, now_ms);
    bool ok = true;
    if (t.menu != Menu::Count) {
        if (t.menu == Menu::Connection) {
            if (conn_lock_ < 0) conn_lock_ = static_cast<int8_t>(t.tab);
            conn_overview_next_ = !conn_actions_next_;
        }
        place_next_ = t.menu == Menu::Diagnosis;   // the PD diagnosis opens as the Log place
        ok = push_menu(t.menu, now_ms);
        place_next_ = false;
        if (!ok) { conn_lock_ = -1; conn_actions_next_ = false; conn_overview_next_ = false; }
    }
    ring_landing_ = false;
    return ok;
}

// +/- skip a page that is refused (a menu while a run owns the ports) and go on in their direction.
void Ui::ring_walk(ring::Pos to, int dir, uint32_t now_ms) {
    const ring::Pos cur = ring_position();
    for (int i = 0; i < ring::kCount && to != cur; i++) {
        if (ring_show(to, now_ms)) return;
        if (dir == 0) return;
        to = dir > 0 ? ring::next(to) : ring::prev(to);
    }
}

void Ui::ring_enter(uint32_t now_ms) {
    ring_view_ = false;
    kit::set_browse(false);
    sync_panel();
    if (uses_opp_rail()) sync_opp_rail();
    if (!paint_pending_) {
        ContentViewport viewport(active_dest(), ctx_.content.w, ctx_.content.h);
        current().update(now_ms);
    }
}

// Depth 1 and already the ring target's own menu, tab and base page, at rest: leaving then only flips the view and repaints
// the strip, the rail and the list focus. Every other chain (a tool page, a deeper menu, an Actions sub-page) is rebuilt at its top.
bool Ui::ring_page_rests() const {
    if (depth_ != 1) return false;
    const ring::Target t = ring::target(ring_position());
    if (t.menu == Menu::Count || t.menu != stack_[0] || t.page != page_) return false;
    switch (t.menu) {
    case Menu::Connection: return connection_page().ring_rest();
    case Menu::Tools: return true;
    case Menu::Battery: return battery_page()->ring_rest();
    case Menu::Diagnosis: return diagnosis_page().ring_rest();
    default: return false;
    }
}

void Ui::ring_leave(uint32_t now_ms) {
    if (page_ == Page::ChargerTest && !depth_) (void)charger_test_.stop();   // leaving the run's page ends it, and frees the ports for the place
    if ((depth_ > 0 && !ring_page_rests()) || page_ == Page::ChargerTest) {
        if (ring_show(ring_position(), now_ms)) return;   // the ring page again, at its top
        if (depth_ > 0) {   // refused (a run owns the ports): the menu goes, the base page shows in the ring
            ring_landing_ = true;
            set_page(page_, now_ms);
            ring_landing_ = false;
            return;
        }
    }
    ring_view_ = true;
    kit::set_browse(true);
    sync_panel();
    if (uses_opp_rail()) sync_opp_rail();
    if (!paint_pending_) {
        ContentViewport viewport(active_dest(), ctx_.content.w, ctx_.content.h);
        current().update(now_ms);
    }
}

void Ui::ring_key(Event e, uint32_t now_ms) {
    const ring::Step s = ring::view_key(ring_position(), e, ring::sanitize_target(ctx_.keep.long_back));
    if (s.what == ring::Do::Show) ring_walk(s.to, e.button == Button::Plus ? 1 : e.button == Button::Minus ? -1 : 0, now_ms);
    else if (s.what == ring::Do::Enter) ring_enter(now_ms);
}

void Ui::open_place(bench::Place p, uint32_t now_ms) {
    BudgetScope scope(*this);
    constexpr ring::Pos kPos[bench::kPlaces] = {ring::Pos::Home, ring::Pos::Link, ring::Pos::Link, ring::Pos::Link,
                                                ring::Pos::Battery, ring::Pos::Log, ring::Pos::K2};
    const ring::Pos pos = kPos[static_cast<int>(p) < bench::kPlaces ? static_cast<int>(p) : 0];
    if (pos == ring::Pos::Link) {
        const conn::Tab tab = p == bench::Place::Cable ? conn::Tab::Cable
                            : p == bench::Place::Device ? conn::Tab::Sink
                            : conn::Tab::Source;
        conn_lock_ = static_cast<int8_t>(tab);
    }
    const bool shown = ring_show(pos, now_ms);
    if (!shown) {
        conn_lock_ = -1;
        conn_overview_next_ = false;
    } else if (ring::can_enter(pos)) {
        ring_enter(now_ms);
    }
}

void Ui::open_place_actions(bench::Place p, uint32_t now_ms) {
    conn_actions_next_ = true;
    open_place(p, now_ms);
    conn_actions_next_ = false;
}

bool Ui::run_action(Action a, uint32_t now_ms, bool confirm) {
    BudgetScope scope(*this);
    ContentViewport viewport(active_dest(), ctx_.content.w, ctx_.content.h);
    // The confirm flag overrides no refusal: inline, every active TX (Identify, the VCONN cable read) is refused
    // outright - an inline K2 has no PD role of its own (2026-11-10). It is only the remote's "yes" for an action that
    // can raise the output above 5 V (remote::action_needs_confirm: FcScan), handed on to the driver.
    if (ports_busy() && a != Action::FcReset) {
        K2LOG("[TEST] action refused: charger test owns ports\n");
        return false;
    }
    switch (a) {
    case Action::PdRescan: {
        const bool ok = wake_source();
        if (menu() == Menu::Connection) top_as<ConnectionPage>().update(now_ms);
        if (!ok) return false;
        break;
    }
    case Action::EmarkerScan: {
        // The method follows the link: nothing supplies VBUS = inject VCONN and ask the cable; a source
        // present = listen only (it is the VCONN source and asks the cable at attach). The active Vconn
        // method is refused inline by the driver - there is no confirm for it any more.
        const pd::EmarkerMode mode = pd::emarker_mode(link_.attached(), ctx_.pm_online, ctx_.vbus_fast_mv);
        K2LOG("[EMK] mode=%s attached=%d inline=%d stale=%d pm_online=%d vbus=%dmV cable_seq=%u seen=%u\n",
                    mode == pd::EmarkerMode::Sniff ? K2_P("sniff") : K2_P("vconn"), int(link_.attached()), int(link_.inline_mode()),
                    int(link_.cable_stale()), int(ctx_.pm_online), int(ctx_.vbus_fast_mv),
                    unsigned(fusb302::cable_seq()), unsigned(cable_seq_seen_));
        ctx_.em_scanning = true;
        ctx_.em_listen_until = 0;
        ctx_.em_found = false;
        ctx_.em_quiet = false;
        refresh_action_views(now_ms);
        // The VCONN scan also reads the cable's SVIDs and modes (and SOP'' on an active one); a sniff only reports.
        bool found = mode == pd::EmarkerMode::Vconn ? fusb302::cable_scan_full(false) : fusb302::read_emarker(false, mode);
        // A sniff only reports what the engine still holds. After a cable swap that is the OLD cable's
        // data: it is not a scan, so it must not clear the stale flag (a new cable_seq does, in tick()).
        if (found && mode == pd::EmarkerMode::Sniff && link_.cable_stale() && fusb302::cable_seq() == cable_seq_seen_) {
            K2LOG("[UI] e-marker sniff: held data belongs to the previous cable, waiting for the new one\n");
            found = false;
        }
        ctx_.em_scanning = false;
        if (mode == pd::EmarkerMode::Vconn && link_.inline_mode()) {
            // refused by the driver (an inline K2 never transmits): no scan ran, so no scan mark
            // and no listen window either
        } else if (found || mode == pd::EmarkerMode::Vconn) {   // a listen that heard nothing is not a scan
            ctx_.em_found = found;
            ctx_.em_scanned = true;
            link_.emarker_scanned(now_ms);
            cable_seq_seen_ = fusb302::cable_seq();
            if (mode == pd::EmarkerMode::Vconn && found) {
                held::poll(now_ms, link_.inline_mode());
                held::mark_cable_probed();
            }
        } else {   // sniff, nothing decoded yet: keep listening for the source's own query
            ctx_.em_listen_until = now_ms + kListenWindowMs;
            K2LOG("[UI] e-marker sniff: nothing yet, listening 5 s\n");
        }
        refresh_action_views(now_ms);
        break;
    }
    case Action::Identify:
        // Inline there is no confirm: an inline K2 never transmits (a second sink on a live wire
        // resets the link, 2026-11-10). The driver refuses; the Sink tab shows the sniffed identity.
        {
            const IdResult r = identify(now_ms);
            refresh_action_views(now_ms);
            return r == IdResult::Started;
        }
    case Action::EnergyReset:
        ctx_.meter.reset(now_ms);
        ctx_.trend.clear();
        ctx_.extremes.reset();     // and Home's PEAK / SAG and the card cycle
        ctx_.keep.boxes.reset();
        ctx_.keep.attention.reset();
        ctx_.live.clear();   // OK on Home also drops the frozen LAST state
        K2LOG("[UI] energy meter reset\n");
        current().update(now_ms);
        break;
    case Action::FcScan:     // only the remote reaches this: the dispatcher and Remote::action refuse it without the confirm byte,
                             // and the driver asks again (a screen scan goes through the Review page, never here)
        fctrig::scan_start(confirm);
        break;
    case Action::FcReset:
        stop_fc(now_ms);
        break;
    }
    return true;
}

// ---------------------------------------------------------------------------------------
// Active PD: trigger and identify
// ---------------------------------------------------------------------------------------

static bool same_choice(const pd::Choice &a, const pd::Choice &b) {
    return a.pos == b.pos && a.mv == b.mv && a.ma == b.ma;
}

Ui::TriggerOutcome Ui::trigger(const pd::Choice &c, uint32_t now_ms, bool confirm, bool over_load) {
    ContentViewport viewport(active_dest(), ctx_.content.w, ctx_.content.h);
    if (ports_busy()) {
        K2LOG("[TEST] PD request refused: test running\n");
        return {pd::Err::Busy, false};
    }
    if (pd::is_fake()) return {pd::Err::NoCaps, false};
    const bool above_5v = trigger_needs_confirm(pd::active_caps(), c, ctx_.cap_mv);
    if (above_5v && loopguard::locked()) {   // before the confirm prompt: nothing to confirm (the sink refuses the same, OverCap)
        K2LOG("[LOOP] trigger PDO%u refused: 5V lock\n", c.pos);
        ctx_.trig_armed = false;
        return {pd::Err::OverCap, false};
    }
    // Lab over-load: the confirm rule is judged first, so a refused-attach state cannot skip it (both flags needed).
    if (pd::over_load_verdict(over_load, above_5v, confirm) == pd::OverLoadVerdict::NeedConfirm) {
        K2LOG("[UI] trigger PDO%u over-load above 5V needs confirm\n", c.pos);
        return {pd::Err::Ok, true};
    }
    if (fusb302::trigger_refusal() != pd::TriggerRefusal::None) {   // before any confirm prompt: no TX ever
        const pd::Err e = fusb302::sink_request(c, confirm, over_load);     // logs [PD] trigger refused: inline (reason) / over-load allowed
        ctx_.trig_armed = false;
        return {e, false};
    }
    if (above_5v && !confirm) {
        K2LOG("[UI] trigger PDO%u needs confirm\n", c.pos);
        return {pd::Err::Ok, true};
    }
    const pd::Err e = fusb302::sink_request(c, confirm, over_load);
    K2LOG("[UI] trigger PDO%u %umV %umA -> %s\n", c.pos, c.mv, c.ma, pd::err_name(e));
    ctx_.trig_armed = false;
    current().update(now_ms);
    return {e, false};
}

Ui::TriggerOutcome Ui::device_trigger(const pd::Choice &c, uint32_t now_ms) {
    ContentViewport viewport(active_dest(), ctx_.content.w, ctx_.content.h);
    if (!pd::is_fake() && trigger_needs_confirm(pd::active_caps(), c, ctx_.cap_mv)) {
        const bool armed = ctx_.trig_armed && same_choice(c, trig_choice_) && now_ms < trig_until_;
        if (!armed) {
            ctx_.trig_armed = true;
            trig_choice_ = c;
            trig_until_ = now_ms + kConfirmWindowMs;
            K2LOG("[UI] trigger PDO%u above 5V: OK again to confirm\n", c.pos);
            current().update(now_ms);
            return {pd::Err::Ok, true};
        }
        return trigger(c, now_ms, true);
    }
    return trigger(c, now_ms, false);
}

void Ui::trigger_release(uint32_t now_ms) {
    ContentViewport viewport(active_dest(), ctx_.content.w, ctx_.content.h);
    cancel_charger_test();
    cancel_twin();
    fusb302::sink_release();
    ctx_.trig_armed = false;
    K2LOG("[UI] trigger release to 5V\n");
    current().update(now_ms);
}

void Ui::stop_fc(uint32_t now_ms) {
    fctrig::stop();
    fc_release_.start(now_ms);
}

bool Ui::trigger_hard_reset(uint32_t now_ms, bool lab_inline) {
    ContentViewport viewport(active_dest(), ctx_.content.w, ctx_.content.h);
    cancel_charger_test();
    cancel_twin();
    const bool ok = fusb302::sink_hard_reset(lab_inline);
    ctx_.trig_armed = false;
    K2LOG("[UI] trigger hard reset%s: %s\n", lab_inline ? K2_P(" (lab inline)") : "", ok ? "sent" : K2_P("refused"));
    current().update(now_ms);
    return ok;
}

bool Ui::link_reset_inline() const {
    if (link_.inline_mode() || ctx_.inline_mode || fusb302::inline_mode() || fusb302::sink_downstream())
        return true;
    const pd::SinkStatus &sk = fusb302::sink_status();
    const bool k2_owns = sk.state == pd::SinkState::Ready || sk.held ||
                         fusb302::pin_rd() != 0 || fusb302::rd_for_run_on() || fusb302::self_powered();
    if (k2_owns) return false;
    if (fusb302::contract().valid) return true;   // foreign contract sniffed on the wire
    const uint8_t acc = fusb302::active_cc();
    if (!link_.attached() || acc == 0) return false;
    // Live VBUS above vSafe5V with no K2-owned contract, or an Rp/Rd divider in the sink band while K2
    // is not pulling Rd, means an active downstream sink sits inline even if K2 attached mid-session.
    if (ctx_.pm_online && ctx_.vbus_fast_mv > pd::kVSafe5MaxMv) return true;
    const int32_t acc_mv = (ctx_.analog && acc == 1) ? ctx_.analog->cc1_mv
                         : (ctx_.analog && acc == 2) ? ctx_.analog->cc2_mv : -1;
    return acc_mv >= 0 && pd::sink_present(acc_mv);
}

Ui::LinkResetResult Ui::link_reset(bool confirmed, uint32_t now_ms, bool allow_bus_powered, bool inline_known) {
    if (ports_busy()) return LinkResetResult::Busy;
    const bool inl = inline_known || link_reset_inline();
    if (inl) {
        if (!allow_bus_powered && (fusb302::self_powered() || power_source::bus_powered())) return LinkResetResult::SelfPowered;
        if (!confirmed) return LinkResetResult::NeedConfirm;
        held::clear_for_reset();
        const bool sent = fusb302::sink_hard_reset(true);
        K2LOG("[UI] link reset (inline Hard Reset): %s\n", sent ? "sent" : K2_P("refused"));
        return sent ? LinkResetResult::HardReset : LinkResetResult::NoLink;
    }
    const pd::SinkStatus &sk = fusb302::sink_status();
    if (sk.state == pd::SinkState::Ready && !fusb302::self_powered() && !power_source::bus_powered()) {
        held::clear_for_reset();
        const bool epr = sk.epr_mode;
        if (fusb302::sink_hard_reset(false)) {
            hr_recover_.arm(now_ms, epr);
            if (!survey::running()) survey::on_refresh(rescan::Result::Cycled, false);
            K2LOG("[UI] link reset (sink Hard Reset): sent\n");
            return LinkResetResult::HardReset;
        }
    }
    held::clear_for_reset();
    if (run_action(Action::PdRescan, now_ms)) return LinkResetResult::Cycled;
    return LinkResetResult::Busy;
}

Ui::IdResult Ui::identify(uint32_t now_ms) {
    ContentViewport viewport(active_dest(), ctx_.content.w, ctx_.content.h);
    if (ports_busy()) {
        K2LOG("[TEST] Identify refused: test running\n");
        return IdResult::Refused;
    }
    // Inline the driver refuses below (identify_start never transmits inline); what the wire already
    // showed - the phone's Discover Identity ACK, Sink_Capabilities, Requests - fills the Sink tab,
    // marked as sniffed.
    // A device ALONE on the K2 (a dual-role phone; VBUS dark, no source caps this attach, wait_kind
    // DualRolePort) is identified actively: the link state decides, not the ADC precheck below - mid-toggle
    // both CC lines can read low for a moment, which is not "no device". The driver presents Rd for it.
    const bool alone = pd::identify_alone(link_.inline_mode() || ctx_.inline_mode, ctx_.pm_online,
                                          ctx_.pm_online ? ctx_.vbus_fast_mv : -1, fusb302::caps_this_attach(),
                                          link_.wait_kind());
    ctx_.id_no_device = !alone && ctx_.no_device();
    if (ctx_.id_no_device) {   // nothing on CC and no VBUS: transmitting would only burn the retries
        K2LOG("[UI] identify refused: no device (no Rp on CC, VBUS low)\n");
        refresh_action_views(now_ms);
        return IdResult::NoDevice;
    }
    const bool ok = fusb302::identify_start();
    // An inline K2 never transmits, not even confirmed: the wire's own sniffed identity
    // (Discover Identity ACK, Sink_Capabilities, Requests) is what the Sink tab shows.
    const bool inline_refusal = !ok && fusb302::inline_mode();
    // Fixed phrases stay in the format string: a %s is cut at 12 bytes by the binlog.
    if (ok) {
        if (alone) K2LOG("[UI] identify started (device alone on the K2)\n");
        else K2LOG("[UI] identify started\n");
    } else if (inline_refusal) {
        K2LOG("[UI] identify refused: inline (no TX; the Sink tab shows the sniffed identity)\n");
    } else {
        K2LOG("[UI] identify refused\n");
    }
    refresh_action_views(now_ms);
    return ok ? IdResult::Started : inline_refusal ? IdResult::Inline : IdResult::Refused;
}

void Ui::debug_fake_pdos(uint8_t n, uint32_t now_ms) {
    BudgetScope scope(*this);
    ContentViewport viewport(active_dest(), ctx_.content.w, ctx_.content.h);
    pd::set_fake(n);
    if (n == 0) {
        link_.clear_force();
        fusb302::clear_source();   // the driver's list goes too, so the remote state agrees
        held::clear_for_reset();
    }
    K2LOG("[UI] debug fake pdos: %d\n", int(n));
    current().update(now_ms);
}

void Ui::debug_link(bool attached, uint32_t now_ms) {
    BudgetScope scope(*this);
    ContentViewport viewport(active_dest(), ctx_.content.w, ctx_.content.h);
    if (attached) link_.force_attached(now_ms);
    else link_.force_detached(now_ms);
    if (!attached) {
        fusb302::clear_source();
        held::clear_for_reset();
        // A real detach at the CC level too: every owner of Rd is dropped and Rd stays off for the hold, so the charger
        // sees the sink leave (a display-only override left a stuck Rd in place, hardware 2026-11-06).
        const bool cc = fusb302::cc_detach();
        K2LOG("[UI] debug detach: CC level %d\n", int(cc));
    }
    K2LOG("[UI] debug link: %s\n", attached ? K2_P("attach") : K2_P("detach"));
    current().update(now_ms);
}

// The views an action reports into: the Connection page (status bar) and the saved-cables browser.
void Ui::refresh_action_views(uint32_t now_ms) {
    ContentViewport viewport(active_dest(), ctx_.content.w, ctx_.content.h);   // rotated layouts scale the logical content onto the panel
    if (menu() == Menu::Connection) top_as<ConnectionPage>().update(now_ms);
    else if (menu() == Menu::Emarker) top_as<EmarkerPage>().update(now_ms);
}

bool Ui::wake_source() {
    if (ports_busy()) return false;
    pd::set_fake(0);   // as rescan_source: a press drops a debug / forced link first
    link_.clear_force();
    const bool dark = ctx_.pm_online && ctx_.vbus_fast_mv >= 0 && ctx_.vbus_fast_mv < raguard::kVbusAttachedMv;
    if (dark && fusb302::sink_status().state != pd::SinkState::Ready && auto_read_block() == pd::SvWhy::None &&
        fusb302::reread_start(pd::kRereadDefault)) {
        if (!survey::running()) survey::on_refresh(rescan::Result::Cycled, false);   // the woken attach is a new session: the survey starts clean
        return true;
    }
    return rescan_source();
}

void Ui::hr_recover_poll(uint32_t now_ms) {
    const bool blocked = ports_busy() || auto_read_block() != pd::SvWhy::None;
    if (hr_recover_.step(now_ms, ctx_.pm_online ? ctx_.vbus_fast_mv : -1, blocked) != pd::HrRecovery::Act::Wake) return;
    K2LOG("[UI] link reset: VBUS did not come back, waking the source\n");
    (void)wake_source();
}

bool Ui::rescan_source() {
    if (ports_busy()) return false;
    pd::set_fake(0);
    link_.clear_force();
    // core/rescan.cpp decides: passive inline, a Get_Source_Cap on a live contract, else the CC cycle (bridged by the Rescan
    // marker on a K2 that lives on the charger). Busy = a negotiation is in flight: nothing happened.
    // The survey (core/survey.cpp) runs from the same press: the questions the page cannot answer from what it heard. A survey
    // still reading owns the sink's gate, so a second press waits for it.
    const bool reading = survey::running();
    const rescan::Result res = reading ? rescan::Result::Busy : rescan::request();
    if (!reading) survey::on_refresh(res, false);   // ports_busy() was refused above: no charger test
    return res != rescan::Result::Busy;
}

bool Ui::open_connection(conn::Tab tab, bool spoof, uint32_t now_ms) {
    BudgetScope scope(*this);
    if (menu() == Menu::Connection && !spoof && connection_page().spec_mode() && connection_page().tab() == tab) {   // already on this place
        ContentViewport viewport(active_dest(), ctx_.content.w, ctx_.content.h);
        pump_paint(now_ms);
        return true;
    }
    if (depth_ > 0) set_page(page_, now_ms);   // a single Connection over the page, whatever was open
    ctx_.now = now_ms;
    conn_lock_ = static_cast<int8_t>(tab);
    conn_actions_next_ = spoof;
    conn_overview_next_ = false;
    if (!push_menu(Menu::Connection, now_ms)) { conn_lock_ = -1; conn_actions_next_ = false; conn_overview_next_ = false; return false; }
    if (spoof) return push_menu(Menu::Spoof, now_ms);
    pump_paint(now_ms);
    return true;
}

bool Ui::land_rescanned(uint32_t now_ms) {
    BudgetScope scope(*this);
    if (!open_connection(conn::Tab::Source, false, now_ms)) return false;
    ContentViewport viewport(active_dest(), ctx_.content.w, ctx_.content.h);
    ConnectionPage &conn = top_as<ConnectionPage>();
    conn.say(K2_P("Rescanned (rebooted)"), now_ms);
    pump_paint(now_ms);
    return true;
}

ChargerTestSession::RemoteStart Ui::charger_test_start(bool all_tests, uint32_t now_ms) {
    if (charger_test_.start_held()) return {ChargerTestSession::RemoteStart::Result::Busy};   // nothing moves, the page stays
    BudgetScope scope(*this);
    ContentViewport viewport(active_dest(), ctx_.content.w, ctx_.content.h);
    ctx_.now = now_ms;
    if (page_ != Page::ChargerTest || depth_) set_page(Page::ChargerTest, now_ms);   // the run lives on its page (leaving stops it)
    const auto r = charger_test_.remote_start(all_tests);
    current().update(now_ms);
    return r;
}

ChargerTestSession::Stopped Ui::charger_test_stop(uint32_t now_ms) {
    BudgetScope scope(*this);
    ContentViewport viewport(active_dest(), ctx_.content.w, ctx_.content.h);
    ctx_.now = now_ms;
    const auto s = charger_test_.stop();
    if (s != ChargerTestSession::Stopped::Nothing) current().update(now_ms);
    return s;
}

bool Ui::goto_page(uint8_t page, uint32_t now_ms) {
    BudgetScope scope(*this);
    if (page < static_cast<uint8_t>(Page::Count)) {
        set_page(static_cast<Page>(page), now_ms);
        return true;
    }
    if (page >= kConnTabAlias && page < kConnTabAlias + conn::kTabs)   // "pd" / "emarker" / "identify" of the old numbering
        return open_connection(static_cast<conn::Tab>(page - kConnTabAlias), false, now_ms);
    if (page < kMenuPageBase + static_cast<uint8_t>(Menu::Count)) {
        const Menu m = static_cast<Menu>(page - kMenuPageBase);
        if (m == Menu::Share || m == Menu::QrTest) return false;   // Share needs a record, QrTest its params: only their openers make them
        if (m == Menu::Connection && menu() == Menu::Connection) return true;   // already there
        if (depth_ > 0) set_page(page_, now_ms);
        return push_menu(m, now_ms);
    }
    return false;
}

bool Ui::capture_needs_scratch() const {
    const Menu m = menu();
    return m == Menu::Share || m == Menu::QrTest || m == Menu::PdStress;
}

void Ui::capture_state_begin() {
    capture_state_ = true;
    capture_toast_visible_ = ctx_.toast.drawn;
    capture_paint_critical_ = fctrig::paint_critical();
}

void Ui::capture_state_end() {
    capture_state_ = false;
}

void Ui::render_for_capture(uint32_t now_ms) {
    PaintBudget *const saved_budget = ctx_.budget;
    uint32_t saved_slot[4], saved_opp[4];
    const Toast saved_toast = ctx_.toast;
    const bool toast_was_visible = capture_state_ ? capture_toast_visible_ : ctx_.toast.drawn;
    const bool paint_critical = capture_state_ ? capture_paint_critical_ : fctrig::paint_critical();
    for (int i = 0; i < 4; i++) { saved_slot[i] = slot_sig_[i]; saved_opp[i] = opp_sig_[i]; }
    ctx_.budget = nullptr;   // captures always render the complete frame atomically
    ctx_.rows.set_budget(nullptr);
    ctx_.now = now_ms;
    sync_surface();
    if (ctx_.toast.active(now_ms)) ctx_.toast.drawn = false;
    ctx_.toast.erase_pending = false;
    ContentViewport viewport(active_dest(), ctx_.content.w, ctx_.content.h);
    rotation_notice_drawn_ = false;
    gfx_fill(ctx_.content, 0, 0, ctx_.content.w, ctx_.content.h, kBg);
    current().invalidate_for_capture();   // the clear erased everything the field caches remember
    ctx_.blank = true;
    current().draw_static();
    draw_panel();
    draw_opp_rail();
    kit::set_browse(ring_view_);
    current().update(now_ms);
    ctx_.blank = false;
    draw_release_notice();
    if (hold_shown_) { hold_shown_ = false; draw_hold_card(hold_ms_); }   // the card is part of the screen
    if (!hold_shown_) {
        draw_rotation_notice(now_ms);
        ctx_.toast.erase_pending = false;
        if (toast_was_visible || (!capture_state_ && !paint_critical)) draw_toast_overlay(now_ms);
    }
    for (int i = 0; i < 4; i++) { slot_sig_[i] = saved_slot[i]; opp_sig_[i] = saved_opp[i]; }
    ctx_.toast = saved_toast;
    ctx_.budget = saved_budget;
    ctx_.rows.set_budget(saved_budget);
}

void Ui::resync(uint32_t now_ms) {
    PaintBudget *const saved_budget = ctx_.budget;
    ctx_.budget = nullptr;
    ctx_.rows.set_budget(nullptr);
    ctx_.now = now_ms;
    sync_surface();
    ContentViewport viewport(active_dest(), ctx_.content.w, ctx_.content.h);
    rotation_notice_drawn_ = false;
    current().invalidate();
    pump_paint(now_ms);
    ctx_.budget = saved_budget;
    ctx_.rows.set_budget(saved_budget);
}

void Ui::refresh(uint32_t now_ms) {
    BudgetScope scope(*this);
    ctx_.now = now_ms;
    ContentViewport viewport(active_dest(), ctx_.content.w, ctx_.content.h);
    pump_paint(now_ms);
}

void Ui::draw_release_notice() {
    if (fc_release_.busy() && menu() != Menu::Trigger) {
        const bool unsafe = fc_release_.state() == trig::Release::State::Unsafe;
        draw_toast(ctx_.content, unsafe ? FooterTone::Error : FooterTone::Caution,
                   unsafe ? K2_P("Unsafe VBUS: unplug source") : K2_P("FC releasing; verify 5V"));
    } else if (loopguard::locked() && menu() != Menu::Trigger) {
        draw_toast(ctx_.content, FooterTone::Caution, loopguard::kLockText);   // on whatever page is up, while the lock holds
    }
#if K2_PDSTRESS
    else if (const char *line = pdstress_.footer(); line && menu() != Menu::Trigger && menu() != Menu::PdStress) {
        draw_toast(ctx_.content, FooterTone::Info, line);   // the run's one line, on whatever page is up
    }
#endif
#if K2_PDSPEC
    else if (const char *line = pdspec_.footer(); line && menu() != Menu::Trigger) {
        draw_toast(ctx_.content, FooterTone::Info, line);
    }
#endif
    // A battery run in progress: the 5 x 5 accent dot (the charger test's running dot) in the content's top-right corner, on
    // every page but a QR code (it could land on the code).
    if (battery_dot()) gfx_fill(ctx_.content, static_cast<int16_t>(ctx_.content.w - kBattDot), 0, kBattDot, kBattDot, kFocus);
}

bool Ui::battery_dot() const {
    return battery::hw::service().rec().active() && menu() != Menu::Share && menu() != Menu::QrTest;
}

void Ui::log_timing(uint32_t now_ms) {
    if (now_ms - page_since_ms_ < 1000) return;
    timing_pending_ = false;
    StrBuf<12> full, fld;
    full.put_fixed(static_cast<int32_t>(full_us_), 1);    // us as thousandths of a ms, 1 decimal
    fld.put_fixed(static_cast<int32_t>(field_max_us_), 1);
    K2LOG("[UI] full=%sms field=%sms page=%d\n", full.c_str(), fld.c_str(), static_cast<int>(page_index()));
}

}  // namespace k2
