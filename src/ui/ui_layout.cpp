// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#include "k2/ui/ui_layout.hpp"

namespace k2 {

Rect opp_slot_rect(const Rect &rail, PanelEdge opp_edge, TextDir dir, unsigned slot) {
    int16_t at = 0;
    for (unsigned i = 0; i < slot && i < 4; ++i) at = static_cast<int16_t>(at + kOppSlotLen[i]);
    const int16_t len = kOppSlotLen[slot & 3];
    if (opp_reversed(opp_edge, dir)) at = static_cast<int16_t>(rail.h - at - len);
    return panel_is_column(opp_edge) ? Rect{rail.x, static_cast<int16_t>(rail.y + at), rail.w, len}
                                     : Rect{static_cast<int16_t>(rail.x + at), rail.y, len, rail.h};
}

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
                            l.content = {0, 0, static_cast<int16_t>(kScreenW - thickness), kScreenH};
                            l.opp_rail = {0, 0, thickness, kScreenH};
                            l.twin_content = {thickness, 0, static_cast<int16_t>(kScreenW - 2 * thickness), kScreenH}; break;
    case PanelEdge::Left:   l.panel = {0, 0, thickness, kScreenH};
                            l.content = {thickness, 0, static_cast<int16_t>(kScreenW - thickness), kScreenH};
                            l.opp_rail = {static_cast<int16_t>(kScreenW - thickness), 0, thickness, kScreenH};
                            l.twin_content = {thickness, 0, static_cast<int16_t>(kScreenW - 2 * thickness), kScreenH}; break;
    case PanelEdge::Top:    l.panel = {0, 0, kScreenW, thickness};
                            l.content = {0, thickness, kScreenW, static_cast<int16_t>(kScreenH - thickness)};
                            l.opp_rail = {0, static_cast<int16_t>(kScreenH - thickness), kScreenW, thickness};
                            l.twin_content = {0, thickness, kScreenW, static_cast<int16_t>(kScreenH - 2 * thickness)}; break;
    case PanelEdge::Bottom: l.panel = {0, static_cast<int16_t>(kScreenH - thickness), kScreenW, thickness};
                            l.content = {0, 0, kScreenW, static_cast<int16_t>(kScreenH - thickness)};
                            l.opp_rail = {0, 0, kScreenW, thickness};
                            l.twin_content = {0, thickness, kScreenW, static_cast<int16_t>(kScreenH - 2 * thickness)}; break;
    }

    const bool rev = edge == PanelEdge::Left || edge == PanelEdge::Bottom;
    for (int i = 0; i < 4; i++) {
        const int bi = rev ? 3 - i : i;
        const int c = rev ? (span - centers[bi]) : centers[bi];
        const int sl = slot_lens[bi];
        int start = c - sl / 2;
        int end = start + sl;
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
