// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The screens behind one small interface. Each page draws its static layout, refreshes its
// changing fields, takes the events the global handler left over and says what the four
// buttons do right now. Pages are plain static objects owned by Ui.
#pragma once
#include "k2/ui/ui_fc_release.hpp"
#include "k2/core/build_config.h"
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include "k2/core/memstat.hpp"
#include "k2/ui/gfx.hpp"
#include "k2/ui/home_cards.hpp"
#include "k2/ui/home_cell.hpp"
#include "k2/ui/home_model.hpp"
#include "k2/ui/kit.hpp"
#include "k2/ui/rail_title.hpp"
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
#include "k2/drivers/usbcfg.hpp"
#include "k2/chargertest/charger_journal.hpp"
#include "k2/pd/pd_limiter.hpp"
#include "k2/pd/transcap.hpp"
#include "k2/fastcharge/ai_recog.hpp"
#include "k2/pd/cable_r.hpp"
#include "k2/chargertest/ct_cable.hpp"
#include "k2/pd/pd_ccmode.hpp"
#include "k2/pd/ra_guard.hpp"
#include "k2/chargertest/resume_text.hpp"
#include "k2/pd/emlib.hpp"
#include "k2/core/bss_zero.hpp"
#include "k2/pdstress/plan.hpp"
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
#include "k2/ui/wait_plus.hpp"
#include "k2/ui/ui_cabler.hpp"
#include "k2/chargertest/charger_test.hpp"

namespace k2 {

class Ui;
#if K2_SPECROWS
namespace specsec { struct Row; }
#endif

// State every page reads; owned by Ui, filled in its tick().
struct UiContext {
    Ui *ui K2_BSS_INIT(nullptr);
    Surface content K2_BSS_ZERO;
    Surface screen K2_BSS_ZERO;            // the same content area in panel pixels (224 x 240 or 240 x 224 by rotation): what a page draws on unscaled
    uint32_t now K2_BSS_INIT(0);            // time of the event being dispatched
    PaintBudget *budget K2_BSS_INIT(nullptr);   // active per-pass slice budget; nullptr during atomic screenshots/boot
    power::Metrics pm K2_BSS_ZERO;             // what the screen shows: One Euro smoothed (Settings > Smoothing)
    power::Metrics pm_avg K2_BSS_ZERO;         // the 128-sample average: the charger test's current, link detection, cable resistance
    std::int32_t vbus_fast_mv K2_BSS_INIT(0);   // VBUS without the 2.5 s average: every judgement of a VBUS transition reads this, pm is display
    EnergyMeter meter;
    trend::CompactTrend trend;   // Home v3 strips & Graphs: V / A / W at 100 ms, 1 s and 10 s resolution
    uint32_t em_listen_until K2_BSS_INIT(0);   // passive listen window after a sniff that heard nothing (0 = none)
    std::optional<adc::Reading> analog;                          // averaged ADC sweep, once available
    vooc::Live vooc K2_BSS_ZERO;           // inline VOOC monitor: a proven session now + the last raw 0x82 reply (fastcharge::vooc_live)
    uint32_t cap_mv = 5000;      // highest voltage a request may ask for (power monitor limits)
    cable_r::Meter cable;                                        // passive two-point cable resistance
    // Home's PEAK and SAG cards: peaks and the worst dip since attach (or the last long OK on Home), fed from the raw sample
    // in Ui::tick against `sag_level_mv`, the level in force (home::sag_level_mv, refreshed every 100 ms while live).
    home::Extremes extremes;
    uint32_t sag_level_mv K2_BSS_INIT(0);
    std::optional<cable_r::Kelvin> kelvin;                       // Kelvin mode: latest valid sense reading
    // Stale lifecycle (design 6.6): when VBUS goes away the screens keep the last live reading.
    LiveTracker live;                                            // when VBUS was last present
    struct Frozen {
        power::Metrics pm K2_BSS_ZERO;
        std::optional<adc::Reading> analog;
        uint32_t elapsed_s K2_BSS_INIT(0);
        home::ProtoInfo proto K2_BSS_ZERO;                       // the protocol row of that reading
    } last;   // the last steady live snapshot (Ui::note_last: a falling VBUS or a released contract never lands here)
    // The one cable-identity list: its selection (what the next charger-test START presents), its saved-clone cache and its
    // armed marker. The wizard's Cable step and Tools -> Spoof both read and change this, so they always agree.
    CableIdentityList cables;
    // The one list view of the simple menu pages (Tools, Settings, Autorotate, USB link): only one is visible at a time, so
    // they share it instead of each owning a 10-row ListView. A page binds it in draw_static().
    kit::RowList rows;
    // Shared transient floating toast pill (Option B: 16 px bottom-center overlay, ~2.5 s auto-expiry).
    Toast toast;
    // What a page remembers after it is left. Pages are built when entered and destroyed when left (page_arena.hpp), so
    // anything that must survive a page switch lives here, not in the page; the page holds a reference under its old name.
    struct Remembered {
        trend::Window home_window K2_BSS_INIT(trend::Window::S10);   // Home: the trend window OK short cycles (S10 = 0)
        home::Qty home_hero K2_BSS_INIT(home::Qty::W);               // Home: the hero quantity Long Back cycles (uiprefs, restored at boot; W = 0)
        std::array<home::BoxKind, home::kBoxCount> home_boxes = home::kDefaultBoxes;   // Settings > Home boxes (uiprefs, restored at boot)
        home::BoxCycler boxes;                            // Home: the 3 configurable 74x66 boxes + 3s fading cyan glow
        home::Attention attention;                        // Home: what is changing now, drives box highlight/pull
        home::DiagDwell diag_dwell;                       // Home: when row 1 first showed each event finding
        uint8_t long_back K2_BSS_INIT(0);                // Settings: the ring position long Back jumps to (ring::Pos; Home = 0; uiprefs)
        int settings_sel K2_BSS_INIT(0);                  // Settings: the highlighted row
        int tools_sel K2_BSS_INIT(0);                     // Tools: the highlighted row
        trig::Model trigger;                              // FC trigger: the draft (protocol, target) survives cancel()
        struct PdStress {                                 // Tools > PD Stress: the setup the next START uses
            std::uint16_t mask = pdstress::kDefaultSeqMask;   // picked sequences (bit = pdstress::Seq)
            std::uint16_t closed_run K2_BSS_INIT(0);      // PdStressSession::run_no() of the result last saved or discarded
            std::uint8_t cycles = 2;                      // index into PdStressPage::kCyclesPct (25 / 50 / 100 / 200 %)
            std::uint8_t pace K2_BSS_INIT(0);             // pdstress::Options::pace
            std::uint8_t sel K2_BSS_INIT(0);              // the Setup list's cursor
        } pdstress;
        std::array<uint8_t, bench::kPlaces> place_sel K2_BSS_ZERO;  // each place's last action: repeating a job is OK, OK, OK
        struct Ripple {                                              // Meter > Ripple: persisted scope settings across face flips
            uint8_t timebase K2_BSS_INIT(0);                         // 0 = Auto, 1..7 = fixed kTimebases[timebase - 1]
            uint8_t scale K2_BSS_INIT(0);                            // 0 = Auto
            uint8_t trigger K2_BSS_INIT(0);                          // 0 = Auto, 1 = Free
            bool hold K2_BSS_INIT(false);
        } ripple;
    } keep;
    // The pending QR scan-test request (remote Cmd::QrTest op 1, k2/ui/ui_qrtest.hpp): the (version, px, quiet) of
    // the next show, consumed by QrTestPage::enter(). The page itself lives in the page arena.
    struct QrTestReq { std::uint8_t version K2_BSS_INIT(0), px K2_BSS_INIT(0), quiet K2_BSS_INIT(0); } qrtest;
    uint8_t rotation K2_BSS_INIT(0);        // physical LCD rotation, supplied by Ui
    // The contract's power ceiling in whole watts while live (graphs::limit_mw: under PPS the requested level), refreshed every
    // 100 ms with the sag level; the trend records it per column for the Graphs page's dotted limit. 0 = none known.
    uint32_t limit_w K2_BSS_INIT(0);
    bool pm_online K2_BSS_INIT(false);
    // True while the Ui paints a whole page onto a screen it knows is kBg everywhere (after the full clear or the erase pass:
    // Ui::paint_incoming, render_for_capture). A page's first paint may then skip a body wipe, since the pixels are already
    // kBg. Never true in the erase replay, a resync or a refresh: those paint over live ink.
    // False while an OPAQUE page paints (Ui::page_opaque / menu_opaque): the switch skipped the erase pass, the old page's ink
    // is still there, and the page must write every pixel of the content itself, the gaps between its cells included
    // (kit::fill_except / kv_ground lay the ground in the same pass, so no pixel is streamed twice).
    bool blank K2_BSS_INIT(false);
    bool keep_status K2_BSS_INIT(false);   // a static repaint that only restores pixels (a toast ended): status toasts are not raised again
    bool em_scanned K2_BSS_INIT(false);
    bool em_scanning K2_BSS_INIT(false);
    bool em_found K2_BSS_INIT(false);
    bool em_quiet K2_BSS_INIT(false);          // the last listen window ended without a cable reply
    bool inline_mode K2_BSS_INIT(false);    // a live sink is attached: active operations are gated (refused, no confirm)
    bool plus_key K2_BSS_INIT(false);       // the physical + key is down right now (Waiting watches it: it is wired to the CC pins)
    fastcharge::Protocol fc K2_BSS_INIT(fastcharge::Protocol::Pending);     // passive D+/D- verdict
    fastcharge::Protocol fc_lines K2_BSS_INIT(fastcharge::Protocol::Pending);   // the pair alone (fastcharge::lines())
    bool trig_armed K2_BSS_INIT(false);     // trigger above 5 V: first OK given, waiting for the second
    cable_r::Mode cable_mode K2_BSS_INIT(cable_r::Mode::Passive);           // Cable R submenu: passive meter or Kelvin 8-wire
    bool id_no_device K2_BSS_INIT(false);   // Identify was refused: nothing on CC, VBUS low (clears itself once something appears)
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
// The live setup the tool readiness reads (the link and the power monitor; ui_tools.hpp).
SetupState setup_state(const UiContext &c);
// The Bench's places (ui_bench.cpp): the masthead icon of each, and what an action of a place does when OK opens it (a tool's
// menu, a page; the place page and the Bench share it).
Icon place_icon(bench::Place p);
void open_bench_act(Ui &ui, const bench::Act &a, uint32_t now_ms);

// FC protocol pill on the Home totals card: the verdict's short name cut to 4 characters
// ("QC3", "AFC", "VOOC"); empty when there is no verdict worth a pill.
void fc_pill_text(fastcharge::Protocol p, StrBuf<6> &out);

// Shared out-of-line page helpers (implemented in src/ui/ui_pages.cpp) to avoid per-TU duplication.
namespace pg {
[[gnu::noinline]] bool put_field_buf(FieldBase &f, char *cur, PaintBudget *bg, std::string_view s,
                                     uint16_t fg = kText, uint16_t bg_col = kBg);
inline bool put_field(FieldBase &f, PaintBudget *bg, std::string_view s, uint16_t fg = kText, uint16_t bg_col = kBg) {
    return put_field_buf(f, f.cur_data(), bg, s, fg, bg_col);
}
[[gnu::noinline]] void invalidate_fields(Field *f, int n);
[[gnu::noinline]] void clear_body(const Surface &c, int16_t y0 = 0);
[[gnu::noinline]] std::string_view stash_str(char *dst, std::size_t cap, std::string_view s);
inline uint32_t mix_str(uint32_t h, std::string_view s) { return k2::mix(h, s); }
inline uint32_t mix_str(std::string_view s) { return k2::mix(s); }
[[gnu::noinline]] uint32_t mix_cstr(uint32_t h, const char *s, std::size_t max_len = ~std::size_t(0));
[[gnu::noinline]] uint32_t mix_u32s(uint32_t h, const uint32_t *w, std::size_t n);
[[gnu::noinline]] uint32_t mix_u16s(uint32_t h, const uint16_t *w, std::size_t n);
[[gnu::noinline]] uint32_t mix_bytes(uint32_t h, const void *p, std::size_t n);
[[gnu::noinline]] uint32_t mix_bytes(const void *p, std::size_t n);
[[gnu::noinline]] void put_exact_unit(StrBufBase &out, uint32_t milli, char unit = 0);
[[gnu::noinline]] void put_unit_or_dash(StrBufBase &out, bool ok, int32_t val, int dec, char unit, bool abs_val = false);
[[gnu::noinline]] std::string_view trim_left(std::string_view s);
inline void put_dv(StrBufBase &out, unsigned dv) { put_exact_unit(out, dv * 100u, 'V'); }
inline void put_volts_trim(StrBufBase &out, int32_t mv) { put_exact_unit(out, mv < 0 ? 0u : static_cast<uint32_t>(mv), 0); }
}  // namespace pg

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
void model_row_text(StrBuf<24> &out);                                           // "HW Q6.03"

// What the Home info row says in place of the protocol tag and value while there is no source:
// "No source" (mode 1), empty (mode 0), nullptr when live or stale. Mode: 0 no meter, 1 idle, 2 live, 3 stale.
const char *home_idle_note(uint8_t mode);

class PageIf {
public:
    virtual void draw_static() = 0;                 // clear-screen layout: title, headers, fields
    virtual void update(uint32_t now_ms) = 0;       // refresh what changed (also the list rows)
    virtual bool on_event(Event e) = 0;             // true = consumed
    virtual Strip labels() const = 0;         // what the four buttons do now
    virtual void invalidate() = 0;                  // forget what is on screen (after a capture)
    virtual void invalidate_for_capture() { invalidate(); }
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
    // Option B Opposite Telemetry Rail descriptor (MastheadCell title/dots + telemetry mode).
    virtual kit::OppRailSpec opp_rail() const { return {}; }
protected:
    ~PageIf() = default;
};

// A page of the rotation (id < Page::Count) or a submenu (id == Page::Count). A submenu has a plain
// title bar, never consumes BACK (the Ui pops it) and labels [+]/[-] as scrolling, not paging.
class PageBase : public PageIf {
public:
    kit::OppRailSpec opp_rail() const override;
protected:
    PageBase(UiContext &c, Page id, rail::Title title) : ctx_(c), id_(id), title_(title) {}
    PageBase(UiContext &c, Page id, rail::Title title, Icon icon) : ctx_(c), id_(id), title_(title), has_icon_(true), icon_(icon) {}
    bool is_menu() const { return id_ == Page::Count; }
    void title_bar();
    // Right end of the top chrome bar: the compact V x A readout, the LAST pill when stale,
    // empty on Home (its hero carries the values).
    void title_badge(bool last);
    // Short [+] / [-] scroll `lv` down / up one row; declined when the list does not overflow.
    bool scroll_event(ListView &lv, Event e, uint32_t now_ms = 0);
    Strip base_labels() const;

    UiContext &ctx_;
    Page id_;
    rail::Title title_;   // the rail name (at most 4 characters, include/k2/ui/rail_title.hpp)
    bool has_icon_ = false;
    Icon icon_ = Icon::Bolt;
    bool bar_icon_ = false;    // a rotation page that shows its icon and title_ in the bar (a Bench place)
    bool bar_valid_ = false;   // the right half of the chrome bar matches bar_sig_
    uint32_t bar_sig_ = 0;
    ReadoutShown readout_shown_;   // what the right half shows, for a repaint of the changed cells only (title_badge)
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
    // Refuse = the "Cable connected" WARNING (the stage keeps its wire number): the Ra guard read a cable on the plug, so a spoof
    // may not reach the charger. Rows: 0 Start anyway (the identity runs armed, the guard's reading goes to the journal),
    // 1 Start, skip spoof (this run without the identity; the stored pick stays). BACK returns to Review.
    // Waiting = START's wait for the charger after the Rd went on (VBUS, then the caps): driven by poll(), never blocks;
    // BACK or OK cancels it.
    enum class Stage : uint8_t { Algorithms, Group, Cable, Identity, Review, Running, Results, Steps, Detail, Resume, Changes, Ask, Refuse, Waiting, Custom, SpecDetail };
    using Proposal = std::array<uint32_t, pd::kMaxPdos + 24>;
    explicit ChargerTestSession(UiContext& c) : ctx_(c), runner_(*this) {
#ifndef __arm__
        card_name_[0] = '\0';
        mfr_[0] = '\0';
        run_cable_name_[0] = '\0';
#endif
    }
    void enter();                  // the page is (re)entered: the wizard starts at Algorithms, or at Resume for a journaled run
    // The page's paint writes every content pixel (Ui::page_opaque): only the Algorithms list (rows from kBodyY, the empty
    // tail slots wiped, bar and footer whole) is proven so (tests/test_ui_capture.cpp walk). `entering` = enter() runs
    // before the paint, so the stage it paints is the one enter() picks, not the one held now.
    bool paints_opaque(bool entering) const { return entering ? !resume_pending_ : stage_ == Stage::Algorithms; }
    void leave();                  // the page is left: a run that is not a resume's Rd is stopped (= cancel())
    void poll(uint32_t);
    // Load an interrupted run from the journal; true = open the page in Resume. `cancel` = BACK was held at power-up: the
    // run is dropped. A run that was started on a charger-powered K2 resumes by itself once VBUS is steady (poll()).
    bool boot_restore(bool cancel = false);
    void cancel();
    [[gnu::noinline]] bool busy() const { return runner_.busy(); }   // 31 callers: one copy, not 31
    // The remote START (Cmd::ChargerTest [4, flags]): the device's own START, the second OK on Review included. It goes to
    // Review (the "All tests" preset first when asked), opens the confirm window through the first press's path (its preview
    // refusals come back at once), and poll() confirms it through the second press's path once the Ra guard's window is full
    // and the window still describes this proposal: the guard, the identity, the Rd, the wait and the final preflight are the
    // device's. Anything that drops a device confirmation (a key, a changed proposal, the window's end) drops this one too.
    struct RemoteStart {
        enum class Result : uint8_t { Accepted, Refused, Busy };
        Result result = Result::Accepted;
        charger_test::Reason reason = charger_test::Reason::None;   // Refused: why (the preflight's reason, as the footer words it)
        uint16_t refusal_mv = 0;                                    // ... and the VBUS it read
    };
    // A run, the wait, a resume or a remote START already holds the session: a START now is Busy and changes nothing.
    bool start_held() const { return runner_.busy() || stage_ == Stage::Waiting || resume_pending_ || remote_confirm_; }
    RemoteStart remote_start(bool all_tests);
    // The device's own stop (BACK on the page, the remote CANCEL), in BACK's order: what it did. The values are the remote's
    // kCtStop* bytes.
    enum class Stopped : uint8_t { Nothing, Run, Wait, Confirmation, Refusal, Resume };
    Stopped stop();
    // PD Stress borrows the deep-sweep point buffer for its record (Runner::claim_sweep_record): empty while a test runs.
    std::span<int16_t> claim_record() { return runner_.claim_sweep_record(); }
    const charger_test::Runner& results() const { return runner_; }
    void spec_apdo(pdspec::Check c, const pdspec::Meas& m) { runner_.spec_apdo(c, m); }
    void spec_apdo_clear() { runner_.spec_apdo_clear(); }
    uint32_t selection_mask() const { return options_.selected; }
    // The PPS deep sweep's point count for the source in hand (the Tests-list line's count); 0 = no PPS APDO: the line is hidden.
    unsigned sweep_points() const;
    // The AVS deep sweep's point count (SPR AVS, plus EPR AVS when the EPR list is in hand, capped at the monitor
    // ceiling); 0 = no AVS APDO offered: the line is hidden.
    unsigned avs_sweep_points() const;
    bool sweep_on() const { return options_.pps_sweep; }
    void set_sweep(bool on) { options_.pps_sweep = on; }
    bool avs_sweep_on() const { return options_.avs_sweep; }
    void set_avs_sweep(bool on) { options_.avs_sweep = on; }
    charger_test::Level level() const { return options_.level; }
    // Settings > Sweep depth: the deep sweeps' step. The draft keeps it (no other row changes it); the sweep rows' point counts follow it.
    ctsweep::Depth sweep_depth() const { return options_.sweep_depth; }
    void set_sweep_depth(ctsweep::Depth d) { options_.sweep_depth = d; invalidate_confirmation(); }
    void set_level(charger_test::Level l);   // with_level: the level and both deep sweeps; a START armed on the old plan is disarmed
    // Settings > Huawei/Samsung tests changed (or first read): the draft drops FCP, SCP and AFC, or takes them back when it was the default.
    void sync_legacy();
    // The options a plan is made from: the draft, plus the hidden units an explicit unit list names (remote runs still reach them).
    charger_test::Options plan_options() const;
    charger_test::Options journal_options() const { auto o = options_; o.selected |= run_extra_; return o; }   // the header names what the run was planned with
    // The SVOOC ceiling stage (opt-in, Cmd::ChargerSvoocCap): Off, On (the far-sink check stays) or UsbA (waived: a legacy A port).
    svooc_ceiling::Mode svooc_cap() const { return options_.svooc_cap; }
    void set_svooc_cap(svooc_ceiling::Mode m) { options_.svooc_cap = m; }
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
    // What the screens show, as data (Cmd::ChargerTest req [3]): the wizard stage and cursor, the START confirmation and the
    // Waiting / Running clocks, the Review preview, the resume and guard state, and the footer's words. The session half is
    // read from RAM, the preview is refreshed here (not by the page tick, so it is live with no page on screen), and the page
    // adds `rows` and `message` while it exists (on_screen). Reads only: nothing is armed, started or moved.
    struct View {
        uint8_t stage = 0, cursor = 0, rows = 0;
        bool on_screen = false, confirming = false, confirm_reattach = false, self_supplied = false, resume_pending = false,
             auto_resume = false, gate_ready = false, resume_held = false, behind_cable = false, resume_rd = false,
             wait_paused = false, wait_retry = false, armed_is_pick = false, rmdo_valid = false, cable_armed = false,
             legacy_shown = false;
        uint32_t confirm_left_ms = 0, wait_elapsed_ms = 0, wait_total_ms = 0, run_elapsed_s = 0;
        uint16_t card_no = 0, delta_runs = 0;
        uint8_t delta_n = 0;
        charger_test::Summary preview;
        unsigned pps_points = 0, avs_points = 0;
        uint8_t topology = 0, run_guard = 0, rmdo = 0;
        char message[28] = {};    // the footer's words (kFooterCells = 27 at most), NUL terminated
        uint8_t message_len = 0;
    };
    [[gnu::noinline]] View view();
    // The give-up clock's whole length: the first attempt of a wait with a charger attached (Rp seen) is longer.
    static uint32_t wait_total_ms();
    // The Review preview over the live reading; the page tick and view() share it (stages up to Review, and Custom).
    void update_summary(const charger_test::Input& in);
    void update_preset_counts(const charger_test::Input& in);
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
    // One Runner tick (runner_.poll with the Input it reads). Kept out of poll(): inlined there with its Input copy, the Runner's
    // own frame pushed poll() to 1056 B in the K2_PDSTRESS=1 build (over FRAME_MAX).
    [[gnu::noinline]] void runner_tick(uint32_t now);
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
    pd::CableLimits physical_cable() const;   // the driver's reading, else what this run read earlier
    pd::Err request_pd(pd::Choice) override;
    fc::Fail request_fc(fc::Proto, int, bool, int) override;
    void fc_keep(bool) override;
    void release(bool) override;
    uint8_t ceiling_start() override;   // svooc_trig::start with the Probe profile; 0 = running, else svooc::Refusal
    void ceiling_stop() override;
    uint8_t spec_start() override;      // PdSpecSession::start_in_test; 0 = running, else pdspec::StartResult (255 = not built)
    void spec_stop() override;
    void spec_collect(specsec::Result&) override;
    void spec_done(const specsec::Result&) override;   // the journal keeps the result (noted here, written by journal_pump)
    bool prog_begin(const charger_test::Step&) override;       // a Deep PPS / AVS hold starts: freeze the row's transition capture 400 ms on
    uint8_t prog_take(const charger_test::Step&) override;     // the frozen capture judged (progcheck::measure): the flag bits, 0 = clean or not measured
    bool spec_move(const pdspec::ProgMove&, uint8_t) override;   // PdSpecSession::test_move for this run
    bool spec_feeding() override;
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
    void amend(const charger_test::Step&) override;   // a row's reason changed after it ended (a fold marks its APDO's fixed rows)
    void log_row_gap(size_t idx, uint32_t now);   // the row's `[CT] gap` line: loop / ui gaps, paint cost, blocked-paint window
    void replan(unsigned follow, unsigned withdrawn, unsigned back) override;   // [TEST] re-plan: a mid-run re-announce was re-planned per row
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
    [[gnu::noinline]] void save_card_store(std::span<const uint8_t> blk);   // save_card's tail: the previous card, the write (with the Deep block) and the delta, on its own frame
    UiContext& ctx_;
    ChargerTestPage* view_ K2_BSS_INIT(nullptr);   // the painted half while it exists
    const char* fc_words_ K2_BSS_INIT(nullptr);    // the footer words of the handshake the last start() opened (null: that protocol has none)
    charger_test::Runner runner_;
    report_card::Identity card_id_ K2_BSS_ZERO;
    // The live inputs of the Results grid that are not part of the runner's stored result: the Xiaomi SVID seen on the wire, the
    // Mi auth state and the monitor ceiling. Frozen when the run ends (freeze_facts), so the grid built on every later entry of
    // Results is the one of the run, whatever the charger does afterwards (an unplugged source must not change its rows).
    struct ResultFacts {
        unsigned cap_mv K2_BSS_INIT(0);
        bool xiaomi_seen K2_BSS_INIT(false), mi_auth K2_BSS_INIT(false), frozen K2_BSS_INIT(false);
    } facts_;   // the one instance (`facts_ = {}` value-initialises, so a new run still clears it)
    void freeze_facts();       // snapshot the live inputs; a run's start clears it (capture_identity)
    report_card::Card card_ K2_BSS_ZERO;   // the run's card while save_card builds and stores it; a member, not a stack copy (the save path is stack-gated, tools/stack_depth.py)
    std::array<report_card::DeltaLine, 8> delta_ K2_BSS_ZERO;
    size_t delta_n_ K2_BSS_INIT(0);
    charger_journal::ResumeGate gate_;
    // Journal writes a row in flight must not do (Runner::quiet()): the section files that hold a decided row or a replanned
    // plan are noted dirty in the journal itself. A write holds the main loop (and the PD engine) for 5 to 250 ms, so none is done
    // in one go: journal_pump() does ONE file per tick behind the row, and only while the PD link has been quiet (see record()).
    uint32_t jr_wait_ms_ K2_BSS_INIT(0);               // when the pending records began to wait for a quiet link (0 = nothing waiting)
    uint32_t arm_t0_ K2_BSS_INIT(0);
    void claim_step(uint32_t now);
    void decide_reattach();
    charger_journal::RunMeta arm_meta_ K2_BSS_ZERO;
    void journal_pump(uint32_t now);        // one file at most per call; there is no drain
    void pump_journal();                    // hw::pump (K2_SPECROWS: with the spec rows' source, the engine while it is this run's)
#if K2_SPECROWS
public:
    // The spec rows file of the run in hand, as the journal writes it (specsec::kRowsHdr + rows): its size (0 = none, or not this run's)
    // and a piece of it, from the engine while it holds this run's records, else from the journal. The remote reads it in chunks.
    uint16_t spec_rows_len() const;
    size_t spec_rows_read(uint16_t off, std::span<uint8_t> out) const;
private:
    uint32_t spec_live_run_ K2_BSS_INIT(0);            // the run whose spec result the PD spec engine still holds (spec_done); 0 = none
#endif
    void arm_step(uint32_t now);
    void arm_abort();
    ctcable::Choice resume_cable_ K2_BSS_ZERO;     // the journaled identity of that run, re-armed by reapply_cable()
    uint32_t resume_judged_ K2_BSS_INIT(0);         // when the guard last judged (and logged) for the resume
    // The draft starts at Medium (no deep sweeps); boot applies the stored Settings > Test level (Ui::charger_test_set_level),
    // Review's Level row and the remote (Cmd 0x5B) change it, and Deep turns both deep sweeps on (with_level). The Tests list
    // lines and the remote knob (charger-sweep, Cmd 0x0F) still toggle each sweep on its own after that.
    // The depth starts at 200 mV (the Options struct keeps Step100 = 0, what a journal without the field ran); boot applies the stored one.
    charger_test::Options options_ = [] { charger_test::Options o; o.sweep_depth = ctsweep::Depth::Step200; return o; }();
    uint32_t run_extra_ K2_BSS_INIT(0);          // hidden-unit algorithm bits the run in flight was planned with (not in the draft; the journal header keeps them)
    bool legacy_hidden_seen_ K2_BSS_INIT(false);   // what sync_legacy() last applied to the draft (the draft starts as shown)
    charger_test::Summary summary_;
    // The Tests list's preset counts (planned_tests at the chosen depth), kept until the depth, the source list or the cable changes.
    std::array<uint8_t, 4> preset_n_ K2_BSS_ZERO;
    uint32_t preset_key_ K2_BSS_INIT(0);
    uint32_t run_t0_ K2_BSS_INIT(0);                  // when the run entered Running (the m:ss on screen)
    StrBuf<32> message_;
    int selection_ K2_BSS_INIT(0), family_ K2_BSS_INIT(0), result_family_ = -1, detail_index_ K2_BSS_INIT(0), detail_row_ K2_BSS_INIT(0);
    // Layout B: the Steps page of a range line lists the range's spec checks after its steps (spec_range_: the APDO's position, 0 =
    // none). SpecDetail shows a merged cell (spec_pos_ 0, detail_index_ = its kOrder index) or slot spec_slot_ of range spec_pos_;
    // spec_back_steps_: BACK returns to the Steps row it came from.
    uint8_t spec_range_ K2_BSS_INIT(0), spec_pos_ K2_BSS_INIT(0), spec_slot_ K2_BSS_INIT(0);
    bool spec_back_steps_ K2_BSS_INIT(false);
    // The Steps page opened from a PD Results row lists that row's steps only (picked(): result_layout::row_has on this copy of the
    // row, its range or fixed levels); any other row lists its protocol family (result_family_, -1 = all).
    result_layout::CompactRow result_row_ K2_BSS_ZERO;
    char result_label_[10] K2_BSS_ZERO;
    const result_layout::CompactRow* picked() const;   // result_row_ while the Steps page lists a PD row, else nullptr
    bool shows(const charger_test::Step&) const;
    // The identity the next START presents lives in ctx_.cables (the list Tools -> Spoof shares), not here.
    ctcable::Choice run_cable_ K2_BSS_ZERO;        // the identity of the current / last run
    ctcable::Facts run_facts_ K2_BSS_ZERO;         // its VID / PID / limits, for the report card
    // Running: the verdict grid follows the step under test (follow_), anchored to the bottom: the running row ends the window
    // (ResultGrid::anchor_bottom), the finished rows stay above. PLUS / MINUS scroll it by hand (follow_ = false, the
    // line the user chose in scroll_line_); the follow resumes kFollowResumeMs after the last press or when the step changes.
    int scroll_line_ K2_BSS_INIT(0);
    uint32_t scroll_ms_ K2_BSS_INIT(0);
    size_t scroll_step_ K2_BSS_INIT(0);
    pd::VbusWait wait_;              // Waiting: the VBUS / caps judgement
    uint32_t wait_t0_ K2_BSS_INIT(0);               // when the press began the wait (the elapsed time on screen and in the log)
    uint32_t wait_slow_ K2_BSS_INIT(0);             // the last attach scan (every pd::kStartWaitSlowMs)
    waitplus::PlusWatch plus_watch_;     // paces the [WAKE] lines while + is held in Waiting (and 2 s after)
    uint32_t wait_pause_until_ K2_BSS_INIT(0);      // the 1 s Rd-off between the two attempts ends here
    raguard::PeakWindow guard_;          // CC peaks over the last ~0.8 s while Cable / Review is open (a PLUS press sags CC2)
    uint32_t guard_t_ K2_BSS_INIT(0);               // when the last sample went into guard_
    uint32_t reattach_t0_ K2_BSS_INIT(0);
    void reattach_begin(uint32_t now);
    void reattach_step(uint32_t now);
    void reattach_drop();
    uint32_t confirm_until_ K2_BSS_INIT(0);
    Proposal confirm_proposal_;
    uint16_t card_no_ K2_BSS_INIT(0), delta_runs_ K2_BSS_INIT(0);   // saved card number (0 = none), runs before this one
    char card_name_[24];       // saved card name of this run, the Results heading
    char mfr_[13];             // Manufacturer_Info name from the source probe, only used to name the card
    char run_cable_name_[ctcable::kNameLen];
    Stage stage_ K2_BSS_INIT(Stage::Algorithms);   // 0
    raguard::Topology topology_ K2_BSS_INIT(raguard::Topology::Unknown);   // 0   // the guard's last reading (the Warning screen, the run's journal)
    // The run's journal is armed (its whole plan written, header last) in pieces too, one write per tick on a quiet link, while the
    // Runner is held in its first Baseline: arm_ 1 = rows and header, 2 = the run's meta record (one more write), 3 = the planned
    // re-attach's marker and Rd gap (one more write), 4 = the source's own claim is awaited before the blink is decided (no flash
    // write, bounded by kClaimWaitMs), 0 = done.
    uint8_t arm_ K2_BSS_INIT(0);
    // The physical cable limits the run has read: the driver forgets them at every attach (a new session per unit, one per source Hard
    // Reset), the run keeps them (the charger's own SOP' query answered by the real cable is read once, in one unit).
    mutable uint16_t seen_cable_mv_ K2_BSS_INIT(0);
    mutable uint16_t seen_cable_ma_ K2_BSS_INIT(0);
    mutable bool seen_cable_ K2_BSS_INIT(false);
    uint8_t run_guard_ K2_BSS_INIT(0);                // the Ra guard's reading of the run in flight, journal / card encoding (RunMeta::guard)
    uint8_t reattach_ K2_BSS_INIT(0);
    bool journal_active_ K2_BSS_INIT(false), resume_pending_ K2_BSS_INIT(false);
    bool journal_closed_ K2_BSS_INIT(false);   // closing() already wrote this run's Finish (poll() need not write it again)
    bool capture_kept_ K2_BSS_INIT(false);          // the frozen transition capture already belongs to a failed row: a later failure (another unit) must not take it over
    bool reattach_ask_ K2_BSS_INIT(false);             // the run's static conditions hold: the source's claim decides the blink (arm_ 4)
    bool claim_asked_ K2_BSS_INIT(false);              // the claim wait started its probe (Get_Source_Cap_Extended and the rest of the quick probe)
    bool self_run_ K2_BSS_INIT(false);              // the charger under test powers this K2 (a key-mode boot): the run keeps VBUS up, its card says so
    bool auto_resume_ K2_BSS_INIT(false);          // the journaled run was unattended (charger-powered K2): resume without the OK press
    bool resume_rd_ K2_BSS_INIT(false);             // the resume presented the Rd (host-powered K2): wait_ carries the wait for the charger
    bool resume_hold_ K2_BSS_INIT(false);           // BACK, or the identity could not be armed: no Rd is presented until OK asks again
    bool resume_refused_ K2_BSS_INIT(false);        // the Ra guard says the charger is behind a cable: the journaled identity cannot be armed
    bool dialog_spoof_ K2_BSS_INIT(false);          // the run presents the spoof (release_run lets go of it)
    bool spoof_user_ K2_BSS_INIT(false);            // ... but the user had armed that very identity in Tools first: release_run leaves it armed
    bool dialog_ra_ K2_BSS_INIT(false);             // the dialog turned the Ra mode on (release_run turns it off)
    bool follow_ = true, confirming_ K2_BSS_INIT(false);
    bool wait_retry_ K2_BSS_INIT(false), wait_paused_ K2_BSS_INIT(false);   // the second attempt is running / the Rd is off between the two
    bool guard_bypass_ K2_BSS_INIT(false);           // "Start anyway": the guard's reading is recorded, the spoof runs regardless
    bool skip_spoof_run_ K2_BSS_INIT(false);         // "Start, skip spoof" on the Refuse stage: this run carries no identity, the stored pick stays
    // The planned re-attach (rdintent::plan_reattach): a self-powered START with a 6 A identity against a source whose offer the
    // cable holds down (rdintent::offer_held_by_cable: below its own claim, nothing above 3 A) makes the K2 blink off once, so the
    // charger reads the cable at a real attach. confirm_reattach_ = the confirmation on screen announces it (only when the
    // source's claim is already in hand); reattach_plan_ = this run does it (decided in the arm, claim_step); reattach_ 1 = the Rd is off for the gap, 2 = the Rd is
    // back and the charger's new contract is awaited (the Runner stays held in its Baseline until then or until the K2 died).
    bool confirm_reattach_ K2_BSS_INIT(false);
    bool reattach_plan_ K2_BSS_INIT(false);
    bool remote_confirm_ K2_BSS_INIT(false);   // a remote START's confirm window: poll() confirms it once the Ra guard's window is full
    bool entering_ K2_BSS_INIT(false);       // enter() is switching the stage: nothing of this page is on screen yet
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
    Strip labels() const override;
    void invalidate() override;
    void invalidate_for_capture() override;
    kit::OppRailSpec opp_rail() const override;
    void leave() override { s_.leave(); }
    // Repaint hooks the session calls while this page is alive.
    void on_stage(bool entering);          // the stage changed: wipe the body (not while entering), drop the caches
    void on_message() { foot_.invalidate(); foot_sig_ = 0; }
    void announce(const char* words);      // a handshake window is about to open: its footer line, painted now (Running only)
    void on_confirmation_dropped();        // the confirm window ended: the footer and START cell show it again
    void on_cable_changed();               // the Cable choice moved from outside: repaint it
    ListState list_state() const;
    void describe(ChargerTestSession::View& v);   // the page's half of the view: the list's row count and the footer's words
    // The paint budget of a Running update (every 100 ms) while a step is in flight (Runner::quiet()). Pixels are the measure:
    // the panel streams ~2.13 us per pixel plus ~15 us per window, and lcd.cpp services USB, FUSB302 and the UFCS handshake
    // once 1 ms has passed since the last round (lcd::kServiceUs, elapsed time) inside a stream. The items (gauge, hero + needle, clock, texts, progress, footer) share
    // kFlightPx (~13 ms); a grid row, the one item that can exceed it, paints alone on a tick where nothing else did, up to
    // kFlightRowPx (the tallest row, 224 x 42 px, plus the strike-throughs of its Skip cells painted over their text, at most
    // kit::kStrikeRowMaxPx, plus a Running cell's outline, kit::kMarkRowMaxPx, ~21 ms). Between steps (and while laying out)
    // there is no cap. The gauge band and the step outline are one pair: the gauge is item 1, the grid's rebuild and
    // ResultGrid::sync_dots (two cell rims a move, never gated) come right after it, so on a step change they move in one tick.
    static constexpr int kFlightPx = 9200;
    // The one tick that may paint more: the start of a row whose engine times its own handshake. The session (not the update) paints
    // the footer line that names the window, kAnnouncePx (the footer band, 224 x 18), after that tick's update and before the
    // engine starts; the screen is still for the next >= 1 s anyway (Runner::paint_blocked). Counted against nothing else.
    static constexpr int kAnnouncePx = 208 * 16 + 6000;   // the pill's erase, frame and text overdraw before a timed engine window
    static constexpr int kFlightRowPx = 208 * 42 + kit::kStrikeRowMaxPx + kit::kMarkRowMaxPx;
    static constexpr int kHeroPx = 5000;         // the 4-digit 48 px readout (4 x 24 x 48) plus its unit
    static constexpr int kNeedlePx = 200;        // the old and the new needle column and the window edge they cross
    static constexpr int kRangePx = 36 * 12;     // a sweep row's gauge move: the old and new marker (6 columns each) and needle (2 each)
    // a sweep row's error hero: three 48 px digit cells, the 24 px unit, the sign (erase + draw), the act and set values (six 24 px cells each)
    static constexpr int kSweepHeroPx = 3 * 24 * 30 + 2 * 12 * 24 + 2 * 12 * 24 + 2 * 6 * 12 * 24;
    static constexpr uint32_t kHeroMs = 250;     // the hero and the needle repaint at most this often in flight (~4 Hz)
    static constexpr uint32_t kSweepHeroMs = 500;   // ... a sweep row's, with the act / set pair, twice a second
    static constexpr uint32_t kFollowResumeMs = 5000;   // a manual scroll of the grid holds this long, or until the step changes
private:
    ChargerTestSession& s_;
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
    unsigned sweep_points() const { return s_.sweep_points(); }
    int step_index(int row) const { return s_.step_index(row); }
    void share_card();
    void ok_event();
    void sync_proposal(uint32_t now);              // drop a stale confirm window, refresh the Review summary
    static void row_fn(void*, int, bool, kit::RowSpec&);        // the RowList stages
    static void result_row_fn(void*, int, kit::ResultRowSpec&); // the Running / Results grid
    void bar();                                     // top bar: the wizard progress or the plain title
    bool foot(FooterTone, std::string_view);        // the footer through the page's one cache
    [[gnu::noinline]] FooterTone footer_line(StrBuf<32> &);   // the stage's footer text; returns its tone
    [[gnu::noinline]] void paint_list();            // RowList stages: bind once, then selection and rows
    [[gnu::noinline]] void paint_review();
    [[gnu::noinline]] bool review_kv(int i, const char *key, std::string_view v, uint16_t color);
    [[gnu::noinline]] void paint_warning();
    [[gnu::noinline]] void paint_waiting(uint32_t now);
    [[gnu::noinline]] void paint_running(uint32_t now);
    [[gnu::noinline]] void paint_results();
    [[gnu::noinline]] void paint_detail();
    [[gnu::noinline]] void paint_spec_detail();    // one PD spec check: its seven summary rows (K2_SPECROWS: a list of its transitions first)
#if K2_SPECROWS
    // The open check's per-transition rows: how many, where they start in the journal's file, the worst one, how many fail. Taken when the
    // stage opens (on_stage); n < 2 (one measurement, none, or no rows file) keeps the summary table.
    struct SpecList { uint16_t n = 0, first = 0, fails = 0; uint8_t worst = 0; bool live = false; };
    SpecList spec_list_{};
    void spec_list_load(int k);
    bool spec_list_row(int i, specsec::Row& r) const;   // specsec::Row: chargertest/spec_rows.hpp (included by the .cpp)
    bool spec_list_on() const { return s_.stage_ == Stage::SpecDetail && spec_list_.n >= 2; }
#else
    static constexpr bool spec_list_on() { return false; }
#endif
    [[gnu::noinline]] void paint_resume();
    [[gnu::noinline]] void paint_hero(bool full, int16_t x, int16_t y, uint8_t chars, int32_t vbus_mv, uint16_t color);
    [[gnu::noinline]] void paint_sweep_hero(int32_t vbus_mv, uint16_t set_mv);   // Running, a sweep row: the signed error and the act / set pair
    [[gnu::noinline]] void draw_sweep_gauge(int16_t y, int32_t vbus_g, const ctsweep::Live& live, int32_t full, bool fresh);
    // Running's static layout, plain or the sweep row's restack; a switch wipes the body below the progress bar first.
    [[gnu::noinline]] void lay_running(bool sweeping, bool wipe);
    void list_row(int, bool, kit::RowSpec&);   // row_fn's body (fills the page's shared text buffers, consumed at once)
    void grid_row(int, kit::ResultRowSpec&);
    // Build the grid's layout and lines into layout_ / lines_. Results: once per entry of the stage, from the stored run and
    // the frozen facts. Running: the light layout of the plan, only when the runner's revision or step changed (grid_key_).
    [[gnu::noinline]] void build_grid(bool running_grid, bool note_cuts = false);
    [[gnu::noinline]] void draw_capture(const k2::transcap::Meta&);   // VBUS thumbnail of the step's stored transition capture
    int last_row() const;          // the last row the cursor may rest on
    int row_count() const;
    // The PD spec section (Deep runs, K2_PDSPEC): rows after the unit rows in the Results grid, then the Changes row.
    int spec_n() const;            // rows shown (0 = no section)
    // Layout B: which merged rows the range lines replace (computed with the grid), the cell of shown row k, the open check.
    specsec::Split split_{};
    size_t spec_cell(int k) const;
    bool spec_open(pdspec::Check &c, specsec::Cell &cell) const;
    const specsec::Apdo *spec_range() const;   // the Steps page's range, when it has spec checks to list
    int range_spec_n() const;                  // ... and how many (0 = none)
    uint8_t line_range_pos(int line) const;    // the APDO position of a Results line's range chip (0 = not a range line)
    int delta_row() const { return static_cast<int>(line_n_) + spec_n(); }   // the Changes pseudo-row, when there is one
    void build_results();      // chips layout of the stored run (k2/chargertest/result_layout.hpp), wrapped into display lines
    // The summary line of Results: the best passing mode per protocol family of the stored run ("PD 20V 3A  QC3 12V").
    void compose_best();
    // The verdict grid's compact descriptors and display lines live in the page, never in scratch: painting streams to the LCD,
    // whose idle hook runs other work, and a scratch lease must not span that. Step-backed rows regenerate tokens on demand.
    result_layout::CompactGrid layout_;
    std::array<result_layout::CompactLine, result_layout::kMaxLines> lines_{};
    StrBuf<32> best_;
    bool best_none_ = false;   // no mode passed: best_ is the PD version and "No passing mode", drawn dim
    size_t line_n_ = 0;
    int results_row_ = 0;      // selected line to restore when BACK leaves the step list
    uint8_t hdr_first_ = 0, hdr_n_ = 0;   // Steps: the picked row's lines shown as the header (its first line, at most two)
    kit::ActionCell action_;               // Review: START / CONFIRM
    kit::ProgressBar prog_;                // Running: steps done of all
    kit::ResultGrid grid_;                 // Running and Results: the verdict grid
    Field hero_;                           // Waiting and Running: the live VBUS digits (a sweep row: the signed error)
    Field act_;                            // Running, a sweep row: the act value beside the error hero
    Elem foot_;                            // footer cache (draw_footer_cached)
    Elem body_;                            // the static part of a dense stage (laid_)
    uint32_t cell_sig_[12] = {};           // dense stages: per-line caches (Waiting lines, Running texts); 0 = not drawn
    uint32_t grid_key_ = 0;                // Running: the runner revision / step the grid was built for
    uint32_t hero_ms_ = 0;                 // when the hero last painted (Running while quiet: 2 Hz)
    CableIdentityList::Row row_;           // the Cable list's row being painted (RowSpec points into it)
    StrBuf<40> row_text_;                  // shared row-text buffers: row specs are consumed at once (docs/UI_DESIGN.md)
    StrBuf<16> row_tail_;
    StrBuf<48> row_note_;
    int needle_px_ = -1;                   // the gauge's needle column, -1 = the strip is not drawn
    int mark_px_ = -1;                     // a sweep row's gauge: the setpoint marker's column, -1 = not drawn (needle_px_ is its needle)
    uint32_t foot_sig_ = 0;                // Running: the footer (tone + text) last painted; 0 = not drawn
    uint32_t hero_sig_ = 0;                // Waiting: the cached VBUS + color signature; 0 = not drawn
    uint16_t hero_color_ = 0;              // the hero's colour on screen (a sweep row: its grade), 0 = not drawn
    uint16_t act_color_ = 0;               // a sweep row's act value colour on screen
    int16_t sign_x_ = -1;                  // a sweep row's error sign cell on screen (left edge), -1 = none
    uint32_t sign_sig_ = 0;                // ... and what it shows (sign + colour)
    bool hero_volts_ = false;              // a sweep row's error hero reads in V, not mV
    int16_t clear_y_ = kFooterY;           // progressive stage-transition wipe cursor (< kFooterY = clearing)
    char cell_txt_[kit::kResultMaxCells][2][14] = {};   // the verdict grid's cells: volts and current text
    char cell_lbl_[10] = {};               // ... and the unit label of the row
    StrBuf<24> spec_txt_;                  // ... and a PD spec row's wide cell ("-327.66V, 255 fail")
    uint8_t bar_key_ = 0;                  // the top bar drawn for this stage + 1 (0 = not drawn)
    uint8_t detail_step_ = 0;
    bool laid_ = false;                   // the stage's static part is on screen
    bool sweep_laid_ = false;             // Running: the layout on screen is the sweep row's (Layout kSweep), not the plain one
    bool name_small_ = false;             // Running (plain): the step name is drawn in the 12 px font (it outgrew the left column)
};

// The Home protocol row's source: the one derive_proto call site. Home paints from it on every live frame, and the
// remote session block reads it through Ui::proto_now(), so the label and the limit are never derived twice.
namespace home {
ProtoInfo live_proto(const UiContext &ctx, const power::Metrics &pm);
}

// Home 2c (user decision 2026-11-12): the walk-by page, the whole 224 x 240 content (no title bar, no footer band). The
// hero cell y0..73 (W, V or A at 64 px over a faint patient-monitor sweep, status edge, badge / alarm line, contract
// bar), the ledger rows x0..139 y76..134 / y136..193 (the two other quantities at 48 px, each with its sweep), the two
// right-column cards x141..223 the cycler picks (Wh/Ah, PEAK, DROP + SAG, CABLE, LINES), the info block y196..239 (PD
// diagnosis, protocol row). The pure parts are home_model / home_cards / home_cell / home_proto; this class owns the
// state and decides what to repaint (src/ui/ui_home.cpp). OK short = trend window, OK long = reset (energy, peaks, sag,
// cycler), Back long = the next hero.
class HomePage final : public PageBase {
public:
    explicit HomePage(UiContext &c) : PageBase(c, Page::Home, rail::kNone) {}
    void draw_static() override;
    void update(uint32_t now_ms) override;
    bool on_event(Event e) override;
    Strip labels() const override;
    void invalidate() override;
    struct ZoneRect { int16_t x0, y0, x1, y1; };
private:
    struct Frame;   // everything one update decides before it draws (ui_home.cpp)
    void sample_values(Frame &f, uint32_t now_ms);
    void update_sweep(const Frame &f, uint32_t now_ms);
    void paint_statics();                 // after an invalidate / mode / hero change: the digit fields, every cell dirty
    void paint_qty(int slot, home::Qty q, const Frame &f);   // slot 0 hero, 1 / 2 ledger rows
    struct QtyHead;   // the head line's texts (ui_home.cpp); the items view them while the cell paints
    // The head line of a quantity cell: the label or the alarm line, the span, the pill or the max badge, appended to
    // `it` at `n`; returns the head's signature. Kept out of paint_qty so its text helpers' locals are not on the
    // stack while the cell paints.
    [[gnu::noinline]] uint32_t qty_head(int slot, home::Qty q, const Frame &f, QtyHead &h, home::Item *it, int &n);
    [[gnu::noinline]] void paint_box(int cell, const Frame &f);    // 0 left, 1 middle, 2 right (3 x 74 px boxes)
    [[gnu::noinline]] void paint_info(const Frame &f);      // the info block: one compositor cell, three repaint zones
    [[gnu::noinline]] bool paint_cell_sliced(int slot_id, uint32_t base_sig, const home::Cell &cell,
                                             int16_t x0, int16_t y0, int16_t x1, int16_t y1);
    [[gnu::noinline]] bool commit_zoned_cell(int slot_id, uint32_t base, uint32_t zone_seed,
                                             const home::Cell &cell, const int *zone_start,
                                             const ZoneRect *zones, int nz,
                                             bool &valid, uint32_t *old_sig, uint32_t *slice_zones,
                                             int16_t cx0, int16_t cy0, int16_t cx1, int16_t cy1,
                                             uint32_t *saved_base = nullptr);
    struct QtyCommit;
    [[gnu::noinline]] void commit_qty_cell(int si, const QtyCommit &q, const home::Cell &cell);
    // Display filter: V, A, W digits and the box values change at most 4 times a second (one throttle, one set).
    Throttle thr_{250};
    int32_t v_mv_ = 0, a_ma_ = 0, w_mw_ = 0;         // |values| as shown
    power::Flow flow_ = power::Flow::Idle;
    struct CardData {                                 // the box & peak values as shown (snapshot with the digits)
        uint32_t peak_mw = 0, peak_mv = 0, peak_ma = 0, sag_bp = 0, sag_level_mv = 0;
        bool has_sag = false;
        home::Drop drop{};
        int32_t lines[4] = {-1, -1, -1, -1};          // CC1, CC2, D+, D- in mV (-1 = no reading)
    } card_;
    uint8_t mode_ = 0xFF;                             // home::Mode last seen (not reset by invalidate); 0xFF = none yet
    home::Qty shown_hero_ = home::Qty::W;             // the hero the cells were laid out for
    bool statics_ok_ = false;                         // paint_statics() ran since the last invalidate / mode / hero change
    bool info_ok_ = false;                            // the info cell holds what info_sig_ says
    uint32_t info_sig_[3] = {};                       // per zone: info row 1, elapsed, info row 2
    trend::Window &window_ = ctx_.keep.home_window;   // remembered across page switches
    // The sweeps survive invalidate() (a capture re-renders strips of one frame); they are reloaded from the
    // trend after a mode or hero change, a window or scale change, or a long gap (page left, stalled loop).
    home::Sweep sw_[3];                               // per slot: hero (110 columns), sub-heroes (52 each)
    bool sweep_ok_ = false;
    uint32_t sweep_t_ = 0;
    uint32_t sweep_seen_ = 0;                         // the `now` of the last update: a replay of one frame never steps
    uint32_t sweep_scale_ = 0;
    uint16_t sweep_epoch_ = 0;                        // bumped on a reload: the cells repaint whole
    home::DigitField hd_[3];                          // per slot
    uint32_t cell_sig_[3][4] = {};                    // per slot: base, head, arrow, bar signatures
    bool cell_valid_[3] = {false, false, false};
    uint32_t box_base_[3] = {};                       // per box cell: kind, mode, glow, rotation
    uint32_t box_sig_[3][3] = {};                     // per box cell: zone signatures (label, row1, row2)
    bool box_valid_[3] = {false, false, false};
    // Row cursor & base signature per sliced compositor cell (0..2 qty, 3..5 boxes, 6 info).
    int16_t slice_y_[7] = {-1, -1, -1, -1, -1, -1, -1};
    uint32_t slice_sig_[7] = {};
    int8_t painted_cursor_[3] = {-1, -1, -1};
    uint8_t slice_dirty_ = 0;
    uint8_t rr_slot_ = 0;                             // round-robin cursor across 7 home surfaces (prevents starvation)
    uint32_t slice_parts_[3][3] = {};                 // head, arrow, bar signatures when qty slice started
    uint32_t slice_box_[3][3] = {};                   // zone signatures when box slice started
    uint32_t slice_info_[3] = {};                     // zone signatures when info slice started
};

// Detail page: every measured value as small text (D+/D-, CC1/CC2, contract, cable, protocol, R, temperature,
// energy, timer, flow). Pure rows are home_*/detail_* above; this class only paints them.
class DetailPage final : public PageBase {
public:
    explicit DetailPage(UiContext &c) : PageBase(c, Page::Detail, rail::kNum, Icon::Meter) {}
    void draw_static() override;
    void update(uint32_t now_ms) override;
    bool on_event(Event) override { return false; }   // view-only in the ring
    Strip labels() const override { return base_labels(); }
    kit::OppRailSpec opp_rail() const override { return {rail::kNum, 0, 0, kit::OppTelemetry::PeakSagR}; }
    void invalidate() override;
private:
    struct CellState { uint16_t fg = kText; bool valid = false; char cur[10] = {}; };
    void reset_cells();
    [[gnu::noinline]] bool put(int slot, std::string_view sv, uint16_t color);
    void paint_flow(bool live, power::Flow flow);
    static constexpr int kFields = 19;   // V, A, W, then 8 rows x 2 receipt cells (Flow paints itself)
    CellState f_[kFields];
    uint8_t flow_sig_ = 0xFF;
    uint8_t static_step_ = 0;
};

// The live verdict the PD page and the remote PD_MATCH command both show (passive reads only).
MatchResult match_live(const pd::Link &link);

// Counts match_live() calls; host tests (tests/test_ui_perf.cpp) read it to prove the PD page's
// cheap pre-check skips the expensive path when nothing changed. Static storage, never read on device.
extern uint32_t g_match_live_calls;

// Counts ConnectionPage::paint_body() passes; host tests (tests/test_ui_conn_flicker.cpp) read it to prove
// a steady link produces 0 body passes over 10 s (the "[CONN] body full=%d rows=%d" log on device).
// Static storage, never read on device.
extern uint32_t g_conn_body_passes;

// What the passive readers say right now: the one implementation behind conn::Facts. The Connection page paints
// from it (its own peak window); the remote session block calls it through Ui::passive_facts() with a one-shot
// window. The topology, the waiting classifier (pd::Link::wait_kind), the crossed-CC verdict and the standby
// evidence (open_cc_mv) are never computed in two places. It reads only; nothing here drives CC.
namespace conn {
void sense_passive(Facts &f, const UiContext &ctx, const pd::Link &link, const raguard::PeakWindow &guard);
}

// Connection (Tools): the three parties on the two ports as tabs, Source / Cable / Sink, with a status bar
// at the bottom and a small action pop-up on OK (ui_connection.hpp holds the pure model). Short +/- switch
// tabs (or move in the pop-up), OK opens the pop-up, long OK shares, BACK closes the pop-up or leaves.
// Passive: it only reads what the engine already knows; its actions go through Ui::run_action and the
// existing inline confirm. The empty states read the port topology from the CC levels the ADC already
// averages (raguard::classify) and never start an active CC operation.
class ConnectionPage final : public PageBase {
public:
    explicit ConnectionPage(UiContext &c) : PageBase(c, Page::Count, rail::kCharger, Icon::Plug) {}
    void enter() override;
    void draw_static() override;
    void update(uint32_t now_ms) override;
    bool on_event(Event e) override;
    Strip labels() const override;
    void invalidate() override;
    void leave() override;
    ListState list_state() const { return view_ == View::Actions ? ctx_.rows.state() : ListState{}; }
    conn::Tab tab() const { return tab_; }
    // Unified 3-slice Link page:
    //   Overview: 3 horizontal slices (Charger / Cable / Sink) on full 208x238 height; direct slice focus when entered.
    //   Actions:  compact slice header + pinned top "Reset & re-read" section + slice-specific entries in kit::RowList.
    //   Spec:     full scrollable fact table for the slice (Charger PDOs/Details, Cable VDOs/R, Sink Request/Caps).
    enum class View : uint8_t { Overview, Actions, Spec };
    void set_overview(conn::Tab focus = conn::Tab::Source);
    void lock(conn::Tab t);
    bool has_pages() const { return n_items_ > 0; }
    int first_act() const { return 0; }
    int act_count() const { return n_items_; }
    void set_actions_mode(bool on);
    View view() const { return view_; }
    bool overview_mode() const { return view_ == View::Overview; }
    bool actions_mode() const { return view_ == View::Actions; }
    bool spec_mode() const { return view_ == View::Spec; }
    // The page is what the ring view shows of it as it stands: 3-slice Overview, nothing of its own in flight.
    bool ring_rest() const {
        return view_ == View::Overview && !em_read_pending_ && !id_asked_ && !iq_asked_ &&
               !(reset_armed_until_ && int32_t(reset_armed_until_ - ctx_.now) > 0) &&
               !(op_until_ && int32_t(op_until_ - ctx_.now) > 0);
    }
    int action_row() const { return act_sel_; }   // the focused action row on Actions page, -1 when not in Actions
    static constexpr int kPlaceRows = 9;   // 9 x 21 px fact rows under identity block (y 46..235, no footer)
    kit::OppRailSpec opp_rail() const override {
        if (view_ == View::Overview) return {rail::kLink, 0, 0, kit::OppTelemetry::Vaw};
        return {title_, 0, 0, kit::OppTelemetry::Vaw};
    }
    int window_first() const { return first_; }   // the first list row on screen
    int result_rows() const { return res_n_; }    // the fact rows of the last paint (the actions follow)
    conn::Facts facts() const { return facts_; }
    int item_count() const { return n_items_; }
    const conn::Item &item(int i) const { return items_[static_cast<std::size_t>(i)]; }
    std::string_view status_line() const { return status_txt_.view(); }   // what the bottom bar says
    // The one "PD present" verdict of this attach (ROADMAP 2026-11-10, Row C): cached in sense(), read by
    // build_items/paint_body and by the host tests.
    pd::PdVerdict pd_verdict() const { return verdict_; }
    pd::PdNoneWhy pd_none_why() const { return none_why_; }
    // A short note on the status line (what an action did, or what a reboot carried over).
    void say(const char *text, uint32_t now_ms, uint32_t ms = 4000) { set_op(text, now_ms, ms); }
private:
    void sense(uint32_t now_ms);           // topology window + facts, once per update
    void auto_wake(uint32_t now_ms);       // Refresh's wake + read, by itself, for a source asleep on the Charger overview
    void build_items();
    void enter_overview(conn::Tab t);
    void enter_actions();
    void enter_spec();
    void exit_actions();
    void activate(const conn::Item &it, uint32_t now_ms);
    void set_op(const char *text, uint32_t now_ms, uint32_t ms = kToastDefaultMs);
    void share_caps(uint32_t now_ms);
    void share_cable(uint32_t now_ms);
    void read_cable(uint32_t now_ms);      // the cable read of Read e-marker; a good read opens the chip details
    void read_failed(uint32_t now_ms);
    void paint_status(uint32_t now_ms);
    [[gnu::noinline]] bool paint_overview(const MatchResult &r, const pd::SourceCaps &caps, std::span<const uint32_t> sink);
    bool paint_actions_ident(const MatchResult &r, const pd::SourceCaps &caps, std::span<const uint32_t> sink);
    bool paint_body(const MatchResult &r, const pd::SourceCaps &caps, std::span<const uint32_t> sink, bool redraw);
    [[gnu::noinline]] bool paint_body_locked(const void *tab_data);
    [[gnu::noinline]] bool paint_no_pd_body(const MatchResult &r, bool redraw);
    [[gnu::noinline]] bool paint_no_pd_locked(const char *line, uint32_t rp_ma);
    // The no-PD gate (ROADMAP 2026-11-10, Row C): the Sink tab on any no-PD verdict with a device on the line, the Source tab
    // only when the source never offered PD. The Cable tab never.
    bool no_pd_body() const {
        return verdict_ == pd::PdVerdict::None &&
               (tab_ == conn::Tab::Sink ? (facts_.inline_sink || facts_.sink_seen)
                                        : tab_ == conn::Tab::Source && none_why_ == pd::PdNoneWhy::SourceNeverOffered);
    }
    bool paint_empty(bool redraw);
    static void row_fn(void *ctx, int index, bool focused, kit::RowSpec &out);
    conn::Tab tab_ = conn::Tab::Source;
    conn::Facts facts_;
    pd::PdVerdict verdict_ = pd::PdVerdict::Pending;
    pd::PdNoneWhy none_why_ = pd::PdNoneWhy::None;
    View view_ = View::Overview;
    bool direct_spec_ = false;             // entered directly into Spec via open_connection() / remote alias
    int act_sel_ = -1;                     // focused action row on Actions page, -1 when not in Actions
    int first_ = 0;                        // first fact row on Spec page (Up / Dn scroll it)
    uint8_t res_n_ = 0, tot_n_ = 0;        // fact rows and total rows of the last Spec paint
    uint32_t id_sig_ = 0;                  // identity block's hash (0 = repaint)
    uint32_t thumb_sig_ = 0;               // scroll thumb's (first, total) as painted
    std::array<uint32_t, 3> slice_sig_{};  // 3-slice Overview per-slice hashes (Charger / Cable / Sink)
    uint32_t reset_armed_until_ = 0;       // 2-press confirm window for inline Reset & re-read
    bool actions_gap_cleared_ = false;     // gap below RowList at kPlaceListY cleared once per Actions entry
    conn::DebouncedSense port_sense_;      // 600 ms debounced idle port-sense classifier
    conn::AutoWake wake_;                  // the once-per-episode wake of a sleeping source on the Charger overview
    bool wake_asleep_ = false;             // wake_candidate held 600 ms on the facts of this tick
    bool asleep_run_ = false;              // the wake_candidate run is going since asleep_since_
    uint32_t asleep_since_ = 0;
    StrBuf<40> status_txt_;                // last status string (for tests / toast mirror)
    bool em_read_pending_ = false;         // Read e-marker started a passive listen window from Actions
    bool id_asked_ = false;                // Identify started from this page: its progress shows in the status bar
    bool iq_asked_ = false;                // inline interrogation started from the Device place
    bool iq_have_ = false;                 // an inline interrogation was started during iq_session_
    uint32_t iq_armed_until_ = 0;          // 2-press confirm window for inline interrogation
    uint32_t iq_session_ = 0;              // fusb302::session() of the last inline interrogation started here
    std::array<conn::Item, conn::kMaxItems> items_{};
    int n_items_ = 0;
    Elem status_;                          // bottom status bar cache
    raguard::PeakWindow guard_;            // CC peaks for the topology (passive reads only)
    uint32_t guard_t_ = 0;
    bool guard_primed_ = false;
    StrBuf<24> op_;                        // last action message, shown in the status bar until op_until_
    uint32_t op_until_ = 0;
    uint32_t sig_ = 0;
    bool sig_valid_ = false;
    bool body_pending_ = false;            // a sliced paint_empty/paint_body did not finish on the last tick
    bool match_valid_ = false;             // cached g_last_match matches cheap_sig_ across resumed body slices
    uint32_t cheap_sig_ = 0;               // scalar pre-check signature; only meaningful while sig_valid_
    // What the body area shows, so a changed row repaints alone: the kind, its tab, its row count and one hash per
    // painted row (key, value, colour, cut, dim). 40 bytes, no copy of the text.
    static constexpr int kBodyRows = 9;
    enum class Body : uint8_t { None, Rows, Empty, NoPd };
    Body body_ = Body::None;
    conn::Tab body_tab_ = conn::Tab::Source;
    uint8_t body_n_ = 0;
    bool tail_cleared_ = false;
    bool close_done_ = false;
    int16_t body_clear_y_ = kFooterY;
    std::array<uint32_t, kBodyRows> row_sig_{};
    // Row D (page flicker): debounced volatile inputs. A value must hold kHoldMs before it reaches a row
    // or a paint signature, so a flapping fast-charge verdict, usb-link family or ADC/VBUS level repaints
    // nothing on a steady link; a value that settles still reaches the page after ~1 s.
    static constexpr uint32_t kHoldMs = 1000;
    struct Held {
        uint32_t value = 0, cand = 0, since = 0;
        bool has = false;
        uint32_t commit(uint32_t v, uint32_t now) {
            if (!has) { has = true; value = cand = v; since = now; }
            else if (v != cand) { cand = v; since = now; }
            if (cand != value && uint32_t(now - since) >= kHoldMs) value = cand;
            return value;
        }
    };
    Held fc_h_, dp_h_, dm_h_, cc1_h_, cc2_h_, vbus_h_, ibus_h_;
    uint32_t fc_ = 0;                                   // the committed (debounced) values the paint reads
    std::int32_t dp_ = -1, dm_ = -1, cc1_ = -1, cc2_ = -1, vbus_ = -1, ibus_ = -1;
    // The inline VOOC monitor's raw note (Sink tab, no PD): 0 = no live session, else 1 << 16 | the last 0x82 reply word,
    // sampled at most every kVoocNoteMs (the reply moves every ~45 ms; digits change at most 4 times a second).
    static constexpr uint32_t kVoocNoteMs = 250;
    uint32_t vooc_ = 0, vooc_at_ = 0;
};

class EmarkerPage final : public PageBase {
public:
    // The saved-cables browser opened from Connection's Cable tab: it starts in the list view and BACK leaves it from there.
    // The list is the kit RowList (names read from flash on demand), one cable's table is a kit KvTable and the delete
    // question a kit AlertBand. The remote's e-marker state reads the driver, not this page.
    explicit EmarkerPage(UiContext &c) : PageBase(c, Page::Count, rail::kEmarker, Icon::Emark) {}
    void enter() override;
    void draw_static() override;
    void update(uint32_t now_ms) override;
    bool on_event(Event e) override;
    Strip labels() const override;
    void invalidate() override;
    void leave() override;
    ListState list_state() const { return menu_.view == EmView::List && n_seqs_ > 0 ? ctx_.rows.state() : ListState{}; }
private:
    static constexpr int kNameSlots = 7;   // names cached for the visible window (7 rows at most): index % 7
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
    std::array<std::uint8_t, kNameSlots> name_idx_{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    StrBuf<12> note_;                      // what a delete answered, shown in the footer until note_until_
    bool note_caution_ = false;
    uint32_t note_until_ = 0;              // 0 = none
    uint32_t view_sig_ = ~0u;              // the view painted: a change wipes the body once
    Elem body_, foot_;                     // Detail / Confirm body and footer caches
};

// Diag v2: three cards (USB, PD, SYS) with label / value rows, state dots, a group pill and a footer
// sentence. A fixed layout: nothing scrolls.
class DiagPage final : public PageBase {
public:
    explicit DiagPage(UiContext &c) : PageBase(c, Page::Count, rail::kSys, Icon::Chip) {}
    void draw_static() override;
    void update(uint32_t now_ms) override;
    bool on_event(Event e) override;
    Strip labels() const override;
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
    uint8_t static_step_ = 0;
    Elem foot_;
};

// Memory (opened from Settings): the memstat rows (k2/core/memstat.hpp) as a KvTable: application stack now and at its low-water
// mark, the interrupt stack, static RAM, the littlefs volume, the firmware image and the heap (none). Only the FS free row takes the
// exception colour, under 20 % of the volume free (caution) and 10 % (error); the footer states the same fact and the remedy.
// The stack rows are plain numbers. Reads the cached snapshot only.
class MemoryPage final : public PageBase {
public:
    explicit MemoryPage(UiContext &c) : PageBase(c, Page::Count, rail::kMemory, Icon::Chip) {}
    void draw_static() override;
    void update(uint32_t now_ms) override;
    bool on_event(Event e) override;
    Strip labels() const override;
    void invalidate() override;
    ListState list_state() const { return ListState{}; }
    static constexpr int kRows = memstat::kRows;
private:
    Field val_[kRows];
    uint8_t static_step_ = 0;
    Elem foot_;
};

// Lines v2: D+ / D- / CC1 / CC2 tags with 0..3.3 V bars (ticks at 0.4 V and 2.0 V), a verdict card with the
// decoded result in pills, the Rp advertisement and the MCU temperature, and a footer sentence.
class LinesPage final : public PageBase {
public:
    explicit LinesPage(UiContext &c) : PageBase(c, Page::Lines, rail::kLine, Icon::Meter) {}
    void draw_static() override;
    void update(uint32_t now_ms) override;
    bool on_event(Event e) override;
    Strip labels() const override;
    void invalidate() override;
    ListState list_state() const { return ListState{}; }   // fixed layout: nothing scrolls
private:
    [[gnu::noinline]] void paint_bar(int i, int32_t mv, bool have);
    [[gnu::noinline]] bool paint_verdict(const std::optional<adc::Reading> &r, uint32_t now_ms);
    Field v_[4];
    int px_[4] = {-1, -1, -1, -1};
    Field pv_[3];                // Rp, D+/D- classifier, VREF + MCU temp
    uint32_t vsig_ = 0;          // verdict row content last painted
    bool painted_ = false;
    uint8_t static_step_ = 0;
};

// The tool slots of the place pages' action rows (ui_places.cpp): slot -> the menu it opens and the name its row shows.
// The Spoof slot is a shortcut: it opens Connection's Cable tab on the Spoof list, so tool_menu(2) names the Spoof menu that
// opens on top of Connection.
Menu tool_menu(int i);
const char *tool_name(int i);

// Staged FC trigger (Tools): Pick/Edit/Review/Apply. Only accepted requests touch D+/D-;
// cancellation hands measured cleanup to Ui so leaving the menu cannot drop the safety guard.
class TriggerPage final : public PageBase {
public:
    explicit TriggerPage(UiContext &c) : PageBase(c, Page::Count, rail::kFcTrig, Icon::Brick) {}
    void draw_static() override;
    void update(uint32_t now_ms) override;
    bool on_event(Event e) override;
    Strip labels() const override;
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
    [[gnu::noinline]] void paint_bar();                   // top bar: title, "n/3" step and the progress rule
    [[gnu::noinline]] void paint_main(uint32_t now_ms);
    [[gnu::noinline]] void paint_release(bool fresh);       // the release monitor: an AlertBand and the measured VBUS
    [[gnu::noinline]] void paint_scan();                    // one verdict row per scan step and the summary
    void push_trace(uint32_t now_ms);
    void draw_trace();
    static void row_fn(void *self, int index, bool focused, kit::RowSpec &out);   // the protocol list

    static constexpr int kTraceLen = 40;  // 10 s at 4 samples per second
    SmallField<8> target_v_, vbus_v_, held_v_;
    SmallField<12> state_v_;
    kit::ActionCell send_;                // Review: SEND, then CONFIRM with the draining bar
    Elem foot_;
    uint8_t kind_ = 255;                  // body painted: 0 main, 1 scan
    uint32_t bsig_ = 0, barsig_ = 0, tsig_ = 0, ysig_ = 0;
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
    explicit PdTriggerPage(UiContext &c) : PageBase(c, Page::Count, rail::kPdTrig, Icon::Pd) {}
    void draw_static() override;
    void update(uint32_t now_ms) override;
    bool on_event(Event e) override;
    Strip labels() const override;
    void invalidate() override;
    void enter() override;
    void leave() override;
    ListState list_state() const { return active_ && wiz_.step == WizStep::Offer ? ctx_.rows.state() : ListState{}; }
private:
    enum class Refusal : uint8_t { None, Inline, NoCaps };
    SetupState refusal_state() const;
    [[gnu::noinline]] void paint_bar();             // top bar: "n/3 PD <step>" and the progress rule
    [[gnu::noinline]] void paint_refusal();         // flow diagram, requirement checklist and the reason footer
    [[gnu::noinline]] void paint_offer(uint32_t now_ms);   // OFFER: the PDOs as kit RowList rows
    [[gnu::noinline]] void paint_value();           // VOLTAGE / CURRENT: key/value table, the big value, the range bar
    [[gnu::noinline]] void paint_confirm(uint32_t now_ms);   // CONFIRM: the request table and the SEND / CONFIRM action cell
    [[gnu::noinline]] void paint_live();            // LIVE: a verdict cell for the contract, requested vs measured
    [[gnu::noinline]] void paint_footer();
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
    uint32_t bsig_ = 0, barsig_ = 0, tsig_ = 0, vsig_ = 0;   // body, top bar, edge marks / resets, verdict
    Elem foot_;
    SmallField<12> f_[2];         // the live cells of the step: power + big value, or measured V + A
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
    explicit CableRPage(UiContext &c) : PageBase(c, Page::Count, rail::kCableR, Icon::Res) {}
    void draw_static() override;
    void update(uint32_t now_ms) override;
    bool on_event(Event e) override;
    Strip labels() const override;
    void invalidate() override;
private:
    [[gnu::noinline]] void paint_table(const CrView &v);
    [[gnu::noinline]] void paint_result(const CrView &v);

    Field val_[2];                 // the two step values (key/value rows 1 and 2)
    uint32_t csig_ = 0, rsig_ = 0;
    uint32_t cleared_until_ = 0;   // Reset pressed: the footer says "Cleared" until then
    Elem foot_;                    // footer element cache (draw_footer_cached)
};

class SettingsPage final : public PageBase {
public:
    // The K2 place (Bench > K2): the firmware's identity (version, then uptime and the USB link) over the settings, then
    // System state and Boot info.
    explicit SettingsPage(UiContext &c) : PageBase(c, Page::Settings, rail::kK2, Icon::Chip) { bar_icon_ = true; }
    void enter() override;
    void draw_static() override;
    void update(uint32_t now_ms) override;
    bool on_event(Event e) override;
    Strip labels() const override;
    void invalidate() override { static_step_ = 0; ctx_.rows.invalidate(); foot_.invalidate(); id_.invalidate(); }
    ListState list_state() const { return ctx_.rows.state(); }
    int selected() const { return sel_; }
    // The row count follows the build: the Screenshot row exists only with K2_SCREENSHOT=1 (the default is 0). ui_pages.cpp
    // static_asserts these against its SettingRow enum, and test_settings_rows0 pins the K2_SCREENSHOT=0 values.
    static constexpr int kScreenshotRows = K2_SCREENSHOT ? 1 : 0;
    static constexpr int kRows = 14 + kScreenshotRows;
    static constexpr int kUnitsRow = 6 + kScreenshotRows;   // Huawei/Samsung: FCP, SCP, AFC in the charger test
    static constexpr int kSweepRow = 7 + kScreenshotRows;   // Sweep depth of the deep sweeps
    static constexpr int kHomeBoxesRow = 9 + kScreenshotRows;
    static constexpr int kDisplayRow = 10 + kScreenshotRows;
    static constexpr int kSystemRow = 11 + kScreenshotRows, kMemoryRow = 12 + kScreenshotRows, kBootRow = 13 + kScreenshotRows;
    static constexpr int16_t kIdHeadY = 3, kIdLineY = 24, kIdDashY = 41, kListY = 44;   // identity y0..43, the list under it
private:
    static void row_fn(void *ctx, int index, bool focused, kit::RowSpec &out);   // reads the live settings: no value cache
    void paint_identity();
    bool share_boot_info();
    int &sel_ = ctx_.keep.settings_sel;   // remembered across page switches
    Elem foot_, id_;
    uint8_t static_step_ = 0;
    uint8_t ct_level_ = 0;                // Test level: the stored default (uiprefs::Prefs::ct_level), read once in enter()
    uint8_t sweep_depth_ = 1;             // Sweep depth: the stored one (uiprefs::Prefs::sweep_depth), read once in enter()
    mutable usbcfg::LinkNote usb_note_;   // the USB Link row's note, filled by row_fn and drawn at once
    mutable char display_tail_[16];       // the Display row's summary ("100%, 60s/5m")
};

// Settings > Home boxes: pick the metric (or Rotating / Off) for each of the 3 Home info boxes.
class HomeBoxesPage final : public PageBase {
public:
    explicit HomeBoxesPage(UiContext &c) : PageBase(c, Page::Count, rail::kHomeBoxes, Icon::Meter) {}
    enum class View : uint8_t { List, PickBox };   // zero = List
    void enter() override;
    void draw_static() override;
    void update(uint32_t now_ms) override;
    bool on_event(Event e) override;
    Strip labels() const override;
    void invalidate() override { ctx_.rows.invalidate(); }
    ListState list_state() const { return ctx_.rows.state(); }
    View view() const { return view_; }
    static constexpr int kRows = 4;   // Box 1, Box 2, Box 3, Reset defaults
private:
    static void row_fn(void *ctx, int index, bool focused, kit::RowSpec &out);
    void show(View v);
    int count() const { return view_ == View::List ? kRows : static_cast<int>(home::BoxKind::Count); }
    int &cur() { return view_ == View::List ? sel_ : pick_; }
    View view_ = View::List;
    int sel_ = 0, pick_ = 0;
};

// Settings > Display: the backlight level and the two idle timeouts (k2/core/idle_hw.hpp, remembered in flash). OK on
// Brightness opens an adjust mode where [+] / [-] step 10 % live; the other two rows open a pick-one list.
class DisplayPage final : public PageBase {
public:
    explicit DisplayPage(UiContext &c) : PageBase(c, Page::Count, rail::kDisplay, Icon::Meter) {}
    enum class View : uint8_t { List, Adjust, PickDim, PickOff };   // zero = List
    void enter() override;
    void draw_static() override;
    void update(uint32_t now_ms) override;
    bool on_event(Event e) override;
    Strip labels() const override;
    void invalidate() override { ctx_.rows.invalidate(); foot_.invalidate(); }
    ListState list_state() const { return ctx_.rows.state(); }
    View view() const { return view_; }
private:
    static void row_fn(void *ctx, int index, bool focused, kit::RowSpec &out);
    void show(View v);
    int count() const { return view_ >= View::PickDim ? 5 : 3; }
    int &cur() { return view_ >= View::PickDim ? pick_ : sel_; }
    View view_ = View::List;
    int sel_ = 0, pick_ = 0;
    Elem foot_;
    mutable char tail_[3][8];   // the three value texts
};

class AutorotatePage final : public PageBase {
public:
    explicit AutorotatePage(UiContext &c) : PageBase(c, Page::Count, rail::kRotate, Icon::Rotate) {}
    void enter() override;
    void draw_static() override;
    void update(uint32_t now_ms) override;
    bool on_event(Event e) override;
    Strip labels() const override;
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
    explicit UsbLinkPage(UiContext &c) : PageBase(c, Page::Count, rail::kUsb, Icon::Usb) {}
    void draw_static() override;
    void update(uint32_t now_ms) override;
    bool on_event(Event e) override;
    Strip labels() const override;
    void invalidate() override;
    void enter() override;
    int selected() const { return sel_; }
private:
    static void row_fn(void *ctx, int index, bool focused, kit::RowSpec &out);
    usbcfg::Setting pending_{};
    int sel_ = 0;
    Elem foot_;                  // footer element cache (draw_footer_cached)
};

// Ring position 7 TOOLS: triggers (Request voltage, Fast charge) and active bench utilities.
class ToolsPage final : public PageBase {
public:
    explicit ToolsPage(UiContext &c) : PageBase(c, Page::Count, rail::kTools, Icon::Bolt), sel_(c.keep.tools_sel) {}
    void enter() override;
    void draw_static() override;
    void update(uint32_t now_ms) override;
    bool on_event(Event e) override;
    Strip labels() const override;
    void invalidate() override { ctx_.rows.invalidate(); }
    ListState list_state() const { return ctx_.rows.state(); }
    int selected() const { return sel_; }
    // The last row is the PD limiter: OK opens its per-tech picker (Off, Fixed, PPS, SPR AVS, Apply[, Reset link]).
    enum class View : uint8_t { List, PickLimit, EditFixed, EditPps, EditAvs, PickMode, PickMaxV, PickMaxA };   // zero = List
    View view() const { return view_; }
private:
    static void row_fn(void *ctx, int index, bool focused, kit::RowSpec &out);
    void show(View v);
    void build_picks();
    int rows() const;
    bool in_tech() const { return view_ >= View::EditFixed && view_ <= View::EditAvs; }
    bool in_sub_pick() const { return view_ >= View::PickMode; }
    int count() const {
        return view_ == View::List ? rows() : view_ == View::PickLimit ? (5 + int(!has_caps_)) : in_tech() ? 3 : sub_count();
    }
    int &cur() { return view_ == View::List ? sel_ : view_ == View::PickLimit ? pick_ : in_tech() ? edit_idx_ : sub_pick_; }
    pd::LimTech &cur_tech();
    const pd::LimTech &cur_tech() const;
    uint16_t tech_max_ma() const;
    void open_amps();
    void pps_bounds(uint16_t &lo, uint16_t &hi) const;
    int sub_count() const;
    uint16_t sub_val(int index) const;
    void fmt_tech_summary(const pd::LimTech &t, StrBufBase &out) const;
    void limit_row(int index, bool focused, kit::RowSpec &out) const;
    void tech_row(int index, bool focused, kit::RowSpec &out) const;
    void sub_pick_row(int index, bool focused, kit::RowSpec &out) const;
    bool apply_limit(bool off);
    void limit_edge();
    bool confirmed_for(uint32_t key) const { return confirm_until_ != 0 && confirm_key_ == key && int32_t(confirm_until_ - ctx_.now) > 0; }
    void ask_confirm(uint32_t key);
    void drop_confirm();   // the window ends: its "OK again" toast goes with it
    int &sel_;
    View view_ = View::List;
    View tech_view_ = View::EditFixed;
    int pick_ = 0;
    int edit_idx_ = 0;
    int sub_pick_ = 0;
    bool has_caps_ = false;
    bool has_pps_ = false, has_avs_ = false;
    uint8_t nfixed_ = 0;
    uint16_t fixed_mvs_[pd::kMaxPdos]{};
    uint16_t fixed_max_ma_ = 0, pps_min_mv_ = 0, pps_max_mv_ = 0, pps_max_ma_ = 0, avs_max_ma_ = 0;
    uint16_t pend_mv_ = 0;       // the voltage picked in Max V, committed with its Max A
    uint16_t amp_offer_ma_ = 0;  // the A list's ceiling: the offer current at that voltage
    uint16_t amp_pick_ma_ = 0;   // the A list's radio: the stored max_ma, 0 when above the offer
    bool va_ = false;            // the A list was reached from Max V
    pd::LimConfig draft_{};
    uint32_t confirm_until_ = 0, confirm_key_ = 0;   // 0 = Off, 1 = Apply, 0xFFFFFFFF = Reset link
    pd::LimState seen_ = pd::LimState::Off;   // for the one-shot give-up result
    mutable StrBuf<12> tail_;
    mutable StrBuf<30> note_;
};

// The QR scan-test screen (remote Cmd::QrTest op 1; the model is k2/ui/ui_qrtest.hpp): ONE full-screen QR over the
// whole 240x240 panel (the button strip included: chrome and caption hidden while it is shown) at the requested
// (version, px, quiet), filled with the max-length kind-'t' link of that version, so a phone scan reads
// "QR scan test v<NN> <N>px". The code is drawn in update(), after the panel, so it covers the strip. BACK pops the
// page (the Ui does it). The encode workspace is leased from the scratch arena during paint().
class QrTestPage final : public PageBase {
public:
    explicit QrTestPage(UiContext &c) : PageBase(c, Page::Count, rail::kQr) {}
    void draw_static() override;
    void update(uint32_t now_ms) override;
    bool on_event(Event e) override;
    Strip labels() const override;
    void invalidate() override { painted_ = false; }
    void enter() override;
    bool hides_strip() const override { return true; }    // the code covers the whole panel

    int version() const { return version_; }
    int px() const { return px_; }
    int quiet() const { return quiet_; }
    int modules() const { return n_; }                    // modules per side of the code drawn, 0 = none
    std::size_t link_len() const { return link_len_; }    // the link length the code carries
    // Rebuilds the link text cleanly (the encode workspace is transient in scratch during paint()).
    std::size_t link_text(std::span<char> out) const { return qrtest::build_link(version_, px_, out); }

private:
    void paint();
    std::size_t link_len_ = 0;
    int version_ = 0, px_ = 0, quiet_ = 0, n_ = 0;
    bool painted_ = false;
};

}  // namespace k2
