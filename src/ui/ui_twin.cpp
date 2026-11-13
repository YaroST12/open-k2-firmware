// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#include "k2/ui/ui_twin.hpp"

#include <algorithm>

#include "k2/core/storage.hpp"
#include "k2/drivers/fusb302.hpp"
#include "k2/pd/pd_link.hpp"
#include "k2/pd/pd_passive.hpp"
#include "k2/pd/pd_source.hpp"
#include "k2/twin/learn.hpp"
#include "k2/twin/present.hpp"
#include "k2/twin/twin_hw.hpp"
#include "k2/twin/usbdata.hpp"
#include "k2/ui/home_model.hpp"
#include "k2/ui/text.hpp"
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
constexpr const char kAskAgainText[] = "Ask charger again";
constexpr const char kAskAgainNote[] = "resets the link, it relists";
constexpr const char *kActionText[] = {"Prove live", "Re-learn from device", "Delete twin"};
constexpr const char *kFooters[] = {
    "Asks the charger. Confirm", "Stopped. 5 V back", "Tool or test running", "Device inline: unplug it",
    "Power monitor not ready", "No charger to run against", "No twin yet. Learn one", "Twin learned", "Twin updated",
    "Already known", "Plug a device in alone", "Nothing to learn from", "Library full", "Twin not stored",
    "Reading the device", "Device did not answer", "Deletes this twin for good", "Twin deleted", "Saved as another twin",
    "Basis: its sink caps", "Basis: its sniffed Request", "No charger: no predictions", "Twin not deleted",
    "Live proof running",
    // A charger that has not offered: the wake (docs/UI_SYSTEM.md 9.9). The K2 presents Rd only after the confirm.
    "Wakes the charger. Confirm", "Waking charger", "Charger has not offered", "Charger did not offer",
    "Stopped. Charger released",
    // Learning a device inline: the K2 resets the link and asks the device itself (docs/DEVICE_TWIN.md section 9).
    "Resets the link. Confirm", "Resetting the link", "Asking the device", "Stopped. Link reset", "Learned. Contract changed",
    "K2 runs on this VBUS", "K2 holds the contract", "No power: add a charger",
    // The source Hard Resets on the first question: asking through this charger is not possible (docs/DEVICE_TWIN.md 9.1).
    "Charger resets when asked",
    // A device inline, no charger list: the Hard Reset makes the charger list again (docs/DEVICE_TWIN.md section 5).
    "Asking the charger again", "Charger offered again", "Reset not sent",
};
constexpr int kFootWakeArm = 24, kFootWaking = 25, kFootSilent = 26, kFootNoOffer = 27, kFootWakeStop = 28;
constexpr int kFootIqArm = 29, kFootIqReset = 30, kFootIqAsk = 31, kFootIqStop = 32, kFootIqDiffers = 33, kFootIqSelf = 34,
              kFootIqOwn = 35, kFootNoPower = 36, kFootIqResets = 37;
constexpr int kFootAskAgain = 38, kFootOfferedAgain = 39, kFootResetNot = 40;
constexpr const char *kNotes[] = {
    "no charger to predict from", "reads a device alone on the K2", "live proof running", "removes it from the K2",
    "no contract, charger", "stopped", "charger sent no caps", "unsafe VBUS, stopped", "gets 100 of 240W, charger",
    "charger refused 20V", "no PS_RDY at 20V", "charger reset at 20V", "out of band 20.0V", "asks the charger for 20V5A",
    "asks for PPS 20.5V5A",
    "wakes the charger first", "waking the charger", "charger has not offered",
};
constexpr int kNoteWakes = 15, kNoteWaking = 16, kNoteSilent = 17;
constexpr const char *kLimit[] = {"full power", "limit: device max", "limit: charger", "limit: cable 5A"};
static_assert(all_fit(kFooters, kSmallAdvPx, kFooterCells * static_cast<int>(kSmallAdvPx)), "a Device twins footer is wider than the footer line");
static_assert(all_fit(kNotes, kLabelAdv, kit::kNoteMaxPx), "a Device twins note is wider than the note box");
static_assert(all_fit(kActionText, kSmallAdvPx, 224 - ListView::kBarW - kit::kTextX), "a Device twins action runs into the scroll lane");
static_assert(fits_px(kSmallAdvPx, kLearnText, 224 - ListView::kBarW - kit::kTextX), "the Learn row text runs into the scroll lane");
static_assert(fits_px(kSmallAdvPx, kAskAgainText, 224 - ListView::kBarW - kit::kTextX), "the Ask charger again row text runs into the scroll lane");
static_assert(fits_px(kLabelAdv, kAskAgainNote, kit::kNoteMaxPx), "the Ask charger again note is wider than the note box");

// The runner's verdict as the kit's verdict cell shows it: a charger that said no or never answered is a fail, a run the
// user stopped is dim. A twin the K2 skipped (nothing sent) keeps its prediction shape: None here.
kit::Verdict cell_of(live::Verdict v) {
    switch (v) {
    case live::Verdict::Granted: return kit::Verdict::Pass;
    case live::Verdict::Cancelled: return kit::Verdict::Stopped;
    case live::Verdict::Skipped:
    case live::Verdict::None: return kit::Verdict::None;
    default: return kit::Verdict::Fail;   // Refused, NoPsRdy, OutOfBand, NoCaps, Reset, Unsafe
    }
}

constexpr uint32_t kLearnWaitMs = 15000;   // an alone-Identify: contract wait (~650 ms) + the VDMs; generous

// Detail geometry (docs/UI_SYSTEM.md 9.2): body lines from kBodyY, the action box under them.
constexpr int16_t kNameH = 20, kGetsH = 26, kBarLineH = 6, kContractH = 20, kRuleH = 4;
constexpr int16_t kLeft = 6, kRight = 218;
constexpr const char *kSourceWord = "Source";   // over the charger's side of the header
constexpr int16_t kCellY = 156, kCellH = 64;     // the armed ActionCell / the proof box (the charger test's START box)
// The source / device split: one 1 px hairline at kSplitX from the "gets" line down. Left of it the charger's facts (left-aligned,
// x6..100), right of it the device's (right-aligned to x218, x110..218). A fact too wide for its half drops to font 12.
constexpr int16_t kSplitX = 104, kLeftRoom = kSplitX - 4 - kLeft, kRightX = kSplitX + 6, kRightRoom = kRight - kRightX;

template <size_t N>
void put_level(StrBuf<N> &b, uint32_t milli, char unit) {
    char t[12];
    const int n = home::fmt_level(t, milli);
    b.put(std::string_view(t, static_cast<std::size_t>(n))).put_char(unit);
}
template <size_t N>
void put_w(StrBuf<N> &b, uint32_t mw) {
    b.put_u32((mw + 500) / 1000).put_char('W');
}
constexpr uint32_t kNoDraw = 0xFFFFFFFFu;   // no power monitor: the live draw is unknown
const char *limiter_word(twin::Limiter l) {
    switch (l) {
    case twin::Limiter::Charger: return "charger";
    case twin::Limiter::Cable: return "cable";
    default: return "device";
    }
}

// The charger's top offer in the protocol-row format: its highest-power fixed level, "PD 20V3A" ("EPR 28V5A" above 20 V).
template <size_t N>
void put_top_offer(StrBuf<N> &b, const pd::SourceCaps &caps) {
    const pd::Pdo *best = nullptr;
    for (std::size_t i = 0; caps.valid && i < caps.num_pdos && i < caps.pdos.size(); i++) {
        const pd::Pdo &p = caps.pdos[i];
        if (p.type != pd::PdoType::Fixed) continue;
        if (!best || p.max_mw > best->max_mw || (p.max_mw == best->max_mw && p.max_mv > best->max_mv)) best = &p;
    }
    if (!best) return;
    b.put(best->max_mv > 20000 ? "EPR " : "PD ");
    put_level(b, best->max_mv, 'V');
    put_level(b, best->max_ma, 'A');
}

// One facts row of the InfoBlock (20 px): the charger's fact left-aligned, the device's right-aligned. Both take font 16, or both
// font 12 when either is wider than its half, so a row never mixes sizes. "--" (nothing known) is faint.
void paint_pair(const Surface &s, int16_t y, std::string_view l, std::string_view r) {
    const bool big = text_width(kSmall, l) <= kLeftRoom && text_width(kSmall, r) <= kRightRoom;
    const Font &f = big ? kSmall : kLabelFont;
    const int16_t ty = static_cast<int16_t>(y + (kit::kInfoPitch - f.height) / 2);
    char lb[kFitBuf], rb[kFitBuf];
    if (!l.empty()) {
        const std::string_view t = fit_data(f, l, kLeftRoom, lb);
        gfx_text(s, f, kLeft, ty, t, l == "--" ? kFaint : kText, kBg);
    }
    if (!r.empty()) {
        const std::string_view t = fit_data(f, r, kRightRoom, rb);
        gfx_text(s, f, static_cast<int16_t>(kRight - text_width(f, t)), ty, t, r == "--" ? kFaint : kText, kBg);
    }
}

void paint_split(const Surface &s, int16_t y, int16_t h) { gfx_fill(s, kSplitX, y, 1, h, kRule); }

// The ink of a prediction's grade: green gets >= wants, amber from 60 %, red below (and no contract, 5 V only).
uint16_t grade_ink(const twin::Outcome &o, uint32_t want) {
    switch (twin::grade(o, want)) {
    case twin::Grade::Full: return kOk;
    case twin::Grade::Partial: return kCaution;
    case twin::Grade::Poor: break;
    }
    return kError;
}

}  // namespace

bool TwinPage::running() const { return twin_hw::live_running(); }

const TwinPage::Entry *TwinPage::entry_by_seq(uint8_t seq) const {
    for (int i = 0; i < n_; i++)
        if (list_[static_cast<std::size_t>(i)].seq == seq) return &list_[static_cast<std::size_t>(i)];
    return nullptr;
}

void TwinPage::draw_static() {
    title_ = detail_ ? "Twin" : "Device twins";
    has_icon_ = !detail_;
    title_bar();
    if (detail_) {
        gfx_fill(ctx_.content, 0, kit::kBelowBar, ctx_.content.w, static_cast<int16_t>(kFooterY - kit::kBelowBar), kBg);
        det_sig_ = 0;
        draw_next_ = 0;   // sample the live draw at once
        cell_.invalidate();
        vbus_shown_.clear();
    } else {
        kit::RowList::clear_body_around(ctx_.content);
        ctx_.rows.init(ctx_.content, kit::kBodyY, &TwinPage::row_fn, this);
        ctx_.rows.set_count(rows());
        ctx_.rows.select(sel_);
    }
    invalidate();
}

void TwinPage::invalidate() {
    foot_.invalidate();
    ctx_.rows.invalidate();
    det_sig_ = 0;
    cell_.invalidate();
}

void TwinPage::repaint() {
    draw_static();
    update(ctx_.now);
}

void TwinPage::enter() {
    loaded_ = false;
    sel_ = 0;
    arm_ = Arm::None;
    note_.clear();
    note_until_ = 0;
    hide_proof_ = false;
    started_ = false;
    iq_wait_ = false;
    renego_wait_ = false;
    renego_ok_ = false;
    detail_ = false;
    wake_ok_ = false;
    title_ = "Device twins";
    has_icon_ = true;
    reload();
    invalidate();
}

// Paged away or popped while this page's own proof is running: ask 5 V back and end it (a run the remote started is left alone).
// The Rd a woken charger's proof left on for the page goes back too: opening the page presents nothing, leaving it leaves nothing.
void TwinPage::leave() {
    if (started_ && running()) ctx_.ui->cancel_twin();
    twin_hw::wake_release();
    if (learn_wait_) fusb302::identify_abort();   // the read this page started: release the line, learn nothing
    learn_wait_ = false;
    if (iq_wait_) twin_hw::inline_cancel();       // the interrogation this page started: its final reset goes out, nothing is learned
    iq_wait_ = false;
    renego_wait_ = false;                         // the Hard Reset is already out; the charger relists without this page
    started_ = false;
    detail_ = false;
    title_ = "Device twins";
    has_icon_ = true;
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
        if (detail_ && e.seq == det_seq_) det_ = t;
    }
    offer_.clear();
    put_top_offer(offer_, pd::active_caps());
    gen_ = storage::generation();
    stamp_ = twin_hw::predict_stamp();
    loaded_ = true;
    sel_ = std::clamp(sel_, 0, list_last());
    if (detail_) build_info();
    det_sig_ = 0;
}

void TwinPage::say(std::string_view text, uint16_t color) {
    note_.clear();
    note_.put(text);
    note_color_ = color;
    note_until_ = ctx_.now + 2500;
}

// The focused twin's note: the prediction ("gets 36 of 55W, cable"), or the proof on this charger ("proved 36W").
void TwinPage::entry_note(const Entry &e, bool running_this, StrBuf<40> &out) const {
    out.clear();
    if (!e.have) {   // no list to predict from: a charger waiting on the plug is a charger that has not offered yet
        if (running_this && twin_hw::live().waking()) out.put(kNotes[kNoteWaking]);
        else if (running_this) out.put(kNotes[2]);
        else out.put(wake_ok_ ? kNotes[kNoteSilent] : kNotes[0]);
        return;
    }
    if (running_this) {
        out.put(kNotes[2]);
        return;
    }
    const live::Slot &slot = twin_hw::live().slot(e.seq);
    const twin::Outcome &o = e.out;
    if (!hide_proof_ && cell_of(slot.verdict) != kit::Verdict::None) {
        switch (slot.verdict) {
        case live::Verdict::Granted: out.put("proved "); put_w(out, o.mw); return;
        case live::Verdict::Cancelled: out.put(kNotes[5]); return;
        case live::Verdict::NoCaps: out.put(kNotes[6]); return;
        case live::Verdict::Unsafe: out.put(kNotes[7]); return;
        case live::Verdict::OutOfBand:
            out.put("out of band");
            if (slot.measured_mv) {
                out.put_char(' ');
                out.put_fixed(slot.measured_mv, 1).put_char('V');
            }
            return;
        case live::Verdict::Refused: out.put("charger refused "); break;
        case live::Verdict::NoPsRdy: out.put("no PS_RDY at "); break;
        case live::Verdict::Reset: out.put("charger reset at "); break;
        default: out.put(live::name(slot.verdict)); return;
        }
        put_level(out, o.mv, 'V');
        return;
    }
    if (o.kind == twin::Kind::None) {
        out.put(kNotes[4]);
    } else if (o.kind == twin::Kind::FiveV) {
        out.put("5V only, ").put(limiter_word(o.limiter));
    } else {
        // observed: from the device's own Request, what it asked for. A predicted PPS contract is a range, so it says "up to":
        // the device picks its own voltage inside the overlap, the K2 does not claim one.
        out.put(o.observed ? "asked " : o.kind == twin::Kind::Pps ? "PPS up to " : "gets ");
        const uint32_t got = (o.mw + 500) / 1000, want = (o.want_mw + 500) / 1000;
        if (want > got) out.put_u32(got).put(" of ").put_u32(want).put("W, ").put(limiter_word(o.limiter));
        else out.put_u32(got).put(o.limiter == twin::Limiter::Device ? "W, device max" : "W, full");
    }
}

void TwinPage::build_row(int index, bool focused, kit::RowSpec &out) {
    if (detail_) build_action_row(index, focused, out);
    else build_list_row(index, focused, out);
}

// What a list row shows: the shape marker (or the verdict cell after a proof) and the full name; the note when focused.
void TwinPage::build_list_row(int index, bool focused, kit::RowSpec &out) {
    out = kit::RowSpec{};
    if (index == n_) {   // "Learn from device"
        out.marker = kit::Marker::Icon;
        out.icon = Icon::Chip;
        out.text = kLearnText;
        if (focused) out.note = kNotes[1];
        return;
    }
    if (index == n_ + 1) {   // "Ask charger again": only while a device is inline and the charger listed nothing
        out.marker = kit::Marker::Icon;
        out.icon = Icon::Rotate;
        out.text = kAskAgainText;
        if (focused) out.note = kAskAgainNote;
        return;
    }
    const Entry &e = list_[static_cast<std::size_t>(index)];
    out.text = std::string_view(e.name.data());
    out.data = true;                                  // a name from the library: fitted by fit_data, counted
    const bool this_running = running() && twin_hw::live().current() == e.seq;
    const kit::Verdict cell = this_running ? kit::Verdict::Running
                              : hide_proof_ ? kit::Verdict::None
                                            : cell_of(twin_hw::live().slot(e.seq).verdict);
    if (this_running || cell != kit::Verdict::None) {
        out.marker = kit::Marker::Verdict;
        out.state = static_cast<uint8_t>(cell);
    } else if (e.have && e.out.kind == twin::Kind::None) {
        out.marker = kit::Marker::Verdict;            // no contract: the dim dash
        out.state = static_cast<uint8_t>(kit::Verdict::None);
    } else if (e.have) {
        out.marker = kit::Marker::Level;
        const bool full = present::is_full(e.out);
        out.state = e.out.kind == twin::Kind::FiveV ? 0 : full ? 2 : 1;
        out.marker_color = full && e.out.kind != twin::Kind::FiveV ? kText : kCaution;
    }
    if (!focused) return;
    entry_note(e, this_running, row_note_);
    out.note = row_note_.view();
}

// The detail's action rows: Prove live (the verdict cell once proved), Re-learn from device, Delete twin.
void TwinPage::build_action_row(int index, bool focused, kit::RowSpec &out) {
    out = kit::RowSpec{};
    out.text = kActionText[std::clamp(index, 0, kActions - 1)];
    const Entry *e = det_entry();
    if (index == 0) {
        if (e && !hide_proof_) {
            const kit::Verdict cell = cell_of(twin_hw::live().slot(e->seq).verdict);
            if (cell != kit::Verdict::None) {
                out.marker = kit::Marker::Verdict;
                out.state = static_cast<uint8_t>(cell);
            }
        }
        const bool provable = e && e->have && e->out.kind != twin::Kind::None && e->out.kind != twin::Kind::FiveV;
        const bool wakes = e && !e->have && wake_ok_ && !running();   // no list yet, a charger on the plug: the proof wakes it first
        if (!provable && !wakes) out.text_color = kDim;
        if (!focused) return;
        row_note_.clear();
        if (!e) {
            row_note_.put(kNotes[0]);
        } else if (!e->have) {
            if (wakes && out.marker != kit::Marker::Verdict) row_note_.put(kNotes[kNoteWakes]);
            else entry_note(*e, running(), row_note_);
        } else if (out.marker == kit::Marker::Verdict) {
            entry_note(*e, false, row_note_);
        } else if (provable) {
            if (e->out.kind == twin::Kind::Pps) {   // a range: the proof asks for its top, the device would pick lower
                row_note_.put("asks for PPS up to ");
                put_level(row_note_, e->out.mv, 'V');
            } else {
                row_note_.put("asks the charger for ");
                put_level(row_note_, e->out.mv, 'V');
            }
            put_level(row_note_, e->out.ma, 'A');
        } else {
            entry_note(*e, false, row_note_);
        }
        out.note = row_note_.view();
        return;
    }
    if (focused) out.note = index == 1 ? kNotes[1] : kNotes[3];
}

void TwinPage::row_fn(void *self, int index, bool focused, kit::RowSpec &out) {
    static_cast<TwinPage *>(self)->build_row(index, focused, out);
}

// The one fact for the footer: the last result or refusal for a moment, else the read or run in flight, the armed confirm,
// then (list) the last run's tally or what this charger does for the stored devices, (detail) what the prediction rests on.
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
    if (iq_wait_) {   // the resets drop VBUS: say which step the link is in
        const pd::IqPhase p = twin_hw::inline_status().phase;
        const bool resetting = p == pd::IqPhase::Reset1 || p == pd::IqPhase::Sniff1 || p == pd::IqPhase::Reset2 || p == pd::IqPhase::Sniff2;
        foot_text_.put(kFooters[resetting ? kFootIqReset : kFootIqAsk]);
        return;
    }
    if (renego_wait_) {
        foot_text_.put(kFooters[kFootAskAgain]);
        return;
    }
    if (running()) {
        if (detail_) {
            foot_text_.put(twin_hw::live().waking() ? kFooters[kFootWaking] : kFooters[23]);
            return;
        }
        if (twin_hw::live().waking()) {
            foot_text_.put(kFooters[kFootWaking]);
            return;
        }
        const live::Runner &r = twin_hw::live();
        foot_text_.put("Live proof ").put_u32(std::min<unsigned>(r.done_count() + 1, r.count())).put(" of ").put_u32(r.count());
        return;
    }
    if (arm_ == Arm::Prove) {
        foot_tone_ = FooterTone::Caution;
        const Entry *e = det_entry();
        if (e && e->have) {
            foot_text_.put(e->out.kind == twin::Kind::Pps ? "Asks for PPS up to " : "Asks the charger for ");
            put_level(foot_text_, e->out.mv, 'V');
        } else {
            foot_text_.put(wake_ok_ ? kFooters[kFootWakeArm] : kFooters[0]);
        }
        return;
    }
    if (arm_ == Arm::Delete || arm_ == Arm::Learn || arm_ == Arm::Renego) {
        foot_tone_ = FooterTone::Caution;
        foot_text_.put(kFooters[arm_ == Arm::Delete ? 16 : kFootIqArm]);   // Learn and Renego both reset the link
        return;
    }
    if (detail_) {
        const Entry *e = det_entry();
        if (!e || !e->have) foot_text_.put(wake_ok_ ? kFooters[kFootSilent] : kFooters[21]);
        else foot_text_.put(e->out.observed ? kFooters[20] : kFooters[19]);
        return;
    }
    unsigned ran = 0, granted = 0;
    if (!hide_proof_) {
        for (int i = 0; i < n_; i++) {
            const live::Verdict v = twin_hw::live().slot(list_[static_cast<std::size_t>(i)].seq).verdict;
            if (v == live::Verdict::None || v == live::Verdict::Skipped) continue;
            ran++;
            if (v == live::Verdict::Granted) granted++;
        }
    }
    if (ran > 0) {
        foot_tone_ = granted == ran ? FooterTone::Ok : FooterTone::Caution;
        foot_text_.put_u32(granted).put(" of ").put_u32(ran).put(" proved");
        return;
    }
    if (n_ > 0 && have_caps_ && offer_.view().size()) {
        foot_text_.put(offer_.view()).put(": ").put_u32(static_cast<uint32_t>(full_)).put(" of ").put_u32(static_cast<uint32_t>(n_)).put(" full");
        return;
    }
    if (n_ > 0 && !have_caps_ && wake_ok_) {   // a charger waits on the plug: it has not offered yet, not "no charger"
        foot_text_.put(kFooters[kFootSilent]);
        return;
    }
    char buf[32];
    present::footer(static_cast<unsigned>(n_), static_cast<unsigned>(full_), have_caps_, buf);
    foot_text_.put(buf);
}

void TwinPage::learn() { learned(twin_hw::learn_from_identify(std::string_view{}, true), false); }

// What a learn stored, or why it stored nothing. `contract_changed`: an interrogation's renegotiated contract differs from the
// first one (the device asked differently once it was asked): learned, but said in amber.
void TwinPage::learned(const twin_hw::Learned &r, bool contract_changed) {
    switch (r.how) {
    case twin_hw::Learn::Ok:
        if (detail_ && r.store.seq != det_seq_) say(kFooters[18], kCaution);   // another device answered: this twin is kept
        else if (contract_changed) say(kFooters[kFootIqDiffers], kCaution);
        else say(r.store.unchanged ? kFooters[9] : r.store.existed ? kFooters[8] : kFooters[7], r.store.unchanged ? kDim : kOk);
        reload();
        if (!detail_)
            if (const Entry *e = entry_by_seq(r.store.seq)) sel_ = static_cast<int>(e - list_.data());
        break;
    case twin_hw::Learn::NotReady: say(kFooters[10], kCaution); break;
    case twin_hw::Learn::NotInline:
    case twin_hw::Learn::NoContract: say(kFooters[11], kCaution); break;
    case twin_hw::Learn::Store: say(r.store.err == twin::Err::Full ? kFooters[12] : kFooters[13], kCaution); break;
    }
}

// OK on Learn (or Re-learn). A device alone on the K2 (a dual-role phone: VBUS dark, no source caps, the toggling Rp) is read
// first: the page starts the same Identify as Who's attached > Sink and learns when it ends (update()). Anything else
// learns from the last Identify, as before (a sink behind the K2 inline, a device identified a moment ago).
// A device inline behind a charger is asked on that link (learn_inline, after a confirm: the run resets the link twice). With
// no device inline, none alone, VBUS dark and no Identify to learn from, nothing can be asked: the footer says to add a charger.
void TwinPage::learn_row(uint32_t now_ms) {
    const pd::Link &link = ctx_.ui->link();
    const bool inline_link = link.inline_mode() || ctx_.inline_mode;
    const int32_t vbus = ctx_.pm_online ? ctx_.vbus_fast_mv : -1;
    const bool alone = pd::identify_alone(inline_link, ctx_.pm_online, vbus, fusb302::caps_this_attach(), link.wait_kind());
    const bool dark = vbus >= 0 && vbus < pd::kDetachVbusMv && !pd::active_caps().valid;   // no VBUS and no charger listing
    const twin::LearnPath path = twin::learn_path(alone, inline_link, fusb302::identify_current(), dark);
    if (path != twin::LearnPath::Interrogate) arm_ = Arm::None;   // the device left the link between the two presses
    switch (path) {
    case twin::LearnPath::Identify:
        if (ctx_.ui->identify(now_ms) == Ui::IdResult::Started) {
            learn_wait_ = true;
            learn_wait_until_ = now_ms + kLearnWaitMs;
            note_.clear();
            note_until_ = 0;
            return;
        }
        break;
    case twin::LearnPath::Interrogate: learn_inline(); return;
    case twin::LearnPath::NoPower: say(kFooters[kFootNoPower], kCaution); return;
    case twin::LearnPath::LastIdentify: break;
    }
    learn();
}

// OK on Learn with a device inline. Like Prove live, the first press asks without the confirm: a real refusal (a tool running,
// the K2's own contract, the K2 powered from this VBUS) is said at once, "needs the confirm" arms for kConfirmWindowMs.
void TwinPage::learn_inline() {
    const bool confirm = arm_ == Arm::Learn && static_cast<int32_t>(ctx_.now - armed_until_) < 0;
    arm_ = Arm::None;
    switch (twin_hw::inline_start(std::string_view{}, true, confirm, ctx_.ui->twin_ctx(ctx_.now))) {
    case pd::IqRefusal::None:
        iq_wait_ = true;
        note_.clear();
        note_until_ = 0;
        break;
    case pd::IqRefusal::NeedConfirm:
        arm_ = Arm::Learn;
        armed_until_ = ctx_.now + Ui::kConfirmWindowMs;
        note_.clear();
        note_until_ = 0;
        break;
    case pd::IqRefusal::Running:
    case pd::IqRefusal::Busy: say(kFooters[2], kCaution); break;
    case pd::IqRefusal::SelfPowered: say(kFooters[kFootIqSelf], kCaution); break;
    case pd::IqRefusal::OwnContract: say(kFooters[kFootIqOwn], kCaution); break;
    case pd::IqRefusal::NoCc:
    case pd::IqRefusal::NotInline: say(kFooters[11], kCaution); break;
    }
}

// OK on "Ask charger again" (a device inline, the charger listed nothing). Same shape as the interrogation: the first OK arms
// (the Hard Reset drops VBUS under the device), a real refusal is said at once, the second OK within kConfirmWindowMs sends the
// one inline Hard Reset (the lab override of `pd reset --inline --confirm`: it makes the charger send its list again, which the
// K2 sniffs). update() then waits for the list.
void TwinPage::renegotiate() {
    const bool confirm = arm_ == Arm::Renego && static_cast<int32_t>(ctx_.now - armed_until_) < 0;
    arm_ = Arm::None;
    const twin_hw::LiveCtx c = ctx_.ui->twin_ctx(ctx_.now);
    if (c.busy || c.owners_busy || running()) {
        say(kFooters[2], kCaution);
        return;
    }
    if (!confirm) {
        arm_ = Arm::Renego;
        armed_until_ = ctx_.now + Ui::kConfirmWindowMs;
        note_.clear();
        note_until_ = 0;
        return;
    }
    if (!ctx_.ui->trigger_hard_reset(ctx_.now, true)) {   // the driver refused (no CC): nothing went out
        say(kFooters[kFootResetNot], kCaution);
        return;
    }
    renego_wait_ = true;
    renego_until_ = ctx_.now + kRenegWaitMs;
    note_.clear();
    note_until_ = 0;
}

// Every tick: the wait for the charger's list after the Hard Reset, and whether the "Ask charger again" row shows (a device
// inline behind a charger that listed nothing, and nothing else owns the link). Not inlined: Ui::tick's frame is near its limit.
[[gnu::noinline]] void TwinPage::renego_poll(uint32_t now_ms) {
    if (renego_wait_) {   // the Hard Reset is out: the charger lists again, or it does not
        if (pd::active_caps().valid) {
            renego_wait_ = false;
            say(kFooters[kFootOfferedAgain], kOk);
        } else if (static_cast<int32_t>(now_ms - renego_until_) >= 0) {
            renego_wait_ = false;
            say(kFooters[kFootNoOffer], kCaution);
        }
    }
    const bool renego_ok = (ctx_.ui->link().inline_mode() || ctx_.inline_mode) && !pd::active_caps().valid && !running() &&
                           !learn_wait_ && !iq_wait_;
    if (renego_ok != renego_ok_) {
        renego_ok_ = renego_ok;
        ctx_.rows.invalidate();
    }
}

// The interrogation is over: what it learned (twin_hw::inline_poll learned and saved it), else the device never answered.
void TwinPage::learn_inline_end() {
    iq_wait_ = false;
    const twin_hw::InlineLearn &l = twin_hw::inline_learn();
    // A source that Hard Resets on the first question is the cause to name, not a device that stayed silent.
    const bool resets = twin_hw::inline_status().why == pd::IqWhy::SourceResets;
    if (!l.done) {
        say(kFooters[resets ? kFootIqResets : 15], kCaution);
        return;
    }
    if (l.result.how == twin_hw::Learn::NoContract) {   // nothing answered, nothing sniffed
        say(kFooters[resets ? kFootIqResets : 15], kCaution);
        return;
    }
    learned(l.result, twin_hw::inline_status().match == pd::IqMatch::Differs);
}

// The Identify this page started is over (or ran out of time): learn from it, or say the device never answered.
void TwinPage::learn_wait_end() {
    learn_wait_ = false;
    if (fusb302::identify_busy()) fusb302::identify_abort();
    if (fusb302::identify_status().state == pd::IdState::Done && fusb302::identify_current()) learn();
    else say(kFooters[15], kCaution);
}

// OK on Prove live. The first press asks the runner without the confirm: its refusal is the real one (a test running, a device
// inline, no monitor, no charger), and "needs the confirm" arms the cell for kConfirmWindowMs. The second press starts it.
void TwinPage::run_entry(const Entry &e) {
    const std::array<uint8_t, 1> seqs{e.seq};
    const bool confirm = arm_ == Arm::Prove && static_cast<int32_t>(ctx_.now - armed_until_) < 0;
    arm_ = Arm::None;
    // keep_wake: a charger woken for this proof keeps its Rd after the run, so the list stays on screen; leave() gives it back.
    switch (twin_hw::live_start(seqs, confirm, ctx_.ui->twin_ctx(ctx_.now), true)) {
    case live::Refusal::None:
        started_ = true;
        hide_proof_ = false;
        note_.clear();
        note_until_ = 0;
        break;
    case live::Refusal::NeedConfirm:
        arm_ = Arm::Prove;
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

void TwinPage::open_detail() {
    const Entry &e = list_[static_cast<std::size_t>(sel_)];
    if (!storage::twin_get(e.seq, det_)) return;
    detail_ = true;
    det_seq_ = e.seq;
    act_ = 0;
    first_ = 0;
    arm_ = Arm::None;
    note_.clear();
    note_until_ = 0;
    build_info();
    repaint();
}

void TwinPage::close_detail() {
    detail_ = false;
    arm_ = Arm::None;
    if (const Entry *e = entry_by_seq(det_seq_)) sel_ = static_cast<int>(e - list_.data());
    sel_ = std::clamp(sel_, 0, list_last());
    ctx_.rows.restart();
    repaint();
}

void TwinPage::delete_twin() {
    arm_ = Arm::None;
    const twin::Err err = storage::twin_remove(det_seq_);
    reload();
    if (err != twin::Err::Ok) {
        say(kFooters[22], kCaution);
        return;
    }
    say(kFooters[17], kOk);
    close_detail();
}

// ---- detail layout ----------------------------------------------------------------------------------------------------------

// The InfoBlock texts of the open twin, two columns under the Source / device header: the charger's offer on the left (its
// fixed span, then its PPS range), the device's wants on the right (its fixed levels, then its PPS range), and what the device
// declared on USB as the last right-hand fact. Rows: fixed levels, PPS ranges (only when either side lists one), USB.
void TwinPage::build_info() {
    info_n_ = 0;
    auto add = [&](const StrBuf<24> &l, const StrBuf<24> &r) {
        auto &lv = info_l_[static_cast<std::size_t>(info_n_)];
        auto &rv = info_r_[static_cast<std::size_t>(info_n_)];
        lv.clear();
        rv.clear();
        lv.put(l.view());
        rv.put(r.view());
        info_n_++;
    };
    auto range = [](StrBuf<24> &b, const char *tag, uint32_t lo, uint32_t hi, uint32_t ma) {
        b.put(tag);
        put_level(b, lo, '-');
        put_level(b, hi, 'V');
        b.put_char(' ');
        put_level(b, ma, 'A');
    };
    // Right column, wants: the device's sink PDOs.
    StrBuf<24> wfixed, wpps, fixed, pps;
    for (std::size_t i = 0; i < det_.num_pdos && i < det_.pdos.size(); i++) {
        const pd::Pdo p = pd::decode_pdo(det_.pdos[i]);
        if (p.type == pd::PdoType::Fixed) {
            if (wfixed.view().size()) wfixed.put_char(' ');
            put_level(wfixed, p.max_mv, 'V');
        } else if (p.type == pd::PdoType::Pps && wpps.view().empty()) {
            range(wpps, "PPS ", p.min_mv, p.max_mv, p.max_ma);
        }
    }
    // Left column, offer: the charger's list now.
    const pd::SourceCaps &caps = pd::active_caps();
    uint32_t lo = 0, hi = 0, ma = 0, mw = 0, plo = 0, phi = 0, pma = 0, nfixed = 0;
    StrBuf<40> levels;                                       // the fixed voltages, "5 9 15 20V" (V once, on the last)
    for (std::size_t i = 0; caps.valid && i < caps.num_pdos && i < caps.pdos.size(); i++) {
        const pd::Pdo &p = caps.pdos[i];
        if (p.type == pd::PdoType::Fixed) {
            lo = lo ? std::min<uint32_t>(lo, p.max_mv) : p.max_mv;
            hi = std::max<uint32_t>(hi, p.max_mv);
            ma = std::max<uint32_t>(ma, p.max_ma);
            mw = std::max<uint32_t>(mw, p.max_mw);
            nfixed++;
            char t[12];
            const int n = home::fmt_level(t, p.max_mv);
            if (levels.view().size()) levels.put_char(' ');
            levels.put(std::string_view(t, static_cast<std::size_t>(n)));
        } else if (p.type == pd::PdoType::Pps && static_cast<uint32_t>(p.max_mv - p.min_mv) > phi - plo) {
            plo = p.min_mv, phi = p.max_mv, pma = p.max_ma;
        }
    }
    if (nfixed == 1) {                                       // one level: it and its own current
        put_level(fixed, hi, 'V');
        put_level(fixed, ma, 'A');
    } else if (nfixed > 1) {
        levels.put_char('V');
        // The list when it fits the column (font 12 at worst); else a span with the best power. Never the largest current: no
        // single level need carry it, and "5-20V 3A" reads as a PPS range.
        if (text_width(kLabelFont, levels.view()) <= kLeftRoom) {   // at most 15 characters: fits the 23 of the buffer
            fixed.put(levels.view());
        } else {
            put_level(fixed, lo, '-');
            put_level(fixed, hi, 'V');
            fixed.put_char(' ');
            put_w(fixed, mw);
        }
    }
    if (phi) range(pps, "PPS ", plo, phi, pma);
    if (fixed.view().empty()) fixed.put("--");    // nothing known: a faint "--" on its side
    if (wfixed.view().empty()) wfixed.put("--");
    add(fixed, wfixed);
    if (!pps.view().empty() || !wpps.view().empty()) add(pps, wpps);
    // USB, a device fact: what it declared while alone ("USB 2.0 480M").
    char u[40];
    twin::declared_text(det_.id_hi, det_.decl, u);
    std::string_view uv(u);
    if (uv.substr(0, 4) == "USB ") uv.remove_prefix(4);
    fixed.clear();
    wpps.clear();
    wpps.put("USB ").put(uv != "-" ? uv : "--");
    add(fixed, wpps);
}

int16_t TwinPage::line_h(int i) const {
    switch (i) {
    case 0: return kNameH;
    case 1: return kGetsH;
    case 2: return kBarLineH;
    case 3: return kContractH;
    case 4: return kRuleH;
    default: return kit::kInfoPitch;
    }
}

// The action box sits under the last body line; lines scroll off the top (whole lines) until the focused action row fits.
// While a cell is armed or a proof runs, the box is the charger test's START box (y156, 64 px) and the lines above it stay put.
void TwinPage::layout_detail() {
    if (arm_ != Arm::None || running()) {
        first_ = 0;
        box_y_ = kCellY;
        return;
    }
    const int n = line_count();
    for (first_ = 0; first_ < n; first_++) {
        int y = kit::kBodyY;
        for (int i = first_; i < n; i++) y += line_h(i);
        if (y + (act_ + 1) * kit::kRowH + kit::kFocusExtra <= kFooterY) {
            box_y_ = static_cast<int16_t>(y);
            return;
        }
    }
    box_y_ = kit::kBodyY;
}

void TwinPage::paint_line(int i, int16_t y) {
    const Surface &s = ctx_.content;
    const Entry *e = det_entry();
    const bool have = e && e->have && e->out.kind != twin::Kind::None;
    char buf[kFitBuf];
    switch (i) {
    case 0: {   // "Source" over what the charger gives (left), the device's name over what it wants (right)
        gfx_text(s, kSmall, kLeft, static_cast<int16_t>(y + 2), kSourceWord, kDim, kBg);
        const int16_t room = static_cast<int16_t>(kRight - kLeft - text_width(kSmall, kSourceWord) - kSmallAdvPx);
        // A long name drops to font 12 on the same baseline rather than lose its end (24 characters fit at 6 px).
        const bool small = text_width(kSmall, det_.name.data()) > room;
        const auto &font = small ? kLabelFont : kSmall;
        const int16_t ny = static_cast<int16_t>(y + 2 + (small ? kSmall.height - kLabelFont.height - 1 : 0));
        const std::string_view name = fit_data(font, det_.name.data(), room, buf);
        gfx_text(s, font, static_cast<int16_t>(kRight - text_width(font, name)), ny, name, kText, kBg);
        return;
    }
    case 1: {   // "gets 36W" ... "wants 55W": font 12 words on the font 24 values' baseline
        const bool proved = e && !hide_proof_ && twin_hw::live().slot(e->seq).verdict == live::Verdict::Granted;
        // Both numbers wear the prediction's grade (gets against wants: full, 60 % and over, below); with no prediction yet
        // gets is a faint "--" and wants stays neutral. No contract and 5 V only grade red.
        uint16_t ink = kFaint, want_ink = kText;
        if (e && e->have) {
            ink = grade_ink(e->out, det_.num_pdos ? twin::want_mw(det_) : e->out.want_mw);   // a Request-only twin: against what it asked
            want_ink = det_.num_pdos ? ink : kText;
        }
        paint_split(s, y, kGetsH);
        // A prediction built from the device's own Request is what it asked for (its contract's power), not what it draws.
        const bool asked = e && e->have && e->out.observed;
        const char *word = proved ? "proved" : asked ? "asked" : "gets";
        const int16_t vy = static_cast<int16_t>(y + 1);
        const int16_t wy = static_cast<int16_t>(vy + kMid.height - kLabelFont.height - 2);
        gfx_text(s, kLabelFont, kLeft, wy, word, kDim, kBg);
        const int16_t gx = static_cast<int16_t>(kLeft + text_width(kLabelFont, word) + 4);
        StrBuf<8> v;
        if (have) put_w(v, e->out.mw);
        else v.put("--");
        gfx_text(s, kMid, gx, vy, v.view(), ink, kBg);
        v.clear();
        // No sink PDOs learned: what the device wants is unknown, never "0W". Its live draw stands there instead.
        const bool unknown = det_.num_pdos == 0;
        if (!unknown) put_w(v, twin::want_mw(det_));
        else if (draw_mw_ == kNoDraw) v.put("--");
        else if (draw_mw_ < 10000) v.put_fixed(static_cast<int32_t>(draw_mw_), 1).put_char('W');
        else put_w(v, draw_mw_);
        const char *rword = unknown ? "draws" : "wants";
        const int16_t vx = static_cast<int16_t>(kRight - text_width(kMid, v.view()));
        gfx_text(s, kMid, vx, vy, v.view(), want_ink, kBg);
        gfx_text(s, kLabelFont, static_cast<int16_t>(vx - 4 - text_width(kLabelFont, rword)), wy, rword, kDim, kBg);
        return;
    }
    case 2: {   // the bar: track = what it wants, fill = what it gets
        const int16_t w = static_cast<int16_t>(kRight - kLeft);
        paint_split(s, y, kBarLineH);   // the bar crosses the hairline
        const uint32_t want = twin::want_mw(det_);
        int16_t fill = 0;
        if (have && want) fill = static_cast<int16_t>(std::min<uint32_t>(w, static_cast<uint32_t>(w) * e->out.mw / want));
        if (fill) gfx_fill(s, kLeft, static_cast<int16_t>(y + 1), fill, 3, grade_ink(e->out, want));   // the fill wears the grade
        if (fill < w) gfx_fill(s, static_cast<int16_t>(kLeft + fill), static_cast<int16_t>(y + 1), static_cast<int16_t>(w - fill), 3, kRule);
        return;
    }
    case 3: {   // "12V3A" (volts, amps in their colours) and what limits it
        paint_split(s, y, kContractH);
        if (!have) {
            gfx_text(s, kSmall, kLeft, static_cast<int16_t>(y + 2), "--", kFaint, kBg);
            return;
        }
        StrBuf<12> v, a;
        put_level(v, e->out.mv, 'V');
        put_level(a, e->out.ma, 'A');
        gfx_text(s, kSmall, kLeft, static_cast<int16_t>(y + 2), v.view(), kVolt, kBg);
        gfx_text(s, kSmall, static_cast<int16_t>(kLeft + text_width(kSmall, v.view())), static_cast<int16_t>(y + 2), a.view(), kAmp, kBg);
        StrBuf<24> lim;
        switch (e->out.limiter) {
        case twin::Limiter::None: lim.put(kLimit[0]); break;
        case twin::Limiter::Device: lim.put(kLimit[1]); break;
        case twin::Limiter::Charger: lim.put(kLimit[2]); break;
        case twin::Limiter::Cable: lim.put("limit: cable "); put_level(lim, e->out.ma, 'A'); break;   // 18 characters at most: clear of the hairline
        }
        gfx_text(s, kLabelFont, static_cast<int16_t>(kRight - text_width(kLabelFont, lim.view())), static_cast<int16_t>(y + 5),
                 lim.view(), kDim, kBg);
        return;
    }
    case 4:
        gfx_fill(s, 0, static_cast<int16_t>(y + 1), s.w, 2, kRule);
        return;
    default: {
        const std::size_t k = static_cast<std::size_t>(i - 5);
        if (k >= info_l_.size()) return;
        paint_split(s, y, kit::kInfoPitch);
        paint_pair(s, y, info_l_[k].view(), info_r_[k].view());
        return;
    }
    }
}

// The detail body. The lines and the action box repaint together when what they show changed (a key, the library, the charger,
// a verdict); otherwise only the RowList, the armed cell's drain bar and the proof's VBUS move.
void TwinPage::paint_detail(uint32_t now_ms) {
    layout_detail();
    const Entry *e = det_entry();
    const bool run = running();
    uint32_t sig = mix_u32(mix_init(), static_cast<uint32_t>(first_) | (static_cast<uint32_t>(box_y_) << 8) |
                                           (static_cast<uint32_t>(arm_) << 16) | (static_cast<uint32_t>(run) << 20));
    // While a proof runs the lines hold still: the charger's list comes and goes with every re-attach, and the verdict, the list
    // and the notes repaint once, when it ends. Only the box's Step / Asked and VBUS rows move meanwhile.
    if (!run) {
        sig = mix_u32(sig, (static_cast<uint32_t>(hide_proof_) << 1) | static_cast<uint32_t>(wake_ok_));
        sig = mix_u32(sig, gen_ ^ (stamp_ << 1));
        if (e) sig = mix_u32(sig, static_cast<uint32_t>(twin_hw::live().slot(e->seq).verdict) + 1);
        // A twin with no sink PDOs shows the live draw instead of "wants": sampled once a second, in 0.1 W steps, so the body
        // repaints only when the figure moves.
        if (e && det_.num_pdos == 0) {
            if (draw_next_ == 0 || static_cast<int32_t>(now_ms - draw_next_) >= 0) {
                draw_next_ = now_ms + 1000;
                if (draw_next_ == 0) draw_next_ = 1;
                const int32_t p = ctx_.pm.power_mw < 0 ? -ctx_.pm.power_mw : ctx_.pm.power_mw;
                draw_mw_ = ctx_.pm_online ? static_cast<uint32_t>((p + 50) / 100) * 100u : kNoDraw;
            }
            sig = mix_u32(sig, draw_mw_ + 7u);
        }
    }
    if (sig == 0) sig = 1;
    const Surface &s = ctx_.content;
    if (sig != det_sig_) {
        det_sig_ = sig;
        int16_t y = kit::kBodyY;
        gfx_fill(s, 0, kit::kBelowBar, s.w, static_cast<int16_t>(box_y_ - kit::kBelowBar), kBg);
        for (int i = first_; i < line_count(); i++) {
            const int16_t h = line_h(i);
            if (y + h > box_y_) break;
            paint_line(i, y);
            y = static_cast<int16_t>(y + h);
        }
        gfx_fill(s, 0, box_y_, s.w, static_cast<int16_t>(kFooterY - box_y_), kBg);
        cell_.init(s, kCellY, kCellH);
        vbus_shown_.clear();
        step_shown_.clear();
        if (arm_ == Arm::None && !run) {
            ctx_.rows.init(s, box_y_, &TwinPage::row_fn, this);
            ctx_.rows.restart();
            ctx_.rows.invalidate();
        }
    }
    if (arm_ != Arm::None) {
        const int32_t left = std::max<int32_t>(0, static_cast<int32_t>(armed_until_ - now_ms));
        StrBuf<24> sub;
        sub.put("closes in ").put_u32(static_cast<uint32_t>((left + 999) / 1000)).put_char('s');
        cell_.draw_armed("CONFIRM", sub.view(), std::clamp<int>(left * 1000 / static_cast<int32_t>(Ui::kConfirmWindowMs), 0, 1000));
    } else if (run) {
        if (vbus_shown_.view().empty() || static_cast<int32_t>(now_ms - vbus_next_) >= 0) {
            StrBuf<12> v;
            if (ctx_.pm_online && ctx_.vbus_fast_mv >= 0) v.put_fixed(ctx_.vbus_fast_mv, 2).put_char('V');
            else v.put("--");
            // "Step: waking the charger" until it lists, then "Asked: 11V3A"; the row repaints only when its text changes.
            const bool waking = twin_hw::live().waking();   // Rd is on, the charger has not listed yet: nothing is asked of it
            StrBuf<24> asked;
            asked.put_char(waking ? 'w' : 'a');
            if (waking) {
                asked.put(kFooters[kFootWaking]);
            } else if (e && e->have) {
                put_level(asked, e->out.mv, 'V');
                put_level(asked, e->out.ma, 'A');
            }
            if (asked.view() != step_shown_.view()) {
                if (!step_shown_.view().empty()) gfx_fill(s, 0, static_cast<int16_t>(kCellY + 4), s.w, kit::kInfoPitch, kBg);
                kit::draw_info_row(s, static_cast<int16_t>(kCellY + 4), waking ? "Step" : "Asked", asked.view().substr(1));
                step_shown_.clear();
                step_shown_.put(asked.view());
            }
            if (v.view() != vbus_shown_.view()) kit::draw_info_row(s, static_cast<int16_t>(kCellY + 24), "VBUS", v.view(), kVolt);
            vbus_shown_.clear();
            vbus_shown_.put(v.view());
            vbus_next_ = now_ms + 250;   // at most 4 updates a second
        }
    } else {
        ctx_.rows.set_count(kActions);
        ctx_.rows.select(act_);
        ctx_.rows.draw(now_ms);
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
        if (iq_wait_) {                               // stop the interrogation; its final reset still goes out, nothing is learned
            iq_wait_ = false;
            twin_hw::inline_cancel();
            say(kFooters[kFootIqStop], kDim);
            update(ctx_.now);
            return true;
        }
        if (running()) {
            const bool was_waking = twin_hw::live().waking();   // cancelled before the list came: the charger's Rd goes back at once
            ctx_.ui->cancel_twin();
            say(kFooters[was_waking ? kFootWakeStop : 1], kDim);
            update(ctx_.now);
            return true;
        }
        if (arm_ != Arm::None) {                      // BACK disarms
            arm_ = Arm::None;
            update(ctx_.now);
            return true;
        }
        if (detail_) {
            close_detail();
            return true;
        }
        return false;                                 // the list, idle: the Ui pops the page
    case Button::Plus:
    case Button::Minus:
        if (running() || learn_wait_ || iq_wait_) return true;
        arm_ = Arm::None;                             // a cursor move disarms
        if (detail_) act_ = std::clamp(act_ + (e.button == Button::Plus ? 1 : -1), 0, kActions - 1);
        else sel_ = std::clamp(sel_ + (e.button == Button::Plus ? 1 : -1), 0, list_last());
        update(ctx_.now);
        return true;
    case Button::Ok:
        if (running() || learn_wait_ || iq_wait_ || renego_wait_) return true;
        if (!detail_) {
            if (sel_ > n_) renegotiate();
            else if (sel_ == n_) learn_row(ctx_.now);
            else open_detail();
        } else if (arm_ == Arm::Delete) {
            if (static_cast<int32_t>(ctx_.now - armed_until_) < 0) {
                delete_twin();
                return true;
            }
            arm_ = Arm::None;
        } else if (act_ == 0) {
            if (const Entry *d = det_entry()) run_entry(*d);
        } else if (act_ == 1) {
            learn_row(ctx_.now);
        } else {
            arm_ = Arm::Delete;
            armed_until_ = ctx_.now + Ui::kConfirmWindowMs;
            note_.clear();
            note_until_ = 0;
        }
        update(ctx_.now);
        return true;
    }
    return false;
}

PanelLabels TwinPage::labels() const {
    PanelLabels l = list_labels(base_labels());
    const char *ok = "";
    if (running() || learn_wait_ || iq_wait_) {
        l.b[static_cast<int>(Button::Back)].shrt = "Stop";
    } else if (arm_ != Arm::None) {
        ok = "Confirm";
    } else if (!detail_) {
        ok = sel_ > n_ ? "Ask" : sel_ == n_ ? "Learn" : "Open";
    } else {
        ok = act_ == 0 ? "Run" : act_ == 1 ? "Learn" : "Delete";
    }
    l.b[static_cast<int>(Button::Ok)] = {ok, "", "", false};
    return l;
}

void TwinPage::update(uint32_t now_ms) {
    title_badge(false);
    if (note_until_ && static_cast<int32_t>(now_ms - note_until_) >= 0) {
        note_until_ = 0;
        note_.clear();
    }
    if (arm_ != Arm::None && static_cast<int32_t>(now_ms - armed_until_) >= 0) arm_ = Arm::None;   // the confirm window ran out
    if (started_ && !running()) {
        started_ = false;
        if (twin_hw::live().reason() == live::Reason::NoOffer) say(kFooters[kFootNoOffer], kCaution);   // the woken charger never listed
    }
    const bool wake_ok = twin_hw::can_wake(ctx_.ui->twin_ctx(now_ms));
    if (wake_ok != wake_ok_ && !running()) {   // (held while a proof runs: every re-attach flips it) a charger waits on the plug (or left): the notes and footers that say so repaint
        wake_ok_ = wake_ok;
        ctx_.rows.invalidate();
        det_sig_ = 0;
    }
    if (learn_wait_ && (!fusb302::identify_busy() || static_cast<int32_t>(now_ms - learn_wait_until_) >= 0)) learn_wait_end();
    if (iq_wait_ && !twin_hw::inline_running()) learn_inline_end();   // the sequencer bounds its own time (pd::kIqTotalMs)
    renego_poll(now_ms);
    if (!loaded_ || gen_ != storage::generation() || stamp_ != twin_hw::predict_stamp()) {
        const bool charger_changed = loaded_ && stamp_ != twin_hw::predict_stamp();
        reload();
        if (charger_changed && !running()) hide_proof_ = true;   // another list: the last run's verdicts no longer describe it
    }
    if (running()) hide_proof_ = false;
    if (detail_ && !det_entry()) {                   // the open twin left the library (the shell deleted it)
        close_detail();
        return;
    }
    if (detail_) {
        paint_detail(now_ms);
    } else {
        sel_ = std::clamp(sel_, 0, list_last());
        ctx_.rows.set_count(rows());
        ctx_.rows.select(sel_);
        ctx_.rows.draw(now_ms);
    }
    compose_footer();
    draw_footer_cached(ctx_.content, foot_tone_, foot_text_.view(), foot_);
}

}  // namespace k2
