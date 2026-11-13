// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#include "k2/ui/capture.hpp"
#if K2_SCREENSHOT
#include "k2/core/log.hpp"
#include "k2/drivers/lcd.hpp"
#include "k2/drivers/systick.hpp"
#include "k2/drivers/usb.hpp"
#include "k2/ui/pixel_sink.hpp"
#include "k2/core/remote_proto.hpp"
#include "k2/ui/rle565.hpp"
#include "k2/ui/ui_layout.hpp"

namespace k2 {

namespace {

constexpr int kStripRows = 4;
static_assert(kScreenH % kStripRows == 0, "strips must tile the screen");
constexpr int kStrips = kScreenH / kStripRows;
constexpr uint32_t kSendTimeoutMs = 500;     // host stopped reading the IN endpoint
constexpr uint8_t kMinFree = 3;              // queue slots left over for log lines

uint16_t g_strip[kScreenW * kStripRows];     // 1920 bytes, the whole cost of the feature
PixelSink g_sink;
bool g_busy = false;                            // a capture owns g_strip

void sink_begin(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1) { g_sink.window(x0, y0, x1, y1); }
void sink_pixels(const uint16_t *px, uint32_t n) { g_sink.push(px, n); }
void sink_fill(uint16_t c, uint32_t n) { g_sink.fill(c, n); }
void sink_band(uint16_t *first, uint16_t *last) {   // drawing outside these rows is discarded: the primitives skip it
    *first = static_cast<uint16_t>(g_sink.first_row());
    *last = static_cast<uint16_t>(g_sink.last_row());
}
const lcd::Sink kSink = {sink_begin, sink_pixels, sink_fill, sink_band};

static_assert(remote::ScreenInfo{}.strip_rows == kStripRows && remote::ScreenInfo{}.num_strips == kStrips,
              "ScreenInfo defaults must match the capture geometry");

// Queues one stream frame once the IN queue has room; false after the timeout. The wait serves the idle hook as well as
// USB (lcd::pump), so PD, UFCS and the transition capture keep their ~1 ms service while the host drains the endpoint.
bool send_frame(uint8_t seq, uint8_t strip, uint8_t flags, const uint8_t *data, size_t len) {
    const uint32_t t0 = systick::millis();
    while (usb::tx_free() < kMinFree) {
        lcd::pump();
        if (systick::millis() - t0 > kSendTimeoutMs) return false;
    }
    std::array<uint8_t, remote::kFrameSize> frame;
    remote::build_stream_frame(frame, seq, strip, flags, std::span<const uint8_t>(data, len));
    usb::send_report(frame.data());
    return true;
}

}  // namespace

static_assert(kCaptureStrips == kStrips && kCaptureStripPixels == kScreenW * kStripRows, "capture.hpp geometry");

bool capture_begin(Ui &ui) {
    if (g_busy) return false;
    g_busy = true;
    ui.sample();
    return true;
}

const uint16_t *capture_strip(Ui &ui, int strip, uint32_t now_ms) {
    lcd::pump();   // the idle hook (PD, UFCS, transcap) runs between strips: the render itself never reaches a stream_service
    g_sink.attach(g_strip, kScreenW, strip * kStripRows, kStripRows);
    lcd::set_sink(&kSink);
    ui.render_for_capture(now_ms);
    lcd::set_sink(nullptr);
    return g_strip;
}

void capture_end(Ui &ui, uint32_t now_ms) {
    ui.resync(now_ms);
    g_busy = false;
}

bool capture_screen(Ui &ui, uint8_t seq, uint32_t now_ms) {
    if (!capture_begin(ui)) return false;
    const uint32_t t0 = systick::millis();
    uint32_t total = 0;
    bool ok = true;
    Rle565Encoder enc;
    uint8_t chunk[remote::kShotChunk];

    for (int strip = 0; strip < kStrips && ok; strip++) {
        lcd::pump();   // between strips: the sink render never reaches a stream_service, so serve PD and USB here
        g_sink.attach(g_strip, kScreenW, strip * kStripRows, kStripRows);
        lcd::set_sink(&kSink);
        ui.render_for_capture(now_ms);
        lcd::set_sink(nullptr);

        enc.reset(g_strip, static_cast<size_t>(kScreenW) * kStripRows);
        while (!enc.done()) {
            const size_t n = enc.next(chunk, sizeof chunk);
            ok = send_frame(seq, static_cast<uint8_t>(strip), enc.done() ? remote::kShotFlagLast : 0, chunk, n);
            if (!ok) break;
            total += static_cast<uint32_t>(n);
        }
    }
    if (!ok) send_frame(seq, 0, remote::kShotFlagAbort, chunk, 0);

    capture_end(ui, now_ms);
    K2LOG("[RMT] screenshot %s: %d bytes in %d ms\n", ok ? "done" : "aborted",
                static_cast<int>(total), static_cast<int>(systick::millis() - t0));
    return ok;
}

}  // namespace k2
#endif  // K2_SCREENSHOT
