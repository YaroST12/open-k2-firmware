// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#include "k2/ui/ui_blackbox.hpp"

#include <algorithm>
#include <cstring>

#include "k2/core/bbpersist.hpp"
#include "k2/pd/pd_vdm.hpp"
#include "k2/ui/ui_widgets.hpp"
#include "k2/ui/ui_style.hpp"

namespace k2 {

namespace {
namespace view = pdtrace::view;

// Timeline geometry (content surface coordinates; the right edge keeps x 221.. for the scrollbar).
constexpr int16_t kTlHeadY = 22, kStripBoxY = 54, kStripBoxH = 38;
constexpr int16_t kStripX = 8, kStripY = 58, kStripH = 30, kStripBoxW = 218;
constexpr int16_t kTlY = 96, kTlRowH = 20;
constexpr int16_t kEmptyListY = 92;     // the Live link row under the empty-page text
// Link page: a heading line, then five key/value rows (the keys are the 12 px font, so the value column starts at x92).
constexpr kit::KvLayout kLinkLayout = {44, kit::kRowH, 6, &kLabelFont, nullptr, 92, 16, Align::Left};

// A capture row is the trigger name (the tail is a time span of at most 7 cells and a gap) in the kit row's text box.
constexpr const char *kTriggerNames[] = {"Contract", "Hard reset", "VBUS dip", "Detach", "D+/D- edges", "6 A refused", "-"};
static_assert(all_fit(kTriggerNames, kSmallAdvPx, kit::kTailX - 8 - 7 * kSmallAdvPx - kit::kTextX), "a trigger name is wider than its row");
constexpr const char *kLinkNames[] = {"USB", "Thunderbolt", "DisplayPort", "Cable", "Partner USB"};
static_assert(all_fit(kLinkNames, 6, 92 - 6 - 6), "a link key is wider than its column");

const char *trigger_name(pdtrace::Trigger t) {
    switch (t) {
    case pdtrace::Trigger::Contract:  return kTriggerNames[0];
    case pdtrace::Trigger::HardReset: return kTriggerNames[1];
    case pdtrace::Trigger::VbusDip:   return kTriggerNames[2];
    case pdtrace::Trigger::Detach:    return kTriggerNames[3];
    case pdtrace::Trigger::Edges:     return kTriggerNames[4];
    case pdtrace::Trigger::Refusal:   return kTriggerNames[5];
    default:                          return kTriggerNames[6];
    }
}

// Only the triggers that mean trouble are coloured: a hard reset and a refused 6 A in red, a VBUS dip in amber.
uint16_t trigger_color(pdtrace::Trigger t) {
    switch (t) {
    case pdtrace::Trigger::HardReset: return kError;
    case pdtrace::Trigger::VbusDip:   return kCaution;
    case pdtrace::Trigger::Refusal:   return kError;
    default:                          return kDim;
    }
}

Icon trigger_icon(pdtrace::Trigger t) {
    switch (t) {
    case pdtrace::Trigger::Contract:  return Icon::Pd;
    case pdtrace::Trigger::HardReset: return Icon::Warn;
    case pdtrace::Trigger::VbusDip:   return Icon::Wave;
    case pdtrace::Trigger::Detach:    return Icon::Plug;
    case pdtrace::Trigger::Edges:     return Icon::Usb;
    case pdtrace::Trigger::Refusal:   return Icon::Cross;
    default:                          return Icon::Info;
    }
}

// "05AC:1234 Apple" / "67W 5PDO+2PPS" / "?": the key text and, apart, the vendor name (may be empty).
struct KeyText {
    char key[28];
    std::size_t n = 0;
    const char *vendor = "";
    bool identity = false;
};

KeyText key_text(const view::Identity &id) {
    KeyText k;
    k.n = view::format_key(id, std::span<char>(k.key, sizeof k.key - 1));
    k.key[k.n] = 0;
    const uint16_t vid = view::key_vid(id);
    if (vid) {
        k.identity = true;
        const char *v = pd::svid_name(vid);
        if (v && v[0] != 'U') k.vendor = v;      // "Unknown" stays out of the header
    }
    return k;
}

uint32_t hash_mix(uint32_t h, std::string_view s) {
    for (char c : s) h = (h ^ static_cast<uint8_t>(c)) * 16777619u;
    return h;
}

// "#7 2.5s 5 msg" (sequence number, time span, messages) as its three parts. Absolute, so it does not change while the
// screen stays up. A capture still recording has no span yet: `rec` is set and the parts stay empty.
struct CaptureParts {
    char text[24];
    std::string_view all, seq, span, count;
    bool rec = false;
};
void capture_parts(CaptureParts &p, const pdtrace::CaptureInfo &info, std::span<const uint8_t> data) {
    if (info.filling) { p.rec = true; return; }
    const std::string_view all(p.text, view::format_capture(data, info.seq, p.text));
    p.all = all;
    const std::size_t a = all.find(' ');
    const std::size_t b = a == std::string_view::npos ? a : all.find(' ', a + 1);
    p.seq = all.substr(0, a);
    if (b == std::string_view::npos) return;
    p.span = all.substr(a + 1, b - a - 1);
    p.count = all.substr(b + 1);
}

// Copies s (cut to fit) into dst[0..cap-1] and returns the view of the copy.
std::string_view stash(char *dst, std::size_t cap, std::string_view s) {
    const std::size_t n = std::min(s.size(), cap - 1);
    std::memcpy(dst, s.data(), n);
    dst[n] = 0;
    return {dst, n};
}

// "41K free" of the flash ring, or "idle, not saving" once no PD traffic came for the idle time.
template <size_t N>
void put_flash(StrBuf<N> &f, const bbpersist::Status &s) {
    if (s.idle) f.put("Idle, not saving. ");
    f.put_u32(s.free_bytes / 1024).put("K free");
}
}  // namespace

// ---- model: identities, groups ------------------------------------------------------------------

void BlackBoxPage::refresh_model() {
    uint32_t age[pdtrace::kCapSlots] = {};
    uint8_t byage[pdtrace::kCapSlots];
    uint8_t n = 0;
    for (uint8_t i = 0; i < pdtrace::kCapSlots; i++) {
        const auto info = pdtrace::capture_info(i);
        SlotId &s = ids_[i];
        if (info.trigger == pdtrace::Trigger::None) { s.valid = false; continue; }
        if (!s.valid || s.len != info.len || s.pre != info.pre || s.filling != info.filling ||
            s.trigger != static_cast<uint8_t>(info.trigger)) {
            s.valid = true;
            s.len = info.len; s.pre = info.pre; s.filling = info.filling;
            s.trigger = static_cast<uint8_t>(info.trigger);
            s.id = view::identify(pdtrace::capture_data(i));
        }
        age[i] = info.age_ms;
        uint8_t k = n++;
        while (k > 0 && age[byage[k - 1]] > age[i]) { byage[k] = byage[k - 1]; k--; }
        byage[k] = i;
    }
    // Groups in order of their newest capture; members newest first.
    uint8_t leader[pdtrace::kCapSlots];     // group -> newest slot
    uint8_t gcount[pdtrace::kCapSlots] = {};
    uint8_t group_of[pdtrace::kCapSlots] = {};
    uint8_t ng = 0;
    for (uint8_t k = 0; k < n; k++) {
        const uint8_t s = byage[k];
        uint8_t g = 0;
        while (g < ng && !view::same_key(ids_[leader[g]].id, ids_[s].id)) g++;
        if (g == ng) leader[ng++] = s;
        group_of[s] = g;
        gcount[g]++;
    }
    nrows_ = 0;
    norder_ = 0;
    for (uint8_t g = 0; g < ng; g++) {
        rows_[nrows_++] = {true, leader[g], gcount[g]};
        for (uint8_t k = 0; k < n; k++) {
            if (group_of[byage[k]] != g) continue;
            rows_[nrows_++] = {false, byage[k], 0};
            order_[norder_++] = byage[k];
        }
    }
    bool have = sel_slot_ == kLive;
    for (uint8_t k = 0; k < norder_; k++) have = have || order_[k] == sel_slot_;
    if (!have || select_first_) sel_slot_ = norder_ ? order_[0] : kLive;
    if (norder_) select_first_ = false;
}

// ---- list ---------------------------------------------------------------------------------------------------

int BlackBoxPage::sel_row() const {
    if (sel_slot_ != kLive)
        for (int i = 0; i < nrows_; i++)
            if (!rows_[i].header && rows_[i].slot == sel_slot_) return i;
    return nrows_;
}

// Row i of the list: a group header (the key and vendor dim, the capture count at the right), a capture (icon of the
// trigger, its name, the time span; the open row adds the sequence number and the message count) or, last, the live link.
void BlackBoxPage::row_fn(void *ctx, int i, bool, kit::RowSpec &out) {
    auto &p = *static_cast<BlackBoxPage *>(ctx);
    // The RowSpec views the page scratch (shared with the timeline's strip columns, which the list never needs).
    char *const abuf = p.sc_.row.a, *const tbuf = p.sc_.row.tail;
    constexpr std::size_t acap = sizeof p.sc_.row.a, tcap = sizeof p.sc_.row.tail;
    out.marker = kit::Marker::Icon;
    if (i >= p.nrows_) {
        out.icon = Icon::Cable;
        out.text = "Live link";
        out.tail = pdtrace::live_link().any() ? "seen" : "not seen";
        return;
    }
    const Row &r = p.rows_[i];
    if (r.header) {
        // "05AC:1234 Apple": the vendor is dropped (not cut) when the row has no room for it.
        const KeyText k = key_text(p.ids_[r.slot].id);
        StrBuf<acap> a;
        StrBuf<tcap> t;
        a.put(std::string_view(k.key, k.n));
        if (k.vendor[0] && text_cells(std::string_view(k.key, k.n)) + 1 + text_cells(k.vendor) <= 20) a.put(" ").put(k.vendor);
        t.put_char('x').put_u32(r.count);
        out.marker = kit::Marker::None;
        out.text = stash(abuf, acap, a.view());
        out.text_color = k.identity ? kText : kDim;
        out.tail = stash(tbuf, tcap, t.view());
        return;
    }
    const auto info = pdtrace::capture_info(r.slot);
    CaptureParts cp;
    capture_parts(cp, info, pdtrace::capture_data(r.slot));
    out.icon = trigger_icon(info.trigger);
    out.marker_color = trigger_color(info.trigger);
    out.text = trigger_name(info.trigger);
    if (cp.rec) {
        out.tail = "REC";
        out.tail_color = kCaution;
        return;
    }
    StrBuf<acap> a;
    a.put(cp.seq).put("  ").put(cp.count);
    out.tail = stash(tbuf, tcap, cp.span);
    out.note = stash(abuf, acap, a.view());
}

void BlackBoxPage::paint_empty_text() {
    const Surface &c = ctx_.content;
    gfx_text(c, kSmall, 8, 30, "No captures yet", kText, kBg);
    gfx_text(c, kLabelFont, 8, 54, "Contract, hard reset,", kDim, kBg);
    gfx_text(c, kLabelFont, 8, 68, "VBUS dip or detach", kDim, kBg);
}

void BlackBoxPage::paint_list() {
    const Surface &c = ctx_.content;
    const bool empty = nrows_ == 0;
    if (bound_ != (empty ? 1 : 0)) {                  // first paint, or the list became empty / non-empty
        gfx_fill(c, 0, kBarH, c.w, static_cast<int16_t>(kFooterY - kBarH), kBg);
        if (empty) paint_empty_text();
        ctx_.rows.init(c, empty ? kEmptyListY : kit::kBodyY, row_fn, this);
        ctx_.rows.invalidate();
        bound_ = empty ? 1 : 0;
    }
    ctx_.rows.set_count(nrows_ + 1);                  // the groups and captures, then the live row
    ctx_.rows.select(sel_row());
    ctx_.rows.draw(ctx_.now);
    const bbpersist::Status fl = bbpersist::status();
    StrBuf<32> f;
    if (empty) {
        f.put("Empty. ");
    } else {
        f.put_u32(norder_).put(norder_ == 1 ? " capture. " : " captures. ");
    }
    put_flash(f, fl);
    draw_footer_cached(c, FooterTone::Info, f.view(), foot_);   // a count, not a good state
}

// ---- timeline -----------------------------------------------------------------------------------

void BlackBoxPage::paint_timeline_head() {
    const Surface &c = ctx_.content;
    const auto info = pdtrace::capture_info(slot_);
    const KeyText k = key_text(ids_[slot_].id);
    CaptureParts cp;
    capture_parts(cp, info, data());
    const std::string_view at = cp.rec ? std::string_view("REC") : cp.all;
    uint32_t sig = hash_mix(2166136261u, at);
    sig = hash_mix(sig, std::string_view(k.key, k.n));
    sig = hash_mix(sig, k.vendor);
    sig = (sig ^ static_cast<uint8_t>(info.trigger)) * 16777619u;
    if (head_painted_ && sig == head_sig_) return;
    gfx_fill(c, 0, kTlHeadY, c.w, static_cast<int16_t>(kStripBoxY - kTlHeadY), kBg);
    draw_icon(c, 6, kTlHeadY + 2, trigger_icon(info.trigger), trigger_color(info.trigger), kBg);
    gfx_text(c, kSmall, 24, kTlHeadY, trigger_name(info.trigger), kText, kBg);
    if (info.filling) {
        const int w = pill_width(3);
        draw_pill(c, static_cast<int16_t>(216 - w), kTlHeadY + 1, "REC", kCaution, kCautionT, true);
    } else {
        const int w = static_cast<int>(at.size()) * kLabelFont.adv;
        gfx_text(c, kLabelFont, static_cast<int16_t>(216 - w), kTlHeadY + 3, at, kDim, kBg);
    }
    const int avail = (216 - 8) / kLabelFont.adv;
    std::size_t kn = std::min<std::size_t>(k.n, static_cast<std::size_t>(avail));
    gfx_text(c, kLabelFont, 8, kTlHeadY + 19, std::string_view(k.key, kn), k.identity ? kText : kDim, kBg);
    if (k.vendor[0]) {
        const int room = avail - static_cast<int>(kn) - 1;
        std::string_view v(k.vendor);
        if (room > 0) {
            if (static_cast<int>(v.size()) > room) v = v.substr(0, static_cast<std::size_t>(room));
            gfx_text(c, kLabelFont, static_cast<int16_t>(8 + (kn + 1) * kLabelFont.adv), kTlHeadY + 19, v, kDim, kBg);
        }
    }
    head_painted_ = true;
    head_sig_ = sig;
}

void BlackBoxPage::paint_link() {
    const Surface &c = ctx_.content;
    const view::Link l = link_live_ ? pdtrace::live_link() : view::link_of(data());
    const view::LinkText t = view::link_text(l);
    char dp[16] = {};
    if (t.dp[0]) { dp[0] = 'D'; dp[1] = 'P'; dp[2] = ' '; dp[3] = 'a'; dp[4] = 'l'; dp[5] = 't'; dp[6] = ' '; dp[7] = t.dp[0]; }
    const char *const rows[5] = {t.usb, t.tbt, dp, t.cable, t.peer};
    uint32_t sig = hash_mix(2166136261u ^ (link_live_ ? 1u : 0u), std::string_view(rows[0]));
    for (int i = 1; i < 5; i++) sig = hash_mix(sig, std::string_view(rows[i]));
    if (link_painted_ && sig == link_sig_) return;
    gfx_fill(c, 0, kBarH, c.w, static_cast<int16_t>(kFooterY - kBarH), kBg);
    gfx_text(c, kSmall, 8, kTlHeadY, link_live_ ? "Live link" : "Link", kText, kBg);
    for (int i = 0; i < 5; i++) {
        const bool seen = rows[i][0] != 0;
        kit::kv_row(c, kLinkLayout, kit::kv_row_y(kLinkLayout, i), kLinkNames[i], seen ? rows[i] : "not seen", seen ? kText : kDim);
    }
    gfx_fill(c, 0, kit::kv_row_y(kLinkLayout, 5), c.w, 1, kRule);
    draw_footer(c, FooterTone::Info, "CC wire only, no data");
    link_painted_ = true;
    link_sig_ = sig;
}

// The VBUS strip sits on the page background between two hairlines (no card).
void BlackBoxPage::draw_strip() {
    const Surface &c = ctx_.content;
    const auto recs = data();
    gfx_fill(c, 0, kStripBoxY, c.w, kStripBoxH, kBg);
    gfx_fill(c, 0, kStripBoxY, c.w, 1, kRule);
    gfx_fill(c, 0, kStripBoxY + kStripBoxH - 1, c.w, 1, kRule);
    const view::Strip st =
        view::vbus_strip(recs, std::span<uint8_t>(sc_.heights, size_t(strip_w_)), uint8_t(kStripH - 1));
    if (!st.valid) {
        gfx_text(c, kLabelFont, kStripX, kStripY + 8, "no VBUS samples", kDim, kBg);
        return;
    }
    for (int x = 0; x < strip_w_; x++) {
        if (sc_.heights[x] == 0xFF) continue;
        const int h = sc_.heights[x] + 1;
        gfx_fill(c, int16_t(kStripX + x), int16_t(kStripY + kStripH - h), 1, int16_t(h), kVolt);
    }
    const int mx = int(view::column_of(st, view::time_at(recs, pdtrace::capture_info(slot_).pre), size_t(strip_w_)));
    gfx_fill(c, int16_t(kStripX + mx), kStripY, 1, kStripH, kCaution);
}

void BlackBoxPage::draw_line_row(void *self, int index, const Rect &, bool, Field &f) {
    const BlackBoxPage &p = *static_cast<const BlackBoxPage *>(self);
    view::Row r;
    if (!view::row_at(p.data(), size_t(index), r)) { f.set_colors(kDim, kBg); f.draw(""); return; }
    char buf[32];
    const size_t n = view::format_row(r, buf);
    f.set_colors(r.dir == '!' ? kError : r.dir == '<' ? kAmp : kVolt, kBg);
    f.draw(std::string_view(buf, n));
}

// ---- page ---------------------------------------------------------------------------------------

void BlackBoxPage::enter_mode(Mode m) {
    mode_ = m;
    const Surface &c = ctx_.content;
    gfx_fill(c, 0, kBarH, c.w, static_cast<int16_t>(kFooterY - kBarH), kBg);   // keep the status bar
    bound_ = -1;
    head_painted_ = false;
    link_painted_ = false;
    foot_.invalidate();
    if (m == Mode::Timeline) {
        const int vis = std::max(1, std::min(ListView::kMaxRows, (kFooterY - kTlY) / kTlRowH));
        list_.init(c, 6, kTlY, 21, kTlRowH, vis, kSmall, &BlackBoxPage::draw_line_row, this);
        strip_w_ = std::min<int>(kStripMax, kStripBoxW - 12);
        strip_dirty_ = true;
        shown_len_ = 0xFFFF;
        list_.invalidate();
    }
}

void BlackBoxPage::draw_static() {
    title_bar();
    for (auto &s : ids_) s.valid = false;     // rescan: the slots may have changed while away
    enter_mode(mode_);
}

void BlackBoxPage::update(uint32_t) {
    title_badge(false);
    if (mode_ == Mode::List) {
        refresh_model();
        paint_list();
        return;
    }
    if (mode_ == Mode::Link) { paint_link(); return; }
    const auto info = pdtrace::capture_info(slot_);
    refresh_model();
    paint_timeline_head();
    if (info.len != shown_len_) {          // still recording, or replaced: recount and repaint
        shown_len_ = info.len;
        strip_dirty_ = true;
        list_.invalidate();
    }
    if (strip_dirty_) { draw_strip(); strip_dirty_ = false; }
    list_.set_count(int(view::count_rows(data())));
    list_.draw(ctx_.now);
    const ListState &st = list_.state();
    StrBuf<30> f;
    char sm[32];
    f.put(std::string_view(sm, view::format_capture_long(data(), info.seq, sm)));
    if (st.overflows()) f.put(" Scroll");
    draw_footer_cached(ctx_.content, FooterTone::Info, f.view(), foot_);   // a recording is a fact, not a verdict
}

bool BlackBoxPage::on_event(Event e) {
    if (e.press != Press::Short) return false;
    if (mode_ == Mode::Link) {
        if (e.button != Button::Back) return false;
        enter_mode(link_from_);
        update(ctx_.now);
        return true;
    }
    if (mode_ == Mode::List) {
        refresh_model();
        if (e.button == Button::Ok) {
            if (sel_slot_ == kLive) {
                link_live_ = true;
                link_from_ = Mode::List;
                enter_mode(Mode::Link);
            } else {
                slot_ = sel_slot_;
                enter_mode(Mode::Timeline);
            }
            update(ctx_.now);
            return true;
        }
        if (e.button == Button::Plus || e.button == Button::Minus) {
            const int total = norder_ + 1;                      // the captures, then the live row
            int pos = norder_;
            for (uint8_t k = 0; k < norder_; k++) if (order_[k] == sel_slot_) pos = k;
            const int n = pos + (e.button == Button::Plus ? 1 : -1);
            if (n >= 0 && n < total) { sel_slot_ = n < norder_ ? order_[n] : kLive; update(ctx_.now); }
            return true;
        }
        return false;
    }
    if (e.button == Button::Ok) {
        link_live_ = false;
        link_from_ = Mode::Timeline;
        enter_mode(Mode::Link);
        update(ctx_.now);
        return true;
    }
    if (e.button == Button::Back) {
        enter_mode(Mode::List);
        update(ctx_.now);
        return true;
    }
    return scroll_event(list_, e, ctx_.now);
}

PanelLabels BlackBoxPage::labels() const {
    PanelLabels l = base_labels();
    l.b[static_cast<int>(Button::Ok)] = {mode_ == Mode::List ? "Open" : mode_ == Mode::Timeline ? "Link" : "", "", "", mode_ == Mode::Link};
    return l;
}

void BlackBoxPage::invalidate() {
    list_.invalidate();
    ctx_.rows.invalidate();
    bound_ = -1;
    head_painted_ = false;
    foot_.invalidate();
    link_painted_ = false;
    strip_dirty_ = true;
}

}  // namespace k2
