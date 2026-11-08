// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#include "k2/ui/ui_layout.hpp"

namespace k2 {

Layout compute_layout(PanelEdge edge, const int16_t (&centers)[4], int16_t slot_len, int16_t thickness) {
    const int16_t lens[4] = {slot_len, slot_len, slot_len, slot_len};
    return compute_layout(edge, centers, lens, thickness);
}

Layout compute_layout(PanelEdge edge, const int16_t (&centers)[4], const int16_t (&slot_lens)[4], int16_t thickness) {
    Layout l{};
    const bool vertical = edge == PanelEdge::Right || edge == PanelEdge::Left;  // panel is a column
    const int16_t span = vertical ? kScreenH : kScreenW;

    switch (edge) {
    case PanelEdge::Right:  l.panel = {static_cast<int16_t>(kScreenW - thickness), 0, thickness, kScreenH};
                            l.content = {0, 0, static_cast<int16_t>(kScreenW - thickness), kScreenH}; break;
    case PanelEdge::Left:   l.panel = {0, 0, thickness, kScreenH};
                            l.content = {thickness, 0, static_cast<int16_t>(kScreenW - thickness), kScreenH}; break;
    case PanelEdge::Top:    l.panel = {0, 0, kScreenW, thickness};
                            l.content = {0, thickness, kScreenW, static_cast<int16_t>(kScreenH - thickness)}; break;
    case PanelEdge::Bottom: l.panel = {0, static_cast<int16_t>(kScreenH - thickness), kScreenW, thickness};
                            l.content = {0, 0, kScreenW, static_cast<int16_t>(kScreenH - thickness)}; break;
    }

    for (int i = 0; i < 4; i++) {
        int start = centers[i] - slot_lens[i] / 2;
        int end = start + slot_lens[i];
        if (start < 0) start = 0;
        if (end > span) end = span;
        int len = end > start ? end - start : 0;
        l.slot[i] = vertical ? Rect{l.panel.x, static_cast<int16_t>(start), l.panel.w, static_cast<int16_t>(len)}
                             : Rect{static_cast<int16_t>(start), l.panel.y, static_cast<int16_t>(len), l.panel.h};
    }
    return l;
}

Button slot_button(const Orientation &o, unsigned slot) {
    static constexpr Button kOrder[4] = {Button::Back, Button::Minus, Button::Ok, Button::Plus};
    if (slot > 3) slot = 3;
    // MADCTL maps the physical right edge to right/top/left/bottom; the latter two reverse
    // the key order along increasing screen coordinates.
    return o.rotation >= 2 ? kOrder[3 - slot] : kOrder[slot];
}

}  // namespace k2
