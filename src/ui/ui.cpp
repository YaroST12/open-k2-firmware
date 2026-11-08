// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#include "k2/ui/ui.hpp"
#include "k2/core/gap_meter.hpp"
#include "k2/core/log.hpp"
#include "k2/fastcharge/fctrig.hpp"
#include "k2/drivers/fusb302.hpp"
#include "k2/drivers/lcd.hpp"
#include "k2/pd/link.hpp"
#include "k2/pd/pd_emarker_plan.hpp"
#include "k2/pd/pd_source.hpp"
#include "k2/drivers/power_calc.hpp"
#include "k2/drivers/systick.hpp"
#include "k2/drivers/usb.hpp"
#include "k2/drivers/buttons.hpp"
#include "k2/ui/ui_calc.hpp"
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

Ui::Ui() : charger_test_(ctx_) {
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
    case Menu::Connection: return pages_.push<ConnectionPage>(ctx_);
    case Menu::Diag: return pages_.push<DiagPage>(ctx_);
    case Menu::CableR: return pages_.push<CableRPage>(ctx_);
    case Menu::BlackBox: return pages_.push<BlackBoxPage>(ctx_);
    case Menu::Spoof: return pages_.push<SpoofPage>(ctx_);
    case Menu::Cards: return pages_.push<CardsPage>(ctx_);
    case Menu::Share: return pages_.push<SharePage>(ctx_, share_rec_);
    case Menu::UsbLink: return pages_.push<UsbLinkPage>(ctx_);
    case Menu::Autorotate: return pages_.push<AutorotatePage>(ctx_);
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
    erase_outgoing(now_ms, *outgoing);     // the outgoing pages are still alive for their erase replay
    pages_.pop_to(changed ? 0 : 1);        // the menus go; so does the rotation page unless it is the one asked for
    if (changed) build_base(p);
    if (p == Page::ChargerTest) pages_.at(0)->enter();
    paint_incoming(now_ms, t0);
}

// The hold that START's own PDO 1 request puts on the sink (Rd presented for a run) is not a foreign tool.
// What keeps an identity (or a START) out, said as a footer line (at most 24 characters) that names the cause and the way out;
// nullptr = nothing does. An identity armed in Tools -> Spoof is not one of them: START with "Off" leaves it armed, and a
// START with an identity of its own replaces it.
const char *Ui::busy_reason() const {
    const auto& sink = fusb302::sink_status();
    const auto fs = fctrig::status();
    if (charger_test_busy()) return "Test running: stop it";
    if (fc_release_.busy()) return "5V release: wait";
    if (fctrig::scan_running() || fs.state == fc::State::Negotiating || fs.state == fc::State::Active) return "QC trigger: stop it";
    if (sink.held && !fusb302::rd_for_run_on()) return "PD hold: release it";
    if (sink.epr_mode) return "EPR contract: exit it";
    if (sink.state != pd::SinkState::Idle && sink.state != pd::SinkState::Ready && sink.state != pd::SinkState::Failed)
        return "PD request: wait";
    if (ctx_.em_scanning) return "E-marker scan: wait";
    if (fusb302::identify_status().state == pd::IdState::Running) return "Identify: wait";
    return nullptr;
}

void Ui::redraw(uint32_t now_ms) {
    rotation_notice_drawn_ = false;
    paint_incoming(now_ms, now_us());
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

void Ui::paint_incoming(uint32_t now_ms, uint32_t t0) {
    ContentViewport viewport(layout_.content);
    current().invalidate();   // the erase pass erased everything the field caches remember; a built page has drawn nothing
    current().sample();
    current().draw_static();
    draw_panel();
    current().update(now_ms);
    draw_release_notice();
    draw_rotation_notice(now_ms);
    last_update_ms_ = now_ms;
    full_us_ = now_us() - t0;
    field_max_us_ = 0;
    page_since_ms_ = now_ms;
    timing_pending_ = true;
}

bool Ui::push_menu(Menu m, uint32_t now_ms) {
    if (ports_busy()) {
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
    erase_outgoing(now_ms, *outgoing);
    paint_incoming(now_ms, t0);
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
    erase_outgoing(now_ms, *outgoing);
    pages_.pop();                                  // the page below is the one that was kept
    paint_incoming(now_ms, t0);
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
    const uint16_t bg = down ? kPanelHi : kPanelBg;
    const uint16_t fg = down ? kBg : lab.dim ? kDim : kText;
    const Rect &r = layout_.slot[slot];
    if (r.w <= 0 || r.h <= 0) return;

    Surface s = surface_of(r);
    gfx_fill(s, 0, 0, s.w, s.h, bg);

    // The label sits in the slot's text zone (divider and padding excluded) and is centred in it.
    char text[24];
    const bool column = panel_is_column(orient_.edge);
    const int along = column ? s.h : s.w;       // slot length along the panel
    const LabelZone zone = label_zone(along, slot);
    const unsigned chars = label_text(text, lab, zone.max_chars);
    if (chars > 0) {
        const int16_t a0 = static_cast<int16_t>(label_origin(zone, chars));
        if (column)   // glyphs rotated 90 degrees, running along the slot
            gfx_text_rot(s, kLabelFont, static_cast<int16_t>((s.w - kLabelFont.height) / 2), a0, text, fg, bg,
                         panel_text_dir(orient_.edge));
        else
            gfx_text(s, kLabelFont, a0, static_cast<int16_t>((s.h - kLabelFont.height) / 2), text, fg, bg);
    }
    if (slot > 0) {   // divider on the slot's leading edge, between this slot and the previous one
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

void Ui::set_pressed(Button b, bool down, uint32_t now_ms) {
    const int k = static_cast<int>(b);
    const bool was_down = down_[k];
    down_[k] = down;
    bool lit = pressed_[k];
    if (down && !was_down) { lit = true; lit_since_[k] = now_ms; suppress_rotated_press_[k] = false; }
    else if (!down && lit && now_ms - lit_since_[k] >= kPressFlashMs) lit = false;
    if (lit == pressed_[k]) return;
    pressed_[k] = lit;
    for (unsigned i = 0; i < 4; i++)
        if (slot_button(orient_, i) == b) draw_slot(i);
}

// Reboot HoldCard (design 6.5, screens.md home_reboot_hold): shown from 1 s of BACK held, counts down to
// the 5 s reboot, and a release before that removes it and repaints the page. Pixels in content coordinates.
void Ui::update_hold_card(uint32_t now_ms) {
    const bool held = down_[static_cast<int>(Button::Back)];
    const uint32_t ms = now_ms - lit_since_[static_cast<int>(Button::Back)];
    if (!held || ms < kHoldCardMs) {
        if (hold_shown_) {   // cancelled: the erase pass replays the card while hold_shown_ is still set
            const uint32_t t0 = now_us();
            erase_outgoing(now_ms, current());
            paint_incoming(now_ms, t0);
            hold_shown_ = false;
            hold_px_ = hold_left_ = -1;
        }
        return;
    }
    draw_hold_card(ms);
}

void Ui::draw_hold_card(uint32_t ms) {
    const Surface &c = ctx_.content;
    const uint32_t total = ButtonDetector::kHoldMs;
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
        gfx_text(c, kLabelFont, 26, 132, "Release Back to cancel", kDim, kSurface2);
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
    if (suppress_rotated_press_[static_cast<int>(b)]) return UiAction::None;
    Event mapped = *event;
    mapped.button = oriented_button(b, orient_.rotation, rotate_buttons_);
    const UiAction action = dispatch_button(mapped, now_ms);
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
        return m.global == Global::Reboot ? UiAction::Reboot : UiAction::None;
    }
    const Dispatch d = dispatch(event, pg, static_cast<unsigned>(Page::Count), current());
    switch (d.global) {
    case Global::PagePrev: set_page(static_cast<Page>(pg - 1), now_ms); break;
    case Global::PageNext: set_page(static_cast<Page>(pg + 1), now_ms); break;
    case Global::Reboot:   return UiAction::Reboot;
    case Global::None:     break;
    }
    if (d.global == Global::None) sync_panel();
    return UiAction::None;
}

bool Ui::run_action(Action a, uint32_t now_ms, bool confirm) {
    ContentViewport viewport(layout_.content);
    if (ports_busy() && a != Action::FcReset) {
        K2LOG("[TEST] action refused: charger test owns ports\n");
        return false;
    }
    switch (a) {
    case Action::PdRescan:
        pd::set_fake(0);
        link_.clear_force();
        fusb302::rescan();   // passive inline (the driver knows), Rd/CC cycle standalone
        if (menu() == Menu::Connection) top_as<ConnectionPage>().update(now_ms);
        break;
    case Action::EmarkerScan: {
        // The method follows the link: nothing supplies VBUS = inject VCONN and ask the cable; a source
        // present = listen only (it is the VCONN source and asks the cable at attach). Only the active
        // one can disturb a live sink, so only it needs the inline confirm.
        const pd::EmarkerMode mode = pd::emarker_mode(link_.attached(), ctx_.pm_online, ctx_.vbus_fast_mv);
        K2LOG("[EMK] mode=%s attached=%d inline=%d stale=%d pm_online=%d vbus=%dmV cable_seq=%u seen=%u\n",
                    mode == pd::EmarkerMode::Sniff ? "sniff" : "vconn", int(link_.attached()), int(link_.inline_mode()),
                    int(link_.cable_stale()), int(ctx_.pm_online), int(ctx_.vbus_fast_mv),
                    unsigned(fusb302::cable_seq()), unsigned(cable_seq_seen_));
        if (mode == pd::EmarkerMode::Vconn && link_.inline_mode() && !confirm) {
            const bool armed = ctx_.inline_armed && confirm_action_ == a && now_ms < confirm_until_;
            if (!armed) {
                ctx_.inline_armed = true;
                confirm_action_ = a;
                confirm_until_ = now_ms + kConfirmWindowMs;
                K2LOG("[UI] e-marker scan inline: charging may be interrupted, confirm to continue\n");
                refresh_action_views(now_ms);
                return false;
            }
        }
        ctx_.inline_armed = false;
        ctx_.em_scanning = true;
        ctx_.em_listen_until = 0;
        ctx_.em_quiet = false;
        refresh_action_views(now_ms);
        bool found = fusb302::read_emarker(link_.inline_mode(), mode);
        // A sniff only reports what the engine still holds. After a cable swap that is the OLD cable's
        // data: it is not a scan, so it must not clear the stale flag (a new cable_seq does, in tick()).
        if (found && mode == pd::EmarkerMode::Sniff && link_.cable_stale() && fusb302::cable_seq() == cable_seq_seen_) {
            K2LOG("[UI] e-marker sniff: held data belongs to the previous cable, waiting for the new one\n");
            found = false;
        }
        ctx_.em_scanning = false;
        if (found || mode == pd::EmarkerMode::Vconn) {   // a listen that heard nothing is not a scan
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
        if (link_.inline_mode() && !confirm) {
            const bool armed = ctx_.inline_armed && confirm_action_ == a && now_ms < confirm_until_;
            if (!armed) {
                ctx_.inline_armed = true;
                confirm_action_ = a;
                confirm_until_ = now_ms + kConfirmWindowMs;
                K2LOG("[UI] identify inline: it talks to a live sink, confirm to continue\n");
                refresh_action_views(now_ms);
                return false;
            }
        }
        ctx_.inline_armed = false;
        {
            const IdResult r = identify(now_ms, true);
            refresh_action_views(now_ms);
            return r == IdResult::Started;
        }
    case Action::EnergyReset:
        ctx_.meter.reset(now_ms);
        ctx_.watts.clear();
        ctx_.volts.clear();
        ctx_.amps.clear();
        ctx_.trend.clear();
        ctx_.live.clear();   // OK on Home also drops the frozen LAST state
        K2LOG("[UI] energy meter reset\n");
        current().update(now_ms);
        break;
    case Action::FcScan:     // the remote path checks --confirm before it gets here
        fctrig::scan_start(true);
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

Ui::TriggerOutcome Ui::trigger(const pd::Choice &c, uint32_t now_ms, bool confirm) {
    ContentViewport viewport(layout_.content);
    if (ports_busy()) {
        K2LOG("[TEST] PD request refused: test running\n");
        return {pd::Err::Busy, false};
    }
    if (pd::is_fake()) return {pd::Err::NoCaps, false};
    if (fusb302::trigger_refusal() != pd::TriggerRefusal::None) {   // before any confirm prompt: no TX ever
        const pd::Err e = fusb302::sink_request(c, confirm);     // logs [PD] trigger refused: inline (reason)
        ctx_.trig_armed = false;
        return {e, false};
    }
    if (trigger_needs_confirm(pd::active_caps(), c, ctx_.cap_mv) && !confirm) {
        K2LOG("[UI] trigger PDO%u needs confirm\n", c.pos);
        return {pd::Err::Ok, true};
    }
    const pd::Err e = fusb302::sink_request(c, confirm);
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
    fusb302::sink_release();
    ctx_.trig_armed = false;
    K2LOG("[UI] trigger release to 5V\n");
    current().update(now_ms);
}

void Ui::stop_fc(uint32_t now_ms) {
    fctrig::stop();
    fc_release_.start(now_ms);
}

void Ui::trigger_hard_reset(uint32_t now_ms) {
    ContentViewport viewport(layout_.content);
    cancel_charger_test();
    fusb302::sink_hard_reset();
    ctx_.trig_armed = false;
    K2LOG("[UI] trigger hard reset\n");
    current().update(now_ms);
}

Ui::IdResult Ui::identify(uint32_t now_ms, bool confirm) {
    ContentViewport viewport(layout_.content);
    if (ports_busy()) {
        K2LOG("[TEST] Identify refused: test running\n");
        return IdResult::Refused;
    }
    if (link_.inline_mode() && !confirm) return IdResult::NeedConfirm;
    ctx_.id_no_device = ctx_.no_device();
    if (ctx_.id_no_device) {   // nothing on CC and no VBUS: transmitting would only burn the retries
        K2LOG("[UI] identify refused: no device (no Rp on CC, VBUS low)\n");
        refresh_action_views(now_ms);
        return IdResult::NoDevice;
    }
    const bool ok = fusb302::identify_start(confirm);
    K2LOG("[UI] identify %s\n", ok ? "started" : "refused");
    refresh_action_views(now_ms);
    return ok ? IdResult::Started : IdResult::Refused;
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
    if (menu() == Menu::Connection && !spoof) {   // already there: just switch the tab
        ContentViewport viewport(layout_.content);
        ConnectionPage &conn = top_as<ConnectionPage>();
        conn.open_tab(tab);
        conn.update(now_ms);
        return true;
    }
    if (depth_ > 0) set_page(page_, now_ms);   // a single Connection on the Tools page, whatever was open
    ctx_.now = now_ms;
    if (!push_menu(Menu::Connection, now_ms)) return false;
    ConnectionPage &conn = top_as<ConnectionPage>();   // just built: the Connection page is on top
    conn.open_tab(tab);
    if (spoof) return push_menu(Menu::Spoof, now_ms);
    ContentViewport viewport(layout_.content);
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
        if (m == Menu::Share) return false;   // needs a record; only open_share makes one
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
    current().draw_static();
    draw_panel();
    current().update(now_ms);
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
    lcd::fill(kBg);
    hold_shown_ = false;
    redraw(now_ms);
}

void Ui::tick(uint32_t now_ms) {
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
        if (pressed_[k] && !down_[k]) set_pressed(static_cast<Button>(k), false, now_ms);
    const auto pm = power::read_display();
    ctx_.pm_online = pm.has_value();
    ctx_.pm = pm.value_or(power::Metrics{});
    ctx_.pm_avg = power::read().value_or(power::Metrics{});
    ctx_.vbus_fast_mv = power::vbus_fast_mv().value_or(ctx_.pm.vbus_mv);
    const auto previous_release = fc_release_.state();
    if (TriggerPage *trigger = find<TriggerPage>(Menu::Trigger)) trigger->poll();   // no page, no session: leave() ended it
    const auto fs = fctrig::status().state;
    const auto &sk = fusb302::sink_status();
    const bool released = fs == fc::State::Idle && !fctrig::scan_running() &&
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
        }   // the trace freezes when stale
        if (ctx_.cable_mode == cable_r::Mode::Passive) ctx_.cable.sample(ctx_.pm_avg.vbus_mv, ctx_.pm_avg.current_ma);
    } else {
        ctx_.cable.reset();
    }
    ctx_.analog = adc::read();
    {   // remember the last live reading for the stale state (Home and the LAST pill)
        const bool live = ctx_.pm_online && ctx_.pm.vbus_mv >= 1000;
        ctx_.live.note(now_ms, live);
        if (live) {
            ctx_.last.pm = ctx_.pm;
            ctx_.last.analog = ctx_.analog;
            ctx_.last.elapsed_s = ctx_.meter.elapsed_s(now_ms);
        }
    }
    // Kelvin mode: the far-end sense wires of the fixture arrive on CC1, CC2 and D-.
    ctx_.kelvin.reset();
    if (ctx_.cable_mode == cable_r::Mode::Kelvin && ctx_.pm_online && ctx_.analog)
        ctx_.kelvin = cable_r::kelvin(ctx_.analog->cc1_mv, ctx_.analog->cc2_mv, ctx_.analog->dm_mv, ctx_.pm_avg.current_ma);
    ctx_.fc = fastcharge::read();
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
    in.self_supply_ma = usb::configured() ? 0 : pd::kSelfSupplyAllowanceMa;   // no USB host: the K2 may run on this VBUS
    {   // VBUS this K2 drives itself: the PD trigger (held or mid-negotiation) or a fast-charge trigger
        const pd::SinkStatus &sk = fusb302::sink_status();
        const bool pd_busy = sk.held || (sk.state != pd::SinkState::Idle && sk.state != pd::SinkState::Failed);
        const fctrig::State fs = fctrig::status().state;
        in.own_contract = pd_busy || fs == fctrig::State::Negotiating || fs == fctrig::State::Active;
    }
    if (pd::is_fake()) link_.force_attached(now_ms);   // fake PDOs count as attached
    const bool was_attached = link_.attached();
    link_.update(in);
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
    if (ctx_.analog) fusb302::sink_cc(ctx_.analog->cc1_mv, ctx_.analog->cc2_mv);
    link::set_inline_mode(link_.inline_mode());
    ctx_.inline_mode = link_.inline_mode();
    charger_test_.poll(now_ms);
    if (ctx_.inline_armed && now_ms >= confirm_until_) ctx_.inline_armed = false;
    if (ctx_.trig_armed && now_ms >= trig_until_) ctx_.trig_armed = false;

    update_hold_card(now_ms);
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
}

void Ui::draw_release_notice() {
    if (fc_release_.busy() && menu() != Menu::Trigger) {
        const bool unsafe = fc_release_.state() == trig::Release::State::Unsafe;
        draw_footer(ctx_.content, unsafe ? FooterTone::Error : FooterTone::Caution,
                    unsafe ? "Unsafe VBUS: unplug source" : "FC releasing; verify 5V");
    }
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
