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
#include "k2/ui/ui_twin.hpp"
#include "k2/ui/ui_loop_probe.hpp"
#include "k2/core/build_config.h"
#if K2_PDSTRESS
#include "k2/ui/ui_pdstress_page.hpp"
#endif
#include "k2/ui/ui_diagnosis.hpp"
#include "k2/ui/ui_battery.hpp"
#include "k2/ui/ui_connection.hpp"
#include "k2/ui/ui_share.hpp"
#include "k2/ui/ui_pages.hpp"
#if K2_PDSTRESS
#include "k2/ui/pd_stress_session.hpp"
#endif
#include "k2/ui/ui_types.hpp"
#include "k2/ui/page_arena.hpp"
#include "k2/twin/twin_hw.hpp"

#include "k2/pd/pd_link.hpp"
#include "k2/drivers/power_monitor.hpp"
#include "k2/ui/orientation.hpp"

namespace k2 {

// Page update period: Ui::tick() runs the current page's sample()+update() at most this often.
// Ships at 100 ms (10 Hz); test_ui_perf prints the px-per-tick cost of every page so a shorter
// period (50 ms / 33 ms) can be picked from device timings.
inline constexpr uint32_t kPageTickMs = 100;

// The page arena: the rotation page on screen and the menus opened above it are built in one buffer (index 0 = the rotation
// page, index n = the menu at stack position n - 1) and exist only while they are shown.
//
// Any rotation page can sit under any menu chain: goto_page / open_connection / land_rescanned (the remote, a rescan after a
// reboot) and the Connection place tools push a chain over whatever page is current, and the menus themselves open further
// menus (Share over Emarker / Cards / Loop probe / Black box ...). The buffer is therefore sized as the largest rotation
// page plus the largest menu chain, and the asserts below prove it for every pair of the lists. A chain that still did not
// fit would be refused by the arena and push_menu() says so on the log and on screen (never silently).
inline constexpr std::size_t kPageArenaDepth = 4;   // a rotation page and up to three menus
inline constexpr std::size_t kMaxMenuDepth = kPageArenaDepth - 1;
// Every rotation page (Page enum; build_page in ui.cpp).
inline constexpr std::size_t kLargestBase =
    std::max({arena_bytes<HomePage>(), arena_bytes<DetailPage>(), arena_bytes<ChargerTestPage>(), arena_bytes<LinesPage>(),
              arena_bytes<ToolsPage>(), arena_bytes<SettingsPage>()});
// Every menu chain the UI can open (top of the list = nearest the rotation page), each at most kMaxMenuDepth deep. Place pages
// (Connection) under each tool of their place (Charger: Request voltage, Fast charge, Report cards > Share, Stress test;
// Cable: Spoof, Resistance, Loop probe > Share; Device: Twins) and Connection's own Share rows, Black box > Share, Diagnosis >
// Black box > Share, Battery > Share, Settings > Diag > Share, Settings > Share (boot info), the Settings sub-pages, QrTest,
// and the Trigger > Emarker > Connection chain.
inline constexpr std::size_t kLongestMenuChain =
    std::max({arena_bytes<ConnectionPage, EmarkerPage, SharePage>(), arena_bytes<ConnectionPage, SpoofPage>(),
              arena_bytes<ConnectionPage, PdTriggerPage>(), arena_bytes<ConnectionPage, TriggerPage>(),
              arena_bytes<ConnectionPage, CardsPage, SharePage>(), arena_bytes<ConnectionPage, CableRPage>(),
              arena_bytes<ConnectionPage, LoopProbePage, SharePage>(), arena_bytes<ConnectionPage, TwinPage>(),
              arena_bytes<ConnectionPage, SharePage>(),
#if K2_PDSTRESS
              arena_bytes<ConnectionPage, PdStressPage>(),
#endif
              arena_bytes<TriggerPage, EmarkerPage, ConnectionPage>(), arena_bytes<CardsPage, SharePage>(),
              arena_bytes<DiagnosisPage, BlackBoxPage, SharePage>(), arena_bytes<BlackBoxPage, SharePage>(),
              arena_bytes<LoopProbePage, SharePage>(), arena_bytes<BatteryPage, SharePage>(), arena_bytes<QrTestPage>(),
              arena_bytes<DiagPage, SharePage>(), arena_bytes<UsbLinkPage>(), arena_bytes<AutorotatePage>()});
inline constexpr std::size_t kLongestChain = kLargestBase + kLongestMenuChain;   // the deepest rotation page + the deepest menus
inline constexpr std::size_t kPageArenaBytes = kLongestChain;
// The named worst cases (each rotation page that has one, the review's reachable chains) hold by construction; spelled out so
// a page that grows or a new chain shows up in the diff of this file, not as a refused menu on a device.
static_assert(kPageArenaBytes >= arena_bytes<ChargerTestPage, ConnectionPage, LoopProbePage, SharePage>(), "charger test + loop probe card");
static_assert(kPageArenaBytes >= arena_bytes<ChargerTestPage, ConnectionPage, TwinPage>(), "charger test + twins");
static_assert(kPageArenaBytes >= arena_bytes<DetailPage, ConnectionPage, TwinPage>(), "detail + twins");
static_assert(kPageArenaBytes >= arena_bytes<DetailPage, ConnectionPage, LoopProbePage>(), "detail + loop probe");
static_assert(kPageArenaBytes >= arena_bytes<HomePage, ConnectionPage, LoopProbePage, SharePage>(), "home + loop probe card");
static_assert(kPageArenaBytes >= arena_bytes<ToolsPage, ConnectionPage, EmarkerPage, SharePage>(), "the deepest Tools chain fits");
static_assert(kPageArenaBytes >= arena_bytes<ChargerTestPage, BlackBoxPage>(), "black box opens over charger test");
static_assert(kPageArenaBytes >= arena_bytes<ToolsPage, QrTestPage>(), "the qrtest page fits over Tools");
static_assert(kPageArenaBytes >= arena_bytes<SettingsPage, DiagPage, SharePage>(), "settings > diag > share fits");
static_assert(kMaxMenuDepth == 3, "the chain lists above are at most three menus deep");

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
    // set_rotate_buttons, configure_rotation and the +/- hold gesture store the result in the uiprefs record.
    void configure_rotation(bool automatic, uint8_t rotation);
    // Boot only, before init(): puts the remembered orientation settings in force so the first frame is already
    // drawn that way (manual = locked at `rotation`; automatic leaves the default until gravity says otherwise).
    // Writes nothing.
    void restore_orientation(bool manual, uint8_t rotation, bool rotate_buttons);
    // Home's hero quantity (Long Back on Home walks W -> V -> A): set_home_hero puts it in force, repaints Home and
    // stores it in the uiprefs record; restore_home_hero is the boot-time half (the stored byte, writes nothing).
    void set_home_hero(home::Qty q);
    void restore_home_hero(uint8_t stored) { ctx_.keep.home_hero = home::qty_from(stored); }
    // No sensor is guessed: a future verified driver supplies screen-axis gravity in mg.
    void feed_gravity(std::optional<orientation::Gravity> gravity, uint32_t now_ms);
    // Called every main-loop pass (~20 ms): samples power, integrates energy, and every
    // ~100 ms updates the changed fields of the current page. Returns Reboot once the OK + Back chord has been held
    // kChordRebootMs (the caller reboots).
    UiAction tick(uint32_t now_ms);
    // Debounced button event (physical or injected): the global handler first, then the page.
    UiAction on_button(Button b, ButtonEvent ev, uint32_t now_ms);
    // The same for a remote press (Remote::press, WebHID, the scripts): `logical` is the key with its
    // default-orientation meaning (plus = next/down, minus = previous/up), never remapped by the rotation or
    // the Rotate buttons setting, and it takes no part in the +/- rotation gesture.
    UiAction on_remote_button(Button logical, ButtonEvent ev, uint32_t now_ms);
    // Strip highlight and held state (the OK + Back reboot chord) for a remote key. Plus / minus light the slot of
    // the physical key that carries that action in the current rotation; a remote key never counts as
    // physically down, so it cannot be one half of the rotation chord.
    void set_remote_pressed(Button logical, bool down, uint32_t now_ms);
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
    // identity shows on the Sink tab instead. Inline is its own result, so the remote answers
    // pd::Err::Inline (not "busy") for it; Refused stays "a charger test is running".
    enum class IdResult : uint8_t { Started, Refused, NoDevice, Inline };
    IdResult identify(uint32_t now_ms);
    // Remote `debug detach` / `debug attach`: hold the link state until the next Rescan.
    void debug_link(bool attached, uint32_t now_ms);
    void debug_fake_pdos(uint8_t n, uint32_t now_ms);   // n PDOs injected; 0 clears them, the forced link and the live list
    const pd::Link &link() const { return link_; }
    // Jump to a page by index: a rotation page, kMenuPageBase + Menu (a submenu), or kConnTabAlias + tab
    // (Connection on that tab). Any open submenu is closed first; false when out of range or refused.
    bool goto_page(uint8_t page, uint32_t now_ms);
    // Fieldbook navigation (CONCEPT 7). The Meter is three faces (Flow, Numbers, Lines) that Up / Dn flip with a wrap; OK on a
    // face opens the Bench; the Bench opens a place on the same page (Page::Tools); a long BACK nobody takes lands on Flow.
    void flip_face(int dir, uint32_t now_ms);
    void show_face(uint32_t now_ms);                          // the face last shown (the Bench's Meter row and its BACK)
    void open_place(bench::Place p, uint32_t now_ms);         // Place::Meter = the Bench's own list
    void to_meter(uint32_t now_ms);
    // Opens Connection (Tools) on `tab`; `spoof` goes straight on to the Spoof list (the Tools shortcut).
    bool open_connection(conn::Tab tab, bool spoof, uint32_t now_ms);
    // K2_WHOS_ATTACHED=0: open_connection opens the single tool of that tab (ConnectionPage::lock), not the tabbed page.
    // The host tests build with K2_WHOS_ATTACHED=1 (the tabbed page's tests) and switch single tools on to test them.
    void set_single_tools(bool on) { single_tools_ = on; }
    bool single_tools() const { return single_tools_; }
    // The comeback of a bridged Refresh (core/rescan.cpp): Connection on its Source tab, saying "Rescanned (rebooted)".
    bool land_rescanned(uint32_t now_ms);
    // Submenus: a small stack (depth 3: Tools > Connection > Saved cables > Share). While one is open it gets every event,
    // the global paging is off and a short BACK pops it. push_menu fails when the stack is full.
    bool push_menu(Menu m, uint32_t now_ms);
    bool pop_menu(uint32_t now_ms);
    unsigned menu_depth() const { return depth_; }
    const char *notice() const { return notice_; }   // the refusal note on the notice strip, or null
    // Share action (long OK on the cable, the source caps, a report card, the charger test results): the opener builds its
    // record into share_buffer() (k2/ui/qrshare_records.hpp), then open_share() names it and pushes the share view. False
    // (nothing pushed) when the record has no part or push_menu refuses.
    std::span<std::uint8_t> share_buffer() { return share_rec_.buffer(); }
    bool open_share(qrshare::Type type, std::uint8_t schema, std::string_view title, std::size_t len, uint32_t now_ms);
    // The open Share view (host tests); it exists only while Menu::Share is on the stack.
    const SharePage &share() const { return *find<SharePage>(Menu::Share); }
    // The open QR scan-test page (host tests); it exists only while Menu::QrTest is on the stack.
    const QrTestPage &qrtest_page() const { return *find<QrTestPage>(Menu::QrTest); }
    // The open Device twins page (host tests); it exists only while Menu::Twin is on the stack. Not const: peek() builds a row.
    TwinPage &twin_page() { return *find<TwinPage>(Menu::Twin); }
    // The Loop probe page while Menu::LoopProbe is on the stack, else nullptr (host tests).
    LoopProbePage *loop_probe_page() const { return find<LoopProbePage>(Menu::LoopProbe); }
    // The PD Stress page while Menu::PdStress is on the stack, else nullptr (host tests).
#if K2_PDSTRESS
    PdStressPage *pd_stress_page() const { return find<PdStressPage>(Menu::PdStress); }
#endif
    // The open PD diagnosis page (host tests); it exists only while Menu::Diagnosis is on the stack.
    const DiagnosisPage &diagnosis_page() const { return *find<DiagnosisPage>(Menu::Diagnosis); }
    // The Battery place while Menu::Battery is on the stack, else nullptr (host tests).
    const BatteryPage *battery_page() const { return find<BatteryPage>(Menu::Battery); }
    // Top submenu; Menu::Count when none is open.
    Menu menu() const { return depth_ ? stack_[depth_ - 1] : Menu::Count; }
    // Page index for the remote: the rotation page, or kMenuPageBase + menu while one is open.
    uint8_t page_index() const { return depth_ ? static_cast<uint8_t>(kMenuPageBase + static_cast<uint8_t>(menu())) : static_cast<uint8_t>(page_); }
    // Panel highlight. A press stays lit for at least kPressFlashMs even if released sooner;
    // tick() clears it. A held button (long-press slot) stays lit until release.
    static constexpr uint32_t kPressFlashMs = 120;
    void set_pressed(Button b, bool down, uint32_t now_ms = 0);
    bool hold_card_visible() const { return hold_shown_; }   // OK + Back held >= 1 s: "Reboot in N"
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
    // True while a multi-pass page switch, clear, or sliced field repaint still has slices queued for subsequent ticks.
    bool painting() const { return paint_pending_; }
    // Runs the next slice of a pending paint (the main loop calls it on every pass that is not the 20 ms tick, so slices follow
    // each other at the pass rate instead of waiting for the next tick). Does only the paint: no sampling, no polls. False and
    // untouched when nothing is pending, or while the OK + Back hold card owns the panel (tick() sequences that one).
    bool paint_more(uint32_t now_ms);
    // The microsecond clock that arms every pass's budget (PaintBudget::arm): a slice then also stops on measured time
    // (PaintBudget::kPassUs). Unset, a slice stops on its pixel cap only (the host tests' default).
    void set_paint_clock(uint32_t (*clock_us)()) { paint_clock_ = clock_us; }
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
    // True when the current page covers the strip (the QR code): its four labels are not drawn (GET_STATE part 3).
    bool strip_hidden() { return current().hides_strip(); }
    bool charger_test_busy() const { return charger_test_.busy(); }
    const charger_test::Runner& charger_test_results() const { return charger_test_.results(); }
    // The draft selection on the wire (Cmd::ChargerTest schema 2): the algorithm mask, with bit 31 = the PPS deep
    // sweep on and bit 30 = the AVS deep sweep on (Options flags, not algorithm bits; both were always 0 before,
    // so old clients read the mask alone).
    uint32_t charger_test_selection() const {
        return charger_test_.selection_mask() | (charger_test_.sweep_on() ? 1u << 31 : 0) |
               (charger_test_.avs_sweep_on() ? 1u << 30 : 0);
    }
    void charger_test_set_sweep(bool on) { charger_test_.set_sweep(on); }
    void charger_test_set_avs_sweep(bool on) { charger_test_.set_avs_sweep(on); }
    // The SVOOC ceiling stage of the next run (Off / On / UsbA, svooc_ceiling::Mode as a byte; false for an unknown mode).
    bool charger_test_set_svooc_cap(uint8_t mode) {
        if (mode >= svooc_ceiling::kModeCount) return false;
        charger_test_.set_svooc_cap(static_cast<svooc_ceiling::Mode>(mode));
        return true;
    }
    uint8_t charger_test_svooc_cap() const { return static_cast<uint8_t>(charger_test_.svooc_cap()); }
    uint8_t charger_test_cursor() const { return static_cast<uint8_t>(charger_test_.cursor()); }
    uint8_t charger_test_stage() const { return static_cast<uint8_t>(charger_test_.stage()); }
    bool charger_test_set_units(std::span<const uint8_t> units) { return charger_test_.set_units(units); }
    bool charger_test_set_cable(ctcable::Choice c) { return charger_test_.set_cable_choice(c); }
    ctcable::Info charger_test_cable_info() const { return charger_test_.cable_info(); }
    bool charger_test_reattach_planned() const { return charger_test_.reattach_planned(); }   // the run does the planned re-attach
    uint8_t charger_test_reattach_state() const { return charger_test_.reattach_state(); }   // 0 none, 1 Rd off, 2 awaiting the contract
    bool charger_test_confirm_reattach() const { return charger_test_.confirm_reattach(); }   // the confirmation announces the blink
    uint8_t charger_test_run_guard() const { return charger_test_.run_guard(); }    // the guard byte the run records
    // What the charger-test screens show as data (Cmd::ChargerTest req [3]); refreshes the Review preview, arms nothing.
    ChargerTestSession::View charger_test_view() { return charger_test_.view(); }
    // The Connection page's facts (topology, waiting classifier, standby evidence) read with no page on screen:
    // conn::sense_passive over a one-shot window of the latest CC levels, the same implementation the page uses.
    [[gnu::noinline]] conn::Facts passive_facts() const;
    // The protocol row of Home (kind, level, limit, highest voltage) for the live reading: home::live_proto.
    [[gnu::noinline]] home::ProtoInfo proto_now() const;
    // The session chrome as data (GET_STATE part 1): what Home, Connection and the list pages derive and the panel shows,
    // every field read from the same helper the screen uses (home::live_proto, conn::sense_passive, busy_why). A few bytes
    // on the stack, no page needed; sel / count are 0xFF / 0 on a page with no list (the charger test has its own view).
    struct Session {
        int32_t mah = 0;
        uint32_t elapsed_s = 0;          // the Home elapsed time: live meter, else the last live snapshot
        uint8_t proto_kind = 0;          // home::ProtoKind of the live reading
        uint16_t limit_ma = 0, max_mv = 0, typec_ma = 0;
        BusyWhy busy = BusyWhy::None;
        bool fc_release = false;         // the 5 V release monitor is waiting
        uint8_t topology = 0;            // raguard::Topology
        uint8_t wait_kind = 0;           // raguard::WaitKind
        uint8_t pd_none_why = 0;         // pd::PdNoneWhy
        uint16_t open_cc_mv = 0xFFFF;    // the open-CC standby reading, 0xFFFF = none
        uint8_t depth = 0;
        uint8_t stack[3] = {0xFF, 0xFF, 0xFF};   // Menu values, outermost first
        uint8_t sub = 0;                 // kSubTabMask | kSubPopup | kSubDetails
        uint8_t sel = 0xFF, count = 0;
        uint8_t home_window = 0;         // trend::Window
        uint8_t flags = 0;               // kSession*
    };
    [[gnu::noinline]] Session session();
    // A Device twin live proof (twin_hw::live_running) and a PD Stress run own the ports like a charger test does.
    bool ports_busy() const { return charger_test_busy() || fc_release_.busy() || twin_live_busy() || pdstress_active() || loop_probe_busy(); }
    bool twin_live_busy() const;
    // A loop probe run (Tools -> Loop probe) drives D+/D-, the backlight and the K2's own Rd: the ports are its until it ends.
    bool loop_probe_busy() const;
    // What the twin live runner needs from the UI: the clock, the monitor ceiling, the latest VBUS and who else owns the ports
    // (the twin run itself is not "else"). The remote's `twin run` and the Device twins page start a run with it.
    twin_hw::LiveCtx twin_ctx(uint32_t now_ms) const;
    void cancel_twin();                  // BACK, the remote `stop`: ends a live proof, 5 V asked
    BusyWhy busy_why() const;          // why an identity / START is refused right now, BusyWhy::None = free
    const char *busy_reason() const;   // the footer text of busy_why(), nullptr = free
    // ---- PD Stress (the resident session; the remote and the shell start and read it through these) ----
#if K2_PDSTRESS
    PdStressSession &pd_stress() { return pdstress_; }
    const PdStressSession &pd_stress() const { return pdstress_; }
    bool pdstress_active() const { return pdstress_.active(); }
#else
    bool pdstress_active() const { return false; }   // K2_PDSTRESS=0: the tool is not built in
#endif
    // Why PD Stress may not start now: another tool owns the ports. The holds, the contract and the sink state a run
    // causes itself are not counted (busy_reason() would refuse the run for its own doing).
    const char *foreign_busy() const;
    // The runs that own the ports until they end, which the dead-man lease therefore leaves alone: a charger test or a PD Stress run.
    bool run_busy() const { return charger_test_busy() || pdstress_active(); }
    // The remote `stop` (the lease expiry and the shell `stop` land there too): ends a charger test, a twin proof and a PD Stress run.
    // The run releases the source to 5 V itself on the next poll; a PD Stress run waiting to resume after a reboot is dropped.
    void cancel_runs();
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
    // A page / menu switch clears the screen, then paints the incoming page from scratch over a fresh arena. How it clears:
    //  - the incoming page is OPAQUE (its kOpaque is true: its full paint writes every content pixel by itself): not at all,
    //    the one pass is the paint, with ctx_.blank false;
    //  - otherwise clear_for_incoming() replays the outgoing page's own paint in lcd erase mode (erase_outgoing(): only its
    //    ink is rewritten) when that costs less than one plain fill of the content (full_px_ is what its last full paint
    //    sent), else fills the content with kBg; both leave ctx_.blank true.
    // The outgoing page must still be alive for the replay; paint_incoming() runs after the arena change. `t0` starts the
    // [UI] timing.
    // `entering`: the page is about to be (re)entered (set_page), so its paint is the entry stage's, not the one on screen.
    bool page_opaque(Page p, bool entering = false) const;
    static bool menu_opaque(Menu m);
    bool clear_for_incoming(uint32_t now_ms, PageIf &outgoing, bool incoming_opaque, bool detach_rows = true);   // returns ctx_.blank for the paint
    void erase_outgoing(uint32_t now_ms, PageIf &outgoing);
    void paint_incoming(uint32_t now_ms, uint32_t t0, bool blank);
    // Full repaint of the current page after a real lcd::fill (init, rotation change), where the screen is already blank.
    void redraw(uint32_t now_ms);
    void draw_panel();
    void draw_release_notice();   // the footer notes every page shares, and the battery recorder's running dot
    bool battery_dot() const;     // a battery run is in progress and the page may carry its dot
    static constexpr int16_t kBattDot = 5, kBattDotX = 224 - kBattDot;
    void sync_panel();   // redraws the slots whose label changed since they were drawn
    uint32_t slot_sig(unsigned slot);
    uint32_t slot_sig_[4] = {};
    void draw_slot(unsigned slot);
    void log_timing(uint32_t now_ms);

    orientation::Tracker gravity_;
    uint32_t rotation_notice_until_ = 0;
    const char *notice_ = nullptr;   // a literal shown on the notice strip instead of the rotation text (refuse_menu)
    void refuse_menu(Menu m, const char *why, uint32_t now_ms);
    void rotation_gesture(uint32_t now_ms);
    void store_orientation(uint8_t rotation);   // writes the orientation fields into /prefs.cfg when they changed
    void draw_rotation_notice(uint32_t now_ms);
    Layout layout_{};
    Surface panel_{};
    void refresh_action_views(uint32_t now_ms);
    unsigned depth_ = 0;
    void update_lit(int k, bool was_held, uint32_t now_ms);
    bool held(int k) const { return down_[k] || remote_down_[k]; }
    UiAction finish_button(UiAction action, uint32_t now_ms);
    uint32_t lit_since_[4] = {};
    // The reboot gesture (OK + Back, ui_calc.hpp RebootChord): fed on every key edge, physical or remote. While it is
    // active the single-key OK / Back events are dropped, each until that key is pressed afresh.
    RebootChord chord_;
    void feed_chord(uint32_t now_ms);
    int hold_px_ = -1;              // progress fill drawn so far
    int hold_left_ = -1;            // countdown drawn so far
    uint32_t hold_ms_ = 0;          // chord held time at the last card draw
    UiContext::Frozen snap_prev_{}; // the live reading one snapshot window ago (note_last compares the next one with it)
    uint32_t snap_ms_ = 0;          // when snap_prev_ was taken
    uint32_t sag_level_ms_ = 0;     // last refresh of ctx_.sag_level_mv
    home::SettledLevel sag_settle_; // the contract level counts for the sag once it has held 1 s
    void update_hold_card(uint32_t now_ms, RebootChord::Step step);
    void draw_hold_card(uint32_t held_ms);

    // What outlives a page is resident: the context (remembered cursors, the trigger draft, the cable meter, the kit), the
    // charger test session (the run, its results and the wizard stage), the FC release monitor and the share record. The
    // arena comes last so the pages are gone before anything they point at.
    UiContext ctx_;
    ChargerTestSession charger_test_;
#if K2_PDSTRESS
    PdStressSession pdstress_;      // after the charger test: it borrows that session's sweep buffer for its record
#endif
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
    uint32_t full_px_ = 0;   // pixels the current page's last full paint sent: what replaying it to erase it would cost

    // Progressive slice-paint state (keeps each call under the 20 ms pass ceiling).
    struct BudgetScope {
        Ui &ui;
        PaintBudget owned{};
        PaintBudget *prev = nullptr;
        bool active = false;
        explicit BudgetScope(Ui &u) : ui(u), prev(u.ctx_.budget) {
            if (!u.booting_ && prev == nullptr) {
                if (u.paint_clock_) owned.arm(u.paint_clock_, PaintBudget::kPassUs);
                u.ctx_.budget = &owned;
                u.ctx_.rows.set_budget(&owned);
                active = true;
            }
        }
        ~BudgetScope() {
            if (active) {
                if (owned.more) ui.paint_pending_ = true;
                ui.ctx_.budget = prev;
                ui.ctx_.rows.set_budget(prev);
            }
        }
        bool more() const { return ui.ctx_.budget && ui.ctx_.budget->more; }
    };
    void start_full_clear();
    void reset_paint_pipeline();
    void pump_paint(uint32_t now_ms);
    uint32_t (*paint_clock_)() = nullptr;
    uint32_t paint_t0_us_ = 0;
    uint64_t paint_px0_ = 0;
    int16_t clear_y_ = 0;
    int16_t clear_y1_ = 0;
    Orientation orient_ = kDefaultOrientation;
    Page page_ = Page::Home;
    Menu stack_[3] = {Menu::Count, Menu::Count, Menu::Count};
    raguard::CrossKind cross_seen_ = raguard::CrossKind::None;   // the crossed-CC verdict last logged ([LINK] cc cross=...)
    pd::PdVerdict pd_verdict_seen_ = pd::PdVerdict::Pending;     // the PD verdict last logged ([LINK] pd verdict=...)
    bool rotation_lock_ = false;
    bool rotation_chord_ = false;
    bool rotate_buttons_ = false;
    bool suppress_rotated_press_[4] = {};
    uint8_t rotation_steps_ = 0;
    uint8_t requested_rotation_ = 0xFF;
    bool rotation_notice_drawn_ = false;
    bool single_tools_ = !K2_WHOS_ATTACHED;
    int8_t conn_lock_ = -1;   // open_connection -> build_menu: the tab a single tool locks to, -1 = the tabbed page
    bool place_next_ = false; // open_place -> build_menu: the PD diagnosis opens as the Log place
    bool pressed_[4] = {};          // lit, indexed by Button
    bool down_[4] = {};             // physically down
    bool remote_down_[4] = {};      // a remote +/- is held, indexed by the physical key it lights
    uint8_t remote_key_[4] = {0xFF, 0xFF, 0xFF, 0xFF};   // by logical button: the physical key lit at press
    bool suppress_chord_[4] = {};
    bool hold_shown_ = false;       // the reboot HoldCard is on screen
    bool extremes_live_ = false;    // VBUS was live at the last tick: a rising edge resets the Home peaks and cards
    bool pdstress_note_ = false;    // a footer note (the PD Stress run's line or the loop lock) was on screen last tick
    bool batt_dot_ = false;         // a battery run was in progress last tick (its dot on screen, draw_release_notice)
    bool timing_pending_ = false;
    bool booting_ = false;
    bool paint_pending_ = false;
    bool paint_blank_ = false;
    bool clear_pending_ = false;
    bool clear_full_lcd_ = false;
    bool chrome_gaps_pending_ = false;
    bool static_pending_ = false;
    bool panel_pending_ = false;
    bool timing_in_flight_ = false;
    uint8_t panel_mask_ = 0;
};

}  // namespace k2
