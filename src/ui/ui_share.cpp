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

// One paint: the frame text, the encoder workspace and the pixel line, all in the scratch arena for the paint only.
struct PaintWork {
    char text[qrshare::kTextBuf];
    qr::Workspace w;
    std::uint16_t line[qrview::kPanelPx];
};

// begin(): the packed block of the record (lz::bound of the buffer).
struct PackWork {
    std::uint8_t out[lz::bound(SharePage::kBuffer)];
};

}  // namespace

qrview::Area SharePage::area() const {
    return {ctx_.screen.x, ctx_.screen.y, ctx_.screen.w, ctx_.screen.h};
}

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
    show(0, ctx_.now);
}

void SharePage::paint(bool clear) {
    ContentViewport::Pause unscaled;   // the code is drawn in panel pixels, not through the 224 x 240 mapping
    const qrview::Area a = area();
    scratch::Lease lease("share");
    auto &k = scratch::as<PaintWork>();
    n_ = 0;
    if (parts() != 0) {
        encodes_++;
        const std::size_t tn = link_
                                   ? (v2() ? qrshare::link::encode_v2(type(), record(), k.text)
                                           : qrshare::link::encode(type(), record(), packed(), k.text))
                                   : qrshare::encode_part(type(), schema(), record(), index_, k.text, packed());
        if (tn != 0) n_ = qr::encode_text(k.text, k.w, kMaxVersion);
    }
    const qrview::Plan p = qrview::plan_in(a, n_, kTextH, kMinModulePx);
    if (clear || p.px == 0 || n_ != drawn_n_) lcd::fill_rect(static_cast<std::uint16_t>(a.x), static_cast<std::uint16_t>(a.y),
                                                              static_cast<std::uint16_t>(a.w), static_cast<std::uint16_t>(a.h), qrview::kWhite);
    drawn_n_ = n_;
    painted_ = true;
    paints_++;
    if (p.px == 0) {
        qrview::draw_line_in(a, a.y + a.h / 2 - qrview::kLineH, parts() == 0 ? "Nothing to share" : "Code too big for the panel");
        return;
    }
    const int y = qrview::draw_code_in(a, k.w, n_, kTextH, kMinModulePx, k.line);

    char tail[12];
    const std::size_t tl = parts() > 1 ? fmt::format(tail, " %u/%u", index_ + 1, parts()) : 0;
    char line[qrview::kCells + 1];
    const std::size_t cells = std::min<std::size_t>(qrview::kCells, static_cast<std::size_t>(a.w / 6));
    const std::string_view shown = link_ ? link_caption(type()) : title();
    const std::size_t tn = std::min(shown.size(), cells > tl ? cells - tl : 0);
    std::memcpy(line, shown.data(), tn);
    std::memcpy(line + tn, tail, tl);
    line[tn + tl] = '\0';
    qrview::draw_line_in(a, y, line);
}

void SharePage::draw_static() { paint(true); }

void SharePage::update(uint32_t now_ms) {
    if (!lcd::get_erase() && cycling_ && parts() > 1 && now_ms - shown_ms_ >= kCycleMs) show(index_ + 1, now_ms);
    if (!painted_) paint(false);
}

bool SharePage::on_event(Event e) {
    if (e.press != Press::Short) return false;
    if (e.button == Button::Ok) {
        if (has_link_form()) {   // the link and the K2 data frame: the same record, two codes
            link_ = !link_;
            index_ = 0;
            painted_ = false;
            update(ctx_.now);
        } else if (parts() > 1) {
            cycling_ = !cycling_;
            shown_ms_ = ctx_.now;
        }
        return true;
    }
    if (e.button == Button::Plus || e.button == Button::Minus) {
        if (parts() > 1 && !link_) {
            cycling_ = false;   // a hand on the buttons: stay on the part chosen
            show(e.button == Button::Plus ? index_ + 1 : index_ + parts() - 1, ctx_.now);
            update(ctx_.now);
        }
        return true;
    }
    return false;   // BACK pops the page
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
