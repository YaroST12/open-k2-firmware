// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Hot-path Bench painter placed in the zero-wait flash window (K2_HOT).
#include "k2/ui/ui_pages.hpp"
#include "k2/core/hot.h"
#include "k2/ui/ui_elem.hpp"
#include "k2/ui/ui_style.hpp"
#include "k2/ui/ui_viewport.hpp"
#include "k2/ui/ui_widgets.hpp"

namespace k2 {

K2_HOT void ToolsPage::paint_bench() {
    PaintBudget *const bg = ctx_.budget;
    uint16_t b0 = 0, b1 = 0;
    const bool banded = ContentViewport::panel_band(b0, b1);
    auto in_band = [&](int16_t ly, int16_t lh) -> bool {
        if (!banded) return true;
        const Rect p = ContentViewport::mapped_rect(
            {ctx_.content.x, static_cast<int16_t>(ctx_.content.y + ly), ctx_.content.w, lh});
        return p.h > 0 && p.y <= b1 && p.y + p.h > b0;
    };
    const bench::Facts f = facts();
    const int sel = selected();
    for (int i = 0; i < bench::kPlaces; i++) {
        const int16_t ry = static_cast<int16_t>(kRowY0 + i * kRowPitch);
        if (!in_band(ry, kRowPitch)) continue;
        const bench::Row r = bench::row(static_cast<bench::Place>(i), f);
        const uint32_t h = mix_u32(mix_u32(2166136261u, static_cast<uint32_t>(r.known) | (i == sel ? 2u : 0u)), ctx_.rotation & 3u) | 1u;
        if (h != sig_[i]) {
            if (bg && !bg->admit(static_cast<int>(ctx_.content.w) * kRowPitch, true)) return;
            sig_[i] = h;
            paint_row(i, r.known, i == sel);
        } else if (bg && !status_[i].valid() && !bg->admit(status_[i].box_px(), true)) {
            return;
        }
        status_[i].draw(r.status.view());
    }
    if (!tail_) {
        if (in_band(0, kRowY0)) {
            if (bg && !bg->admit(static_cast<int>(ctx_.content.w) * kRowY0, true, true)) return;
            gfx_fill(ctx_.content, 0, 0, ctx_.content.w, kRowY0, kBg);
        }
        if (in_band(kBenchEnd, static_cast<int16_t>(ctx_.content.h - kBenchEnd))) {
            if (bg && !bg->admit(static_cast<int>(ctx_.content.w) * (ctx_.content.h - kBenchEnd), true, true)) return;
            gfx_fill(ctx_.content, 0, kBenchEnd, ctx_.content.w, static_cast<int16_t>(ctx_.content.h - kBenchEnd), kBg);
        }
        tail_ = true;
    }
}

}  // namespace k2
