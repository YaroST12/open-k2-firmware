// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The Battery place (Bench > Battery): the battery efficiency recorder's page (docs/BATTERY_RECORDER.md). A place page over
// the Bench, built like the Connection places: the identity block (what is measuring, or the result) over a kit RowList, the
// footer under it. The recorder itself runs in k2::battery::hw whether or not this page is shown; the page only reads it and
// starts, ends or finishes a run.
//
// Three views on one page:
//   Main    the run in RAM: its facts (In, Out, Time, Efficiency) when there is one, then what can be done now (start a
//           charge, a discharge or a round trip, the wiring and the end rule; End charge / Start discharge / Finish run while a
//           run is in progress), Saved runs and Share as QR.
//   Runs    the runs on flash, newest first; OK opens one.
//   Stored  one stored run: its identity, its facts and Share as QR.
// Ending a phase or the run by hand takes a second OK within Ui::kConfirmWindowMs (the footer turns Caution and the OK cap
// reads Confirm), so a stray press never cuts a run of hours. BACK steps out of Stored and Runs, then leaves the place.
// The pure texts and rows are k2/ui/battery_view.hpp (host-tested in tests/test_ui_battery.cpp).
#pragma once
#include <cstdint>

#include "k2/battery/battery.hpp"
#include "k2/ui/battery_view.hpp"
#include "k2/ui/kit.hpp"
#include "k2/ui/ui_pages.hpp"

namespace k2 {

class BatteryPage final : public PageBase {
public:
    explicit BatteryPage(UiContext &c);
    void draw_static() override;
    void update(uint32_t now_ms) override;
    bool on_event(Event e) override;
    PanelLabels labels() const override;
    void invalidate() override;
    void enter() override;
    void leave() override;
    const ListState &list_state() const { return ctx_.rows.state(); }
    int selected() const { return sel_; }
    // The focused row's action in the Main or Stored view (Act::Runs in the Runs view: its rows are runs, not actions).
    battery::view::Act act() const;
    bool viewing_stored() const { return view_ == View::Stored; }
    bool viewing_runs() const { return view_ == View::Runs; }

private:
    enum class View : uint8_t { Main, Runs, Stored };
    static void row_fn(void *self, int index, bool focused, kit::RowSpec &out);
    void action_row(battery::view::Act a, bool focused, kit::RowSpec &out);
    void run_row(int i, bool focused, kit::RowSpec &out);
    void build();                      // rec_ (Main: the run in RAM), shown_ and rows_ of the view
    void load_runs();                  // runs_ from the volume
    int count() const;                 // the list's rows in the current view
    void set_view(View v, int sel);
    void ok();
    void refuse(const char *text, FooterTone tone);
    bool armed() const;

    View view_ = View::Main;
    int sel_ = 0;
    battery::view::Shown shown_{};
    battery::view::Rows rows_{};
    // The record on show: the Stored view's run, or (Main) the run in RAM copied at every build. A member, not a local: the
    // page's paths (an update under the remote's command, the share opener) keep no 250 B record on the stack.
    battery::Record rec_{};
    battery::view::Shown runs_[battery::store::kKeep] = {};
    int n_runs_ = 0;
    int runs_sel_ = 0;                 // the Runs view's selection, kept while a stored run is open
    battery::view::Act armed_act_ = battery::view::Act::In;
    uint32_t armed_until_ = 0;         // a hand end waits for its second OK until then (0 = none armed)
    StrBuf<28> note_;                  // a refusal in the footer until note_until_
    FooterTone note_tone_ = FooterTone::Info;
    uint32_t note_until_ = 0;
    StrBuf<16> tail_;                  // the row row_fn is building: its tail and note (views valid until the next row)
    StrBuf<32> note_row_;
    StrBuf<20> text_;
    uint32_t id_sig_ = 0;              // the identity block as painted (kit::paint_identity)
    bool full_ = true;                 // the next paint lays the identity block whole
    Elem foot_;                        // footer element cache (draw_footer_cached)
};

}  // namespace k2
