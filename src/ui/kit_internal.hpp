// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Internal helpers shared between cold kit.cpp and hot kit_hot.cpp.
#pragma once
#include "k2/ui/kit.hpp"
#include "k2/ui/text.hpp"

namespace k2::kit::detail {

inline const Font &font_or_small(const Font *f) { return f ? *f : kSmall; }

struct LabelLayout {
    const Font *font;
    std::string_view text;
    Box hole;
};

LabelLayout label_layout(int16_t x, int16_t y, int16_t w, int16_t h, std::string_view label, char (&buf)[kFitBuf]);
Box strike_line(const Font &f, int16_t tx, int16_t ty, int16_t tw, int16_t cx, int16_t cw);

}  // namespace k2::kit::detail
