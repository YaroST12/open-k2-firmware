// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The screens behind one small interface. Each page draws its static layout, refreshes its
// changing fields, takes the events the global handler left over and says what the four
// buttons do right now. Pages are plain static objects owned by Ui.
#pragma once
#include "k2/ui/ui_fc_release.hpp"
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include "k2/ui/gfx.hpp"
#include "k2/ui/home_cell.hpp"
#include "k2/ui/home_model.hpp"
#include "k2/ui/kit.hpp"
#include "k2/chargertest/report_card_delta.hpp"
#include "k2/chargertest/result_layout.hpp"
#include "k2/ui/list_view.hpp"
#include "k2/ui/ui_cable_list.hpp"
#include "k2/ui/ui_calc.hpp"
#include "k2/ui/ui_connection.hpp"
#include "k2/ui/ui_match.hpp"
#include "k2/ui/ui_tools.hpp"
#include "k2/ui/ui_emmenu.hpp"
#include "k2/ui/ui_qrtest.hpp"
#include "k2/ui/ui_events.hpp"
#include "k2/ui/ui_labels.hpp"
#include "k2/ui/ui_types.hpp"
#include "k2/ui/ui_widgets.hpp"

#include "k2/drivers/adc.hpp"
#include "k2/chargertest/charger_journal.hpp"
#include "k2/pd/transcap.hpp"
#include "k2/fastcharge/ai_recog.hpp"
#include "k2/pd/cable_r.hpp"
#include "k2/chargertest/ct_cable.hpp"
#include "k2/pd/pd_ccmode.hpp"
#include "k2/pd/ra_guard.hpp"
#include "k2/chargertest/resume_text.hpp"
#include "k2/pd/emlib.hpp"
#include "k2/pd/emrec.hpp"
#include "k2/fastcharge/fastcharge.hpp"
#include "k2/drivers/fusb302.hpp"
#include "k2/ui/trend.hpp"
#include "k2/pd/pd_tabs.hpp"
#include "k2/pd/pd.hpp"
#include "k2/drivers/usbcfg.hpp"
#include "k2/drivers/power_monitor.hpp"
#include "k2/fastcharge/fctrig.hpp"
#include "k2/ui/ui_trigger.hpp"
#include "k2/pd/pd_identify.hpp"
#include "k2/ui/ui_pdtrig.hpp"
#include "k2/ui/ui_wizard.hpp"
#include "k2/ui/ui_cabler.hpp"
#include "k2/chargertest/charger_test.hpp"

namespace k2 {

class Ui;

// State every page reads; owned by Ui, filled in its tick().
struct UiContext {
    Ui *ui = nullptr;
    Surface content{};
    Surface screen{};            // the same content area in panel pixels (224 x 240 or 240 x 224 by rotation): what a page draws on unscaled
    uint8_t rotation = 0;        // physical LCD rotation, supplied by Ui
    uint32_t now = 0;            // time of the event being dispatched
    power::Metrics pm{};             // what the screen shows: One Euro smoothed (Settings > Smoothing)
    power::Metrics pm_avg{};         // the 128-sample average: the charger test's current, link detection, cable resistance
    bool pm_online = false;
    std::int32_t vbus_fast_mv = 0;   // VBUS without the 2.5 s average: every judgement of a VBUS transition reads this, pm is display
    EnergyMeter meter;
    WattHistory watts;           // Big sparklines, one sample per second: forward power (mW)
    trend::Trend trend;          // Home v3 strips: V / A / W at 100 ms, 1 s and 10 s resolution
    WattHistory volts, amps;     // ... VBUS (mV) and |current| (mA)
    bool em_scanned = false;
    bool em_scanning = false;
    uint32_t em_listen_until = 0;   // passive listen window after a sniff that heard nothing (0 = none)
    bool em_quiet = false;          // the last listen window ended without a cable reply
    bool inline_mode = false;    // a live sink is attached: active operations are gated (refused, no confirm)
    std::optional<adc::Reading> analog;                          // averaged ADC sweep, once available
    fastcharge::Protocol fc = fastcharge::Protocol::Pending;     // passive D+/D- verdict
    bool trig_armed = false;     // trigger above 5 V: first OK given, waiting for the second
    uint32_t cap_mv = 5000;      // highest voltage a request may ask for (power monitor limits)
    cable_r::Meter cable;                                        // passive two-point cable resistance
    cable_r::Mode cable_mode = cable_r::Mode::Passive;           // Cable R submenu: passive meter or Kelvin 8-wire
    // The pending QR scan-test request (remote Cmd::QrTest op 1, k2/ui/ui_qrtest.hpp): the (version, px, quiet) of
    // the next show, consumed by QrTestPage::enter(). The page itself lives in the page arena.
    struct QrTestReq { std::uint8_t version = 0, px = 0, quiet = 0; } qrtest;
    std::optional<cable_r::Kelvin> kelvin;                       // Kelvin mode: latest valid sense reading
    // Stale lifecycle (design 6.6): when VBUS goes away the screens keep the last live reading.
    LiveTracker live;                                            // when VBUS was last present
    struct Frozen {
        power::Metrics pm{};
        std::optional<adc::Reading> analog;
        uint32_t elapsed_s = 0;
    } last;                                                      // the last live snapshot
    bool id_no_device = false;   // Identify was refused: nothing on CC, VBUS low (clears itself once something appears)
    // The one cable-identity list: its selection (what the next charger-test START presents), its saved-clone cache and its
    // armed marker. The wizard's Cable step and Tools -> Spoof both read and change this, so they always agree.
    CableIdentityList cables;
    // The one list view of the simple menu pages (Tools, Settings, Autorotate, USB link): only one is visible at a time, so
    // they share it instead of each owning a 10-row ListView. A page binds it in draw_static().
    kit::RowList rows;
    // What a page remembers after it is left. Pages are built when entered and destroyed when left (page_arena.hpp), so
    // anything that must survive a page switch lives here, not in the page; the page holds a reference under its old name.
    struct Remembered {
        trend::Window home_window = trend::Window::S10;   // Home: the trend window OK short cycles
        int tools_sel = 0;                                // Tools: the highlighted tool
        int settings_sel = 0;                             // Settings: the highlighted row
        trig::Model trigger;                              // FC trigger: the draft (protocol, target) survives cancel()
    } keep;
    // The pure precheck over the live readings (unknown = -1): is there a device to ask at all?
    bool no_device() const {
        return pd::identify_no_device(analog ? analog->cc1_mv : -1, analog ? analog->cc2_mv : -1,
                                      pm_online ? pm.vbus_mv : -1);
    }
    bool stale() const { return live.seen() && pm_online && pm.vbus_mv < 1000; }
};

// Pure row text of the pages that show the analog front end (host-tested).
inline constexpr int kLinesAnalogRows = 9;               // rows of lines_row_text
inline constexpr int kLinesRows = kLinesAnalogRows + 2;  // plus the two AI recognition rows (ai::recognise)
void lines_row_text(int index, const std::optional<adc::Reading> &r, fastcharge::Protocol p, StrBuf<24> &out);
// "Contract PDO4 20V 3A" for the sink's Request seen on the line (or "PDO4 20V 3A" when the long
// prefix would overflow the row); empty when no Request was decoded.
void contract_text(const pd::Contract &c, StrBuf<24> &out);
// Cable resistance submenu rows 0..4: live V/A, idle reference, loaded point, result, grade (host-tested).
inline constexpr int kCableRows = 5;
void cable_row_text(int index, bool online, int32_t vbus_mv, int32_t ibus_ma, const cable_r::Meter &m, StrBuf<24> &out);
// The same five rows in Kelvin mode: live V/A, POS and GND legs, sense status, total, grade.
void cable_kelvin_row_text(int index, bool online, int32_t vbus_mv, int32_t ibus_ma,
                           const std::optional<cable_r::Kelvin> &k, StrBuf<24> &out);
// Saved-cable table values (host-tested): row 0 the name, then VID/PID, XID, PD, ID VDO, Cable VDO, Rating.
inline constexpr int kEmDetailRows = 7;
void em_detail_value(int index, const emlib::Entry &ent, StrBuf<24> &out);
void temp_short_text(const std::optional<adc::Reading> &r, StrBuf<8> &out);   // MCU die temp: "MCU37C" / "MCU --"

// FC protocol pill on the Home totals card: the verdict's short name cut to 4 characters
// ("QC3", "AFC", "VOOC"); empty when there is no verdict worth a pill.
void fc_pill_text(fastcharge::Protocol p, StrBuf<6> &out);

// "20V 3A" for the negotiated PD contract (home_contract_text without the "PD " tag), "QC3 9V" for legacy.
void home_contract_short(const pd::Contract &c, fastcharge::Protocol p, int32_t vbus_mv, StrBuf<13> &out);
// "PD 9V 3A" for the negotiated contract (the Request on the line), "QC3 9V" for a detected legacy
// protocol at the measured VBUS; empty when neither (or without VBUS).
void home_contract_text(const pd::Contract &c, fastcharge::Protocol p, int32_t vbus_mv, StrBuf<13> &out);
// "5A 100W" from the cable e-marker, "no e-marker" without one.
void home_cable_text(const pd::Emarker &em, StrBuf<12> &out);
// Detail page rows: "5A 100W 20V EPR", "100W max, 3 PDO" (empty without source caps).
void detail_cable_text(const pd::Emarker &em, StrBuf<28> &out);
void detail_offer_text(const pd::SourceCaps &caps, StrBuf<28> &out);
// "CC1 1.62V"; "CC1 -.--V" while the ADC has no reading (mv < 0).
void home_cc_text(int cc, int32_t mv, StrBuf<10> &out);
void model_row_text(power::Chip chip, StrBuf<24> &out);                        // "N6.55", "--" without a meter

// What the Home info row says in place of the protocol tag and value while there is no source:
// "No source" (mode 1), empty (mode 0), nullptr when live or stale. Mode: 0 no meter, 1 idle, 2 live, 3 stale.
const char *home_idle_note(uint8_t mode);

class PageIf {
public:
    virtual void draw_static() = 0;                 // clear-screen layout: title, headers, fields
    virtual void update(uint32_t now_ms) = 0;       // refresh what changed (also the list rows)
    virtual bool on_event(Event e) = 0;             // true = consumed
    virtual PanelLabels labels() const = 0;         // what the four buttons do now
    virtual void invalidate() = 0;                  // forget what is on screen (after a capture)
    // Copy the live counters the page shows (those that change while the UI is drawing, like the
    // USB stats) so that every draw of one frame sees one consistent set. Default: nothing live.
    virtual void sample() {}
    // The page is about to be replaced (paging, BACK, the remote's set-page). Default: nothing.
    virtual void leave() {}
    // The page was just pushed as a submenu (Ui::push_menu), before its first draw. Default: nothing.
    virtual void enter() {}
    // The page draws over the whole panel, the button strip's edge included (a full-screen QR code): the Ui then leaves the
    // strip unpainted, key-press flashes too, so nothing lands on the code. Default: the strip is shown.
    virtual bool hides_strip() const { return false; }
protected:
    ~PageIf() = default;
};

// A page of the rotation (id < Page::Count) or a submenu (id == Page::Count). A submenu has a plain
// title bar, never consumes BACK (the Ui pops it) and labels [+]/[-] as scrolling, not paging.
class PageBase : public PageIf {
protected:
    PageBase(UiContext &c, Page id, const char *title) : ctx_(c), id_(id), title_(title) {}
    PageBase(UiContext &c, Page id, const char *title, Icon icon) : ctx_(c), id_(id), title_(title), has_icon_(true), icon_(icon) {}
    bool is_menu() const { return id_ == Page::Count; }
    void title_bar();
    // Right end of the top chrome bar: the compact V x A readout, the LAST pill when stale,
    // empty on Home (its hero carries the values).
    void title_badge(bool last);
    // Short BACK goes to the previous page (nothing on the first page).
    bool back_event(Event e);
    // Short [+] / [-] scroll `lv` down / up one row; declined when the list does not overflow.
    static bool scroll_event(ListView &lv, Event e, uint32_t now_ms = 0);
    PanelLabels base_labels() const;

    UiContext &ctx_;
    Page id_;
    const char *title_;
    bool has_icon_ = false;
    Icon icon_ = Icon::Bolt;
    bool bar_valid_ = false;   // the right half of the chrome bar matches bar_sig_
    uint32_t bar_sig_ = 0;
};

class ChargerTestPage;

// The charger test, resident half: the runner, the options, the wizard stage and cursor, the journal, the Ra guard, the
// identity a run presents and the results of the last run. It lives in Ui for as long as the K2 runs, so a run keeps going
// (poll() every tick) and its results stay viewable while the page is not on screen. The painted half is ChargerTestPage,
// which only exists while the page is visible and registers with the session for its repaint hooks (on_stage and so on).
// Painting never touches the runner: the page reads it between measurements, the session never draws.
class ChargerTestSession final : private charger_test::Env {
public:
    // Wizard step 1 is Algorithms on the wire (the Tests step: the presets, "Custom..." and "Last results"), step 2 Cable, step 3 Review.
    // OK on a preset or on a cable PICKS AND ADVANCES: there is no "Next" row. The cursor row of every list is the list's own row.
    // Custom = the per-family list behind "Custom..." (its last row, "Done", advances to Cable); Group = one family's algorithms,
    // opened by a long OK on a family. Both belong to step 1: they are no steps.
    // Cable = the one identity choice of the wizard: Off, the built-in chips, the saved clones (the list Tools -> Spoof shares,
    // ctx.cables). Picking only selects; nothing is armed until START.
    // Identity and Ask are retired (the identity list and the START dialog merged into Cable): they are never entered and
    // only keep their numbers, which the remote protocol, the web client and the tools use.
    // Refuse = the "Cable on plug" WARNING (the stage keeps its wire number): the Ra guard read a cable on the plug, so a spoof
    // may not reach the charger. Rows: 0 Start anyway (the identity runs armed, the guard's reading goes to the journal),
    // 1 Start with the cable OFF, 2 Cancel.
    // Waiting = START's wait for the charger after the Rd went on (VBUS, then the caps): driven by poll(), never blocks;
    // BACK or OK cancels it.
    enum class Stage : uint8_t { Algorithms, Group, Cable, Identity, Review, Running, Results, Steps, Detail, Resume, Changes, Ask, Refuse, Waiting, Custom };
    using Proposal = std::array<uint32_t, pd::kMaxPdos + 24>;
    explicit ChargerTestSession(UiContext& c) : ctx_(c), runner_(*this) {}
    void enter();                  // the page is (re)entered: the wizard starts at Algorithms, or at Resume for a journaled run
    void leave();                  // the page is left: a run that is not a resume's Rd is stopped (= cancel())
    void poll(uint32_t);
    // Load an interrupted run from the journal; true = open the page in Resume. `cancel` = BACK was held at power-up: the
    // run is dropped. A run that was started on a charger-powered K2 resumes by itself once VBUS is steady (poll()).
    bool boot_restore(bool cancel = false);
    void cancel();
    bool busy() const { return runner_.busy(); }
    const charger_test::Runner& results() const { return runner_; }
    uint32_t selection_mask() const { return options_.selected; }
    // The unit order of the next run (Cmd::ChargerUnits); empty = the default order. False for an unknown or repeated unit.
    bool set_units(std::span<const uint8_t> units);
    // The identity the next START presents (Cmd::ChargerCable); false for a malformed choice or a clone not in the library.
    // The Cable step's cursor follows it. Nothing is armed until a START.
    bool set_cable_choice(ctcable::Choice c);
    ctcable::Info cable_info() const;
    Stage stage() const { return stage_; }
    bool reattach_planned() const { return reattach_plan_; }
    uint8_t reattach_state() const { return reattach_; }
    bool confirm_reattach() const { return confirm_reattach_; }
    uint8_t run_guard() const { return run_guard_; }
    int cursor() const { return selection_; }
    void invalidate_confirmation();
    void emit_end(uint32_t now);   // run event for a run that just ended (live debug stream)
    void bind_view(ChargerTestPage* v) { view_ = v; }   // the painted half registers itself while it exists
private:
    friend class ChargerTestPage;
    // [[gnu::noinline]] on the big one-call-site helpers: -Os inlines each into update() / on_event() / poll(), whose
    // frames then add up to several KB of stack (tools/stack_depth.py); a call keeps them apart.
    [[gnu::noinline]] bool start_begin();   // START with no sink inline and VBUS down: present Rd and enter Waiting; false = no wait needed
    void wait_step(uint32_t now);     // one tick of Waiting: wait_tick(), then wait_done() when it is over
    pd::VbusWait::State wait_tick(uint32_t now);   // attach scan, VBUS / caps judgement, the one Rd re-present (Waiting and the resume's Rd)
    [[gnu::noinline]] bool present_rd(uint32_t now);   // e-marker scan on a dead line, Rd on, a fresh VbusWait: START's and the resume's common part
    void wait_done(uint32_t now);     // the wait is over (charger up, or given up): the preflight, then the run starts by itself
    void wait_abort();                // BACK / OK / leaving the page: Rd released, back to Review
    bool request_pending() const;     // the sink engine is still settling the start's own Request
    void ok_event();               // a short OK on the current stage (split from on_event: its frame sits under share_card on the stack's worst path)
    void open_confirmation();      // the first START press: the preview and the confirm window; nothing is touched
    void confirmed_start();        // the second START press: the Ra guard for an identity, then apply_cable() and press_start()
    [[gnu::noinline]] void press_start();            // after the identity is settled: Rd and the wait as the last stage, then launch()
    void launch();                 // the preflight on the final reading, then begin_run() (or start_failed)
    void start_failed(const char* why);              // passive again on Review, the reason on the footer
    void start_failed(charger_test::Reason, const char* busy, int vbus_mv);   // refused START: log line + recorded reason + footer words
    bool confirm_live() const;     // the confirm window is open, unexpired and still describes this proposal
    [[gnu::noinline]] void begin_run();              // the run itself
    [[gnu::noinline]] bool apply_cable(ctcable::Choice);   // arm the identity and turn the Ra mode on; false = refused (message set)
    void release_run();            // Rd, Ra mode and a dialog-armed spoof back to passive
    void guard_sample(uint32_t now);                 // one CC reading into the Ra guard's peak window
    raguard::Topology guard_topology(uint32_t now, bool log = true);  // what the lines say about the charger (cable / direct), judged now
    void fill_cable_names();            // the saved names of the rows the Cable list shows (never loaded while painting)
    void set_stage(Stage, int selection = 0);
    void message(const char*);
    int step_index(int row) const;
    charger_test::Algorithm algorithm_at(int row) const;
    Proposal proposal(charger_test::Input) const;
    charger_test::Input input(uint32_t) const;
    pd::Err request_pd(pd::Choice) override;
    fc::Fail request_fc(fc::Proto, int, bool, int) override;
    void release(bool) override;
    void legacy_only(bool) override;
    void pd_rescan() override;
    void detach() override;
    void attach(charger_test::Mode) override;
    void identity(bool) override;      // Ra mode + spoof on for the Pd-mode units, off for the Legacy ones (only while detached)
    void drop_identity();              // Ra, forced scan line and spoof off; run_cable_ stays for the card header
    void hold(charger_test::Mode) override;
    bool probe_identity(bool) override;
    pd::Err epr_info_ask() override;
    void record(const charger_test::Step&) override;
    void replan(unsigned follow, unsigned withdrawn, unsigned back) override;   // [TEST] re-plan: a mid-run re-announce was re-planned per row
    void hunt_evidence(const charger_test::Step&, const charger_test::HuntEvidence&) override;
    void plan_changed() override;
    bool journal_ready() override;     // no replan's section files are still to be written (an above-5 V request waits for it)
    void journal_waited(uint32_t) override;   // [CJRN] wait-for-replan ms=
    void closing(charger_test::Reason) override;   // the run ends for good: the journal is closed before the rows are skipped
    void note_transition(const charger_test::Step&);   // transition capture: trigger + keep on a failed step
    void phase(charger_test::Phase, size_t) override;
    void attempt(size_t, uint8_t) override;
    void resume_run(uint32_t now);
    // A resume on a K2 that is not self-supplied has no Rd on the line: a USB-C source never turns VBUS on, so the gate below
    // would never open. This presents it the way START does (identity after the Ra guard, e-marker scan, Rd) and carries the wait.
    [[gnu::noinline]] void resume_rd_step(uint32_t now);
    void resume_rd_drop();             // BACK: the Rd (and the identity it came with) goes back, nothing is presented again
    resumetext::In resume_words(uint32_t now) const;   // what the Resume stage says (k2/chargertest/resume_text.hpp)
    bool arm_cable(ctcable::Choice, bool in_run = false);   // apply_cable without the release_run() before it; in_run: the run re-arms its own identity
    [[gnu::noinline]] bool reapply_cable();   // restore the journaled Ra / spoof identity before a resumed run asks the charger anything
    bool boot_load(bool cancel);              // boot_restore's journal half (the replay into the Runner and the page)
    [[gnu::noinline]] void boot_arm_cable();  // a self-powered boot with an identity wanted: spoof + Ra on before the source's attach completes
    void abandon_run();
    void capture_identity();   // charger fingerprint, VID/PID and rated power, taken while the source caps are live
    void save_card();          // report card of a Done run; delta against the charger's earlier card
    [[gnu::noinline]] void save_card_store();   // save_card's tail: the previous card, the write and the delta, on its own frame
    UiContext& ctx_;
    ChargerTestPage* view_ = nullptr;   // the painted half while it exists
    charger_test::Runner runner_;
    char card_name_[24] = {};  // saved card name of this run, the Results heading
    report_card::Identity card_id_{};
    report_card::Card card_{};   // the run's card while save_card builds and stores it; a member, not a stack copy (the save path is stack-gated, tools/stack_depth.py)
    char mfr_[13] = {};   // Manufacturer_Info name from the source probe, only used to name the card
    std::array<report_card::DeltaLine, 8> delta_{};
    size_t delta_n_ = 0;
    uint16_t card_no_ = 0, delta_runs_ = 0;   // saved card number (0 = none), runs before this one
    charger_journal::ResumeGate gate_;
    bool journal_active_ = false, resume_pending_ = false;
    bool journal_closed_ = false;   // closing() already wrote this run's Finish (poll() need not write it again)
    bool capture_kept_ = false;          // the frozen transition capture already belongs to a failed row: a later failure (another unit) must not take it over
    // Journal writes a row in flight must not do (Runner::quiet()): the section files that hold a decided row or a replanned
    // plan are noted dirty in the journal itself. A write holds the main loop (and the PD engine) for 5 to 250 ms, so none is done
    // in one go: journal_pump() does ONE file per tick behind the row, and only while the PD link has been quiet (see record()).
    uint32_t jr_wait_ms_ = 0;               // when the pending records began to wait for a quiet link (0 = nothing waiting)
    // The run's journal is armed (its whole plan written, header last) in pieces too, one write per tick on a quiet link, while the
    // Runner is held in its first Baseline: arm_ 1 = rows and header, 2 = the run's meta record (one more write), 3 = the planned
    // re-attach's marker and Rd gap (one more write), 0 = done.
    uint8_t arm_ = 0;
    uint32_t arm_t0_ = 0;
    charger_journal::RunMeta arm_meta_{};
    void journal_pump(uint32_t now);        // one file at most per call; there is no drain
    void arm_step(uint32_t now);
    void arm_abort();
    bool self_run_ = false;              // the charger under test powers this K2 (a key-mode boot): the run keeps VBUS up, its card says so
    bool auto_resume_ = false;          // the journaled run was unattended (charger-powered K2): resume without the OK press
    ctcable::Choice resume_cable_{};     // the journaled identity of that run, re-armed by reapply_cable()
    bool resume_rd_ = false;             // the resume presented the Rd (host-powered K2): wait_ carries the wait for the charger
    bool resume_hold_ = false;           // BACK, or the identity could not be armed: no Rd is presented until OK asks again
    bool resume_refused_ = false;        // the Ra guard says the charger is behind a cable: the journaled identity cannot be armed
    uint32_t resume_judged_ = 0;         // when the guard last judged (and logged) for the resume
    charger_test::Options options_;
    charger_test::Summary summary_;
    uint32_t run_t0_ = 0;                  // when the run entered Running (the m:ss on screen)
    uint8_t run_guard_ = 0;                // the Ra guard's reading of the run in flight, journal / card encoding (RunMeta::guard)
    StrBuf<32> message_;
    Stage stage_ = Stage::Algorithms;
    int selection_ = 0, family_ = 0, result_family_ = -1, detail_index_ = 0, detail_row_ = 0;
    // The identity the next START presents lives in ctx_.cables (the list Tools -> Spoof shares), not here.
    ctcable::Choice run_cable_{};        // the identity of the current / last run
    ctcable::Facts run_facts_{};         // its VID / PID / limits, for the report card
    char run_cable_name_[ctcable::kNameLen] = {};
    bool dialog_spoof_ = false;          // the run presents the spoof (release_run lets go of it)
    bool spoof_user_ = false;            // ... but the user had armed that very identity in Tools first: release_run leaves it armed
    bool dialog_ra_ = false;             // the dialog turned the Ra mode on (release_run turns it off)
    bool follow_ = true, confirming_ = false;
    // Running: the verdict grid follows the step under test (follow_), anchored to the bottom: the running row ends the window
    // (ResultGrid::anchor_bottom), the finished rows stay above. PLUS / MINUS scroll it by hand (follow_ = false, the
    // line the user chose in scroll_line_); the follow resumes kFollowResumeMs after the last press or when the step changes.
    int scroll_line_ = 0;
    uint32_t scroll_ms_ = 0;
    size_t scroll_step_ = 0;
    pd::VbusWait wait_;              // Waiting: the VBUS / caps judgement
    uint32_t wait_t0_ = 0;               // when the press began the wait (the elapsed time on screen and in the log)
    uint32_t wait_slow_ = 0;             // the last attach scan (every pd::kStartWaitSlowMs)
    uint32_t wait_pause_until_ = 0;      // the 1 s Rd-off between the two attempts ends here
    bool wait_retry_ = false, wait_paused_ = false;   // the second attempt is running / the Rd is off between the two
    raguard::PeakWindow guard_;          // CC peaks over the last ~0.8 s while Cable / Review is open (a PLUS press sags CC2)
    uint32_t guard_t_ = 0;               // when the last sample went into guard_
    raguard::Topology topology_ = raguard::Topology::Unknown;   // the guard's last reading (the Warning screen, the run's journal)
    bool guard_bypass_ = false;           // "Start anyway": the guard's reading is recorded, the spoof runs regardless
    // The planned re-attach (rdintent::plan_reattach): a self-powered START with a 6 A identity against a source that lists SVID
    // 0x2717 makes the K2 blink off once, so the charger reads the cable at a real attach. confirm_reattach_ = the confirmation on
    // screen announces it; reattach_plan_ = this run does it (begin_run); reattach_ 1 = the Rd is off for the gap, 2 = the Rd is
    // back and the charger's new contract is awaited (the Runner stays held in its Baseline until then or until the K2 died).
    bool confirm_reattach_ = false;
    bool reattach_plan_ = false;
    uint8_t reattach_ = 0;
    uint32_t reattach_t0_ = 0;
    void reattach_begin(uint32_t now);
    void reattach_step(uint32_t now);
    void reattach_drop();
    uint32_t confirm_until_ = 0;
    Proposal confirm_proposal_{};
    bool entering_ = false;       // enter() is switching the stage: nothing of this page is on screen yet
};

// The charger test, painted half: built when the page comes on screen, destroyed when it leaves. Everything here is a repaint
// cache or the layout of the stored run; the state lives in the ChargerTestSession it reads through the references below.
// (The shared UI kit, docs/UI_DESIGN.md "Charger test": one painter per stage, each in its own small frame.)
// RowList stages (Tests, Custom, Group, Cable, Steps, Warning, Resume, Changes) are described by row_fn(); the dense stages
// (Review, Waiting, Running, Results, Detail) lay their static part out once (laid_) and then repaint only what changed.
class ChargerTestPage final : public PageBase {
public:
    using Stage = ChargerTestSession::Stage;
    using Proposal = ChargerTestSession::Proposal;
    ChargerTestPage(UiContext& c, ChargerTestSession& s);
    ~ChargerTestPage() { s_.bind_view(nullptr); }
    void enter() override { s_.enter(); }
    void draw_static() override;
    void update(uint32_t now_ms) override;
    bool on_event(Event e) override;
    PanelLabels labels() const override;
    void invalidate() override;
    void leave() override { s_.leave(); }
    // Repaint hooks the session calls while this page is alive.
    void on_stage(bool entering);          // the stage changed: wipe the body (not while entering), drop the caches
    void on_message() { foot_.invalidate(); foot_sig_ = 0; }
    void on_confirmation_dropped();        // the confirm window ended: the footer and START cell show it again
    void on_cable_changed();               // the Cable choice moved from outside: repaint it
    ListState list_state() const;
    // The paint budget of a Running update (every 100 ms) while a step is in flight (Runner::quiet()). Pixels are the measure:
    // the panel streams ~2.13 us per pixel plus ~15 us per window, and lcd.cpp services USB, FUSB302 and the UFCS handshake
    // every 2048 pixels (~4.4 ms) inside a stream. The items (gauge, hero + needle, clock, texts, progress, footer) share
    // kFlightPx (~13 ms); a grid row, the one item that can exceed it, paints alone on a tick where nothing else did, up to
    // kFlightRowPx (the tallest row, 224 x 42 px, plus the strike-throughs of its Skip cells painted over their text, at most
    // kit::kStrikeRowMaxPx, plus a Running cell's dot, kit::kDotRowMaxPx, ~20 ms). Between steps (and while laying out) there is
    // no cap. The gauge band and the step dot are one pair: the gauge is item 1, the grid's rebuild and ResultGrid::sync_dots
    // (kRunDotPx a move, never gated) come right after it, so on a step change the band and the dot move in the same tick.
    static constexpr int kFlightPx = 6000;
    static constexpr int kFlightRowPx = 224 * 42 + kit::kStrikeRowMaxPx + kit::kDotRowMaxPx;
    static constexpr int kHeroPx = 5000;         // the 4-digit 48 px readout (4 x 24 x 48) plus its unit
    static constexpr int kNeedlePx = 200;        // the old and the new needle column and the window edge they cross
    static constexpr uint32_t kHeroMs = 250;     // the hero and the needle repaint at most this often in flight (~4 Hz)
    static constexpr uint32_t kFollowResumeMs = 5000;   // a manual scroll of the grid holds this long, or until the step changes
private:
    ChargerTestSession& s_;
    // The session's state this view reads (and the few flags a press moves), under the names the painters use.
    charger_test::Runner& runner_ = s_.runner_;
    charger_test::Options& options_ = s_.options_;
    charger_test::Summary& summary_ = s_.summary_;
    Stage& stage_ = s_.stage_;
    int& selection_ = s_.selection_;
    int& family_ = s_.family_;
    int& result_family_ = s_.result_family_;
    int& detail_index_ = s_.detail_index_;
    int& detail_row_ = s_.detail_row_;
    size_t& delta_n_ = s_.delta_n_;
    std::array<report_card::DeltaLine, 8>& delta_ = s_.delta_;
    uint16_t& delta_runs_ = s_.delta_runs_;
    uint16_t& card_no_ = s_.card_no_;
    char (&card_name_)[24] = s_.card_name_;
    report_card::Identity& card_id_ = s_.card_id_;
    StrBuf<32>& message_ = s_.message_;
    bool& confirming_ = s_.confirming_;
    uint32_t& confirm_until_ = s_.confirm_until_;
    Proposal& confirm_proposal_ = s_.confirm_proposal_;
    bool& follow_ = s_.follow_;
    int& scroll_line_ = s_.scroll_line_;
    uint32_t& scroll_ms_ = s_.scroll_ms_;
    size_t& scroll_step_ = s_.scroll_step_;
    bool& guard_bypass_ = s_.guard_bypass_;
    bool& confirm_reattach_ = s_.confirm_reattach_;
    bool& auto_resume_ = s_.auto_resume_;
    bool& resume_hold_ = s_.resume_hold_;
    pd::VbusWait& wait_ = s_.wait_;
    uint32_t& wait_t0_ = s_.wait_t0_;
    bool& wait_paused_ = s_.wait_paused_;
    uint32_t& run_t0_ = s_.run_t0_;
    // Session operations a press or a painter asks for.
    charger_test::Input input(uint32_t now) const { return s_.input(now); }
    Proposal proposal(charger_test::Input in) const { return s_.proposal(in); }
    void set_stage(Stage s, int selection = 0) { s_.set_stage(s, selection); }
    void cancel() { s_.cancel(); }
    void wait_abort() { s_.wait_abort(); }
    void open_confirmation() { s_.open_confirmation(); }
    void confirmed_start() { s_.confirmed_start(); }
    bool confirm_live() const { return s_.confirm_live(); }
    void fill_cable_names() { s_.fill_cable_names(); }
    void invalidate_confirmation() { s_.invalidate_confirmation(); }
    void resume_run(uint32_t now) { s_.resume_run(now); }
    resumetext::In resume_words(uint32_t now) const { return s_.resume_words(now); }
    void abandon_run() { s_.abandon_run(); }
    charger_test::Algorithm algorithm_at(int row) const { return s_.algorithm_at(row); }
    int step_index(int row) const { return s_.step_index(row); }
    void share_card();
    void ok_event();
    void sync_proposal(uint32_t now);              // drop a stale confirm window, refresh the Review summary
    static void row_fn(void*, int, bool, kit::RowSpec&);        // the RowList stages
    static void result_row_fn(void*, int, kit::ResultRowSpec&); // the Running / Results grid
    void bar();                                     // top bar: the wizard progress or the plain title
    void foot(FooterTone, std::string_view);        // the footer through the page's one cache
    [[gnu::noinline]] FooterTone footer_line(StrBuf<32> &);   // the stage's footer text; returns its tone
    [[gnu::noinline]] void paint_list();            // RowList stages: bind once, then selection and rows
    [[gnu::noinline]] void paint_review();
    [[gnu::noinline]] void paint_warning();
    [[gnu::noinline]] void paint_waiting(uint32_t now);
    [[gnu::noinline]] void paint_running(uint32_t now);
    [[gnu::noinline]] void paint_results();
    [[gnu::noinline]] void paint_detail();
    [[gnu::noinline]] void paint_resume();
    [[gnu::noinline]] void paint_hero(bool full, int16_t x, int16_t y, uint8_t chars, int32_t vbus_mv, uint16_t color);
    void list_row(int, bool, kit::RowSpec&);   // row_fn's body (fills the page's shared text buffers, consumed at once)
    void grid_row(int, kit::ResultRowSpec&);
    void build_running_grid();                      // the Running verdict grid: the light layout, with the step in flight marked
    [[gnu::noinline]] void draw_capture(const k2::transcap::Meta&);   // VBUS thumbnail of the step's stored transition capture
    int last_row() const;          // the last row the cursor may rest on
    int row_count() const;
    void build_results();      // chips layout of the stored run (k2/chargertest/result_layout.hpp), wrapped into display lines
    // The summary line of Results: the best passing mode per protocol family of the stored run ("PD 20V 3A  QC3 12V").
    void compose_best();
    StrBuf<32> best_;
    result_layout::Layout layout_;
    std::array<result_layout::Line, result_layout::kMaxLines> lines_{};
    size_t line_n_ = 0;
    int results_row_ = 0;      // selected line to restore when BACK leaves the step list
    kit::ActionCell action_;               // Review: START / CONFIRM
    kit::ProgressBar prog_;                // Running: steps done of all
    kit::ResultGrid grid_;                 // Running and Results: the verdict grid
    Field hero_;                           // Waiting and Running: the live VBUS digits
    Elem foot_;                            // footer cache (draw_footer_cached)
    Elem body_;                            // the static part of a dense stage (laid_)
    uint32_t cell_sig_[12] = {};           // dense stages: per-line caches (Waiting lines, Running texts); 0 = not drawn
    uint32_t grid_key_ = 0;                // Running: the runner revision / step the grid was built for
    uint32_t hero_ms_ = 0;                 // when the hero last painted (Running while quiet: 2 Hz)
    uint8_t bar_key_ = 0;                  // the top bar drawn for this stage + 1 (0 = not drawn)
    CableIdentityList::Row row_;           // the Cable list's row being painted (RowSpec points into it)
    StrBuf<40> row_text_;                  // shared row-text buffers: row specs are consumed at once (docs/UI_DESIGN.md)
    StrBuf<16> row_tail_;
    StrBuf<48> row_note_;
    char cell_txt_[kit::kResultMaxCells][2][12] = {};   // the verdict grid's cells: volts and current text
    char cell_lbl_[10] = {};               // ... and the unit label of the row
    int needle_px_ = -1;                   // the gauge's needle column, -1 = the strip is not drawn
    uint32_t foot_sig_ = 0;                // Running: the footer (tone + text) last painted; 0 = not drawn
    uint16_t hero_color_ = 0;
    bool laid_ = false;                   // the stage's static part is on screen
};

// Home v7 "dual hero": the walk-by page, the whole 224 x 240 content (no title bar, no footer band). VBUS cell
// y0..69, IBUS cell y72..141 (64 px digits over a faint patient-monitor trend sweep, status edge, exception pill /
// limit alarm / limit bar), W / Wh / Ah band y144..193, info block y196..239 (USB link, protocol row). The pure
// parts are home_model / home_cell / home_proto; this class owns the state and decides what to repaint
// (src/ui/ui_home.cpp). OK short = trend window, OK long = energy reset.
class HomePage final : public PageBase {
public:
    explicit HomePage(UiContext &c) : PageBase(c, Page::Home, "") {}
    void draw_static() override;
    void update(uint32_t now_ms) override;
    bool on_event(Event e) override;
    PanelLabels labels() const override;
    void invalidate() override;
private:
    struct Frame;   // everything one update decides before it draws (ui_home.cpp)
    void sample_values(Frame &f, uint32_t now_ms);
    void update_sweep(const Frame &f, uint32_t now_ms, int &step_col, int &step_n);
    void paint_statics(const Frame &f);   // what depends on the mode only: rules, units, tags, field colours
    void paint_cell(int cell, const Frame &f, int step_col, int step_n, bool digits_changed, int16_t px0, int16_t px1);
    void paint_band(const Frame &f);
    void paint_info(const Frame &f);
    void draw_rules();         // the three full-width rules
    void draw_band_rules();    // the band's two hairlines (the Wh / Ah boxes touch them)
    // Display filter: V, A and W digits change at most 4 times a second (one throttle, one consistent set).
    Throttle thr_{250};
    int32_t v_mv_ = 0, a_ma_ = 0, w_mw_ = 0;         // |values| as shown
    power::Flow flow_ = power::Flow::Idle;
    home::ProtoInfo last_proto_{};                    // the protocol of the last live reading (Stale keeps it)
    uint8_t mode_ = 0xFF;                             // home::Mode last seen (not reset by invalidate); 0xFF = none yet
    bool statics_ok_ = false;                         // paint_statics() ran since the last invalidate / mode change
    bool band_valid_ = false;                         // the band's fields hold what band_sig_ says
    uint32_t band_sig_ = 0;
    uint16_t usb_col_ = 0, tag_col_ = 0, val_col_ = 0;   // colours the info fields hold
    trend::Window &window_ = ctx_.keep.home_window;   // remembered across page switches
    // The sweep survives invalidate() (a capture re-renders strips of one frame); it is reloaded from the
    // trend after a mode change, a window or scale change, or a long gap (page left, stalled loop).
    home::Sweep sw_[2];
    bool sweep_ok_ = false;
    uint32_t sweep_t_ = 0;
    uint32_t sweep_seen_ = 0;                         // the `now` of the last update: a replay of one frame never steps
    uint32_t sweep_scale_ = 0;
    uint16_t sweep_epoch_ = 0;                        // bumped on a reload: the cells repaint whole
    home::DigitField hd_[2];                          // VBUS / IBUS hero digits
    uint32_t cell_sig_[2][4] = {};                    // per cell: base, head, arrow, bar signatures
    bool cell_valid_[2] = {false, false};
    Field w_, wh_i_, wh_f_, ah_i_, ah_f_;             // band
    Field usb_, el_, tag_, val_, role_;               // info block
};

// Detail page: every measured value as small text (D+/D-, CC1/CC2, contract, cable, protocol, R, temperature,
// energy, timer, flow). Pure rows are home_*/detail_* above; this class only paints them.
class DetailPage final : public PageBase {
public:
    explicit DetailPage(UiContext &c) : PageBase(c, Page::Detail, "Detail", Icon::Chip) {}
    void draw_static() override;
    void update(uint32_t now_ms) override;
    bool on_event(Event e) override { return back_event(e); }
    PanelLabels labels() const override { return base_labels(); }
    void invalidate() override;
private:
    static constexpr int kFields = 16;   // 8 rows x 2 columns (k2-micro-gui phase 1 grid)
    Field f_[kFields];
    uint8_t flow_sig_ = 0xFF;
};

// The live verdict the PD page and the remote PD_MATCH command both show (passive reads only).
MatchResult match_live(const pd::Link &link);

// Counts match_live() calls; host tests (tests/test_ui_perf.cpp) read it to prove the PD page's
// cheap pre-check skips the expensive path when nothing changed. Static storage, never read on device.
extern uint32_t g_match_live_calls;

// Connection (Tools): the three parties on the two ports as tabs, Source / Cable / Sink, with a status bar
// at the bottom and a small action pop-up on OK (ui_connection.hpp holds the pure model). Short +/- switch
// tabs (or move in the pop-up), OK opens the pop-up, long OK shares, BACK closes the pop-up or leaves.
// Passive: it only reads what the engine already knows; its actions go through Ui::run_action and the
// existing inline confirm. The empty states read the port topology from the CC levels the ADC already
// averages (raguard::classify) and never start an active CC operation.
class ConnectionPage final : public PageBase {
public:
    explicit ConnectionPage(UiContext &c) : PageBase(c, Page::Count, "Who's attached", Icon::Plug) {}
    void enter() override;
    void draw_static() override;
    void update(uint32_t now_ms) override;
    bool on_event(Event e) override;
    PanelLabels labels() const override;
    void invalidate() override;
    void leave() override;
    ListState list_state() const { return ListState{}; }   // tabs, not a scrolling list
    conn::Tab tab() const { return tab_; }
    void open_tab(conn::Tab t);
    bool popup_open() const { return popup_; }
    conn::Facts facts() const { return facts_; }
    int popup_count() const { return n_items_; }
    const conn::Item &popup_item(int i) const { return items_[static_cast<std::size_t>(i)]; }
    int popup_selected() const { return popup_sel_; }
    bool details_open() const { return more_; }   // the Source tab's second page (what the Refresh survey read)
    std::string_view status_line() const { return status_txt_.view(); }   // what the bottom bar says
    // A short note on the status line (what an action did, or what a reboot carried over).
    void say(const char *text, uint32_t now_ms, uint32_t ms = 4000) { set_op(text, now_ms, ms); }
private:
    static void popup_row(void *self, int index, bool focused, kit::RowSpec &out);
    void sense(uint32_t now_ms);           // topology window + facts, once per update
    void build_items();
    void activate(const conn::Item &it, uint32_t now_ms);
    void set_op(const char *text, uint32_t now_ms, uint32_t ms = 2500);
    void share_caps(uint32_t now_ms);
    void share_cable(uint32_t now_ms);
    void paint_status(uint32_t now_ms);
    // `redraw` = the body area was wiped or never drawn (a new paint pass, tab, pop-up or empty state): paint it all.
    // Otherwise only the rows whose text, colour or key changed are repainted.
    void paint_body(const MatchResult &r, const pd::SourceCaps &caps, std::span<const uint32_t> sink, bool redraw);
    void paint_tab_indicator();
    void paint_empty();
    void paint_popup(uint32_t now_ms);
    void close_popup();
    conn::Tab tab_ = conn::Tab::Source;
    conn::Facts facts_;
    bool popup_ = false;
    bool popup_drawn_ = false;
    bool more_ = false;                    // Details: the second page of the Source tab (reset by a tab change)
    StrBuf<40> status_txt_;                // the bottom bar as last painted (tests read it; the page never does)
    bool id_asked_ = false;               // Identify started from the pop-up: its progress shows in the status bar
    int popup_sel_ = 0;
    std::array<conn::Item, conn::kMaxItems> items_{};
    int n_items_ = 0;
    Elem status_;                          // bottom status bar cache (the pop-up is the shared kit RowList, ctx_.rows)
    raguard::PeakWindow guard_;            // CC peaks for the topology (passive reads only)
    uint32_t guard_t_ = 0;
    bool guard_primed_ = false;
    StrBuf<24> op_;                        // last action message, shown in the status bar until op_until_
    uint32_t op_until_ = 0;
    uint32_t sig_ = 0;
    bool sig_valid_ = false;
    uint32_t cheap_sig_ = 0;               // scalar pre-check signature; only meaningful while sig_valid_
    bool defaulted_ = false;               // the tab was chosen by default_tab() on enter, not by the user
    // What the body area shows, so a changed row repaints alone: the kind, its tab, its row count and one hash per
    // painted row (key, value, colour, cut, dim). 40 bytes, no copy of the text.
    static constexpr int kBodyRows = 9;
    enum class Body : uint8_t { None, Rows, Empty, Popup };
    Body body_ = Body::None;
    conn::Tab body_tab_ = conn::Tab::Source;
    uint8_t body_n_ = 0;
    std::array<uint32_t, kBodyRows> row_sig_{};
};

class EmarkerPage final : public PageBase {
public:
    // The saved-cables browser opened from Connection's Cable tab: it starts in the list view and BACK leaves it from there.
    // The list is the kit RowList (names read from flash on demand), one cable's table is a kit KvTable and the delete
    // question a kit AlertBand. The remote's e-marker state reads the driver, not this page.
    explicit EmarkerPage(UiContext &c) : PageBase(c, Page::Count, "Saved cables", Icon::Emark) {}
    void enter() override;
    void draw_static() override;
    void update(uint32_t now_ms) override;
    bool on_event(Event e) override;
    PanelLabels labels() const override;
    void invalidate() override;
    void leave() override;
    ListState list_state() const { return menu_.view == EmView::List && n_seqs_ > 0 ? ctx_.rows.state() : ListState{}; }
private:
    static constexpr int kNameSlots = 8;   // names cached for the visible window (7 rows at most): index & 7
    static void row_fn(void *ctx, int index, bool focused, kit::RowSpec &out);
    std::string_view name_of(int index);
    void run_cmd(EmCmd cmd);
    void refresh_lib();
    void load_cur();
    void share_cur();

    EmMenu menu_;
    std::array<std::uint16_t, emlib::kCapacity> seqs_{};   // saved cable numbers, ascending
    int n_seqs_ = 0;
    emlib::Entry cur_{};                   // the cable shown in Detail / Confirm (also the flash read buffer)
    std::uint16_t cur_seq_ = 0;            // seq of cur_, 0 = none loaded
    bool have_cur_ = false;
    std::array<std::array<char, emlib::kNameMax + 1>, kNameSlots> names_{};
    std::array<int16_t, kNameSlots> name_idx_{-1, -1, -1, -1, -1, -1, -1, -1};
    StrBuf<24> note_;                      // what a delete answered, shown in the footer until note_until_
    bool note_caution_ = false;
    uint32_t note_until_ = 0;              // 0 = none
    uint32_t view_sig_ = ~0u;              // the view painted: a change wipes the body once
    Elem body_, foot_;                     // Detail / Confirm body and footer caches
};

// Diag v2: three cards (USB, PD, SYS) with label / value rows, state dots, a group pill and a footer
// sentence. A fixed layout: nothing scrolls.
class DiagPage final : public PageBase {
public:
    explicit DiagPage(UiContext &c) : PageBase(c, Page::Count, "Diag", Icon::Chip) {}
    void draw_static() override;
    void update(uint32_t now_ms) override;
    bool on_event(Event e) override;
    PanelLabels labels() const override;
    void invalidate() override;
    void sample() override;
    ListState list_state() const { return ListState{}; }
    static constexpr int kRows = 9;
private:
    // The USB counters move while a screenshot is being sent (every report bumps TX), and a
    // capture draws the page once per strip: rows read this copy, taken once per frame, or a digit
    // would be cut from two different values.
    struct Live {
        uint32_t resets = 0, isrs = 0, log_sent = 0, log_dropped = 0, poll_gap_max = 0, rx_count = 0;
        uint8_t cc = 0;
        bool configured = false;
    };
    void paint_marks(bool usb_ok, bool fusb_ok, bool meter_ok, bool meter_wait);
    Live live_;
    Field val_[kRows];
    uint8_t marks_ = 0xFF;       // group markers last painted (2 bits per group), 0xFF = none yet
    Elem foot_;
};

// Lines v2: D+ / D- / CC1 / CC2 tags with 0..3.3 V bars (ticks at 0.4 V and 2.0 V), a verdict card with the
// decoded result in pills, the Rp advertisement and the MCU temperature, and a footer sentence.
class LinesPage final : public PageBase {
public:
    explicit LinesPage(UiContext &c) : PageBase(c, Page::Lines, "Lines", Icon::Cable) {}
    void draw_static() override;
    void update(uint32_t now_ms) override;
    bool on_event(Event e) override;
    PanelLabels labels() const override;
    void invalidate() override;
    ListState list_state() const { return ListState{}; }   // fixed layout: nothing scrolls
private:
    void paint_bar(int i, int32_t mv, bool have);
    void paint_verdict(const std::optional<adc::Reading> &r, uint32_t now_ms);
    Field v_[4];
    int px_[4] = {-1, -1, -1, -1};
    Field pv_[2];                // Rp, MCU values
    uint32_t vsig_ = 0;          // verdict row content last painted
    Elem foot_;
    bool painted_ = false;
};

// Tools hub: a list of tools, each opens its own submenu. Short [+] / [-] select, OK opens.
class ToolsPage final : public PageBase {
public:
    explicit ToolsPage(UiContext &c) : PageBase(c, Page::Tools, "Tools", Icon::Chip) {}
    void draw_static() override;
    void update(uint32_t now_ms) override;
    bool on_event(Event e) override;
    PanelLabels labels() const override;
    void invalidate() override;
    ListState list_state() const { return ListState{}; }
    static constexpr int kTools = kToolCount;
    // Row -> submenu. The Spoof row is a shortcut: it opens Connection's Cable tab on the Spoof list (the same
    // CableIdentityList), so tool_menu(2) names the Spoof menu that opens on top of Connection.
    static Menu tool_menu(int i) {
        constexpr Menu kMenus[kToolCount] = {Menu::Trigger, Menu::PdTrigger, Menu::Spoof, Menu::CableR,
                                             Menu::Connection, Menu::BlackBox, Menu::Cards, Menu::Diag};
        return kMenus[i < 0 || i >= kToolCount ? 0 : i];
    }
    static const char *tool_name(int i);
    int selected() const { return sel_; }
private:
    static void row_fn(void *ctx, int index, bool focused, kit::RowSpec &out);
    Readiness rd_[kToolCount];             // readiness of every tool, refreshed in update(); the rows read it
    Elem foot_;                            // footer element cache (draw_footer_cached)
    int &sel_ = ctx_.keep.tools_sel;       // remembered across page switches
};

// Staged FC trigger (Tools): Pick/Edit/Review/Apply. Only accepted requests touch D+/D-;
// cancellation hands measured cleanup to Ui so leaving the menu cannot drop the safety guard.
class TriggerPage final : public PageBase {
public:
    explicit TriggerPage(UiContext &c) : PageBase(c, Page::Count, "FC trigger", Icon::Brick) {}
    void draw_static() override;
    void update(uint32_t now_ms) override;
    bool on_event(Event e) override;
    PanelLabels labels() const override;
    void invalidate() override;
    void sample() override;
    void leave() override;
    const trig::Model &model() const { return model_; }
    ListState list_state() const { return model_.stage() == trig::Stage::Pick ? ctx_.rows.state() : ListState{}; }
    void poll();
    void invalidate_confirmation() { model_.invalidate_confirmation(); }
private:
    trig::Run run() const;
    bool scan_view() const;               // the scan results are showing (not while the release monitor runs)
    void paint_bar();                   // top bar: title, "n/3" step and the progress rule
    void paint_main(uint32_t now_ms);
    void paint_release(bool fresh);       // the release monitor: an AlertBand and the measured VBUS
    void paint_scan();                    // one verdict row per scan step and the summary
    void paint_footer(FooterTone tone, std::string_view text);
    void push_trace(uint32_t now_ms);
    void draw_trace();
    static void row_fn(void *self, int index, bool focused, kit::RowSpec &out);   // the protocol list

    static constexpr int kTraceLen = 40;  // 10 s at 4 samples per second
    Field target_v_, vbus_v_, held_v_, state_v_;
    kit::ActionCell send_;                // Review: SEND, then CONFIRM with the draining bar
    uint8_t kind_ = 255;                  // body painted: 0 main, 1 scan
    uint32_t bsig_ = 0, barsig_ = 0, fsig_ = 0, tsig_ = 0, ysig_ = 0;
    std::array<uint32_t, fc::kScanSteps> tile_sig_{};   // per scan row (verdict, tail) cache
    int16_t trace_mv_[kTraceLen] = {};
    int trace_n_ = 0;
    uint32_t trace_ms_ = 0;
    trig::Model &model_ = ctx_.keep.trigger;   // the draft survives leaving the page (cancel() keeps protocol and target)
    fctrig::Status st_{};
    bool scanning_ = false;
    bool dirty_ = false;              // accepted a request since the last stop/leave
    bool was_running_ = false;
    uint32_t session_ = 0;
    bool show_scan_ = false;          // the grid shows the scan results
    std::size_t scan_step_ = 0;
    std::array<fc::ScanResult, fc::kScanSteps> res_{};
    std::size_t res_n_ = 0;
    fc::Fail refused_ = fc::Fail::None;
};

// PD trigger submenu (opened from Tools): a step wizard over the attached source's PDOs, OFFER ->
// VOLTAGE (PPS / AVS) -> CURRENT -> CONFIRM -> LIVE. Refused inline (a live sink is attached) and
// without caps. [+]/[-] step the value (long = coarse), OK moves on / sends (a second OK above 5 V),
// BACK goes one step back; BACK on OFFER leaves and releases to 5 V (leave()).
class PdTriggerPage final : public PageBase {
public:
    explicit PdTriggerPage(UiContext &c) : PageBase(c, Page::Count, "PD trigger", Icon::Pd) {}
    void draw_static() override;
    void update(uint32_t now_ms) override;
    bool on_event(Event e) override;
    PanelLabels labels() const override;
    void invalidate() override;
    void enter() override;
    void leave() override;
    ListState list_state() const { return active_ && wiz_.step == WizStep::Offer ? ctx_.rows.state() : ListState{}; }
private:
    enum class Refusal : uint8_t { None, Inline, NoCaps };
    SetupState refusal_state() const;
    void paint_bar();             // top bar: "n/3 PD <step>" and the progress rule
    void paint_refusal();         // flow diagram, requirement checklist and the reason footer
    void paint_offer(uint32_t now_ms);   // OFFER: the PDOs as kit RowList rows
    void paint_value();           // VOLTAGE / CURRENT: key/value table, the big value, the range bar
    void paint_confirm(uint32_t now_ms);   // CONFIRM: the request table and the SEND / CONFIRM action cell
    void paint_live();            // LIVE: a verdict cell for the contract, requested vs measured
    void paint_footer();
    void reset_paint();           // forget every signature: the next update repaints
    static void row_fn(void *self, int index, bool focused, kit::RowSpec &out);   // the OFFER list

    Wizard wiz_;
    const pd::SourceCaps *caps_ = nullptr;
    Refusal refusal_ = Refusal::None;
    bool active_ = false;         // the wizard is open (not refused)
    bool blocked_ = false;        // pd::can_trigger refuses: OFFER shows why, OK does nothing
    pd::Err send_err_ = pd::Err::Ok;   // why the last send was refused (shown on CONFIRM)
    uint8_t body_step_ = 0xFF;    // the step the body was last cleared for
    uint32_t armed_at_ = 0;       // when the second-OK window opened (0 = not armed): drains the CONFIRM bar
    uint32_t rsig_ = 0;           // refusal view signature (0 = not painted)
    uint32_t bsig_ = 0, fsig_ = 0, barsig_ = 0, tsig_ = 0, vsig_ = 0;   // body, footer, top bar, edge marks / resets, verdict
    Field f_[2];                  // the live cells of the step: power + big value, or measured V + A
    kit::ProgressBar range_;      // VOLTAGE / CURRENT: where the value sits in its range
    kit::ActionCell send_;        // CONFIRM: SEND, then CONFIRM with the draining bar
    PdoLine line_;                // the row being built by row_fn (strings stay valid until the next call)
};

// Passive cable resistance (opened from Tools): live V/A, the idle reference, the loaded point and
// the result of the two-point method (cable_r::Meter, fed by Ui::tick). OK clears the reference,
// [+] toggles the passive meter / Kelvin 8-wire mode. Three key/value rows (two steps, the load port), the result as a big
// figure with a verdict cell and a grade scale, the footer states what is missing or what the figure includes.
class CableRPage final : public PageBase {
public:
    explicit CableRPage(UiContext &c) : PageBase(c, Page::Count, "Cable R", Icon::Res) {}
    void draw_static() override;
    void update(uint32_t now_ms) override;
    bool on_event(Event e) override;
    PanelLabels labels() const override;
    void invalidate() override;
private:
    void paint_table(const CrView &v);
    void paint_result(const CrView &v);

    Field val_[2];                 // the two step values (key/value rows 1 and 2)
    uint32_t csig_ = 0, rsig_ = 0;
    uint32_t cleared_until_ = 0;   // Reset pressed: the footer says "Cleared" until then
    Elem foot_;                    // footer element cache (draw_footer_cached)
};

class SettingsPage final : public PageBase {
public:
    explicit SettingsPage(UiContext &c) : PageBase(c, Page::Settings, "Settings", Icon::Chip) {}
    void draw_static() override;
    void update(uint32_t now_ms) override;
    bool on_event(Event e) override;
    PanelLabels labels() const override;
    void invalidate() override { ctx_.rows.invalidate(); foot_.invalidate(); }
    ListState list_state() const { return {kRows, kRows, 0}; }
    int selected() const { return sel_; }
private:
    static constexpr int kRows = 6;   // 26 px rows: six fit under the title bar with the footer (the list view shows seven)
    static void row_fn(void *ctx, int index, bool focused, kit::RowSpec &out);   // reads the live settings: no value cache
    int &sel_ = ctx_.keep.settings_sel;   // remembered across page switches
    Elem foot_;
};

class AutorotatePage final : public PageBase {
public:
    explicit AutorotatePage(UiContext &c) : PageBase(c, Page::Count, "Autorotate", Icon::Rotate) {}
    void enter() override;
    void draw_static() override;
    void update(uint32_t now_ms) override;
    bool on_event(Event e) override;
    PanelLabels labels() const override;
    void invalidate() override { ctx_.rows.invalidate(); foot_.invalidate(); }
    ListState list_state() const { return {5, 5, 0}; }
private:
    static void row_fn(void *ctx, int index, bool focused, kit::RowSpec &out);
    int sel_ = 0;
    Elem foot_;
};

// USB link (opened from Settings): USB is off by default; HID (logs and remote) and the Drive are switched
// here and remembered in flash (k2/drivers/usbcfg.hpp). [+] picks the row, OK flips it. A change applies at the
// next boot (the descriptor is built once), so the footer says "Reboot to apply" until they match.
class UsbLinkPage final : public PageBase {
public:
    explicit UsbLinkPage(UiContext &c) : PageBase(c, Page::Count, "USB Link", Icon::Usb) {}
    void draw_static() override;
    void update(uint32_t now_ms) override;
    bool on_event(Event e) override;
    PanelLabels labels() const override;
    void invalidate() override;
    void enter() override;
    int selected() const { return sel_; }
private:
    static void row_fn(void *ctx, int index, bool focused, kit::RowSpec &out);
    usbcfg::Setting pending_{};
    int sel_ = 0;
    Elem foot_;                  // footer element cache (draw_footer_cached)
};

// The QR scan-test screen (remote Cmd::QrTest op 1; the model is k2/ui/ui_qrtest.hpp): ONE full-screen QR over the
// whole 240x240 panel (the button strip included: chrome and caption hidden while it is shown) at the requested
// (version, px, quiet), filled with the max-length kind-'t' link of that version, so a phone scan reads
// "QR scan test v<NN> <N>px". The code is drawn in update(), after the panel, so it covers the strip. BACK pops the
// page (the Ui does it). The encode workspace is a member, so it lives in the page arena, not in .bss.
class QrTestPage final : public PageBase {
public:
    explicit QrTestPage(UiContext &c) : PageBase(c, Page::Count, "QrTest") {}
    void draw_static() override;
    void update(uint32_t now_ms) override;
    bool on_event(Event e) override;
    PanelLabels labels() const override;
    void invalidate() override { painted_ = false; }
    void enter() override;
    bool hides_strip() const override { return true; }    // the code covers the whole panel

    int version() const { return version_; }
    int px() const { return px_; }
    int quiet() const { return quiet_; }
    int modules() const { return n_; }                    // modules per side of the code drawn, 0 = none
    std::size_t link_len() const { return link_len_; }    // the link length the code carries
    // Rebuilds the link text (the generator uses ws_.temp as working storage, so the text is not kept there).
    std::size_t link_text(std::span<char> out) const { return qrtest::build_link(version_, px_, out); }
    const std::uint8_t* code() const { return ws_.code; } // the packed symbol (for host tests)

private:
    void paint();
    qrtest::Workspace ws_;        // the v25-sized encode workspace (member: page arena)
    int version_ = 0, px_ = 0, quiet_ = 0, n_ = 0;
    std::size_t link_len_ = 0;
    bool painted_ = false;
};

}  // namespace k2
