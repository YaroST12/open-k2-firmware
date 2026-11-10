// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Screens and the button-label panel. Hardware-facing: draws through gfx and reads the
// FUSB302 / power monitor drivers; the logic it relies on lives in the pure helpers.
// The pages themselves are in ui_pages.hpp; this class owns them, the panel and the routing.
#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include "k2/drivers/buttons.hpp"
#include "k2/ui/gfx.hpp"
#include "k2/ui/ui_calc.hpp"
#include "k2/ui/ui_events.hpp"
#include "k2/ui/ui_labels.hpp"
#include "k2/ui/ui_layout.hpp"
#include "k2/ui/ui_blackbox.hpp"
#include "k2/ui/ui_spoof.hpp"
#include "k2/ui/ui_cards.hpp"
#include "k2/ui/ui_connection.hpp"
#include "k2/ui/ui_share.hpp"
#include "k2/ui/ui_pages.hpp"
#include "k2/ui/ui_types.hpp"
#include "k2/ui/page_arena.hpp"

#include "k2/pd/pd_link.hpp"
#include "k2/drivers/power_monitor.hpp"
#include "k2/ui/orientation.hpp"

namespace k2 {

// Page update period: Ui::tick() runs the current page's sample()+update() at most this often.
// Ships at 100 ms (10 Hz); test_ui_perf prints the px-per-tick cost of every page so a shorter
// period (50 ms / 33 ms) can be picked from device timings.
inline constexpr uint32_t kPageTickMs = 100;

// The page arena: the rotation page on screen and the menus opened above it are built in one buffer (index 0 = the rotation
// page, index n = the menu at stack position n - 1) and exist only while they are shown. The buffer holds every base page with
// the largest menu on it, and the longest menu chains the UI opens. A chain that does not fit is refused by the arena (the
// menu is not opened), never overrun; these asserts keep that safety net unreachable.
inline constexpr std::size_t kPageArenaDepth = 4;   // a rotation page and up to three menus
inline constexpr std::size_t kLargestBase = std::max({arena_bytes<HomePage>(), arena_bytes<DetailPage>(), arena_bytes<ChargerTestPage>(),
                                                      arena_bytes<LinesPage>(), arena_bytes<ToolsPage>(), arena_bytes<SettingsPage>()});
inline constexpr std::size_t kLargestMenu = std::max({arena_bytes<TriggerPage>(), arena_bytes<EmarkerPage>(), arena_bytes<PdTriggerPage>(),
                                                      arena_bytes<CableRPage>(), arena_bytes<BlackBoxPage>(), arena_bytes<SpoofPage>(),
                                                      arena_bytes<UsbLinkPage>(), arena_bytes<AutorotatePage>(), arena_bytes<CardsPage>(),
                                                      arena_bytes<SharePage>(), arena_bytes<ConnectionPage>(), arena_bytes<DiagPage>()});
// Tools > Connection > Saved cables > Share, and the other three-menu chains (Connection > Spoof, Cards / Black box > Share).
inline constexpr std::size_t kLongestChain =
    std::max({arena_bytes<ToolsPage, ConnectionPage, EmarkerPage, SharePage>(), arena_bytes<ToolsPage, ConnectionPage, SpoofPage>(),
              arena_bytes<ToolsPage, CardsPage, SharePage>(), arena_bytes<ToolsPage, BlackBoxPage, SharePage>(),
              arena_bytes<ChargerTestPage, SharePage>()});
inline constexpr std::size_t kPageArenaBytes = std::max(kLargestBase + kLargestMenu, kLongestChain);
static_assert(kPageArenaBytes >= kLargestBase + kLargestMenu, "any rotation page takes any one menu");
static_assert(kPageArenaBytes >= arena_bytes<ToolsPage, ConnectionPage, EmarkerPage, SharePage>(), "the deepest menu chain fits");
static_assert(kPageArenaBytes >= arena_bytes<ChargerTestPage, SharePage>(), "the charger test shares its card");
// The QR scan-test page carries its encode workspace as a member, so it is opened only over a base page small
// enough for both (qrtest::show lands on Tools first when the current base is too big); it never takes a menu on top.
static_assert(kPageArenaBytes >= arena_bytes<ToolsPage, QrTestPage>(), "the qrtest page with its workspace fits over the Tools base");

class Ui {
public:
    Ui();
    // `cancel_resume` = BACK was held at power-up: an interrupted charger test is dropped, not resumed.
    void init(uint32_t now_ms, bool cancel_resume = false);
    void set_rotation(uint8_t rotation, uint32_t now_ms);
    uint8_t rotation() const { return orient_.rotation; }
    bool rotation_locked() const { return rotation_lock_; }
    bool rotate_buttons() const { return rotate_buttons_; }
    void set_rotate_buttons(bool enabled);
    // Applied after event rendering so a changed viewport never mixes orientations in one draw.
    void configure_rotation(bool automatic, uint8_t rotation);
    // No sensor is guessed: a future verified driver supplies screen-axis gravity in mg.
    void feed_gravity(std::optional<orientation::Gravity> gravity, uint32_t now_ms);
    // Called every main-loop pass (~20 ms): samples power, integrates energy, and every
    // ~100 ms updates the changed fields of the current page.
    void tick(uint32_t now_ms);
    // Debounced button event (physical or injected): the global handler first, then the page.
    UiAction on_button(Button b, ButtonEvent ev, uint32_t now_ms);
    // Runs a page action directly, whichever page is showing; only the fields of the current
    // page are redrawn. Blocks for the e-marker scan, like the OK button does.
    // Returns false when the action was refused: inline (a live sink is attached) the e-marker
    // scan may interrupt charging, so it needs `confirm` (remote --confirm) or a second OK within
    // kConfirmWindowMs on the device. Rescan is passive inline and never needs one.
    bool run_action(Action a, uint32_t now_ms, bool confirm = false);
    static constexpr uint32_t kConfirmWindowMs = 5000;
    static constexpr uint32_t kListenWindowMs = 5000;   // passive e-marker listen after a quiet sniff

    // Active PD (trigger). Every path refuses inline (a live sink is attached) and without caps.
    // A request above 5 V needs a confirm: `confirm` from the remote (--confirm, the WebHID dialog)
    // or, on the device, a second OK within kConfirmWindowMs (device_trigger). The hard cap is
    // the power monitor's limit, enforced again inside the driver.
    struct TriggerOutcome { pd::Err err; bool need_confirm; };
    // `over_load` is the lab-only remote bypass (pd request --over-load); the UI paths never pass it.
    TriggerOutcome trigger(const pd::Choice &c, uint32_t now_ms, bool confirm, bool over_load = false);
    TriggerOutcome device_trigger(const pd::Choice &c, uint32_t now_ms);   // OK twice
    void trigger_release(uint32_t now_ms);       // back to 5 V
    bool trigger_hard_reset(uint32_t now_ms, bool lab_inline = false);   // false = the driver refused it
    // Identify the port partner. Inline it is refused (an inline K2 never transmits); the sniffed
    // identity shows on the Sink tab instead.
    enum class IdResult : uint8_t { Started, Refused, NoDevice };
    IdResult identify(uint32_t now_ms);
    // Remote `debug detach` / `debug attach`: hold the link state until the next Rescan.
    void debug_link(bool attached, uint32_t now_ms);
    void debug_fake_pdos(uint8_t n, uint32_t now_ms);   // n PDOs injected; 0 clears them, the forced link and the live list
    const pd::Link &link() const { return link_; }
    // Jump to a page by index: a rotation page, kMenuPageBase + Menu (a submenu), or kConnTabAlias + tab
    // (Connection on that tab). Any open submenu is closed first; false when out of range or refused.
    bool goto_page(uint8_t page, uint32_t now_ms);
    // Opens Connection (Tools) on `tab`; `spoof` goes straight on to the Spoof list (the Tools shortcut).
    bool open_connection(conn::Tab tab, bool spoof, uint32_t now_ms);
    // The comeback of a bridged Refresh (core/rescan.cpp): Connection on its Source tab, saying "Rescanned (rebooted)".
    bool land_rescanned(uint32_t now_ms);
    // Submenus: a small stack (depth 3: Tools > Connection > Saved cables > Share). While one is open it gets every event,
    // the global paging is off and a short BACK pops it. push_menu fails when the stack is full.
    bool push_menu(Menu m, uint32_t now_ms);
    bool pop_menu(uint32_t now_ms);
    unsigned menu_depth() const { return depth_; }
    // Share action (long OK on the cable, the source caps, a report card, the charger test results): the opener builds its
    // record into share_buffer() (k2/ui/qrshare_records.hpp), then open_share() names it and pushes the share view. False
    // (nothing pushed) when the record has no part or push_menu refuses.
    std::span<std::uint8_t> share_buffer() { return share_rec_.buffer(); }
    bool open_share(qrshare::Type type, std::uint8_t schema, std::string_view title, std::size_t len, uint32_t now_ms);
    // The open Share view (host tests); it exists only while Menu::Share is on the stack.
    const SharePage &share() const { return *find<SharePage>(Menu::Share); }
    // The open QR scan-test page (host tests); it exists only while Menu::QrTest is on the stack.
    const QrTestPage &qrtest_page() const { return *find<QrTestPage>(Menu::QrTest); }
    // Top submenu; Menu::Count when none is open.
    Menu menu() const { return depth_ ? stack_[depth_ - 1] : Menu::Count; }
    // Page index for the remote: the rotation page, or kMenuPageBase + menu while one is open.
    uint8_t page_index() const { return depth_ ? static_cast<uint8_t>(kMenuPageBase + static_cast<uint8_t>(menu())) : static_cast<uint8_t>(page_); }
    // Panel highlight. A press stays lit for at least kPressFlashMs even if released sooner;
    // tick() clears it. A held button (long-press slot) stays lit until release.
    static constexpr uint32_t kPressFlashMs = 120;
    void set_pressed(Button b, bool down, uint32_t now_ms = 0);
    bool hold_card_visible() const { return hold_shown_; }   // BACK held >= 1 s: "Reboot in N"
    bool lit(Button b) const { return pressed_[static_cast<int>(b)]; }
    void show_rebooting();
    // Screenshot support. render_for_capture() redraws the whole screen (current page and panel)
    // through the lcd sink, so the caller points the sink at a strip first. It never touches the
    // panel. resync() then repaints the real LCD fields from the current data, because the
    // capture pass left their "already drawn" caches out of step with what the panel shows.
    // sample() takes the page's live counters once; call it before a capture so every strip draws
    // the same numbers (tick() and page changes sample by themselves).
    void sample() { current().sample(); }
    void render_for_capture(uint32_t now_ms);
    void resync(uint32_t now_ms);
    // Paints the current page now from its own change signatures, without invalidating them: a page
    // whose input just changed state shows it at once and rewrites only what differs (no flicker).
    void refresh(uint32_t now_ms);
    // Clears the panel and draws the current page from scratch: for the caller that drew over the whole screen itself
    // (the boot-info QR screen) and now hands it back.
    void repaint(uint32_t now_ms);
    Page page() const { return page_; }
    int32_t energy_mwh() const { return ctx_.meter.mwh(); }
    const UiContext &context() const { return ctx_; }   // read-only view for tests and diagnostics
    UiContext &context() { return ctx_; }               // the remote sets the pending qrtest request through this one
    // Passive cable-resistance meter (fed by tick); the remote reads and clears it.
    const cable_r::Meter &cable() const { return ctx_.cable; }
    void cable_reset() { ctx_.cable.reset(); ctx_.kelvin.reset(); }
    cable_r::Mode cable_mode() const { return ctx_.cable_mode; }
    void set_cable_mode(cable_r::Mode m) { ctx_.cable_mode = m; cable_reset(); }
    const std::optional<cable_r::Kelvin> &kelvin() const { return ctx_.kelvin; }
    // What the four buttons do on the current page (Button order).
    PanelLabels labels() { return current().labels(); }
    bool charger_test_busy() const { return charger_test_.busy(); }
    const charger_test::Runner& charger_test_results() const { return charger_test_.results(); }
    uint32_t charger_test_selection() const { return charger_test_.selection_mask(); }
    uint8_t charger_test_cursor() const { return static_cast<uint8_t>(charger_test_.cursor()); }
    uint8_t charger_test_stage() const { return static_cast<uint8_t>(charger_test_.stage()); }
    bool charger_test_set_units(std::span<const uint8_t> units) { return charger_test_.set_units(units); }
    bool charger_test_set_cable(ctcable::Choice c) { return charger_test_.set_cable_choice(c); }
    ctcable::Info charger_test_cable_info() const { return charger_test_.cable_info(); }
    bool charger_test_reattach_planned() const { return charger_test_.reattach_planned(); }   // the run does the planned re-attach
    uint8_t charger_test_reattach_state() const { return charger_test_.reattach_state(); }   // 0 none, 1 Rd off, 2 awaiting the contract
    bool charger_test_confirm_reattach() const { return charger_test_.confirm_reattach(); }   // the confirmation announces the blink
    uint8_t charger_test_run_guard() const { return charger_test_.run_guard(); }    // the guard byte the run records
    bool ports_busy() const { return charger_test_busy() || fc_release_.busy(); }
    const char *busy_reason() const;   // why an identity / START is refused right now, nullptr = free
    bool spoof_busy() const { return busy_reason() != nullptr; }
    const trig::Release &fc_release() const { return fc_release_; }
    void stop_fc(uint32_t now_ms);
    bool boot_restore_charger_test(bool cancel = false) { return charger_test_.boot_restore(cancel); }
    void cancel_charger_test() { charger_test_.cancel(); }
    // Scroll state of the current page's list (count 0 / visible 1 on pages without one).
    ListState list_state();
    // Read-only view of the Connection page (tab, pop-up rows, facts) for the host tests.
    // It exists only while Menu::Connection is on the stack.
    const ConnectionPage &connection_page() const { return *find<ConnectionPage>(Menu::Connection); }
    // The page arena (host tests): how many pages are alive and how many bytes of the arena they use.
    std::size_t live_pages() const { return pages_.depth(); }
    std::size_t page_bytes() const { return pages_.used(); }

private:
    UiAction dispatch_button(Event event, uint32_t now_ms);
    PageIf &current() { return *pages_.at(depth_); }
    // The page of `m` when it is on the menu stack, else nullptr. Menu stack_[i] lives at arena index i + 1.
    template <class T>
    T *find(Menu m) const {
        for (unsigned i = 0; i < depth_; i++)
            if (stack_[i] == m) return static_cast<T *>(pages_.at(i + 1));
        return nullptr;
    }
    template <class T>
    T &top_as() { return static_cast<T &>(current()); }
    PageIf *build_base(Page p);   // builds the rotation page on an empty arena
    PageIf *build_menu(Menu m);   // builds a menu above the pages that are alive; nullptr when it does not fit
    void set_page(Page p, uint32_t now_ms);
    // A page / menu switch is two passes around the arena change: erase_outgoing() wipes the page that was on screen by
    // replaying its own paint in lcd erase mode (only its ink is rewritten, not the whole screen), while it is still alive;
    // paint_incoming() then draws the current page from scratch, over a fresh arena. `t0` starts the [UI] timing.
    void erase_outgoing(uint32_t now_ms, PageIf &outgoing);
    void paint_incoming(uint32_t now_ms, uint32_t t0);
    // Full repaint of the current page after a real lcd::fill (init, rotation change), where the screen is already blank.
    void redraw(uint32_t now_ms);
    void draw_panel();
    void draw_release_notice();
    void sync_panel();   // redraws the slots whose label changed since they were drawn
    uint32_t slot_sig(unsigned slot);
    uint32_t slot_sig_[4] = {};
    void draw_slot(unsigned slot);
    void log_timing(uint32_t now_ms);

    Orientation orient_ = kDefaultOrientation;
    orientation::Tracker gravity_;
    bool rotation_lock_ = false;
    bool rotation_chord_ = false;
    bool rotate_buttons_ = false;
    bool suppress_rotated_press_[4] = {};
    uint8_t rotation_steps_ = 0;
    uint8_t requested_rotation_ = 0xFF;
    uint32_t rotation_notice_until_ = 0;
    bool rotation_notice_drawn_ = false;
    void rotation_gesture(uint32_t now_ms);
    void draw_rotation_notice(uint32_t now_ms);
    Layout layout_{};
    Surface panel_{};
    Page page_ = Page::Home;
    Menu stack_[3] = {Menu::Count, Menu::Count, Menu::Count};
    void refresh_action_views(uint32_t now_ms);
    unsigned depth_ = 0;
    bool pressed_[4] = {};          // lit, indexed by Button
    bool down_[4] = {};             // physically down
    uint32_t lit_since_[4] = {};
    bool hold_shown_ = false;       // the reboot HoldCard is on screen
    int hold_px_ = -1;              // progress fill drawn so far
    int hold_left_ = -1;            // countdown drawn so far
    uint32_t hold_ms_ = 0;          // BACK held time at the last card draw
    void update_hold_card(uint32_t now_ms);
    void draw_hold_card(uint32_t held_ms);

    // What outlives a page is resident: the context (remembered cursors, the trigger draft, the cable meter, the kit), the
    // charger test session (the run, its results and the wizard stage), the FC release monitor and the share record. The
    // arena comes last so the pages are gone before anything they point at.
    UiContext ctx_;
    ChargerTestSession charger_test_;
    trig::Release fc_release_;
    ShareRecord share_rec_;         // the record the Share view shows: filled by the opener before the page exists
    StackArena<PageIf, kPageArenaBytes, kPageArenaDepth> pages_;

    uint32_t last_update_ms_ = 0;
    uint32_t cable_seq_seen_ = 0;   // fusb302::cable_seq() last folded into the link state
    pd::Link link_;
    uint32_t trig_until_ = 0;
    pd::Choice trig_choice_{};

    // Timing for the [UI] log: static draw time and the slowest update after a page change.
    uint32_t full_us_ = 0, field_max_us_ = 0, page_since_ms_ = 0;
    bool timing_pending_ = false;
};

}  // namespace k2
