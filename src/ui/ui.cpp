// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#include "k2/ui/ui.hpp"
#include "k2/core/gap_meter.hpp"
#include "k2/core/log.hpp"
#include "k2/core/loop_guard.hpp"
#include "k2/core/paint_meter.hpp"
#include "k2/core/rescan.hpp"
#include "k2/core/survey.hpp"
#include "k2/fastcharge/fctrig.hpp"
#include "k2/drivers/fusb302.hpp"
#include "k2/drivers/lcd.hpp"
#include "k2/pd/link.hpp"
#include "k2/pd/pd_emarker_plan.hpp"
#include "k2/pd/pd_source.hpp"
#include "k2/pd/power_source.hpp"
#include "k2/drivers/power_calc.hpp"
#include "k2/drivers/systick.hpp"
#include "k2/drivers/usb.hpp"
#include "k2/drivers/buttons.hpp"
#include "k2/loopprobe/loop_probe_hw.hpp"
#include "k2/ui/kit.hpp"
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
    case Page::Tools: return pages_.push<ToolsPage>(ctx_);
    case Page::Settings: return pages_.push<SettingsPage>(ctx_);
    default: return pages_.push<HomePage>(ctx_);
    }
}

PageIf *Ui::build_menu(Menu m) {
    switch (m) {
    case Menu::Trigger: return pages_.push<TriggerPage>(ctx_);
    case Menu::Emarker: return pages_.push<EmarkerPage>(ctx_);
    case Menu::Connection: {
        ConnectionPage *p = pages_.push<ConnectionPage>(ctx_);
        if (p && conn_lock_ >= 0) p->lock(static_cast<conn::Tab>(conn_lock_));   // a single tool, before enter() and the first paint
        conn_lock_ = -1;
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
    case Menu::Twin: return pages_.push<TwinPage>(ctx_);
    case Menu::LoopProbe: return pages_.push<LoopProbePage>(ctx_);
#if K2_PDSTRESS
    case Menu::PdStress: return pages_.push<PdStressPage>(ctx_);
#endif
    case Menu::Diagnosis: return pages_.push<DiagnosisPage>(ctx_);
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
        case Menu::Twin: return top_as<TwinPage>().list_state();
        case Menu::Diagnosis: return top_as<DiagnosisPage>().list_state();
        default: return ListState{};
        }
    }
    switch (page_) {
    case Page::ChargerTest: return top_as<ChargerTestPage>().list_state();
    case Page::Lines: return top_as<LinesPage>().list_state();
    case Page::Tools: return top_as<ToolsPage>().list_state();
    case Page::Settings: return top_as<SettingsPage>().list_state();
    default: return ListState{};
    }
}

void Ui::init(uint32_t now_ms, bool cancel_resume) {
    lcd::set_rotation(orient_.rotation);
    layout_ = compute_layout(orient_.edge, kSlotCenters, kSlotLens, kPanelThickness);
    ctx_.content = {0, 0, 224, 240};   // logical content; ContentViewport maps it onto layout_.content
    ctx_.screen = surface_of(layout_.content);
    panel_ = surface_of(layout_.panel);
    ctx_.meter.reset(now_ms);

    lcd::fill(kBg);
    set_page(Page::Home, now_ms);
    ctx_.cables.load();   // the remembered cable selection, before boot_restore's boot-time arming reads it
    if (charger_test_.boot_restore(cancel_resume)) set_page(Page::ChargerTest, now_ms);   // reboot-resume: "Resume run?" (or by itself)
#if K2_PDSTRESS
    else pdstress_.boot_restore(cancel_resume, now_ms);   // a PD Stress journal resumes by itself once VBUS is steady
#endif
    // A loop probe record still marked in progress: the K2 restarted inside its CC step. Close it (CC Interrupted / Rebooted);
    // Tools -> Loop probe and `k2_remote loop-probe` report it. The probe itself never resumes.
    if (const auto step = loopprobe::boot_check(loopprobe::hw::store()))
        K2LOG("[LPRB] boot: closed the run interrupted in %s\n", loopprobe::step_name(*step));
    K2LOG("[ROT] Settings > Autorotate: auto or manual\n");
}

void Ui::set_rotation(uint8_t rotation, uint32_t now_ms) {
    rotation &= 3;
    // orient_ is only ever assigned from orientation_for() (and kDefaultOrientation ==
    // orientation_for(0)), so the edge always matches the rotation: a plain compare suffices and
    // the button strip stays attached to the physical button edge in every rotation.
    if (rotation == orient_.rotation) return;
    charger_test_.invalidate_confirmation();
    if (rotate_buttons_ && (rotation >= 2) != (orient_.rotation >= 2)) {
        ctx_.keep.trigger.invalidate_confirmation();
        suppress_rotated_press_[static_cast<int>(Button::Minus)] = down_[static_cast<int>(Button::Minus)];
        suppress_rotated_press_[static_cast<int>(Button::Plus)] = down_[static_cast<int>(Button::Plus)];
    }
    orient_ = orientation_for(rotation);
    ctx_.rotation = rotation;
    layout_ = compute_layout(orient_.edge, kSlotCenters, kSlotLens, kPanelThickness);
    ctx_.content = {0, 0, 224, 240};
    ctx_.screen = surface_of(layout_.content);
    panel_ = surface_of(layout_.panel);
    lcd::set_rotation(rotation);
    lcd::fill(kBg);
    hold_shown_ = false;
    redraw(now_ms);
    K2LOG("[ROT] rotation=%u mode=%s\n", unsigned(rotation), rotation_lock_ ? "lock" : "auto");
}

void Ui::set_rotate_buttons(bool enabled) {
    if (rotate_buttons_ == enabled) return;
    if (orient_.rotation >= 2) {
        ctx_.keep.trigger.invalidate_confirmation();
        charger_test_.invalidate_confirmation();
        suppress_rotated_press_[static_cast<int>(Button::Minus)] = down_[static_cast<int>(Button::Minus)];
        suppress_rotated_press_[static_cast<int>(Button::Plus)] = down_[static_cast<int>(Button::Plus)];
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
    current().invalidate();   // Home paints its new layout on the next update
    uiprefs::Prefs p = uiprefs::stored();
    const uiprefs::Prefs before = p;
    p.home_hero = home::qty_byte(q);
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
    rotation_notice_drawn_ = false;
    requested_rotation_ = automatic ? gravity_.rotation() : static_cast<uint8_t>(rotation & 3);
    K2LOG("[ROT] settings=%s degrees=%u\n", automatic ? "auto" : "manual", unsigned(requested_rotation_) * 90);
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
    rotation_notice_until_ = now_ms + 5000;
    rotation_notice_drawn_ = false;
    draw_rotation_notice(now_ms);
    K2LOG("[ROT] %s %u; hold +/- together to cycle\n", rotation_lock_ ? "LOCK" : "AUTO", unsigned(rotation()));
    store_orientation(rotation());   // the chord is the same setting as Settings > Autorotate: remembered the same way
}

void Ui::draw_rotation_notice(uint32_t now_ms) {
    if (!rotation_notice_until_ ||
        static_cast<int32_t>(now_ms - rotation_notice_until_) >= 0) return;
    ContentViewport viewport(layout_.content);
    StrBuf<24> notice;
    if (rotation_lock_) notice.put("Manual ").put_u32(rotation() * 90).put(" deg");
    else notice.put("Autorotate: Auto");
    draw_footer(ctx_.content, FooterTone::Info, notice.view());
    rotation_notice_drawn_ = true;
}

// Opaque pages: the full paint (draw_static + update with ctx_.blank false) writes every content pixel, panel strip aside
// (draw_panel owns it). A page that is not listed is erased before it paints. The pixel-exact walk of every switch in
// tests/test_ui_capture.cpp proves each entry: it compares the switch against a clean paint, in all four rotations.
bool Ui::page_opaque(Page p) {
    switch (p) {
    case Page::Home: case Page::Detail: case Page::Lines: case Page::Tools: case Page::Settings: return true;
    default: return false;   // ChargerTest: the stage decides what its paint covers
    }
}

bool Ui::menu_opaque(Menu m) {
    switch (m) {
    case Menu::UsbLink: case Menu::Diag: return true;
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
    const bool blank = clear_for_incoming(now_ms, *outgoing, page_opaque(p));   // the outgoing pages are still alive for a replay
    pages_.pop_to(changed ? 0 : 1);        // the menus go; so does the rotation page unless it is the one asked for
    if (changed) build_base(p);
    if (p == Page::ChargerTest) pages_.at(0)->enter();
    paint_incoming(now_ms, t0, blank);
}

bool Ui::twin_live_busy() const { return twin_hw::live_running(); }

twin_hw::LiveCtx Ui::twin_ctx(uint32_t now_ms) const {
    twin_hw::LiveCtx c;
    c.now = now_ms;
    c.vbus_fast_mv = ctx_.pm_online ? ctx_.vbus_fast_mv : -1;
    c.cap_mv = ctx_.cap_mv;
    c.busy = charger_test_busy() || fc_release_.busy() || loop_probe_busy();
    c.inline_link = link_.inline_mode() || ctx_.inline_mode;
    return c;
}

void Ui::cancel_twin() { twin_hw::live_cancel(); }

void Ui::cancel_runs() {
    cancel_charger_test();
    cancel_twin();
#if K2_PDSTRESS
    pdstress_.cancel();
#endif
    if (LoopProbePage *probe = find<LoopProbePage>(Menu::LoopProbe)) probe->cancel(ctx_.now);
}

bool Ui::loop_probe_busy() const {
    const LoopProbePage *probe = find<LoopProbePage>(Menu::LoopProbe);
    return probe && probe->running();
}

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
    case BusyWhy::TestRunning: return "Test running: stop it";
    case BusyWhy::FcRelease: return "5V release: wait";
    case BusyWhy::QcTrigger: return "QC trigger: stop it";
    case BusyWhy::PdHold: return "PD hold: release it";
    case BusyWhy::Epr: return "EPR contract: exit it";
    case BusyWhy::PdRequest: return "PD request: wait";
    case BusyWhy::EmScan: return "E-marker scan: wait";
    case BusyWhy::Identify: return "Identify: wait";
    case BusyWhy::TwinRun: return "Twin proof: stop it";
#if K2_PDSTRESS
    case BusyWhy::PdStress: return pdstress_.busy_reason();   // "PD Stress: stop it" / "PD Stress: resuming"
#else
    case BusyWhy::PdStress: return "PD Stress: stop it";   // unreachable: pdstress_active() is false
#endif
    case BusyWhy::LoopProbe: return "Loop probe: stop it";
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
            rows = list_state().count > 0;
            break;
        case Menu::Spoof: case Menu::UsbLink: case Menu::Autorotate: case Menu::BlackBox: rows = true; break;
        case Menu::Connection: {
            const ConnectionPage &cp = connection_page();
            s.sub = static_cast<uint8_t>((static_cast<uint8_t>(cp.tab()) & kSubTabMask) | (cp.popup_open() ? kSubPopup : 0) |
                                         (cp.details_open() ? kSubDetails : 0));
            if (cp.popup_open()) {
                s.sel = static_cast<uint8_t>(cp.popup_selected());
                s.count = static_cast<uint8_t>(cp.popup_count());
            }
            break;
        }
        default: break;
        }
    } else {
        rows = page_ == Page::Tools || page_ == Page::Settings;
    }
    if (rows) {
        const int n = ctx_.rows.count(), c = ctx_.rows.selected();
        s.count = static_cast<uint8_t>(n > 255 ? 255 : n < 0 ? 0 : n);
        s.sel = static_cast<uint8_t>(c < 0 ? 0 : c > 254 ? 254 : c);
    }
    s.home_window = static_cast<uint8_t>(ctx_.keep.home_window);
    s.flags = static_cast<uint8_t>((hold_shown_ ? kSessionHoldCard : 0) | (rotation_notice_until_ ? kSessionRotationNotice : 0) |
                                   (ctx_.em_scanning ? kSessionEmScanning : 0) | (ctx_.em_listen_until ? kSessionEmListening : 0) |
                                   (ctx_.em_quiet ? kSessionEmQuiet : 0) | (ctx_.id_no_device ? kSessionIdNoDevice : 0));
    return s;
}

// The loop lock's footer line (8 px a character in the 220 px footer band).
static_assert(fits_px(8, loopguard::kLockText, 220), "the loop lock line is wider than the footer band");

// PD Stress asks this one: the same owners as busy_reason() minus the ones a run causes itself (a held contract, the sink state, an
// EPR contract). Its own run is not "foreign".
const char *Ui::foreign_busy() const {
    const auto fs = fctrig::status();
    if (loopguard::locked()) return loopguard::kLockText;   // a run starts nothing above 5 V, a restore waits (k2/core/loop_guard.hpp)
    if (charger_test_busy()) return "Test running: stop it";
    if (loop_probe_busy()) return "Loop probe: stop it";
    if (fc_release_.busy()) return "5V release: wait";
    if (fctrig::scan_running() || fs.state == fc::State::Negotiating || fs.state == fc::State::Active ||
        fctrig::svooc_running())
        return "QC trigger: stop it";
    if (ctx_.em_scanning) return "E-marker scan: wait";
    if (fusb302::identify_status().state == pd::IdState::Running) return "Identify: wait";
    return nullptr;
}

void Ui::redraw(uint32_t now_ms) {
    rotation_notice_drawn_ = false;
    paint_incoming(now_ms, now_us(), true);   // the callers have just filled the screen
}

void Ui::erase_outgoing(uint32_t now_ms, PageIf &outgoing) {
    ContentViewport viewport(layout_.content);
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
    if (hold_shown_) gfx_fill(ctx_.content, 12, 70, 200, 84, kBg);   // the hold card's box (its own draw is cached)
    draw_rotation_notice(now_ms);
    lcd::set_erase(false);
}

// Clears what the incoming page must not find under it; false = nothing was cleared and the page paints over live ink.
bool Ui::clear_for_incoming(uint32_t now_ms, PageIf &outgoing, bool incoming_opaque) {
    constexpr uint32_t kContentPx = 224u * 240u;
    if (!incoming_opaque) {
        if (full_px_ < kContentPx) {
            erase_outgoing(now_ms, outgoing);   // a replay writes what the outgoing paint wrote: cheaper than a plain fill
        } else {
            ContentViewport viewport(layout_.content);
            gfx_fill(ctx_.content, 0, 0, ctx_.content.w, ctx_.content.h, kBg);
        }
    }
    // Whatever the card and the notice drew is gone now (wiped, or painted over by the opaque page): redraw them on demand.
    rotation_notice_drawn_ = false;
    hold_shown_ = false;
    hold_px_ = hold_left_ = -1;
    return !incoming_opaque;
}

void Ui::paint_incoming(uint32_t now_ms, uint32_t t0, bool blank) {
    ContentViewport viewport(layout_.content);
    const std::uint64_t px0 = paint::meter.px_count();
    current().invalidate();   // the erase pass erased everything the field caches remember; a built page has drawn nothing
    current().sample();
    ctx_.blank = blank;       // true: the screen is kBg under the whole content (a fill or the erase pass ran); false: opaque page
    // No page paints the rows between its top bar's hairline and the body (kBarH..kBodyY; the wizard rule also leaves a gap
    // pixel between its segments) nor the two rows between a list's bottom and the footer: an opaque page lays that ground
    // first (5 rows, 1,120 px), the page's own paint goes over it. Home has neither bar nor footer.
    if (!blank && !(depth_ == 0 && page_ == Page::Home)) {
        gfx_fill(ctx_.content, 0, kBarH, ctx_.content.w, static_cast<int16_t>(kit::kBodyY - kBarH), kBg);
        gfx_fill(ctx_.content, 0, kit::kListBottom, ctx_.content.w, static_cast<int16_t>(kFooterY - kit::kListBottom), kBg);
    }
    current().draw_static();
    draw_panel();
    current().update(now_ms);
    ctx_.blank = false;
    draw_release_notice();
    draw_rotation_notice(now_ms);
    full_px_ = static_cast<uint32_t>(paint::meter.px_count() - px0);
    last_update_ms_ = now_ms;
    full_us_ = now_us() - t0;
    field_max_us_ = 0;
    page_since_ms_ = now_ms;
    timing_pending_ = true;
}

bool Ui::push_menu(Menu m, uint32_t now_ms) {
    // The PD Stress page opens over its own run (a run the remote started shows there, and its BACK stops it); it only reads the session.
    if (ports_busy() && !(m == Menu::PdStress && pdstress_active())) {
        K2LOG("[TEST] menu refused: test/release in progress\n");
        return false;
    }
    if (m >= Menu::Count || depth_ >= 3) return false;
    const uint32_t t0 = now_us();
    PageIf *const outgoing = &current();   // stays alive under the new menu
    PageIf *const page = build_menu(m);
    if (!page) return false;               // the arena is full (the static_asserts in ui.hpp keep this unreachable)
    stack_[depth_++] = m;
    page->enter();
    const bool blank = clear_for_incoming(now_ms, *outgoing, menu_opaque(m));
    paint_incoming(now_ms, t0, blank);
    return true;
}

bool Ui::open_share(qrshare::Type type, std::uint8_t schema, std::string_view title, std::size_t len, uint32_t now_ms) {
    if (!share_rec_.begin(type, schema, title, len)) {
        K2LOG("[SHARE] nothing to share (%u bytes)\n", unsigned(len));
        return false;
    }
    ctx_.now = now_ms;   // enter() times the first part from it
    if (!push_menu(Menu::Share, now_ms)) return false;
    K2LOG("[SHARE] type=%u %uB in %u part(s)\n", unsigned(type), unsigned(len), share_rec_.parts());
    return true;
}

bool Ui::pop_menu(uint32_t now_ms) {
    if (depth_ == 0) return false;
    const uint32_t t0 = now_us();
    PageIf *const outgoing = pages_.at(depth_);   // the top menu
    --depth_;
    outgoing->leave();
    const bool incoming_opaque = depth_ > 0 ? menu_opaque(stack_[depth_ - 1]) : page_opaque(page_);   // the page below
    const bool blank = clear_for_incoming(now_ms, *outgoing, incoming_opaque);
    pages_.pop();                                  // the page below is the one that was kept
    paint_incoming(now_ms, t0, blank);
    return true;
}

// ---------------------------------------------------------------------------------------
// Button label panel
// ---------------------------------------------------------------------------------------

void Ui::draw_slot(unsigned slot) {
    ContentViewport::Pause unscaled_labels;
    const Button b = slot_button(orient_, slot);
    const Button action = oriented_button(b, orient_.rotation, rotate_buttons_);
    const SlotLabel lab = current().labels().b[static_cast<int>(action)];
    const bool down = pressed_[static_cast<int>(b)];
    slot_sig_[slot] = slot_sig(slot);
    if (current().hides_strip()) return;   // a full-panel page (the QR code) owns the strip's pixels
    const uint16_t bg = down ? kPanelHi : kPanelBg;
    const uint16_t fg = down ? kBg : lab.dim ? kDim : kText;
    const Rect &r = layout_.slot[slot];
    if (r.w <= 0 || r.h <= 0) return;

    Surface s = surface_of(r);

    // The label sits in the slot's text zone (divider and padding excluded) and is centred in it. The slot's ground is
    // painted around the label and the divider, which cover their own pixels: every pixel of the slot is streamed once.
    char text[24];
    const bool column = panel_is_column(orient_.edge);
    const int along = column ? s.h : s.w;       // slot length along the panel
    const LabelZone zone = label_zone(along, slot);
    const unsigned chars = label_text(text, lab, zone.max_chars);
    kit::Box holes[2];
    int nh = 0;
    int16_t tx = 0, ty = 0;
    if (chars > 0) {
        const int16_t a0 = static_cast<int16_t>(label_origin(zone, chars));
        const int16_t tw = static_cast<int16_t>(text_width(kLabelFont, std::string_view(text, chars)));
        tx = column ? static_cast<int16_t>((s.w - kLabelFont.height) / 2) : a0;
        ty = column ? a0 : static_cast<int16_t>((s.h - kLabelFont.height) / 2);
        const int16_t fh = static_cast<int16_t>(kLabelFont.height);
        holes[nh++] = {tx, ty, column ? fh : tw, column ? tw : fh};
    }
    if (slot > 0)   // divider on the slot's leading edge, between this slot and the previous one
        holes[nh++] = column ? kit::Box{0, 0, s.w, kDividerPx} : kit::Box{0, 0, kDividerPx, s.h};
    kit::fill_except(s, {0, 0, s.w, s.h}, holes, nh, bg);
    if (chars > 0) {
        if (column)   // glyphs rotated 90 degrees, running along the slot
            gfx_text_rot(s, kLabelFont, tx, ty, std::string_view(text, chars), fg, bg, panel_text_dir(orient_.edge));
        else
            gfx_text(s, kLabelFont, tx, ty, std::string_view(text, chars), fg, bg);
    }
    if (slot > 0) {
        if (column) gfx_fill(s, 0, 0, s.w, kDividerPx, kPanelLine);
        else gfx_fill(s, 0, 0, kDividerPx, s.h, kPanelLine);
    }
}

void Ui::draw_panel() {
    for (unsigned i = 0; i < 4; i++) draw_slot(i);
}

// FNV-1a over the label texts and the dim flag of what the current page shows in this slot.
uint32_t Ui::slot_sig(unsigned slot) {
    const Button physical = slot_button(orient_, slot);
    const SlotLabel lab = current().labels().b[static_cast<int>(oriented_button(physical, orient_.rotation, rotate_buttons_))];
    uint32_t h = 2166136261u;
    for (const char *t : {lab.shrt, lab.lng, lab.lng_alt}) {
        for (; *t; t++) h = (h ^ static_cast<uint8_t>(*t)) * 16777619u;
        h = (h ^ 0xFFu) * 16777619u;
    }
    return (h ^ (lab.dim ? 1u : 0u)) * 16777619u;
}

void Ui::sync_panel() {
    for (unsigned i = 0; i < 4; i++)
        if (slot_sig(i) != slot_sig_[i]) draw_slot(i);
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
            const bool blank = clear_for_incoming(now_ms, current(), opaque);   // the replay sees hold_shown_ still set
            paint_incoming(now_ms, t0, blank);
        }
        return;
    }
    draw_hold_card(ms);
}

void Ui::draw_hold_card(uint32_t ms) {
    const Surface &c = ctx_.content;
    const uint32_t total = kChordRebootMs;
    const int left = hold_secs_left(ms, total);
    const int px = hold_fill_px(ms, total, 172);
    hold_ms_ = ms;
    if (!hold_shown_) {
        hold_shown_ = true;
        hold_px_ = hold_left_ = -1;
        fill_round(c, 12, 70, 200, 84, 8, kSurface2);
        frame_round(c, 12, 70, 200, 84, 8, 2, kError);
        draw_icon(c, 26, 86, Icon::Warn, kError, kSurface2);
        fill_round(c, 26, 118, 172, 6, 1, kRule);
        gfx_text(c, kLabelFont, 26, 132, "Release to cancel", kDim, kSurface2);
    }
    if (left != hold_left_) {
        hold_left_ = left;
        char t[12] = "Reboot in ";
        t[10] = static_cast<char>('0' + left);
        t[11] = 0;
        gfx_text(c, kMid, 44, 82, t, kText, kSurface2);
    }
    if (px != hold_px_) {
        hold_px_ = px;
        if (px > 0) fill_round(c, 26, 118, px, 6, 1, kError);
    }
}

void Ui::show_rebooting() {
    ContentViewport viewport(layout_.content);
    gfx_text(ctx_.content, kMid, 14, 100, "Rebooting...", kWarn, kBg);
}

// ---------------------------------------------------------------------------------------
// Input and periodic work
// ---------------------------------------------------------------------------------------

UiAction Ui::on_button(Button b, ButtonEvent ev, uint32_t now_ms) {
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
    ContentViewport viewport(layout_.content);
    static const char *const names[4] = {"BACK", "-", "OK", "+"};
    K2LOG("[KEY] [%s] %s page=%d\n", names[static_cast<int>(event.button)],
                event.press == Press::Long ? "long" : event.press == Press::Hold ? "hold" : "short", static_cast<int>(page_index()));

    ctx_.now = now_ms;
    if (event.button == Button::Back && event.press == Press::Hold && menu() == Menu::Trigger)
        top_as<TriggerPage>().on_event({Button::Back, Press::Long});
    if (event.button == Button::Back && event.press == Press::Hold && !depth_ && page_ == Page::ChargerTest)
        charger_test_.cancel();
    const unsigned pg = static_cast<unsigned>(page_);
    if (depth_) {   // modal: no paging, the submenu sees everything, a short BACK it declines pops it
        const Dispatch m = dispatch_modal(event, current());
        if (m.route == Route::Pop) pop_menu(now_ms);
        else sync_panel();
        return UiAction::None;
    }
    const Dispatch d = dispatch(event, pg, static_cast<unsigned>(Page::Count), current(),
                                page_ == Page::ChargerTest && charger_test_.holds_paging());
    switch (d.global) {
    case Global::PagePrev: set_page(static_cast<Page>(pg - 1), now_ms); break;
    case Global::PageNext: set_page(static_cast<Page>(pg + 1), now_ms); break;
    case Global::None:     break;
    }
    if (d.global == Global::None) sync_panel();
    return UiAction::None;
}

bool Ui::run_action(Action a, uint32_t now_ms, bool confirm) {
    ContentViewport viewport(layout_.content);
    // The confirm flag overrides no refusal: inline, every active TX (Identify, the VCONN cable read) is refused
    // outright - an inline K2 has no PD role of its own (2026-11-10). It is only the remote's "yes" for an action that
    // can raise the output above 5 V (remote::action_needs_confirm: FcScan), handed on to the driver.
    if (ports_busy() && a != Action::FcReset) {
        K2LOG("[TEST] action refused: charger test owns ports\n");
        return false;
    }
    switch (a) {
    case Action::PdRescan:
        pd::set_fake(0);
        link_.clear_force();
        // core/rescan.cpp decides: passive inline, a Get_Source_Cap on a live contract, else the CC cycle (bridged by the Rescan
        // marker on a K2 that lives on the charger). Busy = a negotiation is in flight: nothing happened.
        // The survey (core/survey.cpp) runs from the same press: the questions the page cannot answer from what it heard. A survey
        // still reading owns the sink's gate, so a second press waits for it.
        {
            const bool reading = survey::running();
            const rescan::Result res = reading ? rescan::Result::Busy : rescan::request();
            const bool busy = res == rescan::Result::Busy;
            if (!reading) survey::on_refresh(res, false);   // ports_busy() was refused above: no charger test
            if (menu() == Menu::Connection) top_as<ConnectionPage>().update(now_ms);
            if (busy) return false;
        }
        break;
    case Action::EmarkerScan: {
        // The method follows the link: nothing supplies VBUS = inject VCONN and ask the cable; a source
        // present = listen only (it is the VCONN source and asks the cable at attach). The active Vconn
        // method is refused inline by the driver - there is no confirm for it any more.
        const pd::EmarkerMode mode = pd::emarker_mode(link_.attached(), ctx_.pm_online, ctx_.vbus_fast_mv);
        K2LOG("[EMK] mode=%s attached=%d inline=%d stale=%d pm_online=%d vbus=%dmV cable_seq=%u seen=%u\n",
                    mode == pd::EmarkerMode::Sniff ? "sniff" : "vconn", int(link_.attached()), int(link_.inline_mode()),
                    int(link_.cable_stale()), int(ctx_.pm_online), int(ctx_.vbus_fast_mv),
                    unsigned(fusb302::cable_seq()), unsigned(cable_seq_seen_));
        ctx_.em_scanning = true;
        ctx_.em_listen_until = 0;
        ctx_.em_quiet = false;
        refresh_action_views(now_ms);
        bool found = fusb302::read_emarker(false, mode);
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
            ctx_.em_scanned = true;
            link_.emarker_scanned(now_ms);
            cable_seq_seen_ = fusb302::cable_seq();
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
        ctx_.watts.clear();
        ctx_.volts.clear();
        ctx_.amps.clear();
        ctx_.trend.clear();
        ctx_.extremes.reset();     // and Home's PEAK / SAG and the card cycle
        ctx_.keep.cards.reset();
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
    ContentViewport viewport(layout_.content);
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
    ContentViewport viewport(layout_.content);
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
    ContentViewport viewport(layout_.content);
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
    ContentViewport viewport(layout_.content);
    cancel_charger_test();
    cancel_twin();
    const bool ok = fusb302::sink_hard_reset(lab_inline);
    ctx_.trig_armed = false;
    K2LOG("[UI] trigger hard reset%s: %s\n", lab_inline ? " (lab inline)" : "", ok ? "sent" : "refused");
    current().update(now_ms);
    return ok;
}

Ui::IdResult Ui::identify(uint32_t now_ms) {
    ContentViewport viewport(layout_.content);
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
    ContentViewport viewport(layout_.content);
    pd::set_fake(n);
    if (n == 0) {
        link_.clear_force();
        fusb302::clear_source();   // the driver's list goes too, so the remote state agrees
    }
    K2LOG("[UI] debug fake pdos: %d\n", int(n));
    current().update(now_ms);
}

void Ui::debug_link(bool attached, uint32_t now_ms) {
    ContentViewport viewport(layout_.content);
    if (attached) link_.force_attached(now_ms);
    else link_.force_detached(now_ms);
    if (!attached) {
        fusb302::clear_source();
        // A real detach at the CC level too: every owner of Rd is dropped and Rd stays off for the hold, so the charger
        // sees the sink leave (a display-only override left a stuck Rd in place, hardware 2026-11-06).
        const bool cc = fusb302::cc_detach();
        K2LOG("[UI] debug detach: CC level %d\n", int(cc));
    }
    K2LOG("[UI] debug link: %s\n", attached ? "attach" : "detach");
    current().update(now_ms);
}

// The views an action reports into: the Connection page (status bar) and the saved-cables browser.
void Ui::refresh_action_views(uint32_t now_ms) {
    ContentViewport viewport(layout_.content);   // rotated layouts scale the logical content onto the panel
    if (menu() == Menu::Connection) top_as<ConnectionPage>().update(now_ms);
    else if (menu() == Menu::Emarker) top_as<EmarkerPage>().update(now_ms);
}

bool Ui::open_connection(conn::Tab tab, bool spoof, uint32_t now_ms) {
    if (menu() == Menu::Connection && !spoof && (!single_tools_ || connection_page().tab() == tab)) {   // already there: switch the tab
        ContentViewport viewport(layout_.content);
        ConnectionPage &conn = top_as<ConnectionPage>();
        conn.open_tab(tab);
        conn.update(now_ms);
        return true;
    }
    if (depth_ > 0) set_page(page_, now_ms);   // a single Connection on the Tools page, whatever was open
    ctx_.now = now_ms;
    conn_lock_ = single_tools_ ? static_cast<int8_t>(tab) : -1;   // Charger ID / Cable read / Device ID (K2_WHOS_ATTACHED=0)
    if (!push_menu(Menu::Connection, now_ms)) { conn_lock_ = -1; return false; }
    ConnectionPage &conn = top_as<ConnectionPage>();   // just built: the Connection page is on top
    if (!single_tools_) conn.open_tab(tab);
    if (spoof) return push_menu(Menu::Spoof, now_ms);
    ContentViewport viewport(layout_.content);
    conn.update(now_ms);
    return true;
}

bool Ui::land_rescanned(uint32_t now_ms) {
    if (!open_connection(conn::Tab::Source, false, now_ms)) return false;
    ContentViewport viewport(layout_.content);
    ConnectionPage &conn = top_as<ConnectionPage>();
    conn.say("Rescanned (rebooted)", now_ms);
    conn.update(now_ms);
    return true;
}

bool Ui::goto_page(uint8_t page, uint32_t now_ms) {
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

void Ui::render_for_capture(uint32_t now_ms) {
    ctx_.now = now_ms;
    ContentViewport viewport(layout_.content);
    rotation_notice_drawn_ = false;
    gfx_fill(ctx_.content, 0, 0, ctx_.content.w, ctx_.content.h, kBg);
    current().invalidate();   // the clear erased everything the field caches remember
    ctx_.blank = true;
    current().draw_static();
    draw_panel();
    current().update(now_ms);
    ctx_.blank = false;
    draw_release_notice();
    if (hold_shown_) { hold_shown_ = false; draw_hold_card(hold_ms_); }   // the card is part of the screen
    if (!hold_shown_) draw_rotation_notice(now_ms);
}

void Ui::resync(uint32_t now_ms) {
    ctx_.now = now_ms;
    ContentViewport viewport(layout_.content);
    rotation_notice_drawn_ = false;
    current().invalidate();
    current().update(now_ms);
    draw_release_notice();
    draw_rotation_notice(now_ms);
}

void Ui::refresh(uint32_t now_ms) {
    ctx_.now = now_ms;
    ContentViewport viewport(layout_.content);
    current().update(now_ms);
    draw_release_notice();
    draw_rotation_notice(now_ms);
}

void Ui::repaint(uint32_t now_ms) {
    hold_shown_ = false;
    // An opaque page paints every pixel of the content and the strip every pixel of its slots (the switch into one skips the
    // erase for the same reason, test_ui_capture checks both pixel for pixel): the full-screen clear would only be overdraw.
    if (depth_ == 0 ? page_opaque(page_) : menu_opaque(menu())) {
        rotation_notice_drawn_ = false;
        paint_incoming(now_ms, now_us(), false);
        return;
    }
    lcd::fill(kBg);
    redraw(now_ms);
}

UiAction Ui::tick(uint32_t now_ms) {
    gap::ui.note(now_ms);
    if (requested_rotation_ < 4) {
        const uint8_t target = requested_rotation_;
        requested_rotation_ = 0xFF;
        set_rotation(target, now_ms);
    }
    ContentViewport viewport(layout_.content);
    if (!down_[static_cast<int>(Button::Minus)] && !down_[static_cast<int>(Button::Plus)]) rotation_chord_ = false;
    ctx_.now = now_ms;
    for (int k = 0; k < 4; k++)   // end a press flash that outlived its press
        if (pressed_[k] && !held(k)) update_lit(k, true, now_ms);
    const auto pm = power::read_display();
    ctx_.pm_online = pm.has_value();
    ctx_.pm = pm.value_or(power::Metrics{});
    ctx_.pm_avg = power::read().value_or(power::Metrics{});
    ctx_.vbus_fast_mv = power::vbus_fast_mv().value_or(ctx_.pm.vbus_mv);
    const auto previous_release = fc_release_.state();
    if (TriggerPage *trigger = find<TriggerPage>(Menu::Trigger)) trigger->poll();   // no page, no session: leave() ended it
    if (LoopProbePage *probe = find<LoopProbePage>(Menu::LoopProbe)) probe->poll(now_ms);   // the same: leaving the page ends a run
    const auto fs = fctrig::status().state;
    const auto &sk = fusb302::sink_status();
    const bool released = fs == fc::State::Idle && !fctrig::scan_running() && !fctrig::svooc_running() &&
        !sk.held && !sk.epr_mode &&
        (sk.state == pd::SinkState::Idle || sk.state == pd::SinkState::Failed ||
         (sk.state == pd::SinkState::Ready && sk.mv == 5000));
    fc_release_.poll(now_ms, ctx_.pm_online ? ctx_.vbus_fast_mv : -1, released);
    if (previous_release != fc_release_.state() &&
        (fc_release_.state() == trig::Release::State::Safe || fc_release_.state() == trig::Release::State::Unsafe))
        current().invalidate();
    if (ctx_.pm_online) {
        // energy: the unfiltered sample, so smoothing never changes the Wh total
        const power::Metrics raw = power::read_raw().value_or(ctx_.pm);
        ctx_.meter.update(now_ms, raw.power_mw, raw.current_ma);
        if (ctx_.pm.vbus_mv >= 1000) {
            ctx_.watts.push(now_ms, ctx_.pm.power_mw);
            ctx_.volts.push(now_ms, ctx_.pm.vbus_mv);
            // |current| without negating INT32_MIN (UB); the trace saturates at INT32_MAX.
            ctx_.amps.push(now_ms, static_cast<int32_t>(std::min<uint32_t>(power::magnitude(ctx_.pm.current_ma), INT32_MAX)));
            ctx_.trend.push(now_ms, static_cast<uint32_t>(ctx_.pm.vbus_mv) / 10, power::magnitude(ctx_.pm.current_ma) / 10,
                            power::magnitude(ctx_.pm.power_mw) / 10);
            // Home's PEAK and SAG cards: every raw sample counts (a short spike the display filter rounds away is a peak).
            ctx_.extremes.sample(raw.vbus_mv, raw.current_ma, raw.power_mw, ctx_.sag_level_mv);
        }   // the trace freezes when stale
        if (ctx_.cable_mode == cable_r::Mode::Passive) ctx_.cable.sample(ctx_.pm_avg.vbus_mv, ctx_.pm_avg.current_ma);
    } else {
        ctx_.cable.reset();
    }
    ctx_.analog = adc::read();
    ctx_.plus_key = down_[static_cast<int>(Button::Plus)];   // the physical key (remote presses never set down_): the Waiting stage logs while it is held
    {   // remember the last live reading for the stale state (Home and the LAST pill)
        const bool live = ctx_.pm_online && ctx_.pm.vbus_mv >= 1000;
        ctx_.live.note(now_ms, live);
        if (live) {
            // The LAST snapshot takes a reading only once it has held for a window (home::steady_snapshot): the tail of a
            // session (a released contract, VBUS falling through the display filter) is never what stays on screen.
            const bool attach = !extremes_live_;
            if (attach || now_ms - snap_ms_ >= home::kSnapshotMs) {
                const UiContext::Frozen cur{ctx_.pm, ctx_.analog, ctx_.meter.elapsed_s(now_ms), proto_now()};
                if (attach || home::steady_snapshot(snap_prev_.pm.vbus_mv, cur.pm.vbus_mv, snap_prev_.proto, cur.proto))
                    ctx_.last = cur;   // on attach at once, so even a short session has a snapshot
                snap_prev_ = cur;
                snap_ms_ = now_ms;
            }
            ctx_.last.elapsed_s = ctx_.meter.elapsed_s(now_ms);   // the session's length runs to its last live tick
        }
        if (live && !extremes_live_) {   // an attach: Home's peaks, sag and card cycle start over (they stay frozen while stale)
            ctx_.extremes.reset();
            ctx_.keep.cards.reset();
            ctx_.keep.attention.reset();
            sag_settle_.reset();
        }
        extremes_live_ = live;
        if (live && now_ms - sag_level_ms_ >= 100) {   // the level a sag is measured against (home::sag_level_mv), settled
            sag_level_ms_ = now_ms;
            const fctrig::Status fst = fctrig::status();
            const bool qc3 = fst.state == fctrig::State::Active && (fst.proto == fctrig::Proto::Qc3 || fst.proto == fctrig::Proto::Qc3Plus);
            const uint32_t level = home::sag_level_mv(proto_now(), qc3 && fst.target_mv > 0 ? static_cast<uint32_t>(fst.target_mv) : 0u);
            ctx_.sag_level_mv = sag_settle_.update(now_ms, level);
        } else if (!live) {
            ctx_.sag_level_mv = 0;
        }
    }
    // Kelvin mode: the far-end sense wires of the fixture arrive on CC1, CC2 and D-.
    ctx_.kelvin.reset();
    if (ctx_.cable_mode == cable_r::Mode::Kelvin && ctx_.pm_online && ctx_.analog)
        ctx_.kelvin = cable_r::kelvin(ctx_.analog->cc1_mv, ctx_.analog->cc2_mv, ctx_.analog->dm_mv, ctx_.pm_avg.current_ma);
    ctx_.fc = fastcharge::read();
    ctx_.vooc = fastcharge::vooc_live();
    {   // hard cap for requests and the measured VBUS for the engine's over-voltage abort
        const power::Chip chip = power::chip();
        ctx_.cap_mv = pd::request_cap_mv(power::max_request_mv(chip), power::max_vbus_mv(chip));
        fusb302::sink_set_cap(ctx_.cap_mv);
        if (ctx_.pm_online) fusb302::sink_vbus(ctx_.vbus_fast_mv);
    }

    pd::LinkInput in;
    in.now_ms = now_ms;
    in.vbus_known = ctx_.pm_online;
    in.vbus_mv = ctx_.vbus_fast_mv;
    in.current_known = ctx_.pm_online;
    in.current_ma = ctx_.pm_avg.current_ma;
    in.cc = fusb302::active_cc();
    in.caps_rx = fusb302::rx_count();
    in.requests = fusb302::contract().requests;
    // The PD verdict's inputs (ROADMAP 2026-11-10, Row C): any PD message decoded this attach, and whether the
    // sink side answered anything (a GoodCRC / Request / caps / VDM from the far side this attach).
    in.any_pd_msg = fusb302::pd_seen_attach();
    in.sink_answered = fusb302::over_load_facts().far_pd_message;
    in.far_sink_cc = fusb302::sink_cc_far();   // a sink's Rd on CC with none of ours: "possibly inline" even when the link is silent
    in.self_supply_ma = usb::configured() ? 0 : pd::kSelfSupplyAllowanceMa;   // no USB host: the K2 may run on this VBUS
    // The waiting classifier's input: the passive CC levels and the same passive gate the connection page uses.
    in.cc1_mv = ctx_.analog ? ctx_.analog->cc1_mv : -1;
    in.cc2_mv = ctx_.analog ? ctx_.analog->cc2_mv : -1;
    in.passive = !fusb302::rd_for_run_on() && !fusb302::ra_active() && !fusb302::self_powered() && !link_.inline_mode();
    in.line_held = fusb302::idle_wake_line() != 0 || fusb302::pin_rd() != 0;   // a hold of ours (Wake sleeping port, boot-pinned Rd): not the far ports' own levels
    {   // VBUS this K2 drives itself: the PD trigger (held or mid-negotiation) or a fast-charge trigger
        const pd::SinkStatus &sk = fusb302::sink_status();
        const bool pd_busy = sk.held || (sk.state != pd::SinkState::Idle && sk.state != pd::SinkState::Failed);
        const fctrig::State fs = fctrig::status().state;
        in.own_contract = pd_busy || fs == fctrig::State::Negotiating || fs == fctrig::State::Active;
    }
    if (pd::is_fake()) link_.force_attached(now_ms);   // fake PDOs count as attached
    const bool was_attached = link_.attached();
    link_.update(in);
    if (link_.cc_cross() != cross_seen_) {   // one log line per edge of the crossed-CC verdict (Source tab, bar, Home and the remote state read it)
        cross_seen_ = link_.cc_cross();
        K2LOG("[LINK] cc cross=%s cc1=%u cc2=%u\n", cross_seen_ == raguard::CrossKind::Crossed ? "crossed" : cross_seen_ == raguard::CrossKind::Hint ? "hint" : "none",
              unsigned(in.cc1_mv < 0 ? 0 : in.cc1_mv), unsigned(in.cc2_mv < 0 ? 0 : in.cc2_mv));
    }
    if (link_.pd_verdict() != pd_verdict_seen_) {   // one log line per edge of the "PD present" verdict (Row C; %s is cut at 12 bytes)
        pd_verdict_seen_ = link_.pd_verdict();
        K2LOG("[LINK] pd verdict=%s caps=%u\n", pd::pd_verdict_token(link_.pd_verdict(), link_.pd_none_why()),
              unsigned(link_.caps_rx_this_attach()));
    }
    if (was_attached && !link_.attached()) fusb302::clear_source();   // one attach = one list
    if (link_.take_new_source()) fusb302::new_source();
    if (fusb302::cable_seq() != cable_seq_seen_) {   // the source's own SOP' exchange was decoded
        cable_seq_seen_ = fusb302::cable_seq();
        ctx_.em_scanned = true;
        ctx_.em_listen_until = 0;
        ctx_.em_quiet = false;
        link_.emarker_scanned(now_ms);
    } else if (ctx_.em_listen_until && static_cast<int32_t>(now_ms - ctx_.em_listen_until) >= 0) {
        ctx_.em_listen_until = 0;   // the window is over and the cable never answered
        ctx_.em_quiet = true;
        K2LOG("[UI] e-marker listen window over: nothing heard, replug the charger end\n");
    }
    fusb302::set_inline(link_.inline_mode());
    fusb302::set_wait_kind(link_.wait_kind());   // the alone-Identify decision reads this (pd::identify_alone)
    if (ctx_.analog) fusb302::sink_cc(ctx_.analog->cc1_mv, ctx_.analog->cc2_mv);
    link::set_inline_mode(link_.inline_mode(), link_.inline_cause());
    ctx_.inline_mode = link_.inline_mode();
    charger_test_.poll(now_ms);
    twin_hw::live_poll(twin_ctx(now_ms));   // the twin proof ends itself under the loop lock (its busy input, twin_hw.cpp)
#if K2_PDSTRESS
    if (loopguard::locked() && pdstress_.running()) pdstress_.cancel();   // the HID-cable loop 5 V lock: released to 5 V at this poll
    pdstress_.poll(now_ms);   // after the charger test's poll: that one clears the run-owner flag a run of ours has set
    const bool stress_note = pdstress_.footer() != nullptr || loopguard::locked();   // a footer note: the run's line or the loop lock
#else
    const bool stress_note = loopguard::locked();
#endif
    if (pdstress_note_ && !stress_note) current().invalidate();   // the note goes: the page repaints its own footer
    pdstress_note_ = stress_note;
    if (ctx_.trig_armed && now_ms >= trig_until_) ctx_.trig_armed = false;

    const RebootChord::Step chord = chord_.step(now_ms);
    update_hold_card(now_ms, chord);
    if (chord == RebootChord::Step::Reboot) return UiAction::Reboot;
    if (!hold_shown_ && now_ms - last_update_ms_ >= kPageTickMs) {
        last_update_ms_ = now_ms;
        uint32_t t0 = now_us();
        current().sample();
        current().update(now_ms);
        draw_release_notice();
        if (rotation_notice_until_ && static_cast<int32_t>(now_ms - rotation_notice_until_) >= 0) {
            rotation_notice_until_ = 0;
            current().invalidate();
            current().update(now_ms);
        } else draw_rotation_notice(now_ms);
        sync_panel();
        uint32_t dt = now_us() - t0;
        if (dt > field_max_us_) field_max_us_ = dt;
    }
    if (timing_pending_) log_timing(now_ms);
    return UiAction::None;
}

void Ui::draw_release_notice() {
    if (fc_release_.busy() && menu() != Menu::Trigger) {
        const bool unsafe = fc_release_.state() == trig::Release::State::Unsafe;
        draw_footer(ctx_.content, unsafe ? FooterTone::Error : FooterTone::Caution,
                    unsafe ? "Unsafe VBUS: unplug source" : "FC releasing; verify 5V");
    } else if (loopguard::locked() && menu() != Menu::Trigger) {
        draw_footer(ctx_.content, FooterTone::Caution, loopguard::kLockText);   // on whatever page is up, while the lock holds
    }
#if K2_PDSTRESS
    else if (const char *line = pdstress_.footer(); line && menu() != Menu::Trigger && menu() != Menu::PdStress) {
        draw_footer(ctx_.content, FooterTone::Info, line);   // the run's one line, on whatever page is up (its own page has its footer)
    }
#endif
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
