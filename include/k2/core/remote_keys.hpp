// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The strip as a wire value (GET_STATE part 3): the panel's labels (Button order Back Minus Ok Plus) cut to the wire's
// 7 character words. Pure over a PanelLabels, so the host tests run it over every page and menu.
#pragma once
#include <cstddef>
#include "k2/core/remote_proto.hpp"
#include "k2/ui/ui_labels.hpp"

namespace k2::remote {

// `hidden` = the page covers the strip (Page::hides_strip), so none of the four labels is drawn.
inline KeysInfo keys_from_labels(const PanelLabels &p, bool hidden) {
    KeysInfo k;
    auto copy = [](char (&dst)[kKeyChars + 1], const char *src) {
        size_t n = 0;
        for (; src[n] && n < kKeyChars; n++) dst[n] = src[n];
        dst[n] = 0;
    };
    for (size_t i = 0; i < 4; i++) {
        copy(k.k[i].shrt, p.b[i].shrt);
        copy(k.k[i].lng, p.b[i].lng);
        if (p.b[i].dim) k.flags = static_cast<uint8_t>(k.flags | (1u << i));
    }
    if (hidden) k.flags = static_cast<uint8_t>(k.flags | 0x10u);
    return k;
}

}  // namespace k2::remote
