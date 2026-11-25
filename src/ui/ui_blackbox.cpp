// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#include "k2/ui/ui_blackbox.hpp"

#include <algorithm>
#include <cstring>

#include "k2/core/bbpersist.hpp"
#include "k2/pd/pd_vdm.hpp"
#include "k2/ui/ui_pages.hpp"
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
constexpr const char *kTriggerNames[] = {K2_P("Contract"), K2_P("Hard reset"), K2_P("VBUS dip"), K2_P("Detach"), K2_P("D+/D- edges"), K2_P("6 A refused"), "-"};
static_assert(all_fit_p(kTriggerNames, kSmallAdvPx, kit::kTailX - 8 - 7 * kSmallAdvPx - kit::kTextX), "a trigger name is wider than its row");
constexpr const char *kLinkNames[] = {"USB", K2_P("Thunderbolt"), K2_P("DisplayPort"), "Cable", K2_P("Partner USB")};
static_assert(all_fit_p(kLinkNames, 6, 92 - 6 - 6), "a link key is wider than its column");

struct TrigMeta { const char *name; uint16_t color; Icon icon; };
constexpr TrigMeta kTrigMeta[7] = {
    {kTriggerNames[6], kDim,     Icon::Info},   // None = 0
    {kTriggerNames[0], kDim,     Icon::Pd},     // Contract = 1
    {kTriggerNames[1], kError,   Icon::Warn},   // HardReset = 2
    {kTriggerNames[2], kCaution, Icon::Wave},   // VbusDip = 3
    {kTriggerNames[3], kDim,     Icon::Plug},   // Detach = 4
    {kTriggerNames[4], kDim,     Icon::Usb},    // Edges = 5
    {kTriggerNames[5], kError,   Icon::Cross},  // Refusal = 6
};
const TrigMeta &trig_meta(pdtrace::Trigger t) {
    const unsigned i = static_cast<unsigned>(t);
    return kTrigMeta[i < 7 ? i : 0];
}
const char *trigger_name(pdtrace::Trigger t) { return trig_meta(t).name; }
uint16_t trigger_color(pdtrace::Trigger t) { return trig_meta(t).color; }
Icon trigger_icon(pdtrace::Trigger t) { return trig_meta(t).icon; }

// "05AC:1234 Apple" / "67W 5PDO+2PPS" / "?": the key text and, apart, the vendor name (may be empty).
struct KeyText {
    char key[28];
    std::size_t n = 0;
    char vendor[pd::kVendorMax + 1] = "";   // a copy: the name is decoded per lookup
    bool identity = false;
};

KeyText key_text(const view::Identity &id) {
    KeyText k;
    k.n = view::format_key(id, std::span<char>(k.key, sizeof k.key - 1));
    k.key[k.n] = 0;
    const uint16_t vid = view::key_vid(id);
    if (vid) {
        k.identity = true;
        const pd::VendorName v = pd::svid_name(vid);
        if (v.view() != "Unknown") std::memcpy(k.vendor, v.c_str(), sizeof k.vendor);   // "Unknown" stays out of the header
    }
    return k;
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
    using namespace std::string_view_literals;
    auto &p = *static_cast<BlackBoxPage *>(ctx);
    // The RowSpec views the page scratch (shared with the timeline's strip columns, which the list never needs).
    char *const abuf = p.sc_.row.a, *const tbuf = p.sc_.row.tail;
    constexpr std::size_t acap = sizeof p.sc_.row.a, tcap = sizeof p.sc_.row.tail;
    out.marker = kit::Marker::Icon;
    if (i >= p.nrows_) {
        out.icon = Icon::Cable;
        out.text = K2_PSV("Live link");
        out.tail = pdtrace::live_link().any() ? "seen"sv : K2_PSV("not seen");
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
        out.text = pg::stash_str(abuf, acap, a.view());
        out.text_color = k.identity ? kText : kDim;
        out.tail = pg::stash_str(tbuf, tcap, t.view());
        return;
    }
    const auto info = pdtrace::capture_info(r.slot);
    CaptureParts cp;
    capture_parts(cp, info, pdtrace::capture_data(r.slot));
    out.icon = trigger_icon(info.trigger);
    out.marker_color = trigger_color(info.trigger);
    strpack::assign_sv(out.text, trigger_name(info.trigger));
    if (cp.rec) {
        out.tail = "REC"sv;
        out.tail_color = kCaution;
        return;
    }
    StrBuf<acap> a;
    a.put(cp.seq).put("  ").put(cp.count);
    out.tail = pg::stash_str(tbuf, tcap, cp.span);
    out.note = pg::stash_str(abuf, acap, a.view());
}

void BlackBoxPage::paint_empty_text() {
    const Surface &c = ctx_.content;
    gfx_text(c, kSmall, 8, 30, K2_PSV("No captures yet"), kText, kBg);
    gfx_text(c, kLabelFont, 8, 54, K2_PSV("Contract, hard reset,"), kDim, kBg);
    gfx_text(c, kLabelFont, 8, 68, K2_PSV("VBUS dip or detach"), kDim, kBg);
}

void BlackBoxPage::paint_list() {
    const Surface &c = ctx_.content;
    const bool empty = nrows_ == 0;
    if (bound_ != (empty ? 1 : 0)) {                  // first paint, or the list became empty / non-empty
        if (!ctx_.blank) pg::clear_body(c);   // a blank screen is kBg already
        if (empty) paint_empty_text();
        ctx_.rows.init(c, empty ? kEmptyListY : kit::kBodyY, row_fn, this);
        ctx_.rows.invalidate();
        bound_ = empty ? 1 : 0;
    }
    ctx_.rows.set_count(nrows_ + 1);                  // the groups and captures, then the live row
    ctx_.rows.select(sel_row());
    ctx_.rows.draw(ctx_.now, ctx_.budget);
}

// ---- timeline -----------------------------------------------------------------------------------

void BlackBoxPage::paint_timeline_head() {
    const Surface &c = ctx_.content;
    const auto info = pdtrace::capture_info(slot_);
    const KeyText k = key_text(ids_[slot_].id);
    CaptureParts cp;
    capture_parts(cp, info, data());
    const std::string_view at = cp.rec ? std::string_view("REC") : cp.all;
    uint32_t sig = pg::mix_str(at);
    sig = pg::mix_str(sig, std::string_view(k.key, k.n));
    sig = pg::mix_cstr(sig, k.vendor);
    sig = mix_u32(sig, static_cast<uint8_t>(info.trigger));
    if (head_painted_ && sig == head_sig_) return;
    gfx_fill(c, 0, kTlHeadY, c.w, static_cast<int16_t>(kStripBoxY - kTlHeadY), kBg);
    draw_icon(c, 6, kTlHeadY + 2, trigger_icon(info.trigger), trigger_color(info.trigger), kBg);
    gfx_text(c, kSmall, 24, kTlHeadY, trigger_name(info.trigger), kText, kBg);
    const int16_t right = static_cast<int16_t>(c.w - 8);
    if (info.filling) {
        const int w = kit::chip_w("REC");
        kit::draw_chip(c, static_cast<int16_t>(right - w), kTlHeadY + 1, "REC", kCaution, kBg);
    } else {
        const int w = static_cast<int>(at.size()) * kLabelFont.adv;
        gfx_text(c, kLabelFont, static_cast<int16_t>(right - w), kTlHeadY + 3, at, kDim, kBg);
    }
    const int avail = (right - 8) / kLabelFont.adv;
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
    StrBuf<16> dp;
    if (t.dp[0]) dp.put(K2_P("DP alt ")).put_char(t.dp[0]);
    const char *const rows[5] = {t.usb, t.tbt, dp.c_str(), t.cable, t.peer};
    uint32_t sig = 2166136261u ^ (link_live_ ? 1u : 0u);
    for (int i = 0; i < 5; i++) sig = pg::mix_cstr(sig, rows[i]);
    if (link_painted_ && sig == link_sig_) return;
    pg::clear_body(c);
    gfx_text(c, kSmall, 8, kTlHeadY, link_live_ ? K2_PSV("Live link") : std::string_view("Link"), kText, kBg);
    for (int i = 0; i < 5; i++) {
        const bool seen = rows[i][0] != 0;
        std::string_view key, val = K2_PSV("not seen");
        strpack::assign_sv(key, kLinkNames[i]);
        if (seen) strpack::assign_sv(val, rows[i]);
        kit::kv_row(c, kLinkLayout, kit::kv_row_y(kLinkLayout, i), key, val, seen ? kText : kDim);
    }
    gfx_fill(c, 0, kit::kv_row_y(kLinkLayout, 5), c.w, 1, kRule);
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
        gfx_text(c, kLabelFont, kStripX, kStripY + 8, K2_PSV("no VBUS samples"), kDim, kBg);
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
    char buf[32];
    size_t n = 0;
    uint16_t col = kDim;
    if (view::row_at(p.data(), size_t(index), r)) {
        n = view::format_row(r, buf);
        col = r.dir == '!' ? kError : r.dir == '<' ? kAmp : kVolt;
    }
    f.set_colors(col, kBg);
    f.draw(std::string_view(buf, n));
}

// ---- page ---------------------------------------------------------------------------------------

void BlackBoxPage::enter_mode(Mode m) {
    mode_ = m;
    const Surface &c = ctx_.content;
    if (!ctx_.blank) pg::clear_body(c);   // keep the status bar; a blank screen is kBg already
    bound_ = -1;
    head_painted_ = false;
    link_painted_ = false;
    foot_.invalidate();
    if (m == Mode::Timeline) {
        const int vis = std::max(1, std::min(ListView::kMaxRows, (kFooterY - kTlY) / kTlRowH));
        ctx_.rows.detach();
        ListView &lv = ctx_.rows.raw();
        lv.init(c, 6, kTlY, 21, kTlRowH, vis, kSmall, &BlackBoxPage::draw_line_row, this);
        lv.set_count(int(view::count_rows(data())), false);
        lv.set_offset(tl_offset_);
        strip_w_ = std::min<int>(kStripMax, kStripBoxW - 12);
        strip_dirty_ = true;
        shown_len_ = 0xFFFF;
        lv.invalidate();
    } else if (m == Mode::List) {
        ctx_.rows.detach();
        tl_offset_ = 0;
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
        ctx_.rows.raw().invalidate();
    }
    if (strip_dirty_) { draw_strip(); strip_dirty_ = false; }
    ListView &lv = ctx_.rows.raw();
    lv.set_count(int(view::count_rows(data())));
    lv.draw(ctx_.now, ctx_.budget);
    const ListState &st = lv.state();
    tl_offset_ = st.offset;
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
                tl_offset_ = 0;
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
    const bool handled = scroll_event(ctx_.rows.raw(), e, ctx_.now);
    if (handled) tl_offset_ = ctx_.rows.state().offset;
    return handled;
}

Strip BlackBoxPage::labels() const {
    Strip l = base_labels();
    l[Button::Ok] = mode_ == Mode::List ? sl::Open : mode_ == Mode::Timeline ? sl::Link : mode_ == Mode::Link ? sl::Dim : sl::None;
    return l;
}

void BlackBoxPage::invalidate() {
    ctx_.rows.invalidate();
    bound_ = -1;
    head_painted_ = false;
    foot_.invalidate();
    link_painted_ = false;
    strip_dirty_ = true;
}

}  // namespace k2
