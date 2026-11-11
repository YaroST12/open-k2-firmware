// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Tools -> Loop probe: the HID-cable loop probe (k2/loopprobe/loop_probe.hpp, docs/LOOP_PROBE.md) on the K2.
//
// The Review look of the charger test, nothing new: a KvTable of the five steps (key = the step, value = its state in this
// run or the stored one: "--", "running", "done 0.7s", "skip no 5V", "stop VBUS>5.5V", "rebooted"), a rule, then the
// START ActionCell with its caution hint (the last step may cut VBUS) and the confirm arm (second OK within
// Ui::kConfirmWindowMs). While a run is live the ActionCell area holds a ProgressBar and two InfoBlock rows (Step n/5 and
// the elapsed time). The run ends in the existing QR share view (Type::LoopProbe); a long OK shares the last result later.
// The runner lives in this page: Ui::tick polls it through poll() while the page is on the stack, and leaving the page
// (BACK idle, paging away, the remote's stop) cancels a live run, which restores every line and saves the record.
#pragma once
#include <array>
#include <cstdint>
#include <string_view>

#include "k2/loopprobe/loop_probe.hpp"
#include "k2/ui/ui_pages.hpp"

namespace k2 {

class LoopProbePage final : public PageBase {
public:
    explicit LoopProbePage(UiContext &c);
    void draw_static() override;
    void update(uint32_t now_ms) override;
    bool on_event(Event e) override;
    PanelLabels labels() const override;
    void invalidate() override;
    void enter() override;
    void leave() override;

    void poll(uint32_t now_ms);                  // Ui::tick: advances a live run, opens the share view when it finishes
    void cancel(uint32_t now_ms);                // ends a live run (BACK, leave(), Ui::cancel_runs)
    bool running() const { return runner_.running(); }
    bool armed() const { return confirm_until_ != 0; }
    std::string_view footer() const { return foot_text_.view(); }   // what the footer shows now (tests)
    // The value column of step `i` as the table shows it now (tests).
    std::string_view step_text(int i) const;

    static constexpr int16_t kRuleY = kit::kBodyY + static_cast<int16_t>(loopprobe::kSteps) * kit::kRowH;   // 154
    static constexpr int16_t kActionY = kRuleY + 2, kActionH = 64;                                              // 156..219
    static constexpr int16_t kValueX = 80;       // the value column: 17 cells of font 16 to x216
    static constexpr uint8_t kValueChars = 17;
    static constexpr int16_t kBarY = kActionY + 4, kStepRowY = kBarY + 8, kTimeRowY = kStepRowY + kit::kInfoPitch + 2;

private:
    enum class Mode : uint8_t { Idle, Running };
    bool start(uint32_t now_ms);                 // the second OK: refusals land in the footer note
    bool share(uint32_t now_ms);                 // the run's record, else the stored one, into the QR view; false: nothing to share
    void reload();                               // the stored result's step table (no Record on the stack)
    void compose_steps();                        // the five value strings and colours
    void compose_footer(uint32_t now_ms);
    void say(std::string_view text, uint16_t color);
    void paint_table();
    void paint_action(uint32_t now_ms);
    void paint_progress(uint32_t now_ms);

    loopprobe::Runner runner_;
    std::array<loopprobe::Record::StepRec, loopprobe::kSteps> stored_{};
    bool have_stored_ = false;
    bool ran_ = false;                           // this visit started a run: the table shows runner_.record()
    bool live_ = false;                          // the run poll() last saw was live (its end is an edge)
    Mode painted_ = Mode::Idle;
    bool laid_ = false;                          // the rule and the area under it are laid for painted_
    uint32_t confirm_until_ = 0;
    std::array<StrBuf<18>, loopprobe::kSteps> val_;
    std::array<uint16_t, loopprobe::kSteps> col_{};
    std::array<uint16_t, loopprobe::kSteps> drawn_col_{};   // the colour each cell was last painted in
    std::array<Field, loopprobe::kSteps> cell_;           // the value column (char-diffed)
    StrBuf<27> foot_text_;
    FooterTone foot_tone_ = FooterTone::Info;
    StrBuf<27> note_;                            // refusal / result line, shown for a moment
    uint16_t note_color_ = 0;
    uint32_t note_until_ = 0;
    uint32_t info_sig_ = 0;
    kit::ActionCell action_;
    kit::ProgressBar bar_;
    Elem foot_;                                  // footer element cache (draw_footer_cached)
};

}  // namespace k2
