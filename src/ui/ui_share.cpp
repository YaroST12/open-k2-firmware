// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The share view (see ui_share.hpp).
#include "k2/ui/ui_share.hpp"

#include <algorithm>
#include <cstring>

#include "k2/core/fmt.hpp"
#include "k2/drivers/lcd.hpp"
#include "k2/core/lz.hpp"
#include "k2/core/scratch.hpp"
#include "k2/ui/ui_viewport.hpp"

namespace k2 {
namespace {

namespace lk = qrshare::link;

// One paint, all in the scratch arena for the paint only (no per-page workspace: the page arena is tight). The encoder takes the
// payload at the front of `temp` and uses the rest as working storage, so nothing needs `temp` once the symbol is in `code`: the
// pixel line reuses it.
//  - A link is byte mode up to the largest version the look policy shows without the hint (version 21, buffers of
//    buf_len_for(21)); its text is built straight at the front of `temp`.
//  - A K2 data frame is alphanumeric text in a buffer of its own, at most version 12.
constexpr std::size_t kLinkBuf = (qr::buf_len_for(lk::kMaxVersion) + 7) & ~std::size_t{7};
struct LinkWork {
    alignas(8) std::uint8_t code[kLinkBuf];
    union {
        std::uint8_t temp[kLinkBuf];
        std::uint16_t line[qrview::kPanelPx];
    };
};
struct FrameWork {
    char text[qrshare::kTextBuf];
    alignas(8) std::uint8_t code[qr::kBufLen];
    union {
        std::uint8_t temp[qr::kBufLen];
        std::uint16_t line[qrview::kPanelPx];
    };
};
union PaintWork {
    LinkWork link;
    FrameWork frame;
};
static_assert(sizeof(LinkWork::temp) >= sizeof(std::uint16_t) * qrview::kPanelPx, "the pixel line fits the encoder's temp");
static_assert(sizeof(FrameWork::temp) >= sizeof(std::uint16_t) * qrview::kPanelPx, "the pixel line fits the encoder's temp");
static_assert(sizeof(PaintWork) <= scratch::kSize, "one paint fits the scratch arena");

// begin(): the packed block of the record (lz::bound of the buffer).
struct PackWork {
    std::uint8_t out[lz::bound(SharePage::kBuffer)];
};

}  // namespace

bool ShareRecord::begin(qrshare::Type type, std::uint8_t schema, std::string_view title, std::size_t len) {
    type_ = type;
    schema_ = schema;
    title_len_ = std::min(title.size(), kTitleMax);
    std::memcpy(title_buf_.data(), title.data(), title_len_);
    title_buf_[title_len_] = '\0';
    len_ = len <= rec_.size() ? len : 0;
    packed_ = false;
    v2_ = false;
    if (len_ != 0 && qrshare::records::has_v2(type)) {
        // the card and the cable re-pack to schema v2 in place, in the one share record buffer (never longer than the v1
        // record, and smaller than any LZ4 block of it): the link form comes out as the smaller code
        const std::size_t n = qrshare::records::pack_v2_in_place(type, {rec_.data(), len_});
        if (n != 0) {
            len_ = n;
            schema_ = qrshare::records::kSchemaV2;
            v2_ = true;
        }
    }
    if (!v2_ && len_ != 0) {   // LZ4 the record in place when that saves a quarter (the block is built in the scratch arena)
        scratch::Lease lease("share");
        auto &z = scratch::as<PackWork>();
        const std::size_t n = qrshare::pack({rec_.data(), len_}, z.out);
        if (n != 0) {
            std::memcpy(rec_.data(), z.out, n);
            len_ = n;
            packed_ = true;
        }
    }
    parts_ = qrshare::part_count(len_);
    if (parts_ == 0) len_ = 0;
    return parts_ != 0;
}

void SharePage::show(unsigned index, uint32_t now_ms) {
    index_ = parts() ? index % parts() : 0;
    shown_ms_ = now_ms;
    painted_ = false;
}

void SharePage::enter() {
    encodes_ = paints_ = 0;
    drawn_n_ = 0;
    link_ = has_link_form();
    cycling_ = !link_ && parts() > 1;
    n_ = 0;
    info_ = caption_shown_ = busy_shown_ = false;
    plan_ = {};
    look_ = {};
    show(0, ctx_.now);
}

void SharePage::wipe() {
    const qrview::Area a = area();   // an erase pass turns this into the background, whatever the white says
    lcd::fill_rect(static_cast<std::uint16_t>(a.x), static_cast<std::uint16_t>(a.y), static_cast<std::uint16_t>(a.w),
                   static_cast<std::uint16_t>(a.h), qrview::kWhite);
}

// The code, full screen and centred as the look policy of its version says (k2/ui/qrview.hpp look_for): the box alone is centred
// on the panel; the caption goes in the margin under it when that holds a line, otherwise it is left out (the text view, Long OK,
// has it). A code of version 18 or more says "Preparing code..." before the encode and polls USB through it.
void SharePage::paint(bool clear) {
    ContentViewport::Pause unscaled;   // the code is drawn in panel pixels, not through the 224 x 240 mapping
    const qrview::Area a = area();
    painted_ = true;
    paints_++;
    n_ = 0;
    plan_ = {};
    look_ = {};
    caption_shown_ = busy_shown_ = false;
    const int mid = a.y + a.h / 2 - qrview::kLineH / 2;   // the one-line notices sit on the panel's middle
    if (parts() == 0) {
        wipe();
        drawn_n_ = 0;
        qrview::draw_line_in(a, mid, "Nothing to share");
        return;
    }

    scratch::Lease lease("share");
    auto &k = scratch::as<PaintWork>();
    std::uint8_t *code;
    std::uint8_t *temp;
    std::uint16_t *line;
    std::size_t tn;
    if (link_) {
        code = k.link.code;
        temp = k.link.temp;
        line = k.link.line;
        const std::span<char> text(reinterpret_cast<char *>(temp), kLinkBuf);
        tn = v2() ? lk::encode_v2(type(), record(), text) : lk::encode(type(), record(), packed(), text);
    } else {
        code = k.frame.code;
        temp = k.frame.temp;
        line = k.frame.line;
        tn = qrshare::encode_part(type(), schema(), record(), index_, k.frame.text, packed());
    }
    encodes_++;
    if (tn != 0) {
        if (link_ && tn > static_cast<std::size_t>(kBusyChars)) {   // the slow ones: say so before the encode
            wipe();
            qrview::draw_line_in(a, mid, kBusyText.data());
            busy_shown_ = true;
        }
        qr::set_poll(&lcd::pump);   // USB and the PD engine stay served through the eight mask candidates
        n_ = link_ ? qr::encode_placed(code, temp, tn, lk::kMaxVersion)
                   : qr::encode_text_placed(k.frame.text, code, temp, kFrameMaxVersion);
        qr::set_poll(nullptr);
    }
    if (n_ > 0) {
        look_ = qrview::look_for(qrview::version_of(n_));
        plan_ = qrview::plan_in(a, n_, 0, kMinModulePx, look_.quiet, look_.px);
    }
    if (clear || busy_shown_ || plan_.px == 0 || n_ != drawn_n_) wipe();
    drawn_n_ = n_;
    if (plan_.px == 0) {
        qrview::draw_line_in(a, mid, "Code too big for the panel");
        return;
    }
    const int y = qrview::draw_code_raw_in(a, code, n_, 0, kMinModulePx, line, look_.quiet, look_.px);
    caption_shown_ = qrview::caption_fits(plan_.box, kTextH);
    if (!caption_shown_) return;

    char tail[12];
    const std::size_t tl = parts() > 1 ? fmt::format(tail, " %u/%u", index_ + 1, parts()) : 0;
    char text[qrview::kCells + 1];
    const std::size_t cells = std::min<std::size_t>(qrview::kCells, static_cast<std::size_t>(a.w / 6));
    const std::string_view shown = link_ ? link_caption(type()) : title();
    const std::size_t sn = std::min(shown.size(), cells > tl ? cells - tl : 0);
    std::memcpy(text, shown.data(), sn);
    std::memcpy(text + sn, tail, tl);
    text[sn + tl] = '\0';
    qrview::draw_line_in(a, y, text);
}

// The text view (Long OK): what the code is and how it reads, the camera hint a wide code needs, and the keys. Text only, so
// it builds no code; the lines never overlap anything because nothing else is on the panel.
void SharePage::paint_info() {
    ContentViewport::Pause unscaled;
    const qrview::Area a = area();
    painted_ = true;
    drawn_n_ = 0;   // the panel holds no code now: the next code paint wipes the text off first
    wipe();
    char tail[12];
    const std::size_t tl = parts() > 1 && !link_ ? fmt::format(tail, " %u/%u", index_ + 1, parts()) : 0;
    char text[qrview::kCells + 1];
    const std::size_t cells = std::min<std::size_t>(qrview::kCells, static_cast<std::size_t>(a.w / 6));
    const std::size_t tn = std::min(title().size(), cells > tl ? cells - tl : 0);
    std::memcpy(text, title().data(), tn);
    std::memcpy(text + tn, tail, tl);
    text[tn + tl] = '\0';
    constexpr int kPitch = qrview::kLineH + 4;
    int y = 16;
    qrview::draw_line_in(a, y, text);
    y += kPitch;
    qrview::draw_line_in(a, y, link_ ? link_caption(type()).data() : "K2 data code");   // link_caption() is NUL terminated text
    y += kPitch;
    if (n_ > 0) {
        fmt::format(text, "QR version %d, %d px modules", version(), look_.px);
        qrview::draw_line_in(a, y, text);
        y += kPitch;
        const std::string_view hint = hint_for(look_);
        if (!hint.empty()) qrview::draw_line_in(a, y, hint.data());
    }
    y = a.y + a.h - 2 * kPitch;
    if (has_link_form()) qrview::draw_line_in(a, y, link_ ? "OK: K2 data code" : "OK: link code");
    qrview::draw_line_in(a, y + kPitch, "BACK or long OK: the code");
}

void SharePage::draw_static() {
    if (lcd::get_erase()) {   // the outgoing page's erase replay: the background over what was drawn, nothing is built
        wipe();
        return;
    }
    if (info_) paint_info();
    else paint(true);
}

void SharePage::update(uint32_t now_ms) {
    if (lcd::get_erase()) return;
    if (!info_ && cycling_ && parts() > 1 && now_ms - shown_ms_ >= kCycleMs) show(index_ + 1, now_ms);
    if (painted_) return;
    if (info_) paint_info();
    else paint(false);
}

bool SharePage::on_event(Event e) {
    if (e.button == Button::Ok && e.press == Press::Long) {   // the text view and back
        if (n_ == 0 && !info_) return true;                   // no code, nothing to describe
        info_ = !info_;
        painted_ = false;
        update(ctx_.now);
        return true;
    }
    if (e.press != Press::Short) return false;
    if (info_) {   // any key leaves the text view for the code; BACK is only that
        info_ = false;
        painted_ = false;
        if (e.button == Button::Back) {
            update(ctx_.now);
            return true;
        }
    }
    if (e.button == Button::Ok) {
        if (has_link_form()) {   // the link and the K2 data frame: the same record, two codes
            link_ = !link_;
            index_ = 0;
            painted_ = false;
        } else if (parts() > 1) {
            cycling_ = !cycling_;
            shown_ms_ = ctx_.now;
        }
    } else if (e.button == Button::Plus || e.button == Button::Minus) {
        if (parts() > 1 && !link_) {
            cycling_ = false;   // a hand on the buttons: stay on the part chosen
            show(e.button == Button::Plus ? index_ + 1 : index_ + parts() - 1, ctx_.now);
        }
    } else {
        return false;   // BACK pops the page
    }
    update(ctx_.now);   // repaints when something changed (or when the text view was just left)
    return true;
}

PanelLabels SharePage::labels() const {
    PanelLabels l = base_labels();
    for (Button b : {Button::Ok, Button::Minus, Button::Plus}) l.b[static_cast<int>(b)] = {};
    if (has_link_form()) {   // names the code the press switches TO
        l.b[static_cast<int>(Button::Ok)] = {link_ ? "K2 data" : "Link", "", "", false};
    } else if (parts() > 1) {
        l.b[static_cast<int>(Button::Ok)] = {cycling_ ? "Hold" : "Auto", "", "", false};
        l.b[static_cast<int>(Button::Minus)] = {"Prev", "", "", false};
        l.b[static_cast<int>(Button::Plus)] = {"Next", "", "", false};
    }
    return l;
}

}  // namespace k2
