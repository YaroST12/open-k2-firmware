// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#include "k2/ui/ui_orientation.hpp"
#include "k2/ui/ui_style.hpp"
#include "k2/ui/ui_calc.hpp"

namespace k2 {

void draw_physical_arrow(const Surface& s, int16_t x, int16_t y, bool toward_top,
                         uint8_t rotation, uint16_t color) {
    rotation = static_cast<uint8_t>((rotation + (toward_top ? 0 : 2)) & 3);
    for (int row = 1; row < 15; ++row) {
        for (int col = 1; col < 15; ++col) {
            const bool head = row <= 6 && (col == 8 - row || col == 9 - row ||
                                          col == 6 + row || col == 7 + row);
            const bool shaft = row >= 5 && (col == 7 || col == 8);
            if (!head && !shaft) continue;
            const int xx = rotation == 0 ? col : rotation == 1 ? row :
                           rotation == 2 ? 15 - col : 15 - row;
            const int yy = rotation == 0 ? row : rotation == 1 ? 15 - col :
                           rotation == 2 ? 15 - row : col;
            gfx_fill(s, static_cast<int16_t>(x + xx), static_cast<int16_t>(y + yy), 1, 1, color);
        }
    }
}

void draw_physical_dash(const Surface& s, int16_t x, int16_t y, uint8_t rotation, uint16_t color) {
    if (rotation & 1) gfx_fill(s, static_cast<int16_t>(x + 4), static_cast<int16_t>(y + 7), 8, 2, color);
    else              gfx_fill(s, static_cast<int16_t>(x + 7), static_cast<int16_t>(y + 4), 2, 8, color);
}

void draw_physical_chevron(const Surface& s, int16_t x, int16_t y, bool toward_top,
                           uint8_t rotation, uint16_t color) {
    rotation &= 3;
    if (!(rotation & 1)) { draw_chevron(s, x, y, toward_top != (rotation == 2), color); return; }
    for (int row = 0; row < kChevH; ++row) {
        const int left = chevron_left(toward_top, row);
        const int col = rotation == 1 ? row : kChevH - 1 - row;
        for (int arm : {left, 8 - left}) {
            const int yy = rotation == 1 ? kChevW - 2 - arm : arm;
            gfx_fill(s, static_cast<int16_t>(x + col), static_cast<int16_t>(y + yy), 1, 2, color);
        }
    }
}

void draw_physical_flow_diagram(const Surface& s, int16_t x, int16_t y, int16_t h,
                               const FlowSpec& spec, uint8_t rotation) {
    rotation &= 3;
    if (rotation == 0) { draw_flow_diagram(s, x, y, h, spec); return; }
    if (rotation == 2) {
        FlowSpec mirrored = spec;
        mirrored.top_role = spec.bottom_role; mirrored.bottom_role = spec.top_role;
        mirrored.top_color = spec.bottom_color; mirrored.bottom_color = spec.top_color;
        mirrored.top_tint = spec.bottom_tint; mirrored.bottom_tint = spec.top_tint;
        mirrored.flow = spec.flow == Arrow::Idle ? Arrow::Idle : spec.flow == Arrow::Up ? Arrow::Down : Arrow::Up;
        draw_flow_diagram(s, x, y, h, mirrored);
        const auto g = flow_geom(h);
        gfx_fill(s, static_cast<int16_t>(x + 6), static_cast<int16_t>(y + g.top_port_y), 18, 8, kSurface);
        gfx_fill(s, static_cast<int16_t>(x + 6), static_cast<int16_t>(y + g.bottom_port_y), 18, 8, kSurface);
        draw_port_female(s, static_cast<int16_t>(x + 6), static_cast<int16_t>(y + g.top_port_y), spec.bottom_color);
        draw_port_male(s, static_cast<int16_t>(x + 6), static_cast<int16_t>(y + g.bottom_port_y), spec.top_color, kSurface);
        return;
    }
    fill_round(s, x, y, kFlowW, h, 6, kSurface);
    StrBuf<12> top, bottom;
    top.put("TOP ").put(spec.top_role); bottom.put("BOT ").put(spec.bottom_role);
    gfx_text(s, kLabelFont, static_cast<int16_t>(x + 4), static_cast<int16_t>(y + 2),
             top.view(), spec.top_color, kSurface);
    gfx_text(s, kLabelFont, static_cast<int16_t>(x + 4), static_cast<int16_t>(y + h - 15),
             bottom.view(), spec.bottom_color, kSurface);
    const int16_t port_y = static_cast<int16_t>(y + h / 2 - 6);
    const int16_t male_x = static_cast<int16_t>(x + (rotation == 1 ? 4 : 42));
    const int16_t female_x = static_cast<int16_t>(x + (rotation == 1 ? 42 : 4));
    fill_round(s, male_x, port_y, 8, 12, 2, spec.top_color);
    gfx_fill(s, static_cast<int16_t>(male_x + 3), static_cast<int16_t>(port_y + 3), 2, 6, kSurface);
    frame_round(s, female_x, port_y, 8, 12, 2, 1, spec.bottom_color);
    gfx_fill(s, static_cast<int16_t>(female_x + 3), static_cast<int16_t>(port_y + 3), 2, 6, spec.bottom_color);
    if (spec.flow != Arrow::Idle)
        draw_physical_chevron(s, static_cast<int16_t>(x + 27), static_cast<int16_t>(port_y + 1),
                              spec.flow == Arrow::Up, rotation, kFlow);
    else gfx_fill(s, static_cast<int16_t>(x + 16), static_cast<int16_t>(port_y + 5), 22, 2, kFaint);
    if (!spec.caption.empty())
        gfx_text(s, kLabelFont, static_cast<int16_t>(x + 4), static_cast<int16_t>(port_y + 14),
                 spec.caption, spec.caption_color, kSurface);
}

}  // namespace k2
