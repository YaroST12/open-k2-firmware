// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#include "k2/ui/ui_twin.hpp"

#include <algorithm>

#include "k2/core/storage.hpp"
#include "k2/drivers/fusb302.hpp"
#include "k2/pd/pd_link.hpp"
#include "k2/twin/present.hpp"
#include "k2/twin/twin_hw.hpp"
#include "k2/ui/ui.hpp"
#include "k2/ui/ui_elem.hpp"
#include "k2/ui/ui_style.hpp"

namespace k2 {

namespace {

namespace live = twin::live;
namespace present = twin::present;

// The fixed texts. Footers are one line of at most kFooterCells (font 16), notes one line of kNoteMaxPx (font 12); a text
// that does not fit fails the build, it is never cut.
constexpr const char kLearnText[] = "Learn from device";
constexpr const char *kFooters[] = {
    "Asks the charger. Confirm", "Stopped. 5 V back", "Tool or test running", "Device inline: unplug it",
    "Power monitor not ready", "No charger to run against", "No twin yet. Learn one", "Twin learned", "Twin updated",
    "Already known", "Plug a device in alone", "Nothing to learn from", "Library full", "Twin not stored",
    "Reading the device", "Device did not answer",
};
constexpr const char *kNotes[] = {
    "no charger to predict from", "reads a device alone on the K2", "live proof running",
    present::note(twin::Limiter::None), present::note(twin::Limiter::Device),
    present::note(twin::Limiter::Charger), present::note(twin::Limiter::Cable),
};
static_assert(all_fit(kFooters, kSmallAdvPx, kFooterCells * static_cast<int>(kSmallAdvPx)), "a Device twins footer is wider than the footer line");
static_assert(all_fit(kNotes, kLabelAdv, kit::kNoteMaxPx), "a Device twins note is wider than the note box");
static_assert(fits_px(kSmallAdvPx, kLearnText, 224 - ListView::kBarW - kit::kTextX), "the Learn row text runs into the scroll lane");

// The runner's verdict as the kit's verdict cell shows it: a charger that said no or never answered is a fail, a prediction
// the K2 would not risk is a skip (never red), a run the user stopped is dim.
kit::Verdict cell_of(live::Verdict v) {
    switch (v) {
    case live::Verdict::Granted: return kit::Verdict::Pass;
    case live::Verdict::Skipped: return kit::Verdict::Skip;
    case live::Verdict::Cancelled: return kit::Verdict::Stopped;
    case live::Verdict::None: return kit::Verdict::None;
    default: return kit::Verdict::Fail;   // Refused, NoPsRdy, OutOfBand, NoCaps, Reset, Unsafe
    }
}

constexpr uint32_t kLearnWaitMs = 15000;   // an alone-Identify: contract wait (~650 ms) + the VDMs; generous

}  // namespace

bool TwinPage::running() const { return twin_hw::live_running(); }

const TwinPage::Entry *TwinPage::entry_by_seq(uint8_t seq) const {
    for (int i = 0; i < n_; i++)
        if (list_[static_cast<std::size_t>(i)].seq == seq) return &list_[static_cast<std::size_t>(i)];
    return nullptr;
}

void TwinPage::draw_static() {
    title_bar();
    ctx_.rows.init(ctx_.content, kit::kBodyY, &TwinPage::row_fn, this);
    ctx_.rows.set_count(rows());
    ctx_.rows.select(sel_);
    invalidate();
}

void TwinPage::invalidate() {
    foot_.invalidate();
    ctx_.rows.invalidate();
}

void TwinPage::enter() {
    loaded_ = false;
    sel_ = 0;
    armed_seq_ = 0;
    note_.clear();
    note_until_ = 0;
    hide_proof_ = false;
    started_ = false;
    reload();
    invalidate();
}

// Paged away or popped while this page's own proof is running: ask 5 V back and end it (a run the remote started is left alone).
void TwinPage::leave() {
    if (started_ && running()) ctx_.ui->cancel_twin();
    if (learn_wait_) fusb302::identify_abort();   // the read this page started: release the line, learn nothing
    learn_wait_ = false;
    started_ = false;
}

// The library and the prediction per twin against the charger's list now. One flash read per twin, only when the library or the
// charger changed (update() compares the stamps), never while a row paints.
void TwinPage::reload() {
    std::array<uint8_t, twin::kCapacity> seqs{};
    const unsigned found = storage::twin_list(seqs);
    n_ = 0;
    full_ = 0;
    have_caps_ = false;
    twin::Twin t;
    for (unsigned i = 0; i < found && i < seqs.size(); i++) {
        if (!storage::twin_get(seqs[i], t)) continue;
        Entry &e = list_[static_cast<std::size_t>(n_++)];
        e.seq = seqs[i];
        e.name = t.name;
        e.out = {};
        e.have = twin_hw::predict(t, e.out);
        if (e.have) {
            have_caps_ = true;
            if (present::is_full(e.out)) full_++;
        }
    }
    gen_ = storage::generation();
    stamp_ = twin_hw::predict_stamp();
    loaded_ = true;
    sel_ = std::clamp(sel_, 0, rows() - 1);
}

void TwinPage::say(std::string_view text, uint16_t color) {
    note_.clear();
    note_.put(text);
    note_color_ = color;
    note_until_ = ctx_.now + 2500;
}

// What a row shows. Text and tail are views into the entry / the page's row buffers, valid until the next call.
void TwinPage::build_row(int index, bool focused, kit::RowSpec &out) {
    out = kit::RowSpec{};
    if (index >= n_) {   // "Learn from device"
        out.marker = kit::Marker::Icon;
        out.icon = Icon::Chip;
        out.text = kLearnText;
        if (focused) out.note = kNotes[1];
        return;
    }
    const Entry &e = list_[static_cast<std::size_t>(index)];
    out.text = std::string_view(e.name.data());
    out.data = true;                                  // a name from the library: fitted by fit_data, counted
    const live::Slot &slot = twin_hw::live().slot(e.seq);
    const bool this_running = running() && twin_hw::live().current() == e.seq;
    kit::Verdict cell = kit::Verdict::None;
    if (this_running) cell = kit::Verdict::Running;
    else if (!hide_proof_) cell = cell_of(slot.verdict);
    if (cell != kit::Verdict::None || this_running) {
        out.marker = kit::Marker::Verdict;
        out.state = static_cast<uint8_t>(cell);
    }
    row_tail_.clear();
    if (e.have) {
        char buf[16];
        present::summary(e.out, buf);
        row_tail_.put(buf);
        out.tail_color = present::is_full(e.out) ? kText : (e.out.kind == twin::Kind::None ? kDim : kCaution);
    } else {
        row_tail_.put("--");                          // an absent value is a dim placeholder
        out.tail_color = kDim;
    }
    out.tail = row_tail_.view();
    if (!focused) return;
    row_note_.clear();
    if (!e.have) {
        row_note_.put(kNotes[0]);
    } else if (this_running) {
        row_note_.put(kNotes[2]);
    } else if (cell != kit::Verdict::None) {
        char buf[32];
        present::proof(slot.verdict, slot.measured_mv, e.out, buf);
        row_note_.put(buf);
    } else {
        row_note_.put(present::note(e.out.limiter));
    }
    out.note = row_note_.view();
}

void TwinPage::row_fn(void *self, int index, bool focused, kit::RowSpec &out) {
    static_cast<TwinPage *>(self)->build_row(index, focused, out);
}

// The one fact for the footer: the last result or refusal for a moment, else the run's progress, the armed confirm, the last
// run's tally, else what this charger does for the stored devices.
void TwinPage::compose_footer() {
    foot_text_.clear();
    foot_tone_ = FooterTone::Info;
    if (note_.view().size()) {
        foot_tone_ = footer_tone_of(note_color_);
        foot_text_.put(note_.view());
        return;
    }
    if (learn_wait_) {
        foot_text_.put(kFooters[14]);
        return;
    }
    if (running()) {
        const live::Runner &r = twin_hw::live();
        foot_text_.put("Live proof ").put_u32(std::min<unsigned>(r.done_count() + 1, r.count())).put(" of ").put_u32(r.count());
        return;
    }
    if (armed_seq_ != 0) {
        foot_tone_ = FooterTone::Caution;
        foot_text_.put(kFooters[0]);
        return;
    }
    unsigned ran = 0, granted = 0;
    if (!hide_proof_) {
        for (int i = 0; i < n_; i++) {
            const live::Verdict v = twin_hw::live().slot(list_[static_cast<std::size_t>(i)].seq).verdict;
            if (v == live::Verdict::None) continue;
            ran++;
            if (v == live::Verdict::Granted) granted++;
        }
    }
    if (ran > 0) {
        foot_tone_ = granted == ran ? FooterTone::Ok : FooterTone::Caution;
        foot_text_.put_u32(granted).put(" of ").put_u32(ran).put(" granted");
        return;
    }
    char buf[32];
    present::footer(static_cast<unsigned>(n_), static_cast<unsigned>(full_), have_caps_, buf);
    foot_text_.put(buf);
}

void TwinPage::learn() {
    const twin_hw::Learned r = twin_hw::learn_from_identify(std::string_view{}, true);
    switch (r.how) {
    case twin_hw::Learn::Ok:
        say(r.store.unchanged ? kFooters[9] : r.store.existed ? kFooters[8] : kFooters[7], r.store.unchanged ? kDim : kOk);
        reload();
        if (const Entry *e = entry_by_seq(r.store.seq)) sel_ = static_cast<int>(e - list_.data());
        break;
    case twin_hw::Learn::NotReady: say(kFooters[10], kCaution); break;
    case twin_hw::Learn::NotInline:
    case twin_hw::Learn::NoContract: say(kFooters[11], kCaution); break;
    case twin_hw::Learn::Store: say(r.store.err == twin::Err::Full ? kFooters[12] : kFooters[13], kCaution); break;
    }
}

// OK on the Learn row. A device alone on the K2 (a dual-role phone: VBUS dark, no source caps, the toggling Rp) is read
// first: the page starts the same Identify as Who's attached > Sink and learns when it ends (update()). Anything else
// learns from the last Identify, as before (a sink behind the K2 inline, a device identified a moment ago).
void TwinPage::learn_row(uint32_t now_ms) {
    const pd::Link &link = ctx_.ui->link();
    const bool alone = pd::identify_alone(link.inline_mode() || ctx_.inline_mode, ctx_.pm_online,
                                          ctx_.pm_online ? ctx_.vbus_fast_mv : -1, fusb302::caps_this_attach(),
                                          link.wait_kind());
    if (alone && ctx_.ui->identify(now_ms) == Ui::IdResult::Started) {
        learn_wait_ = true;
        learn_wait_until_ = now_ms + kLearnWaitMs;
        note_.clear();
        note_until_ = 0;
        return;
    }
    learn();
}

// The Identify this page started is over (or ran out of time): learn from it, or say the device never answered.
void TwinPage::learn_wait_end() {
    learn_wait_ = false;
    if (fusb302::identify_busy()) fusb302::identify_abort();
    if (fusb302::identify_status().state == pd::IdState::Done && fusb302::identify_current()) learn();
    else say(kFooters[15], kCaution);
}

// OK on a twin row. The first press asks the runner without the confirm: its refusal is the real one (a test running, a device
// inline, no monitor, no charger), and "needs the confirm" arms the row for kConfirmWindowMs. The second press starts it.
void TwinPage::run_row() {
    const Entry &e = list_[static_cast<std::size_t>(sel_)];
    const std::array<uint8_t, 1> seqs{e.seq};
    const bool confirm = armed_seq_ == e.seq && static_cast<int32_t>(ctx_.now - armed_until_) < 0;
    armed_seq_ = 0;
    switch (twin_hw::live_start(seqs, confirm, ctx_.ui->twin_ctx(ctx_.now))) {
    case live::Refusal::None:
        started_ = true;
        hide_proof_ = false;
        note_.clear();
        note_until_ = 0;
        break;
    case live::Refusal::NeedConfirm:
        armed_seq_ = e.seq;
        armed_until_ = ctx_.now + Ui::kConfirmWindowMs;
        note_.clear();
        note_until_ = 0;
        break;
    case live::Refusal::Busy: say(kFooters[2], kCaution); break;
    case live::Refusal::Inline: say(kFooters[3], kCaution); break;
    case live::Refusal::NoMonitor: say(kFooters[4], kCaution); break;
    case live::Refusal::NoSource: say(kFooters[5], kCaution); break;
    case live::Refusal::NoTwins: say(kFooters[6], kCaution); break;
    }
}

bool TwinPage::on_event(Event e) {
    if (e.press != Press::Short) return false;
    switch (e.button) {
    case Button::Back:
        if (learn_wait_) {                            // stop the read; nothing is learned
            learn_wait_ = false;
            fusb302::identify_abort();
            say(kFooters[1], kDim);
            update(ctx_.now);
            return true;
        }
        if (!running()) return false;                 // idle: the Ui pops the page
        ctx_.ui->cancel_twin();
        say(kFooters[1], kDim);
        update(ctx_.now);
        return true;
    case Button::Plus:
    case Button::Minus:
        sel_ = std::clamp(sel_ + (e.button == Button::Plus ? 1 : -1), 0, rows() - 1);
        armed_seq_ = 0;                               // a cursor move disarms
        update(ctx_.now);
        return true;
    case Button::Ok:
        if (!running() && !learn_wait_) {
            if (sel_ >= n_) learn_row(ctx_.now);
            else run_row();
        }
        update(ctx_.now);
        return true;
    }
    return false;
}

PanelLabels TwinPage::labels() const {
    PanelLabels l = list_labels(base_labels());
    const char *ok = "";
    if (running() || learn_wait_) l.b[static_cast<int>(Button::Back)].shrt = "Stop";
    if (running() || learn_wait_) ok = "";
    else if (sel_ >= n_) ok = "Learn";
    else ok = armed_seq_ != 0 ? "Confirm" : "Run";
    l.b[static_cast<int>(Button::Ok)] = {ok, "", "", false};
    return l;
}

void TwinPage::update(uint32_t now_ms) {
    title_badge(false);
    if (note_until_ && static_cast<int32_t>(now_ms - note_until_) >= 0) {
        note_until_ = 0;
        note_.clear();
    }
    if (armed_seq_ != 0 && static_cast<int32_t>(now_ms - armed_until_) >= 0) armed_seq_ = 0;   // the confirm window ran out
    if (started_ && !running()) started_ = false;
    if (learn_wait_ && (!fusb302::identify_busy() || static_cast<int32_t>(now_ms - learn_wait_until_) >= 0)) learn_wait_end();
    if (!loaded_ || gen_ != storage::generation() || stamp_ != twin_hw::predict_stamp()) {
        const bool charger_changed = loaded_ && stamp_ != twin_hw::predict_stamp();
        reload();
        if (charger_changed && !running()) hide_proof_ = true;   // another list: the last run's verdicts no longer describe it
    }
    if (running()) hide_proof_ = false;
    sel_ = std::clamp(sel_, 0, rows() - 1);
    if (armed_seq_ != 0 && (sel_ >= n_ || list_[static_cast<std::size_t>(sel_)].seq != armed_seq_)) armed_seq_ = 0;
    ctx_.rows.set_count(rows());
    ctx_.rows.select(sel_);
    ctx_.rows.draw(now_ms);
    compose_footer();
    draw_footer_cached(ctx_.content, foot_tone_, foot_text_.view(), foot_);
}

}  // namespace k2
