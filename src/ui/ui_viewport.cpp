// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#include "k2/ui/ui_viewport.hpp"
#include <algorithm>

namespace k2 {
namespace {
ContentViewport* active;
uint16_t g_line[240];   // the retained output row, shared: only `active` writes it
constexpr int kLogicalW = 224, kLogicalH = 240;
int mapped(int coordinate, int extent, int logical) { return coordinate * extent / logical; }
}

Rect ContentViewport::native_rect(Rect source, int cell_offset) {
    if (!active || lcd::get_sink() == active->previous_) {
        source.x = static_cast<int16_t>(source.x + cell_offset);
        return source;
    }
    const Rect &d = active->destination_;
    const int x = d.x + mapped(source.x, d.w, kLogicalW) + cell_offset;
    const int y = d.y + mapped(source.y, d.h, kLogicalH);
    return {static_cast<int16_t>(x), static_cast<int16_t>(y),
            static_cast<int16_t>(std::min<int>(source.w, d.x + d.w - x)),
            static_cast<int16_t>(std::min<int>(source.h, d.y + d.h - y))};
}

bool ContentViewport::stretched() { return active && lcd::get_sink() != active->previous_; }

Rect ContentViewport::mapped_rect(Rect source) {
    if (!active || lcd::get_sink() == active->previous_) return source;
    const Rect &d = active->destination_;
    const int x = mapped(source.x, d.w, kLogicalW), y = mapped(source.y, d.h, kLogicalH);
    return {static_cast<int16_t>(d.x + x), static_cast<int16_t>(d.y + y),
            static_cast<int16_t>(mapped(source.x + source.w, d.w, kLogicalW) - x),
            static_cast<int16_t>(mapped(source.y + source.h, d.h, kLogicalH) - y)};
}

ContentViewport::ContentViewport(Rect destination) : destination_(destination) {
    if (active || (destination.x == 0 && destination.y == 0 &&
                   destination.w == kLogicalW && destination.h == kLogicalH)) return;
    static constexpr lcd::Sink sink{begin, pixels, fill};
    owns_ = true;
    previous_ = lcd::get_sink();
    active = this;
    lcd::set_sink(&sink);
}

ContentViewport::~ContentViewport() {
    if (!owns_) return;
    lcd::set_sink(previous_);
    active = nullptr;
}

ContentViewport::Pause::Pause() : saved_(lcd::get_sink()) {
    if (active) lcd::set_sink(active->previous_);
}
ContentViewport::Pause::~Pause() { lcd::set_sink(saved_); }

void ContentViewport::begin(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t) {
    active->x0_ = x0;
    active->y0_ = y0;
    active->width_ = static_cast<uint16_t>(x1 - x0 + 1);
    active->column_ = active->row_ = 0;
}
void ContentViewport::pixels(const uint16_t* px, uint32_t count) {
    for (uint32_t i = 0; i < count; ++i) active->push(px[i]);
}
void ContentViewport::fill(uint16_t color, uint32_t count) {
    for (uint32_t i = 0; i < count; ++i) active->push(color);
}
void ContentViewport::push(uint16_t color) {
    const int x = x0_ + column_;
    const int start = mapped(x, destination_.w, kLogicalW);
    const int end = mapped(x + 1, destination_.w, kLogicalW);
    for (int i = start; i < end && i < 240; ++i) g_line[i] = color;
    if (++column_ == width_) {
        flush();
        column_ = 0;
        ++row_;
    }
}
void ContentViewport::flush() {
    const int x0 = mapped(x0_, destination_.w, kLogicalW);
    const int x1 = mapped(x0_ + width_, destination_.w, kLogicalW);
    const int y0 = mapped(y0_ + row_, destination_.h, kLogicalH);
    const int y1 = mapped(y0_ + row_ + 1, destination_.h, kLogicalH);
    if (x1 <= x0 || y1 <= y0) return;
    const lcd::Sink* saved = lcd::get_sink();
    lcd::set_sink(previous_);
    for (int y = y0; y < y1; ++y) {
        lcd::stream_begin(static_cast<uint16_t>(destination_.x + x0),
                          static_cast<uint16_t>(destination_.y + y),
                          static_cast<uint16_t>(destination_.x + x1 - 1),
                          static_cast<uint16_t>(destination_.y + y));
        lcd::stream_pixels({g_line + x0, static_cast<size_t>(x1 - x0)});
        lcd::stream_end();
    }
    lcd::set_sink(saved);
}

}  // namespace k2
