// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#include "k2/ui/ui_cards.hpp"

#include <algorithm>
#include <cstring>

#include "k2/chargertest/card_tone.hpp"
#include "k2/chargertest/report_card_delta.hpp"
#include "k2/chargertest/report_card_hw.hpp"
#include "k2/chargertest/step_label.hpp"
#include "k2/ui/ui.hpp"
#include "k2/ui/ui_style.hpp"
#include "k2/ui/text.hpp"

namespace k2 {

namespace rc = report_card;
namespace tone = report_card::tone;

namespace {

// 100 mV units: "3.3V", "21V", "20.5V".
template <size_t N>
void put_volts(StrBuf<N> &s, unsigned dv) {
    s.put_u32(dv / 10);
    if (dv % 10) s.put_char('.').put_u32(dv % 10);
    s.put_char('V');
}

// Milliamps in 50 mA steps: "3A", "1.5A", "4.55A".
template <size_t N>
void put_amps(StrBuf<N> &s, unsigned ma) {
    s.put_u32(ma / 1000);
    const unsigned frac = (ma % 1000) / 10;
    if (frac) {
        s.put_char('.');
        if (frac % 10) s.put_char(char('0' + frac / 10)).put_char(char('0' + frac % 10));
        else s.put_char(char('0' + frac / 10));
    }
    s.put_char('A');
}

// Hex digits of the fingerprint, 8 wide.
template <size_t N>
void put_fp(StrBuf<N> &s, uint32_t fp) { s.put_hex(fp, 8); }

// Copies s (cut to fit) into dst[0..cap-1] and returns the view of the copy.
std::string_view stash(char *dst, std::size_t cap, std::string_view s) {
    const std::size_t n = std::min(s.size(), cap - 1);
    std::memcpy(dst, s.data(), n);
    dst[n] = 0;
    return {dst, n};
}

constexpr unsigned kHandshakeDv = 55;   // a group whose highest passing voltage is 5 V got a handshake, not a charge level

// The open card: a 36 px head, then rows of 14 px (S12 text, a hairline on the top line). Row 0 is PD fixed, then one row per
// range, a static Handover row, an optional QC4+/QC5 fact row, then the tested legacy groups in two half-width columns.
constexpr int16_t kDY = 60, kDP = 14;
constexpr int16_t kColW0 = 111, kColX1 = 112;   // the left column ends at the divider on x111
constexpr int kPdWhyPx = 136;                   // the PD fixed row's cause: x84 .. the right margin
constexpr int16_t kRightEdge = 214;            // value right edge of the full-width rows

struct Sel { bool range; unsigned idx; };       // range: index into Card::range, else the ct::Group number

// Rows of the QC4+/QC5 fact: 0 without a label; 1 when "Name (justification)" fits the 208 px of an S12 line; else 2, the name
// and under it the justification ("via PPS 3.3-21V 6.25A").
int qc_fact_lines(const rc::Card &c) {
    char t[48];
    if (!rc::qc_label_text(c, t, sizeof t)) return 0;
    return text_width(kLabelFont, t) <= 224 - 16 ? 1 : 2;
}

// Top of the first legacy row (its hairline).
int16_t legacy_y(const rc::Card &c) {
    return int16_t(kDY + kDP * (1 + c.n_ranges) + kDP + kDP * qc_fact_lines(c) +
                     (rc::oppo_note(c) ? kDP : 0) + (rc::scp_cap_ma(c) ? kDP : 0));
}
int legacy_cap(const rc::Card &c) {
    int per_col = 0;
    while (legacy_y(c) + kDP * per_col + kDP < kFooterY) per_col++;
    int n = 0;
    for (unsigned g = 1; g < rc::kGroups; g++) n += c.group[g].verdict != rc::GV::None;
    return std::min(n, 2 * per_col);
}
Sel sel_at(const rc::Card &c, int row) {
    if (row <= 0) return {false, 0};
    if (row <= int(c.n_ranges)) return {true, unsigned(row - 1)};
    int k = row - 1 - int(c.n_ranges);
    for (unsigned g = 1; g < rc::kGroups; g++) {
        if (c.group[g].verdict == rc::GV::None) continue;
        if (k-- == 0) return {false, g};
    }
    return {false, 0};
}

struct Slot { int16_t x, y, w; };
Slot cell_at(const rc::Card &c, int row) {
    if (row <= int(c.n_ranges)) return {0, int16_t(kDY + kDP * row), 224};
    const int k = row - 1 - int(c.n_ranges);
    return {int16_t((k & 1) ? kColX1 : 0), int16_t(legacy_y(c) + kDP * (k / 2)), int16_t((k & 1) ? 112 : kColW0)};
}

constexpr kit::KvLayout kHandover = {kDY, kDP, 8, &kLabelFont, &kLabelFont, 80, 24, Align::Left};

// Every verdict marker of a card is the Results grid's tone (report_card::tone, result_layout::roll_up): green all passed, amber
// some or capped, red none passed, grey nothing offered or nothing ran.
kit::Verdict to_kit(tone::Tone t) {
    switch (t) {
    case tone::Tone::Pass: return kit::Verdict::Pass;
    case tone::Tone::Partial: return kit::Verdict::Partial;
    case tone::Tone::Fail: return kit::Verdict::Fail;
    case tone::Tone::Skip: return kit::Verdict::Skip;
    default: return kit::Verdict::None;
    }
}
uint16_t tone_color(tone::Tone t) {
    switch (t) {
    case tone::Tone::Pass: return kOk;
    case tone::Tone::Partial: return kCaution;
    case tone::Tone::Fail: return kError;
    default: return kDim;
    }
}
// Every firmware string this screen draws goes through these two (the one place that calls gfx_text; tests/test_ui_no_truncation
// scans this file for any other). `max_px` is the box the text must fit: x to the next thing on the row. On the host a text that
// does not fit is a gate failure (K2_TRUNC_LOG); on the device fit_text is the last-resort cut. Names the user typed use
// fit_data / fit_text_mid in paint_head and paint_confirm only.
void put_line(const Surface &c, const Font &f, int16_t x, int16_t y, int max_px, std::string_view s, uint16_t fg, uint16_t bg K2_TRUNC_DECL) {
    char buf[kFitBuf];
    gfx_text(c, f, x, y, fit_text(f, s, max_px, buf K2_TRUNC_ARG), fg, bg);
}
// Text with check / cross marks (draw_marked_text): the width is the marked width, a longer text is reported and not drawn past the box.
void put_marked(const Surface &c, const Font &f, int16_t x, int16_t y, int max_px, std::string_view s, uint16_t fg, uint16_t bg K2_TRUNC_DECL) {
    if (marked_width(f, s) > max_px) K2_TRUNC_NOTE(trunc::Kind::Cut, s, max_px);
    draw_marked_text(c, f, x, y, s, fg, bg);
}
// Same, right-aligned so that it ends at `right`; `left` is the end of whatever is to its left (the box is right - left).
void put_marked_right(const Surface &c, const Font &f, int16_t right, int16_t left, int16_t y, std::string_view s, uint16_t fg, uint16_t bg K2_TRUNC_DECL) {
    put_marked(c, f, int16_t(right - marked_width(f, s)), y, right - left, s, fg, bg K2_TRUNC_ARG);
}
// The name of a card row. Struck through (the Results grid's skip look, dim ink) only for a unit that was really skipped; a unit
// that never answered (a protocol the charger does not carry) is plain dim, like its dash glyph. A tested row is plain text.
void card_name(const Surface &c, int16_t x, int16_t y, int max_px, std::string_view name, tone::Tone t, bool struck, uint16_t bg) {
    if (struck) {
        if (marked_width(kLabelFont, name) > max_px) K2_TRUNC_NOTE_HERE(trunc::Kind::Cut, name, max_px);
        kit::draw_struck_text(c, kLabelFont, x, y, name, kDim, bg);
    } else {
        put_line(c, kLabelFont, x, y, max_px, name, t == tone::Tone::Skip || t == tone::Tone::Pending ? kDim : kText, bg);
    }
}
// The Verdict word of a sheet: the tone of the card's rows. Grey is "Skipped" for a unit that was skipped, "No reply" for a
// protocol that never answered, "Not run" for a range with no step measured.
const char *verdict_word(tone::Tone t, bool skipped, bool range) {
    switch (t) {
    case tone::Tone::Pass: return "Pass";
    case tone::Tone::Partial: return "Partial";
    case tone::Tone::Fail: return "Fail";
    default: return skipped ? "Skipped" : range ? "Not run" : "No reply";
    }
}

}  // namespace

// ---- text ---------------------------------------------------------------------------------------

void cards_title_text(const rc::Card &c, StrBuf<24> &out) {
    if (c.name[0]) { out.put(c.name); return; }
    if (c.id.vid || c.id.pid) { out.put_hex(c.id.vid, 4).put_char(':').put_hex(c.id.pid, 4); return; }
    out.put_char('#');
    put_fp(out, c.id.fp);
}

namespace {

// A card the charger was tested on again with the same result (rc::same_results) says how often and when it last was: the
// fullest of "x3 last boot 9", "x3 b9", "x3" that fits `room` cells. Nothing for a card seen once; nothing is ever cut.
void put_repeat(StrBuf<32> &out, const rc::Card &c, size_t room) {
    if (c.times <= 1) return;
    for (int form = 0; form < 3; ++form) {
        StrBuf<32> t;
        t.put(" x").put_u32(c.times);
        if (form == 0) t.put(" last boot ").put_u32(c.last_boots);
        else if (form == 1) t.put(" b").put_u32(c.last_boots);
        if (t.view().size() <= room) { out.put(t.view()); return; }
    }
}

}  // namespace

// The open list row's note: the name and the boot counter, 30 cells at most. A repeated card shows the boot of its latest repeat
// instead ("x3 boot 9"): that is when it was last seen, and the first boot is in the card's detail footer.
void cards_note_text(const rc::Card &c, StrBuf<32> &out) {
    constexpr size_t kNoteCells = 30;
    StrBuf<24> t;
    cards_title_text(c, t);
    out.put(t.view());
    if (c.times <= 1) { out.put("  boot ").put_u32(std::min<uint32_t>(c.boots, 999)); return; }
    const size_t room = kNoteCells - t.view().size();
    StrBuf<32> tail;
    for (int form = 0; form < 3; ++form) {   // "  x3 boot 9", "  x3 b9", "  x3"
        tail.clear();
        tail.put("  x").put_u32(c.times);
        if (form == 0) tail.put(" boot ").put_u32(std::min<uint32_t>(c.last_boots, 999));
        else if (form == 1) tail.put(" b").put_u32(std::min<uint32_t>(c.last_boots, 999));
        if (tail.view().size() <= room) break;
    }
    out.put(tail.view());
}

void cards_range_text(const rc::Range &r, StrBuf<32> &out) {
    out.put_u32(r.lo_dv / 10);
    if (r.lo_dv % 10) out.put_char('.').put_u32(r.lo_dv % 10);
    out.put_char('-');
    put_volts(out, r.hi_dv);
}

void cards_amps_text(unsigned ma, StrBuf<8> &out) {
    if (ma) put_amps(out, ma);
}

void cards_range_status_text(const rc::Card &c, const rc::Range &r, StrBuf<16> &out) {
    if (r.fail_dv) {
        if (r.max_dv && tone::capped(c)) {
            out.put("caps ");
            put_volts(out, r.max_dv);
        } else {
            out.put("at ");
            put_volts(out, r.fail_dv);
        }
    } else if (tone::range(c, r) == tone::Tone::Partial) {
        out.put("reached ");
        put_volts(out, r.max_dv);
    }
}

void cards_pd_why_text(const rc::GroupResult &r, StrBuf<24> &out) {
    const char *why = rc::group_why(0, r);
    if (fits_px(kLabelFont.adv, why, kPdWhyPx)) out.put(why);
    else out.put("see detail");   // "source shut off (needs replug)" is 30 cells: the detail screen spells it out, nothing is cut
}

void cards_summary_text(const rc::Card &c, StrBuf<12> &out) {
    const tone::Counts n = tone::counts(c);
    if (!n.pass && !n.partial && !n.fail) return;
    out.put_u32(std::min(n.pass, 99u)).put_char('P');
    if (n.partial) out.put_char(' ').put_u32(std::min(n.partial, 99u)).put_char('~');
    if (n.fail) out.put_char(' ').put_u32(std::min(n.fail, 99u)).put_char('F');
}

void cards_group_max_text(const rc::GroupResult &r, StrBuf<16> &out) {
    if (rc::group_handshake_ok(r) || rc::group_detected(r)) return;   // the wording belongs to the group detail screen
    if (!r.max_mv) return;
    out.put("max ");
    put_volts(out, rc::detail::dv_round(r.max_mv));
}

void cards_group_fail_text(const rc::GroupResult &r, StrBuf<16> &out) {
    if (!r.fail_mv) return;
    out.put_char(kMarkCross);
    put_volts(out, rc::detail::dv_round(r.fail_mv));
}

void cards_group_cell_text(const rc::GroupResult &r, StrBuf<16> &out) {
    const unsigned mx = rc::detail::dv_round(r.max_mv), fl = rc::detail::dv_round(r.fail_mv);
    if (rc::group_handshake_ok(r) || rc::group_detected(r)) {
        out.put_char(kMarkCheck);   // both count as a pass; "handshake ok" / "detected" is spelled out in the group detail
    } else if (r.verdict == rc::GV::Pass) {
        if (!mx) out.put_char(kMarkCheck);
        else if (mx <= kHandshakeDv) out.put_char(kMarkCheck).put_u32(mx / 10).put("V only");
        else out.put_char(kMarkCheck), put_volts(out, mx);
    } else if (r.verdict == rc::GV::Fail) {
        if (mx) {
            put_volts(out, mx);
            if (fl) { out.put_char(kMarkCross); put_volts(out, fl); } else out.put_char(kMarkCross);
        } else {
            out.put_char(kMarkCross);
            if (fl) put_volts(out, fl);
        }
    } else {
        out.put("skip");
    }
}

void cards_handover_text(const rc::Card &c, StrBuf<32> &out) {
    using HO = charger_test::HandoverOutcome;
    switch (c.handover) {
    case HO::NotTried: out.put("not tried"); return;
    case HO::FellBack: out.put("fell back"); break;
    case HO::StayedPd: out.put("stayed in PD"); break;
    case HO::Fault:    out.put("fault"); break;
    case HO::Reboot:   out.put("rebooted"); break;
    case HO::InContract: out.put("in PD contract"); break;
    }
    if (c.handover_ds) {
        out.put_char(' ').put_u32(c.handover_ds / 10).put_char('.').put_u32(c.handover_ds % 10).put_char('s');
    }
}

void cards_stats_text(const rc::Card &c, StrBuf<32> &out) {
    out.put("run ").put_u32(c.runs).put(" boot ").put_u32(std::min<uint32_t>(c.boots, 99999));
    // A repeated card ("x3 last boot 9") comes first: it is the newer fact, and it takes the room before the optional tail.
    put_repeat(out, c, static_cast<size_t>(kFooterCells) - out.view().size());
    // One optional tail, the fullest form that still fits the 27-cell footer: a run that went through power losses says so (in
    // place of the firmware), and a run the charger itself powered says so too; the words shorten before anything is cut.
    const bool self = (c.cable.flags & rc::kCableSelfPowered) != 0;
    const size_t room = static_cast<size_t>(kFooterCells) - out.view().size();
    StrBuf<32> tail;
    for (int form = 0; form < 3; ++form) {   // 0 full words, 1 "self-pwr r2", 2 "self r16"
        tail.clear();
        if (self) tail.put(form == 0 ? " self-powered" : form == 1 ? " self-pwr" : " self");
        if (c.resumed) tail.put(!self && form == 0 ? " resumed " : " r").put_u32(c.resumed);
        else if (c.cable.flags & rc::kCableReattach) tail.put(form == 0 ? " re-attach" : form == 1 ? " reatt" : " re");   // the planned re-attach, not a resume
        else if (!self && c.fw && form == 0) tail.put(" fw ").put_hex(c.fw, 8);
        if (tail.view().size() <= room) { out.put(tail.view()); break; }
    }
}

void cards_id_text(const rc::Card &c, StrBuf<32> &out) {
    if (c.id.vid || c.id.pid) out.put_hex(c.id.vid, 4).put_char(':').put_hex(c.id.pid, 4).put("  ");
    if (c.id.pdp_w) out.put_u32(c.id.pdp_w).put("W  ");   // rated power from the source probe
    out.put("fp ");
    put_fp(out, c.id.fp);
}

// ---- the detail sheet ---------------------------------------------------------------------------

unsigned cards_wrap(std::string_view text, unsigned cells, std::string_view *lines, unsigned max_lines) {
    unsigned n = 0;
    for (;;) {
        while (!text.empty() && text.front() == ' ') text.remove_prefix(1);
        if (text.empty()) return n;
        if (n == max_lines) return 0;
        std::size_t len = text.size();
        if (len > cells) {
            const std::size_t sp = text.rfind(' ', cells);   // a space at index <= cells ends a line of at most `cells`
            if (sp == std::string_view::npos || sp == 0) return 0;
            len = sp;
        }
        std::string_view line = text.substr(0, len);
        while (!line.empty() && line.back() == ' ') line.remove_suffix(1);
        lines[n++] = line;
        text.remove_prefix(len);
    }
}

namespace {

// Adds a row (undone by --n when the sheet would outgrow its screen: rows are added most important first, so the last ones go).
SheetRow *sheet_add(CardSheet &s, const char *key, uint16_t color, std::string_view v) {
    if (s.n >= unsigned(kSheetRows)) return nullptr;
    SheetRow &r = s.row[s.n++];
    r.key = key;
    r.verdict = false;
    r.glyph = kit::Verdict::None;
    r.color = color;
    if (v.size() <= std::size_t(kSheetBox)) {
        r.below = false;
        r.line[0] = v;
        r.lines = 1;
    } else {
        r.below = true;
        r.lines = uint8_t(cards_wrap(v, kSheetWrap, r.line, kSheetLines));
    }
    if (r.lines && s.height() > kFooterY - kDY) { --s.n; return nullptr; }
    return &r;
}
// A number the row owns (r.num): the view of a temporary would dangle.
SheetRow *sheet_add_num(CardSheet &s, const char *key, uint16_t color, std::string_view v) {
    if (s.n >= unsigned(kSheetRows)) return nullptr;
    char *dst = s.row[s.n].num;
    return sheet_add(s, key, color, stash(dst, sizeof s.row[s.n].num, v));
}
void sheet_verdict(CardSheet &s, tone::Tone t, const char *word) {
    SheetRow &r = s.row[s.n++];
    r.key = "Verdict";
    r.verdict = true;
    r.glyph = to_kit(t);
    r.color = tone_color(t);
    r.below = false;
    r.line[0] = word;
    r.lines = 1;
}

}  // namespace

void cards_sheet(const rc::Card &c, bool range, unsigned idx, CardSheet &out) {
    out.n = 0;
    if (range) {
        const rc::Range &r = c.range[idx];
        const tone::Tone rt = tone::range(c, r);
        sheet_verdict(out, rt, verdict_word(rt, false, true));
        StrBuf<32> rg;
        cards_range_text(r, rg);
        sheet_add_num(out, "Offered", kVolt, rg.view());
        StrBuf<16> v;
        if (r.max_dv) { put_volts(v, r.max_dv); sheet_add_num(out, "Max reached", kVolt, v.view()); }
        v.clear();
        if (r.fail_dv) { put_volts(v, r.fail_dv); sheet_add_num(out, "Failed at", rt == tone::Tone::Fail ? kError : kCaution, v.view()); }
        StrBuf<8> a;
        cards_amps_text(r.ma, a);
        if (a.view().size()) sheet_add_num(out, "Max current", kAmp, a.view());
        if (r.pos) { StrBuf<8> p; p.put_char('#').put_u32(r.pos); sheet_add_num(out, "PDO", kText, p.view()); }
        // A failing range of a card whose PD group failed only through a range: the group's cause is this range's cause too.
        if (r.fail_dv && tone::fixed_ok_despite_group(c)) sheet_add(out, "Reason", kCaution, rc::group_why(0, c.group[0]));
        return;
    }
    const rc::GroupResult &r = c.group[idx];
    const tone::Tone gt = tone::group(c, idx);
    sheet_verdict(out, gt, verdict_word(gt, r.verdict == rc::GV::Skip, false));
    StrBuf<16> v;
    if (rc::group_handshake_ok(r)) sheet_add(out, "Max reached", kVolt, "handshake ok");
    else if (rc::group_detected(r)) sheet_add(out, "Max reached", kVolt, "detected");
    else if (r.max_mv) { put_volts(v, rc::detail::dv_round(r.max_mv)); sheet_add_num(out, "Max reached", kVolt, v.view()); }
    v.clear();
    if (r.fail_mv) { put_volts(v, rc::detail::dv_round(r.fail_mv)); sheet_add_num(out, "Failed at", gt == tone::Tone::Fail ? kError : kCaution, v.view()); }
    if (idx == 0) {
        StrBuf<8> a;
        cards_amps_text(c.pd_ma, a);
        if (a.view().size()) sheet_add_num(out, "Max current", kAmp, a.view());
    }
    // The cause, only for a fail or skip of the group's own levels: a pass has nothing to explain, and a PD group that failed
    // only through a range (the range's sheet says so) reads as passed. One row when the engine's detail and the reason repeat
    // each other, else both: the detail (what the engine saw) and the coarse reason.
    const bool own = (r.verdict == rc::GV::Fail || r.verdict == rc::GV::Skip) && !(idx == 0 && tone::fixed_ok_despite_group(c));
    if (own) {
        const char *why = rc::group_why(idx, r);
        const char *reason = r.reason && r.reason != uint8_t(charger_test::Reason::NoQc3Plus) ? charger_test::name(charger_test::Reason(r.reason)) : nullptr;
        const uint16_t wc = gt == tone::Tone::Skip ? kDim : kCaution;
        if (!reason || std::strcmp(reason, why) == 0) {
            sheet_add(out, "Reason", wc, why);
        } else {
            sheet_add(out, "Why", wc, why);
            sheet_add(out, "Reason", kDim, reason);
        }
    }
    // The UFCS modes of a v6 card, one row each ("5-20V 3A"): what the charger advertises, shown for a pass as well. Older cards
    // hold none and show the volts alone. Rows past the screen's height drop, the last mode first.
    if (idx == unsigned(charger_test::Group::Ufcs)) {
        static constexpr const char *kModeKey[rc::kMaxUfcs] = {"Mode 1", "Mode 2", "Mode 3", "Mode 4"};
        for (unsigned i = 0, n = rc::ufcs_modes(c); i < n; ++i) {
            const rc::UfcsRange &m = c.ufcs[i];
            if (!sheet_add_num(out, kModeKey[i], kAmp, step_label::range(m.lo_dv * 100u, m.hi_dv * 100u, m.ma).view())) break;
        }
    }
    if (own && r.detail) { StrBuf<8> d; d.put_u32(r.detail); sheet_add_num(out, "Detail code", kDim, d.view()); }
}

// ---- page ---------------------------------------------------------------------------------------

void CardsPage::refresh() {
    n_ = int(rc::hw::numbers(nums_));
    if (idx_ >= n_) idx_ = n_ > 0 ? n_ - 1 : 0;
}

int CardsPage::rows() const { return 1 + int(cur_.n_ranges) + legacy_cap(cur_); }

void CardsPage::load_cur() {
    cur_ = rc::Card{};
    if (n_ > 0) rc::hw::get(nums_[idx_], cur_);
}

// The shown card as a share record, then the share view.
void CardsPage::share_cur() {
    load_cur();
    StrBuf<24> title;
    cards_title_text(cur_, title);
    const std::size_t len = qrshare::records::card(cur_, ctx_.ui->share_buffer());
    ctx_.ui->open_share(qrshare::Type::Card, qrshare::kSchemaCard, title.view(), len, ctx_.now);
}

void CardsPage::enter() {
    idx_ = 0;
    row_ = 0;
    mode_ = Mode::List;
    note_.clear();
    note_until_ = 0;
    refresh();
    painted_ = false;
    content_sig_ = 0;
    prev_row_ = -1;
    foot_.invalidate();
}

void CardsPage::draw_static() {
    title_bar();
    painted_ = false;
    content_sig_ = 0;
}

void CardsPage::invalidate() {
    painted_ = false;
    content_sig_ = 0;
    foot_.invalidate();
}

bool CardsPage::on_event(Event e) {
    if (e.press == Press::Long && e.button == Button::Back) {   // long BACK: delete, after a confirm
        if (mode_ == Mode::Confirm || mode_ == Mode::Group || n_ == 0) return true;
        mode_ = Mode::Confirm;
        painted_ = false;
        return true;
    }
    if (e.press == Press::Long && e.button == Button::Ok && mode_ == Mode::Detail) {   // Share: the card as QR
        share_cur();
        return true;
    }
    if (e.press != Press::Short) return false;
    switch (mode_) {
    case Mode::List:
        if (e.button == Button::Ok) {
            if (n_ == 0) return true;
            mode_ = Mode::Detail;
            row_ = 0;
            painted_ = false;
            return true;
        }
        if (e.button == Button::Plus || e.button == Button::Minus) {
            if (n_ > 0) idx_ = std::clamp(idx_ + (e.button == Button::Plus ? 1 : -1), 0, n_ - 1);
            painted_ = false;
            return true;
        }
        return false;   // BACK pops the menu
    case Mode::Detail:
    case Mode::Group:
        if (e.button == Button::Plus || e.button == Button::Minus) {   // move the focus over the rows
            load_cur();
            row_ = std::clamp(row_ + (e.button == Button::Plus ? 1 : -1), 0, rows() - 1);
            painted_ = false;
            return true;
        }
        if (e.button == Button::Ok) {
            if (mode_ == Mode::Detail) { mode_ = Mode::Group; painted_ = false; }
            return true;
        }
        if (e.button == Button::Back) { mode_ = mode_ == Mode::Group ? Mode::Detail : Mode::List; painted_ = false; return true; }
        return false;
    case Mode::Confirm:
        if (e.button == Button::Ok) {
            const bool ok = n_ > 0 && rc::hw::remove(nums_[idx_]) == rc::Err::Ok;
            refresh();
            mode_ = Mode::List;
            note_.clear();
            note_.put(ok ? "Card deleted" : "Delete failed");
            note_color_ = ok ? kOk : kError;
            note_until_ = ctx_.now + 2500;
            painted_ = false;
            return true;
        }
        if (e.button == Button::Back) { mode_ = n_ > 0 ? Mode::Detail : Mode::List; painted_ = false; return true; }
        return true;   // +/- do nothing while asking
    }
    return false;
}

PanelLabels CardsPage::labels() const {
    PanelLabels l = base_labels();
    switch (mode_) {
    case Mode::List:
        l.b[int(Button::Ok)] = {n_ > 0 ? "Open" : "", "", "", false};
        l.b[int(Button::Back)] = {"Back", n_ > 0 ? "Delete" : "", n_ > 0 ? "Del" : "", false};
        break;
    case Mode::Detail:
        l.b[int(Button::Ok)] = {"Open", "Share", "", false};
        l.b[int(Button::Back)] = {"Back", "Delete", "Del", false};
        l.b[int(Button::Minus)] = {kUp, "", "", false};
        l.b[int(Button::Plus)] = {kDn, "", "", false};
        break;
    case Mode::Group:
        l.b[int(Button::Ok)] = {"", "", "", true};
        l.b[int(Button::Back)] = {"Back", "", "", false};
        l.b[int(Button::Minus)] = {kUp, "", "", false};
        l.b[int(Button::Plus)] = {kDn, "", "", false};
        break;
    case Mode::Confirm:
        l.b[int(Button::Ok)] = {"Delete", "", "", false};
        l.b[int(Button::Back)] = {"Back", "", "", false};
        l.b[int(Button::Minus)] = {"", "", "", true};
        l.b[int(Button::Plus)] = {"", "", "", true};
        break;
    }
    return l;
}

void CardsPage::clear_body() {
    const Surface &c = ctx_.content;
    gfx_fill(c, 0, kBarH, c.w, int16_t(kFooterY - kBarH), kBg);
}

void CardsPage::update(uint32_t now_ms) {
    title_badge(false);
    if (note_until_ && int32_t(now_ms - note_until_) >= 0) { note_until_ = 0; note_.clear(); }
    // Content signature: mode, card list, store generation, note -- any change rebuilds the body.
    uint32_t csig = uint32_t(mode_) * 7u + uint32_t(n_) * 1031u + 1;
    csig = (csig ^ rc::hw::generation()) * 16777619u;   // every save, rename and delete bumps it
    for (int i = 0; i < n_; i++) csig = (csig ^ nums_[i]) * 16777619u;
    for (char ch : note_.view()) csig = (csig ^ uint8_t(ch)) * 16777619u;
    if (mode_ == Mode::Group) csig = (csig ^ uint32_t(row_ + 1)) * 16777619u;   // the sub-screen is rebuilt for every row
    // Move signature: +/- navigation only.
    const uint32_t msig = uint32_t(mode_) * 7u + uint32_t(row_) * 211u + uint32_t(idx_) * 31u + 1;
    if (csig != content_sig_) {
        content_sig_ = csig;
        sig_ = msig;
        if (!ctx_.blank && !(mode_ == Mode::List && n_ == 0)) clear_body();   // a blank screen is kBg; the empty list paints its own ground
        switch (mode_) {
        case Mode::List:    n_ ? paint_list(true) : paint_empty(); break;
        case Mode::Detail:  paint_detail(); break;
        case Mode::Group:   paint_group(); break;
        case Mode::Confirm: paint_confirm(); break;
        }
        painted_ = !(ctx_.budget && ctx_.budget->more);
        return;
    }
    if (painted_ && msig == sig_) return;
    sig_ = msig;
    // Navigation-only change (or continuing a sliced list draw): no body wipe; the list writes only the rows that changed,
    // the card moves its focus bar.
    if (mode_ == Mode::List) { if (n_ > 0) paint_list(false); }
    else if (mode_ == Mode::Detail) move_focus();
    painted_ = !(ctx_.budget && ctx_.budget->more);
}

void CardsPage::paint_empty() {
    const Surface &c = ctx_.content;
    // The body ground and the four constant lines in one pass; the table is width-checked at compile time.
    static constexpr const char *kEmptyTitle = "No report cards yet";
    static constexpr const char *kEmptyHint[3] = {"Finish a Charger test and the", "K2 keeps one card per charger",
                                                   "that offers PD source caps."};
    static_assert(fits_px(8, kEmptyTitle, 224 - 16) && all_fit(kEmptyHint, 6, 224 - 16), "empty-state copy fits (16 px font: 8 px/cell, 12 px font: 6)");
    const kit::TextRun runs[4] = {{8, 30, &kSmall, kEmptyTitle, kText},      {8, 54, &kLabelFont, kEmptyHint[0], kDim},
                                  {8, 68, &kLabelFont, kEmptyHint[1], kDim}, {8, 82, &kLabelFont, kEmptyHint[2], kDim}};
    kit::paint_texts(c, {0, kBarH, c.w, static_cast<int16_t>(kFooterY - kBarH)}, runs, 4, kBg);
    draw_footer_cached(c, FooterTone::Info, note_.view().size() ? note_.view() : "0 cards saved", foot_);
}

// One saved charger: the verdict cell, the name, "5P 2F" (checks passed, failed); the open row adds the full name and boots.
void CardsPage::row_fn(void *self, int index, bool focused, kit::RowSpec &out) {
    CardsPage &p = *static_cast<CardsPage *>(self);
    out = kit::RowSpec{};
    if (index < 0 || index >= p.n_ || !rc::hw::get(p.nums_[index], p.cur_)) return;
    const rc::Card &cd = p.cur_;
    StrBuf<12> t;
    cards_summary_text(cd, t);
    out.marker = kit::Marker::Verdict;
    out.state = uint8_t(to_kit(tone::card(cd)));
    out.tail = stash(p.line_.tail, sizeof p.line_.tail, t.view());
    out.tail_color = kDim;
    StrBuf<24> title;
    cards_title_text(cd, title);
    const int avail = 178 - int(out.tail.size()) * int(kSmall.adv) + (out.tail.empty() ? 8 : 0);   // x28 .. the tail's left gap
    char fitted[kFitBuf];
    out.text = stash(p.line_.text, sizeof p.line_.text, fit_text_mid(kSmall, title.view(), avail, fitted));
    if (focused) {
        StrBuf<32> note;
        cards_note_text(cd, note);
        out.note = stash(p.line_.note, sizeof p.line_.note, note.view());
    }
}

void CardsPage::paint_list(bool rebuild) {
    const Surface &c = ctx_.content;
    if (rebuild) {
        ctx_.rows.init(c, kit::kBodyY, &CardsPage::row_fn, this);
        ctx_.rows.invalidate();
    }
    ctx_.rows.set_count(n_);
    ctx_.rows.select(idx_);
    ctx_.rows.draw(ctx_.now, ctx_.budget);
    StrBuf<24> st;
    if (note_.view().size()) st.put(note_.view());
    else st.put_u32(uint32_t(idx_ + 1)).put_char('/').put_u32(uint32_t(n_)).put(n_ == 1 ? " charger" : " chargers");
    draw_footer_cached(c, !note_.view().size() ? FooterTone::Info : note_color_ == kError ? FooterTone::Error : FooterTone::Ok,
                       st.view(), foot_);
}

// The head of the open card and of its sub-screen: a verdict cell and the title (S16), a dim line under them.
// A line that is a name the user typed (title_data / sub_data) is cut by design (fit_text_mid / fit_data: counted by the gate, not
// failed); every other line is ours and put_line fails the host gate when it does not fit.
void CardsPage::paint_head(kit::Verdict v, std::string_view title, std::string_view sub, bool title_data, bool sub_data) {
    const Surface &c = ctx_.content;
    char fitted[kFitBuf];
    kit::draw_verdict(c, 8, 28, 12, 12, v, "", kBg);
    const int tbox = c.w - kit::kTextX - 8;
    if (title_data) gfx_text(c, kSmall, kit::kTextX, 26, fit_text_mid(kSmall, title, tbox, fitted), kText, kBg);
    else put_line(c, kSmall, kit::kTextX, 26, tbox, title, kText, kBg);
    if (sub_data) gfx_text(c, kLabelFont, 8, 44, fit_data(kLabelFont, sub, c.w - 16, fitted), kDim, kBg);
    else put_line(c, kLabelFont, 8, 44, c.w - 16, sub, kDim, kBg);
}

// One row of the open card, repainted in place: the interior of its 14 px, the focus fill and bar, the verdict cell, the text.
void CardsPage::paint_cell(int row, bool focused) {
    const Surface &c = ctx_.content;
    const rc::Card &cd = cur_;
    const Slot g = cell_at(cd, row);
    const uint16_t bg = focused ? kFocusT : kBg;
    const int16_t ty = int16_t(g.y + kit::row_text_y(kDP, kLabelFont.height));
    gfx_fill(c, g.x, int16_t(g.y + 1), g.w, int16_t(kDP - 1), bg);
    if (focused) gfx_fill(c, g.x, int16_t(g.y + 1), 3, int16_t(kDP - 1), kFocus);
    if (row == 0) {   // PD fixed
        const rc::GroupResult &r = cd.group[0];
        const tone::Tone pt = tone::group(cd, 0);
        kit::draw_verdict(c, 8, int16_t(g.y + 1), 12, 12, to_kit(pt), "", bg);
        card_name(c, kit::kTextX, ty, 84 - 4 - kit::kTextX, "PD fixed", pt, r.verdict == rc::GV::Skip, bg);
        StrBuf<16> mx, fl;
        cards_group_max_text(r, mx);
        cards_group_fail_text(r, fl);
        put_marked_right(c, kLabelFont, 150, 84, ty, mx.view(), kVolt, bg);
        if (fl.view().size()) {
            put_marked_right(c, kLabelFont, kRightEdge, 150, ty, fl.view(), pt == tone::Tone::Fail ? kError : kCaution, bg);
        } else if (pt != tone::Tone::Pass && !mx.view().size() && (r.verdict == rc::GV::Fail || r.verdict == rc::GV::Skip)) {
            // Nothing passed and no target is stored (a skip, an old card): name the cause. With a maximum there is no room for
            // the words ("capped below target" is 19 cells; the box after "max 20V" holds 10), and the group's detail screen
            // carries them. A group that failed only through a range reads as its fixed levels (green): the range row says why.
            StrBuf<24> why;
            cards_pd_why_text(r, why);
            put_line(c, kLabelFont, 84, ty, kPdWhyPx, why.view(), tone_color(pt), bg);
        }
    } else if (row <= int(cd.n_ranges)) {   // a PPS / AVS range
        const rc::Range &r = cd.range[row - 1];
        const tone::Tone rt = tone::range(cd, r);
        constexpr int16_t kRangeX = 58;
        kit::draw_verdict(c, 8, int16_t(g.y + 1), 12, 12, to_kit(rt), "", bg);
        card_name(c, kit::kTextX, ty, kRangeX - 4 - kit::kTextX, rc::detail::alg_label(r.alg), rt, rt == tone::Tone::Skip, bg);
        StrBuf<32> rg;
        cards_range_text(r, rg);
        put_line(c, kLabelFont, kRangeX, ty, kRightEdge - kRangeX, rg.view(), kVolt, bg);
        StrBuf<8> am;
        cards_amps_text(r.ma, am);
        int16_t end = int16_t(kRangeX + int(rg.view().size()) * kLabelFont.adv);   // the end of the left items; the status goes after it
        if (am.view().size()) {
            const int16_t ax = int16_t(end + kLabelFont.adv);
            put_line(c, kLabelFont, ax, ty, kRightEdge - ax, am.view(), kAmp, bg);
            end = int16_t(ax + int(am.view().size()) * kLabelFont.adv);
        }
        StrBuf<16> st;
        cards_range_status_text(cd, r, st);
        put_marked_right(c, kLabelFont, kRightEdge, int16_t(end + kLabelFont.adv), ty, st.view(), tone_color(rt), bg);
    } else {   // a legacy group, half width: verdict cell, name, the value right-aligned to the cell edge
        const unsigned gi = sel_at(cd, row).idx;
        const rc::GroupResult &r = cd.group[gi];
        const tone::Tone gt = tone::group(cd, gi);
        kit::draw_verdict(c, int16_t(g.x + 4), int16_t(g.y + 1), 12, 12, to_kit(gt), "", bg);
        StrBuf<16> v;
        cards_group_cell_text(r, v);
        // The verdict cell already says pass or fail: drop the leading mark and a lone trailing cross.
        std::string_view s = v.view();
        if (!s.empty() && (s.front() == kMarkCheck || s.front() == kMarkCross)) s.remove_prefix(1);
        if (!s.empty() && s.back() == kMarkCross) s.remove_suffix(1);
        // Amber when part of the group passed: the handshake works, a higher level does not. A protocol that never answered is
        // grey and says so ("no reply", the Results grid's wording), never a red cross with a target it never reached.
        if (gt == tone::Tone::Skip && r.verdict == rc::GV::Fail) s = "no reply";
        const uint16_t vc = gt == tone::Tone::Pass ? kVolt : gt == tone::Tone::Partial ? kCaution : kDim;
        // The name's box ends 2 px before the value starts; the value's right edge is 2 px inside the cell.
        const int16_t nx = int16_t(g.x + 17), vr = int16_t(g.x + g.w - 2);
        const int vw = marked_width(kLabelFont, s);
        const std::string_view name = charger_test::name(static_cast<charger_test::Group>(gi));
        card_name(c, nx, ty, vr - vw - 2 - nx, name, gt, r.verdict == rc::GV::Skip, bg);
        put_marked_right(c, kLabelFont, vr, int16_t(nx + int(name.size()) * kLabelFont.adv), ty, s, vc, bg);
    }
}

// Detail +/-: the old and the new focused row repaint, nothing else.
void CardsPage::move_focus() {
    if (prev_row_ == row_) return;
    if (prev_row_ >= 0) paint_cell(prev_row_, false);
    paint_cell(row_, true);
    prev_row_ = row_;
}

void CardsPage::paint_detail() {
    const Surface &c = ctx_.content;
    load_cur();
    const rc::Card &cd = cur_;
    row_ = std::clamp(row_, 0, rows() - 1);
    StrBuf<24> t;
    cards_title_text(cd, t);
    StrBuf<32> id;
    cards_id_text(cd, id);
    paint_head(to_kit(tone::card(cd)), t.view(), id.view(), true, false);

    // PD fixed, then the PPS / AVS ranges.
    for (int r = 0; r <= int(cd.n_ranges); r++) {
        gfx_fill(c, 0, cell_at(cd, r).y, c.w, 1, kRule);
        paint_cell(r, r == row_);
    }
    // Handover: a fact row.
    const int16_t y_h = int16_t(kDY + kDP * (1 + cd.n_ranges));
    StrBuf<32> ho;
    cards_handover_text(cd, ho);
    const bool tried = cd.handover != charger_test::HandoverOutcome::NotTried;
    const bool ho_bad = cd.handover == charger_test::HandoverOutcome::Fault || cd.handover == charger_test::HandoverOutcome::Reboot;
    kit::kv_row(c, kHandover, y_h, "Handover", ho.view(), ho_bad ? kError : tried ? kText : kFaint, true);
    // A QC4+/QC5 source gets one fact line ahead of the groups ("QC5 (Qualcomm VID, PPS 3.3-21V)").
    int16_t ly = int16_t(y_h + kDP);
    {
        char label[48];
        auto fact = [&](const char *text, uint16_t ink) {
            gfx_fill(c, 0, ly, c.w, 1, kRule);
            put_line(c, kLabelFont, 8, int16_t(ly + kit::row_text_y(kDP, kLabelFont.height)), c.w - 16, text, ink, kBg);
            ly = int16_t(ly + kDP);
        };
        if (qc_fact_lines(cd) == 1) {
            rc::qc_label_text(cd, label, sizeof label);
            fact(label, kText);
        } else if (qc_fact_lines(cd) == 2) {   // too long for one line (a 6.25 A non-Moto 3.3-21 V PPS): the name, then why
            fact(rc::qc_label_name(rc::qc_label(cd)), kText);
            rc::qc_label_justification(cd, label, sizeof label);
            fact(label, kText);
        }
        if (const char *note = rc::oppo_note(cd)) fact(note, kFaint);   // passive: the source listed OPPO's SVID, nothing was sent
        char scp[24];
        if (rc::scp_note(cd, scp, sizeof scp)) fact(scp, kFaint);      // passive: what the adapter's SCP readout reported
    }
    // Legacy fast-charge protocols: two columns under a divider.
    const int first = 1 + int(cd.n_ranges), cells = rows() - first;
    if (cells <= 0) {
        gfx_fill(c, 0, ly, c.w, 1, kRule);
        put_line(c, kLabelFont, 8, int16_t(ly + kit::row_text_y(kDP, kLabelFont.height)), c.w - 16, "No legacy protocols tested", kFaint, kBg);
    } else {
        const int lines = (cells + 1) / 2;
        for (int i = 0; i <= lines; i++) gfx_fill(c, 0, int16_t(ly + kDP * i), c.w, 1, kRule);
        if (cells > 1) gfx_fill(c, kColW0, ly, 1, int16_t(kDP * lines), kRule);
        for (int r = first; r < rows(); r++) paint_cell(r, r == row_);
    }
    prev_row_ = row_;
    StrBuf<32> st;
    cards_stats_text(cd, st);
    draw_footer_cached(c, FooterTone::Info, st.view(), foot_);
}

// Sub-screen of the focused row: every value the card stores for that protocol group or range.
void CardsPage::paint_group() {
    const Surface &c = ctx_.content;
    load_cur();
    const rc::Card &cd = cur_;
    row_ = std::clamp(row_, 0, rows() - 1);
    const Sel sel = sel_at(cd, row_);
    StrBuf<24> head;
    cards_title_text(cd, head);
    StrBuf<32> title;
    kit::Verdict glyph;
    if (sel.range) {
        const rc::Range &r = cd.range[sel.idx];
        StrBuf<32> rg;
        cards_range_text(r, rg);
        title.put(rc::detail::alg_label(r.alg)).put_char(' ').put(rg.view());
        glyph = to_kit(tone::range(cd, r));
    } else {
        title.put(sel.idx == 0 ? "PD fixed" : charger_test::name(static_cast<charger_test::Group>(sel.idx)));
        glyph = to_kit(tone::group(cd, sel.idx));
    }
    paint_head(glyph, title.view(), head.view(), false, true);

    CardSheet sh;
    cards_sheet(cd, sel.range, sel.idx, sh);
    int16_t y = kDY;
    for (unsigned i = 0; i < sh.n; ++i) {
        const SheetRow &r = sh.row[i];
        gfx_fill(c, 0, y, c.w, 1, kRule);
        if (!r.below) {
            put_line(c, kLabelFont, 8, int16_t(y + kit::row_text_y(kSheetPitch, kLabelFont.height)), kSheetValueX - 8 - 4, r.key, kDim, kBg);
            const int16_t ty = int16_t(y + kit::row_text_y(kSheetPitch, kSmall.height));
            if (r.verdict) {
                kit::draw_verdict(c, kSheetValueX, int16_t(y + (kSheetPitch - 12) / 2), 12, 12, r.glyph, "", kBg);
                put_line(c, kSmall, kSheetValueX + 16, ty, c.w - kSheetValueX - 16, r.line[0], r.color, kBg);
            } else {
                put_line(c, kSmall, kSheetValueX, ty, kSheetBox * kSmall.adv, r.line[0], r.color, kBg);
            }
        } else {
            put_line(c, kLabelFont, 8, int16_t(y + 3), c.w - 16, r.key, kDim, kBg);
            for (unsigned k = 0; k < r.lines; ++k)
                put_line(c, kSmall, 8, int16_t(y + kSheetKeyH + int(k) * kSheetLineH), kSheetWrap * kSmall.adv, r.line[k], r.color, kBg);
        }
        y = int16_t(y + r.height());
    }
    gfx_fill(c, 0, y, c.w, 1, kRule);
    if (y + 14 <= kFooterY) put_line(c, kLabelFont, 8, int16_t(y + 4), c.w - 16, "Per-step results are not stored", kFaint, kBg);
    StrBuf<24> pos;
    pos.put_u32(uint32_t(row_ + 1)).put_char('/').put_u32(uint32_t(rows())).put(" rows");
    draw_footer_cached(c, FooterTone::Info, pos.view(), foot_);
}

void CardsPage::paint_confirm() {
    const Surface &c = ctx_.content;
    load_cur();
    StrBuf<24> t;
    cards_title_text(cur_, t);
    char fitted[kFitBuf];
    kit::draw_alert_band(c, kit::kBodyY, false, "DELETE?");
    gfx_text(c, kSmall, 8, 68, fit_text_mid(kSmall, t.view(), c.w - 16, fitted), kText, kBg);
    put_line(c, kLabelFont, 8, 92, c.w - 16, "History of this charger is lost.", kDim, kBg);
    put_line(c, kLabelFont, 8, 106, c.w - 16, "The next test starts a new card.", kDim, kBg);
    draw_footer_cached(c, FooterTone::Caution, "History is lost for good", foot_);
}

}  // namespace k2
