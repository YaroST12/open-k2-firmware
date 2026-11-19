// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Internal helpers shared between hot ui_widgets.cpp and cold ui_widgets_cold.cpp.
#pragma once
#include "k2/ui/ui_widgets.hpp"
#include "k2/ui/ui_style.hpp"

namespace k2::detail {

constexpr int kMaxW = kScreenW;
constexpr int kReadoutX = 140;   // the top bar's right-hand readout area starts here (draw_top_readout)

enum class BandKind : uint8_t { Footer, StatusLeft, Readout, Pager };
struct BandOp {
    BandKind kind = BandKind::Footer;
    FooterStyle fst{};
    const Font *font = nullptr;
    std::string_view s0{}, s1{}, s2{};
    int16_t fy = 0, tx = 0, ty = 0, sep = 0;
    bool flag = false;
    Icon icon = Icon::Bolt;
    uint16_t badge_color = kInk2;
    StatusReadout mode = StatusReadout::None;
    Arrow flow = Arrow::Idle;
    uint8_t rotation = 0;
    int count = 0, current = 0;
};

// The part of a band that goes to the panel, in band pixels from its top-left (columns c0..c1-1, rows r0..r1-1).
struct BandClip {
    int c0 = 0, c1 = kScreenW, r0 = 0, r1 = kScreenW;
};

bool fits(const Surface &s, int x, int y, int w, int h);
bool clip_box_to_band(const Surface &s, int x, int y, int w, int h, int &r0, int &r1);
void draw_band(const Surface &s, int x, int y, int h, const BandOp &op, int max_w = kScreenW, BandClip clip = {});
void stream_mono(const Surface &s, int16_t x, int16_t y, int sz, int src,
                 const uint16_t *rows, int msb, uint16_t fg, uint16_t bg);

}  // namespace k2::detail
