// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Boot-info screen (see k2/ui/bootscreen.hpp).
#include "k2/ui/bootscreen.hpp"

#include <cstring>
#include <span>
#include <string_view>

#include "k2/core/fmt.hpp"
#include "k2/ui/qr.hpp"
#include "k2/ui/qrshare.hpp"
#include "k2/ui/qrview.hpp"
#include "k2/core/scratch.hpp"

namespace k2::bootscreen {
namespace {

// Everything one draw needs, leased from the scratch arena: the K2B1 text (the record), the share frame text, the QR
// workspace and one pixel line.
struct Work {
    char record[bootinfo::kQrMax + 1];
    char text[qrshare::kTextBuf];
    qr::Workspace w;
    std::uint16_t line[kPanelPx];
};
static_assert(sizeof(Work) <= scratch::kSize);

const char* rd_name(std::uint8_t pin) { return pin == 1 ? "CC1" : pin == 2 ? "CC2" : "none"; }

}  // namespace

void format_lines(const bootinfo::Snapshot& s, std::uint32_t frame, Lines& out) {
    fmt::format(out.id, "fw %s %s p%u", s.fw, s.git, unsigned(s.proto));
    fmt::format(out.up, "up %u.%u s  frame %u", unsigned(s.t_ms / 1000), unsigned(s.t_ms % 1000 / 100), unsigned(frame));
    const char* m = s.mode == bootmode::Mode::PlusRdCc2 ? "plus" : s.mode == bootmode::Mode::OkRdCc1 ? "ok" : "plain";
    if (s.move.moved)
        fmt::format(out.mode, "%s  Rd %s>%s at %u ms", m, rd_name(s.move.from), rd_name(s.move.to), unsigned(s.move.ms));
    else
        fmt::format(out.mode, "%s  Rd %s, no move", m, rd_name(s.pin_now));
}

void draw(const bootinfo::Snapshot& s, std::uint32_t frame) {
    scratch::Lease lease("bootscreen");
    auto& k = scratch::as<Work>();
    const std::size_t len = bootinfo::format(s, k.record);
    const std::size_t tl = qrshare::encode_part(
        qrshare::Type::BootInfo, qrshare::kSchemaBootInfo,
        {reinterpret_cast<const std::uint8_t*>(k.record), len}, 0, k.text);
    const int n = tl ? qr::encode_text(k.text, k.w, kMaxVersion) : 0;
    const int y = qrview::draw_code(k.w, n, kTextH, k.line, frame <= 1);

    Lines l;
    format_lines(s, frame, l);
    qrview::draw_line(y, l.id);
    qrview::draw_line(y + qrview::kLineH, l.up);
    qrview::draw_line(y + 2 * qrview::kLineH, l.mode);
}

void Controller::start(std::uint32_t now_ms) {
    active_ = true;
    first_ = true;
    released_ = false;
    last_ms_ = now_ms;
    released_ms_ = now_ms;
    frame_ = 0;
}

Controller::Action Controller::tick(std::uint32_t now_ms, bool key_down) {
    if (!active_) return Action::None;
    if (key_down) {
        released_ = false;
    } else if (!released_) {
        released_ = true;
        released_ms_ = now_ms;
    }
    if (first_ || (key_down && now_ms - last_ms_ >= kRefreshMs)) {
        first_ = false;
        last_ms_ = now_ms;
        frame_++;
        return Action::Redraw;
    }
    if (released_ && now_ms - released_ms_ >= kHoldMs) {
        active_ = false;
        return Action::Exit;
    }
    return Action::None;
}

}  // namespace k2::bootscreen
