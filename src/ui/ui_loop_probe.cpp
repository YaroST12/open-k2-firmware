// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#include "k2/ui/ui_loop_probe.hpp"

#include <algorithm>

#include "k2/core/log.hpp"
#include "k2/core/version.hpp"
#include "k2/loopprobe/loop_probe_hw.hpp"
#include "k2/ui/kit.hpp"
#include "k2/ui/qrshare.hpp"
#include "k2/ui/trunc.hpp"
#include "k2/ui/ui.hpp"
#include "k2/ui/ui_style.hpp"
#include "k2/ui/ui_widgets.hpp"

namespace k2 {

namespace {

namespace lp = loopprobe;

constexpr kit::KvLayout kTable{kit::kBodyY, kit::kRowH, 6, nullptr, nullptr, LoopProbePage::kValueX, LoopProbePage::kValueChars,
                               Align::Left};

// The keys are the steps in run order (lp::Step); the value column says what each step did.
constexpr const char *kKeys[lp::kSteps] = {"Baseline", "Drop", "D+/D-", "E-marker", "CC"};
static_assert(all_fit(kKeys, kSmallAdvPx, LoopProbePage::kValueX - 6 - 4), "a loop probe step name runs into the value column");

// lp::Why, short, after "skip " / "stop " in the value column.
constexpr const char *kWhyShort[] = {"", "no meter", "VBUS>5.5V", "aux>5.8V", "no 5V", "host on HID", "refused", "none seen",
                                     "stopped", "save failed", "rebooted"};
static_assert(sizeof(kWhyShort) / sizeof(kWhyShort[0]) == static_cast<std::size_t>(lp::Why::Rebooted) + 1, "one short reason per Why");
static_assert(all_fit(kWhyShort, kSmallAdvPx, (LoopProbePage::kValueChars - 5) * static_cast<int>(kSmallAdvPx)),
              "\"skip \" + a short reason is wider than the value column");

// The fixed footers and notes (font 16, one line).
constexpr const char *kFooters[] = {
    "Cable into own HID port",  "Last result on flash",     "Last run rebooted in CC", "No power meter",
    "VBUS above 5.5V: refused", "Aux above 5.8V: refused", "Running. 5V only",        "CC step: VBUS may drop",
    "Stopped. Result saved",    "Done. Result saved",      "No result to share",
};
enum Foot : uint8_t { kIdle, kStored, kRebooted, kNoMeter, kVbusHigh, kAuxHigh, kRunning, kCcStep, kStopped, kDone, kNothing };
static_assert(all_fit(kFooters, kSmallAdvPx, kFooterTextPx), "a loop probe footer is wider than the footer line");

constexpr const char kHint[] = "Last step may cut VBUS";   // the ActionCell's caution line (font 12)
static_assert(fits_px(kLabelAdv, kHint, 224 - 40 - 15), "the START hint is wider than the action cell");

constexpr uint32_t kNoteMs = 2500;
constexpr uint32_t kClockMs = 250;   // the elapsed time repaints at most 4 times a second

}  // namespace

LoopProbePage::LoopProbePage(UiContext &c) : PageBase(c, Page::Count, "Loop probe", Icon::Usb), runner_(lp::hw::env()) {}

// ---- lifecycle ----------------------------------------------------------------------------------------------------------------

void LoopProbePage::enter() {
    ran_ = live_ = false;
    confirm_until_ = 0;
    note_.clear();
    note_until_ = 0;
    reload();
    compose_steps();
    compose_footer(ctx_.now);
}

// Paged away, popped or replaced while a run is live: end it (the runner restores every line and saves the record).
void LoopProbePage::leave() { cancel(ctx_.now); }

void LoopProbePage::reload() {
    const auto buf = ctx_.ui->share_buffer();   // scratch: no share view is open under this page
    const std::size_t n = lp::load_record_bytes(lp::hw::store(), buf);
    have_stored_ = n != 0 && lp::read_steps(std::span<const uint8_t>(buf.data(), n), stored_);
}

void LoopProbePage::draw_static() {
    title_bar();
    laid_ = false;
    invalidate();
}

void LoopProbePage::invalidate() {
    laid_ = false;
    for (Field &f : cell_) f.invalidate();
    drawn_col_.fill(0);
    action_.invalidate();
    bar_.invalidate();
    info_sig_ = 0;
    foot_.invalidate();
}

// ---- the run ------------------------------------------------------------------------------------------------------------------

bool LoopProbePage::start(uint32_t now_ms) {
    if (const char *busy = ctx_.ui->busy_reason()) {
        say(busy, kCaution);
        return false;
    }
    const lp::Start r = runner_.start(now_ms, kFwVersion);
    K2LOG("[LPRB] start result=%u\n", static_cast<unsigned>(r));
    switch (r) {
    case lp::Start::Ok:
        ran_ = live_ = true;
        note_.clear();
        note_until_ = 0;
        return true;
    case lp::Start::NoMonitor: say(kFooters[kNoMeter], kCaution); break;
    case lp::Start::VbusHigh: say(kFooters[kVbusHigh], kCaution); break;
    case lp::Start::AuxHigh: say(kFooters[kAuxHigh], kCaution); break;
    case lp::Start::Running: break;
    }
    return false;
}

void LoopProbePage::poll(uint32_t now_ms) {
    if (!runner_.running()) return;
    runner_.poll(now_ms);
    if (runner_.running() || !live_) return;
    live_ = false;
    const lp::Record &r = runner_.record();
    const lp::Record::StepRec &last = r.at(runner_.step());
    K2LOG("[LPRB] end step=%s status=%u why=%s ms=%u\n", lp::step_name(runner_.step()), unsigned(last.status),
          lp::why_name(static_cast<lp::Why>(last.why)), unsigned(now_ms - r.start_ms));
    // The run ended by itself (all steps, or a guard stopped it): the record goes straight to the share view.
    share(now_ms);
}

void LoopProbePage::cancel(uint32_t now_ms) {
    if (!runner_.running()) return;
    runner_.cancel(now_ms);
    live_ = false;
    K2LOG("[LPRB] cancelled step=%s\n", lp::step_name(runner_.step()));
    say(kFooters[kStopped], kDim);
}

bool LoopProbePage::share(uint32_t now_ms) {
    const auto buf = ctx_.ui->share_buffer();
    std::size_t len = 0;
    if (ran_ && !runner_.running()) len = lp::encode(runner_.record(), buf);
    else if (!runner_.running()) len = lp::load_record_bytes(lp::hw::store(), buf);
    if (len != 0 && ctx_.ui->open_share(qrshare::Type::LoopProbe, qrshare::kSchemaLoop, "Loop probe", len, now_ms)) return true;
    say(kFooters[kNothing], kCaution);
    return false;
}

// ---- texts --------------------------------------------------------------------------------------------------------------------

void LoopProbePage::say(std::string_view text, uint16_t color) {
    note_.clear();
    note_.put(text);
    note_color_ = color;
    note_until_ = ctx_.now + kNoteMs;
    if (note_until_ == 0) note_until_ = 1;
}

std::string_view LoopProbePage::step_text(int i) const {
    return i >= 0 && i < static_cast<int>(lp::kSteps) ? val_[static_cast<std::size_t>(i)].view() : std::string_view{};
}

// One value per step from the run of this visit, else the stored result, else the dim placeholder.
void LoopProbePage::compose_steps() {
    const bool live = runner_.running();
    for (std::size_t i = 0; i < lp::kSteps; i++) {
        lp::Record::StepRec st{};
        if (ran_) st = runner_.record().step[i];
        else if (have_stored_) st = stored_[i];
        StrBuf<18> &v = val_[i];
        v.clear();
        uint16_t color = kDim;
        const std::size_t why = std::min<std::size_t>(st.why, sizeof(kWhyShort) / sizeof(kWhyShort[0]) - 1);
        switch (static_cast<lp::Status>(st.status)) {
        case lp::Status::Done:
            v.put("done ").put_fixed(st.dur_ms, 1).put_char('s');
            color = kText;
            break;
        case lp::Status::Skipped:
            v.put("skip ").put(kWhyShort[why]);
            break;
        case lp::Status::Aborted:
            if (static_cast<lp::Why>(st.why) == lp::Why::Cancelled) v.put("stopped");
            else {
                v.put("stop ").put(kWhyShort[why]);
                color = kCaution;
            }
            break;
        case lp::Status::Interrupted:
            v.put("rebooted");
            color = kCaution;
            break;
        default:
            if (live && static_cast<std::size_t>(runner_.step()) == i) {
                v.put("running");
                color = kFocus;
            } else {
                v.put("--");
            }
            break;
        }
        col_[i] = color;
    }
}

// The one fact for the footer: a refusal or a result for a moment, else the run's state, else what is stored.
void LoopProbePage::compose_footer(uint32_t now_ms) {
    foot_text_.clear();
    foot_tone_ = FooterTone::Info;
    if (note_until_ && static_cast<int32_t>(now_ms - note_until_) >= 0) {
        note_until_ = 0;
        note_.clear();
    }
    if (!note_.view().empty()) {
        foot_tone_ = note_color_ == kDim ? FooterTone::Info : footer_tone_of(note_color_);
        foot_text_.put(note_.view());
        return;
    }
    if (runner_.running()) {
        const bool cc = runner_.step() == lp::Step::Cc;
        foot_tone_ = cc ? FooterTone::Caution : FooterTone::Info;
        foot_text_.put(kFooters[cc ? kCcStep : kRunning]);
        return;
    }
    if (ran_) {
        const lp::Record::StepRec &last = runner_.record().at(runner_.step());
        const bool stopped = static_cast<lp::Status>(last.status) == lp::Status::Aborted &&
                             static_cast<lp::Why>(last.why) == lp::Why::Cancelled;
        foot_tone_ = stopped ? FooterTone::Info : FooterTone::Ok;
        foot_text_.put(kFooters[stopped ? kStopped : kDone]);
        return;
    }
    if (have_stored_) {
        const bool rebooted = static_cast<lp::Status>(stored_[static_cast<std::size_t>(lp::Step::Cc)].status) == lp::Status::Interrupted;
        foot_tone_ = rebooted ? FooterTone::Caution : FooterTone::Info;
        foot_text_.put(kFooters[rebooted ? kRebooted : kStored]);
        return;
    }
    if (!ctx_.pm_online) {
        foot_tone_ = FooterTone::Caution;
        foot_text_.put(kFooters[kNoMeter]);
        return;
    }
    foot_text_.put(kFooters[kIdle]);
}

// ---- paint --------------------------------------------------------------------------------------------------------------------

void LoopProbePage::paint_table() {
    const Surface &c = ctx_.content;
    if (!laid_) {
        if (!ctx_.blank) kit::kv_ground(c, kTable, kKeys, static_cast<int>(lp::kSteps));
        kit::kv_static(c, kTable, kKeys, static_cast<int>(lp::kSteps));
        kit::kv_cells(c, kTable, cell_.data(), static_cast<int>(lp::kSteps));
        gfx_fill(c, 0, kRuleY, c.w, 2, kRule);   // the section rule under the table (over its closing hairline)
        drawn_col_.fill(0);
    }
    for (std::size_t i = 0; i < lp::kSteps; i++) {
        if (drawn_col_[i] != col_[i]) {
            cell_[i].set_colors(col_[i], kBg);
            cell_[i].invalidate();
            drawn_col_[i] = col_[i];
        }
        cell_[i].draw(val_[i].view());
    }
}

void LoopProbePage::paint_action(uint32_t now_ms) {
    if (confirm_until_ != 0) {
        const int32_t left = std::max<int32_t>(0, static_cast<int32_t>(confirm_until_ - now_ms));
        const int permille = std::clamp<int>(left * 1000 / static_cast<int32_t>(Ui::kConfirmWindowMs), 0, 1000);
        StrBuf<24> sub;
        sub.put("closes in ").put_u32(static_cast<uint32_t>((left + 999) / 1000)).put_char('s');
        action_.draw_armed("CONFIRM", sub.view(), permille);
    } else {
        action_.draw_idle("START", kHint);
    }
}

void LoopProbePage::paint_progress(uint32_t now_ms) {
    const lp::Record &r = runner_.record();
    const int done = static_cast<int>(runner_.step());
    bar_.set(done, static_cast<int>(lp::kSteps));
    const uint32_t elapsed = now_ms - r.start_ms;
    const uint32_t sig = (static_cast<uint32_t>(done) << 24) ^ (elapsed / kClockMs) ^ 0x5A000000u;
    if (sig == info_sig_) return;
    const bool step_changed = (info_sig_ >> 24) != (sig >> 24) || info_sig_ == 0;
    info_sig_ = sig;
    if (step_changed) {
        StrBuf<20> s;
        s.put_u32(static_cast<uint32_t>(done + 1)).put_char('/').put_u32(static_cast<uint32_t>(lp::kSteps)).put_char(' ').put(lp::step_name(runner_.step()));
        kit::draw_info_row(ctx_.content, kStepRowY, "Step", s.view());
    }
    StrBuf<12> t;
    t.put_fixed(static_cast<int32_t>(elapsed / 100 * 100), 1).put_char('s');
    kit::draw_info_row(ctx_.content, kTimeRowY, "Time", t.view());
}

void LoopProbePage::update(uint32_t now_ms) {
    title_badge(false);
    if (confirm_until_ != 0 && static_cast<int32_t>(now_ms - confirm_until_) >= 0) confirm_until_ = 0;   // the window ran out
    if (runner_.running()) confirm_until_ = 0;
    const Mode mode = runner_.running() ? Mode::Running : Mode::Idle;
    const bool relay = !laid_ || mode != painted_;
    compose_steps();
    paint_table();
    const Surface &c = ctx_.content;
    if (relay) {
        // The area under the rule changes its kind: one ground fill, then the new kind paints in full.
        if (laid_ || !ctx_.blank) gfx_fill(c, 0, kActionY, c.w, kActionH, kBg);
        action_.invalidate();
        bar_.invalidate();
        bar_.init(c, 4, kBarY, static_cast<int16_t>(c.w - 8));
        action_.init(c, kActionY, kActionH);
        info_sig_ = 0;
        painted_ = mode;
        laid_ = true;
    }
    if (mode == Mode::Running) paint_progress(now_ms);
    else paint_action(now_ms);
    compose_footer(now_ms);
    draw_footer_cached(c, foot_tone_, foot_text_.view(), foot_);
}

// ---- input --------------------------------------------------------------------------------------------------------------------

bool LoopProbePage::on_event(Event e) {
    if (e.button == Button::Ok && e.press == Press::Long) {
        if (!runner_.running() && !share(ctx_.now)) update(ctx_.now);   // the share view paints itself; a refusal is ours
        return true;
    }
    if (e.press != Press::Short) return false;
    switch (e.button) {
    case Button::Back:
        if (!runner_.running()) return false;   // idle: the Ui pops the page
        cancel(ctx_.now);
        update(ctx_.now);
        return true;
    case Button::Ok:
        if (runner_.running()) return true;
        if (confirm_until_ != 0 && static_cast<int32_t>(ctx_.now - confirm_until_) < 0) {
            confirm_until_ = 0;
            start(ctx_.now);
        } else if (const char *busy = ctx_.ui->busy_reason()) {
            say(busy, kCaution);                // refuse before the arm: nothing to confirm
        } else if (!ctx_.pm_online) {
            say(kFooters[kNoMeter], kCaution);
        } else {
            confirm_until_ = ctx_.now + Ui::kConfirmWindowMs;
            if (confirm_until_ == 0) confirm_until_ = 1;
            note_.clear();
            note_until_ = 0;
        }
        update(ctx_.now);
        return true;
    case Button::Minus:
    case Button::Plus:
        return false;
    }
    return false;
}

PanelLabels LoopProbePage::labels() const {
    PanelLabels l = base_labels();
    const bool live = runner_.running();
    l.b[static_cast<int>(Button::Back)].shrt = live ? "Stop" : "Back";
    l.b[static_cast<int>(Button::Minus)] = {"", "", "", false};
    l.b[static_cast<int>(Button::Plus)] = {"", "", "", false};
    const char *ok = live ? "" : confirm_until_ != 0 ? "Confirm" : "Start";
    const char *share = !live && (ran_ || have_stored_) ? "Share" : "";
    l.b[static_cast<int>(Button::Ok)] = {ok, share, "", false};
    return l;
}

}  // namespace k2
