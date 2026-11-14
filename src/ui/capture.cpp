// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#include "k2/ui/capture.hpp"
#if K2_SCREENSHOT
#include <algorithm>
#include <array>
#include "k2/core/hot.h"
#include "k2/core/log.hpp"
#include "k2/core/remote_proto.hpp"
#include "k2/core/scratch.hpp"
#include "k2/drivers/lcd.hpp"
#include "k2/drivers/systick.hpp"
#include "k2/drivers/usb.hpp"
#include "k2/ui/pixel_sink.hpp"
#include "k2/ui/rle565.hpp"
#include "k2/ui/ui_layout.hpp"

namespace k2 {

namespace {

constexpr int kStripRows = kScreenStripRows;
static_assert(kScreenH % kStripRows == 0, "strips must tile the screen");
constexpr uint32_t kSendTimeoutMs = 500;     // host stopped reading the IN endpoint
constexpr uint8_t kMinFree = 3;              // queue slots left over for log lines
constexpr size_t kStripBytes = static_cast<size_t>(kScreenW) * kStripRows * sizeof(uint16_t);
static_assert(kStripBytes + remote::kShotBulkHeader + remote::kShotBulkChunk <= scratch::kSize,
              "6-row capture strip and RLE chunk must fit the scratch arena");

PixelSink *g_sink_ptr = nullptr;             // non-null while a capture owns the LCD sink

#if K2_NET || !defined(__arm__)
constexpr int kHttpRows = 4;
constexpr int kHttpStrips = kScreenH / kHttpRows;
uint16_t g_http_strip[kScreenW * kHttpRows];
PixelSink g_http_sink;
static_assert(kCaptureStrips == kHttpStrips && kCaptureStripPixels == kScreenW * kHttpRows,
              "capture.hpp HTTP geometry");
#endif

K2_HOT void sink_begin(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1) { g_sink_ptr->window(x0, y0, x1, y1); }
K2_HOT void sink_pixels(const uint16_t *px, uint32_t n) { g_sink_ptr->push(px, n); }
K2_HOT void sink_fill(uint16_t c, uint32_t n) { g_sink_ptr->fill(c, n); }
K2_HOT void sink_band(uint16_t *first, uint16_t *last) {   // drawing outside these rows is discarded: the primitives skip it
    *first = static_cast<uint16_t>(g_sink_ptr->first_row());
    *last = static_cast<uint16_t>(g_sink_ptr->last_row());
}
const lcd::Sink kSink = {sink_begin, sink_pixels, sink_fill, sink_band};

// Queues one 64-byte stream frame on HID or CDC (legacy kShotViaCdc mode); false after the timeout.
// lcd::pump() keeps PD, UFCS and transcap serviced while waiting on USB TX headroom.
K2_HOT bool send_frame(bool cdc, uint8_t seq, uint8_t strip, uint8_t flags, const uint8_t *data, size_t len) {
    const uint32_t t0 = systick::millis();
    if (cdc) {
        while (usb::cdc_free() < remote::kFrameSize) {
            usb::cdc_flush();
            lcd::pump();
            if (systick::millis() - t0 > kSendTimeoutMs) return false;
        }
    } else {
        while (usb::tx_free() < kMinFree) {
            lcd::pump();
            if (systick::millis() - t0 > kSendTimeoutMs) return false;
        }
    }
    std::array<uint8_t, remote::kFrameSize> frame;
    remote::build_stream_frame(frame, seq, strip, flags, std::span<const uint8_t>(data, len));
    if (cdc) {
        usb::cdc_write(frame.data(), static_cast<uint32_t>(frame.size()));
        usb::cdc_flush();
    } else {
        usb::send_report(frame.data());
    }
    return true;
}

// Writes one compact bulk chunk (8-byte header + RLE bytes) over CDC without frame padding, flushing when the TX FIFO fills
// and once after queueing the chunk so the USB ISR drains the tail asynchronously during the next strip's render.
K2_HOT bool cdc_write_all(const uint8_t *buf, size_t len) {
    const uint32_t t0 = systick::millis();
    size_t off = 0;
    while (off < len) {
        const uint32_t avail = usb::cdc_free();
        if (avail == 0) {
            usb::cdc_flush();
            lcd::pump();
            if (systick::millis() - t0 > kSendTimeoutMs) return false;
            continue;
        }
        const size_t n = std::min<size_t>(len - off, avail);
        const uint32_t wrote = usb::cdc_write(buf + off, static_cast<uint32_t>(n));
        if (wrote == 0) {
            usb::cdc_flush();
            lcd::pump();
            if (systick::millis() - t0 > kSendTimeoutMs) return false;
            continue;
        }
        off += wrote;
        if (usb::cdc_free() == 0) usb::cdc_flush();
    }
    usb::cdc_flush();
    return true;
}

K2_HOT bool send_strip(uint8_t transport, uint8_t seq, uint8_t strip, const uint16_t *px, size_t n_px,
                       uint8_t *work, size_t work_cap, uint32_t &total) {
    Rle565Encoder enc;
    enc.reset(px, n_px);
    if (transport == remote::kShotViaCdcBulk) {
        uint8_t *const rle = work + remote::kShotBulkHeader;
        const size_t rle_cap = std::min(work_cap - remote::kShotBulkHeader, remote::kShotBulkChunk);
        while (!enc.done()) {
            const size_t n = enc.next(rle, rle_cap);
            const uint8_t flags = enc.done() ? remote::kShotFlagLast : 0;
            remote::build_bulk_header({work, remote::kShotBulkHeader}, seq, strip, flags, static_cast<uint16_t>(n));
            if (!cdc_write_all(work, remote::kShotBulkHeader + n)) return false;
            total += static_cast<uint32_t>(n);
        }
        return true;
    }
    const bool cdc = transport == remote::kShotViaCdc;
    while (!enc.done()) {
        const size_t n = enc.next(work, remote::kShotChunk);
        if (!send_frame(cdc, seq, strip, enc.done() ? remote::kShotFlagLast : 0, work, n)) return false;
        total += static_cast<uint32_t>(n);
    }
    return true;
}

void send_abort(uint8_t transport, uint8_t seq, uint8_t *work) {
    if (transport == remote::kShotViaCdcBulk) {
        remote::build_bulk_header({work, remote::kShotBulkHeader}, seq, 0, remote::kShotFlagAbort, 0);
        cdc_write_all(work, remote::kShotBulkHeader);
    } else {
        send_frame(transport == remote::kShotViaCdc, seq, 0, remote::kShotFlagAbort, work, 0);
    }
}

K2_HOT bool run_strips(Ui &ui, PixelSink &sink, uint16_t *strip_buf, uint8_t *work, size_t work_cap,
                       int strip_rows, uint8_t seq, uint32_t now_ms, uint8_t transport, uint32_t &total) {
    const int strips = kScreenH / strip_rows;
    bool ok = true;
    for (int strip = 0; strip < strips && ok; strip++) {
        lcd::pump();   // between strips: the sink render never reaches a stream_service, so serve PD and USB here
        sink.attach(strip_buf, kScreenW, strip * strip_rows, strip_rows);
        lcd::set_sink(&kSink);
        ui.render_for_capture(now_ms);
        lcd::set_sink(nullptr);
        ok = send_strip(transport, seq, static_cast<uint8_t>(strip), strip_buf,
                        static_cast<size_t>(kScreenW) * static_cast<size_t>(strip_rows), work, work_cap, total);
    }
    if (!ok) send_abort(transport, seq, work);
    return ok;
}

K2_HOT bool capture_screen_scratch(Ui &ui, PixelSink &sink, uint8_t seq, uint32_t now_ms, uint8_t transport, uint32_t &total) {
    scratch::Lease lease("capture");
    auto *const strip_buf = reinterpret_cast<uint16_t *>(scratch::arena());
    uint8_t *const work = scratch::arena() + kStripBytes;
    return run_strips(ui, sink, strip_buf, work, scratch::kSize - kStripBytes, kStripRows, seq, now_ms, transport, total);
}

// Pages that lease scratch::arena() during paint() fall back to 1-row stack strips (240 * 2 + 128 B < stack limit).
[[gnu::noinline]] bool capture_screen_small(Ui &ui, PixelSink &sink, uint8_t seq, uint32_t now_ms, uint8_t transport,
                                            uint32_t &total) {
    uint16_t strip_buf[kScreenW];
    uint8_t work[128];
    static_assert(sizeof(work) - remote::kShotBulkHeader >= 55, "chunk fits a worst-case RLE565 token");
    return run_strips(ui, sink, strip_buf, work, sizeof(work), 1, seq, now_ms, transport, total);
}

}  // namespace

bool capture_busy() { return g_sink_ptr != nullptr; }

#if K2_NET || !defined(__arm__)
bool capture_begin(Ui &ui) {
    if (g_sink_ptr != nullptr) return false;
    g_sink_ptr = &g_http_sink;
    ui.sample();
    return true;
}

const uint16_t *capture_strip(Ui &ui, int strip, uint32_t now_ms) {
    lcd::pump();   // the idle hook (PD, UFCS, transcap) runs between strips: the render itself never reaches a stream_service
    g_http_sink.attach(g_http_strip, kScreenW, strip * kHttpRows, kHttpRows);
    lcd::set_sink(&kSink);
    ui.render_for_capture(now_ms);
    lcd::set_sink(nullptr);
    return g_http_strip;
}

void capture_end(Ui &ui, uint32_t now_ms) {
    ui.resync(now_ms);
    g_sink_ptr = nullptr;
}
#else
bool capture_begin(Ui &) { return false; }
const uint16_t *capture_strip(Ui &, int, uint32_t) { return nullptr; }
void capture_end(Ui &, uint32_t) {}
#endif

bool capture_screen(Ui &ui, uint8_t seq, uint32_t now_ms, uint8_t transport) {
    if (g_sink_ptr != nullptr) return false;
    PixelSink sink;
    g_sink_ptr = &sink;
    ui.sample();
    const bool cdc = transport != remote::kShotViaHid;
    if (cdc) usb::set_cdc_hold(true);   // no shell text inside the stream
    const uint32_t t0 = systick::millis();
    uint32_t total = 0;
    const bool ok = ui.capture_needs_scratch()
        ? capture_screen_small(ui, sink, seq, now_ms, transport, total)
        : capture_screen_scratch(ui, sink, seq, now_ms, transport, total);
    if (cdc) usb::set_cdc_hold(false);
    ui.resync(now_ms);
    g_sink_ptr = nullptr;
    K2LOG("[RMT] screenshot %s: %d bytes in %d ms\n", ok ? "done" : "aborted",
                static_cast<int>(total), static_cast<int>(systick::millis() - t0));
    return ok;
}

}  // namespace k2
#endif  // K2_SCREENSHOT
