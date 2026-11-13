// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// ST7789V driver, see k2/drivers/lcd.hpp. Commands and their arguments are polled 8-bit SPI3 writes;
// the pixel stream switches SPI3 to 16-bit data frames (one store per pixel). The panel
// wiring is in k2/drivers/pins.hpp.
#include "k2/drivers/lcd.hpp"
#include "k2/drivers/lcd_timing.hpp"
#include "k2/core/hot.h"
#include "k2/core/log.hpp"
#include "k2/core/paint_meter.hpp"

#include <algorithm>
#include <cstddef>

#include "k2/drivers/pins.hpp"
#include "k2/drivers/regs.hpp"
#include "k2/drivers/systick.hpp"
#include "k2/drivers/usb.hpp"

namespace k2::lcd {

namespace {

using namespace reg;
namespace p = pins;

// ST7789 commands used here.
enum class Cmd : std::uint8_t {
    SLPIN = 0x10, SLPOUT = 0x11, DISPOFF = 0x28, NORON = 0x13, INVON = 0x21, DISPON = 0x29,
    CASET = 0x2A, RASET = 0x2B, RAMWR = 0x2C, MADCTL = 0x36, COLMOD = 0x3A,
};

constexpr std::uint8_t kMadctl[4] = {0x00, 0x60, 0xC0, 0xA0};
constexpr std::uint16_t kRamOffset = 80;  // 240x240 window inside the 240x320 controller RAM
// SPI3 clock divider off APB1 (120 MHz): kBootSpiDiv (lcd_timing.hpp, DIV4 = 30 MHz, panel checked by the user
// 2026-11-12) is the boot value and ships; DIV8 (15 MHz) and DIV2 (60 MHz, near the ST7789 write-clock limit, visual
// check only) are lab values (set_spi_div, not persisted). The service cadence during a paint is elapsed time
// (lcd_timing.hpp kServiceUs), so it does not depend on the divider.
constexpr std::uint16_t ctrl1_for(SpiDiv div) { return (Spi::CTRL1_LCD & ~Spi::CTRL1_BR_MASK) | br_field(div); }
SpiDiv g_div = kBootSpiDiv;
std::uint16_t g_ctrl1 = ctrl1_for(kBootSpiDiv);
bool g_stream_open;   // a pixel stream owns CS: the divider must not change
static_assert(br_field(SpiDiv::Div2) == Spi::CTRL1_BR_DIV2 && br_field(SpiDiv::Div4) == Spi::CTRL1_BR_DIV4 &&
              br_field(SpiDiv::Div8) == Spi::CTRL1_BR_DIV8, "divider table matches the register bits");
static_assert(ctrl1_for(SpiDiv::Div2) == 0xC347, "CTRL1 /2 encoding");
static_assert(ctrl1_for(SpiDiv::Div4) == 0xC34F, "CTRL1 /4 encoding");
static_assert(ctrl1_for(SpiDiv::Div8) == 0xC357, "CTRL1 /8 encoding");
static_assert((ctrl1_for(SpiDiv::Div2) | Spi::CTRL1_DSIZE) == 0xCB47, "CTRL1 16-bit /2 encoding");
static_assert((ctrl1_for(SpiDiv::Div4) | Spi::CTRL1_DSIZE) == 0xCB4F, "CTRL1 16-bit /4 encoding");
static_assert((ctrl1_for(SpiDiv::Div8) | Spi::CTRL1_DSIZE) == 0xCB57, "CTRL1 16-bit /8 encoding");

// DMA streaming for the pixel stream (SPI3 TX request). Build with -DK2_LCD_DMA=0 for the
// polled path only (the fallback if the channel mapping misbehaves on a board).
#ifndef K2_LCD_DMA
#define K2_LCD_DMA 1
#endif
constexpr bool kLcdDma = K2_LCD_DMA != 0;
// A transfer whose count does not move for this long (SPI3 stuck, wrong request mapping) is
// abandoned: the remainder goes out polled and DMA stays off. One pixel takes 1.07 us at DIV8.
constexpr std::uint32_t kDmaStallMs = 3;

// DMA2 channel 2 carries the SPI3 TX request (inferred from the STM32F1-style layout, fixed
// mapping, no flexible-request setup). The block layout is the same as the one in adc.cpp.
struct DmaChannel { R32 CTRL, DTCNT, PADDR, MADDR, reserved; };
struct DmaRegs {
    R32 STS, CLR;
    DmaChannel ch[7];
    static constexpr std::uint32_t CTRL_EN = 1u << 0, CTRL_DIR_MEM2PER = 1u << 4, CTRL_MINC = 1u << 7,
                                   CTRL_PSIZE16 = 1u << 8, CTRL_MSIZE16 = 1u << 10, CTRL_PRIO_HIGH = 2u << 12;
};
static_assert(offsetof(DmaRegs, ch) == 0x08);
inline DmaRegs& dma2() { return at<DmaRegs>(0x40020400); }
constexpr std::uint32_t kAhbenDma2 = 1u << 1;
constexpr unsigned kDmaCh = 1;                              // channel 2 (index from 0)
constexpr std::uint32_t kDmaClearCh = 0xFu << (4 * kDmaCh);  // GIF/TCIF/HTIF/TEIF of that channel

std::uint16_t g_x_off, g_y_off;
const Sink* g_sink;  // non-null while a screenshot is being rendered

// Command path: the data register must be written as a byte or the SPI would send a 16-bit
// frame (DSIZE is 0 outside a pixel stream). The pixel path uses spi_write16 instead.
// Bounded waits: one frame takes ~1.1..2.1 us (~256..512 CPU cycles); kSpiSpins polls are well
// over 100 us. A stuck SPI3 (clock off, peripheral wedged) drops the frame and logs once instead
// of hanging the main loop with no debugger to see it.
constexpr std::uint32_t kSpiSpins = 20000;
std::uint32_t g_spi_timeouts;

K2_HOT void spi_timeout(const char* where) {
    if (g_spi_timeouts++ == 0) K2LOG("[LCD] FAULT SPI3 %s timeout STS=%04x\n", where, unsigned(spi3().STS));
}
K2_HOT void spi_write8(std::uint8_t byte) {
    Spi& spi = spi3();
    for (std::uint32_t n = kSpiSpins; !(spi.STS & Spi::STS_TDBE);)
        if (--n == 0) { spi_timeout("TDBE"); return; }
    *reinterpret_cast<volatile std::uint8_t*>(&spi.DT) = byte;
}
// 16-bit data frame (DSIZE=1): one store shifts out both bytes of a pixel, MSB first -- the
// same wire order as the old two 8-bit writes, with half the TDBE spin-waits and bus frames.
K2_HOT void spi_write16(std::uint16_t word) {
    Spi& spi = spi3();
    for (std::uint32_t n = kSpiSpins; !(spi.STS & Spi::STS_TDBE);)
        if (--n == 0) { spi_timeout("TDBE"); return; }
    *reinterpret_cast<volatile std::uint16_t*>(&spi.DT) = word;
}
K2_HOT void spi_wait_idle() {
    for (std::uint32_t n = kSpiSpins; spi3().STS & Spi::STS_BF;)
        if (--n == 0) { spi_timeout("BF"); return; }
}

// ST7789 command frames must stay 8-bit, so a pixel stream flips DSIZE on for its duration and
// back off afterwards. DSIZE is only changed while the bus is idle (BF clear, CS high) and with
// SPE cleared, as the AT32F403A requires; the switch happens once per stream, never per pixel.
K2_HOT void spi_frame_16bit(bool on) {
    Spi& spi = spi3();
    spi_wait_idle();
    spi.CTRL1 = g_ctrl1 & ~Spi::CTRL1_SPIEN;  // SPE=0 while DSIZE changes
    spi.CTRL1 = on ? (g_ctrl1 | Spi::CTRL1_DSIZE) : g_ctrl1;
}

void dma_wait();     // below: finishes an in-flight pixel transfer, serving PD/USB meanwhile
void close_frame();  // below: dma_wait, then ends a pixel stream's frame left open by stream_end (CS high, 8-bit frames)

// One chip-select frame: DC low for a command byte, then (separately framed) DC high data.
K2_HOT void command(Cmd cmd) {
    close_frame();
    p::lcd_dc.clear();
    p::lcd_cs.clear();
    spi_write8(static_cast<std::uint8_t>(cmd));
    spi_wait_idle();
    p::lcd_cs.set();
}
K2_HOT void data(std::span<const std::uint8_t> bytes) {
    close_frame();
    p::lcd_dc.set();
    p::lcd_cs.clear();
    for (std::uint8_t b : bytes) spi_write8(b);
    spi_wait_idle();
    p::lcd_cs.set();
}
K2_HOT void command(Cmd cmd, std::span<const std::uint8_t> args) {
    command(cmd);
    data(args);
}
K2_HOT void command(Cmd cmd, std::uint8_t arg) { command(cmd, {&arg, 1}); }

// CASET / RASET take big-endian start and end, RAMWR then opens the pixel stream. All three go
// out under ONE chip-select assertion (CS low once) instead of three frames with a CS toggle +
// idle wait each; CS stays low and DC ends high so the pixel stream continues the same frame.
K2_HOT void set_window(std::uint16_t x0, std::uint16_t y0, std::uint16_t x1, std::uint16_t y1) {
    x0 += g_x_off; x1 += g_x_off;
    y0 += g_y_off; y1 += g_y_off;
    const std::uint8_t cols[4] = {std::uint8_t(x0 >> 8), std::uint8_t(x0), std::uint8_t(x1 >> 8), std::uint8_t(x1)};
    const std::uint8_t rows[4] = {std::uint8_t(y0 >> 8), std::uint8_t(y0), std::uint8_t(y1 >> 8), std::uint8_t(y1)};
    close_frame();
    p::lcd_cs.clear();
    p::lcd_dc.clear();
    spi_write8(static_cast<std::uint8_t>(Cmd::CASET));
    spi_wait_idle();
    p::lcd_dc.set();
    for (std::uint8_t b : cols) spi_write8(b);
    spi_wait_idle();
    p::lcd_dc.clear();
    spi_write8(static_cast<std::uint8_t>(Cmd::RASET));
    spi_wait_idle();
    p::lcd_dc.set();
    for (std::uint8_t b : rows) spi_write8(b);
    spi_wait_idle();
    p::lcd_dc.clear();
    spi_write8(static_cast<std::uint8_t>(Cmd::RAMWR));
    spi_wait_idle();
    p::lcd_dc.set();  // CS left low: the bytes that follow are the pixel stream
}

void (*g_idle_hook)() = nullptr;  // set_idle_hook(): time-critical work during long draws

// The service cadence (lcd_timing.hpp kServiceUs, docs/ARCH_REWORK.md E1) on the core-cycle clock. systick::ticks() is
// SysTick VAL plus the millisecond count, so it is correct with IRQs enabled (the pending bit covers a wrap not yet counted).
constexpr std::uint32_t kServiceTicks = kServiceUs * (systick::kSysclkHz / 1'000'000u);
ServicePacer g_pacer{kServiceTicks};

K2_HOT void run_round() {
    usb::service();
    if (g_idle_hook) g_idle_hook();
    g_pacer.finish(systick::ticks());
}

// One usb::service() + idle-hook round when kServiceUs has passed since the last one ended. Never nested: a stream call
// reached from inside the round (the hook, usb::service) finds the round open and returns.
K2_HOT void stream_service() {
    if (g_pacer.start(systick::ticks())) run_round();
}

K2_HOT void push_pixel(std::uint16_t c) { spi_write16(c); }

// ---- DMA pixel transfer ----------------------------------------------------------------------
// One transfer is in flight at most, and every one is left running when the stream call returns, so the caller's CPU work
// (rendering the next run of glyph pixels, laying out the next box) overlaps the wire time:
//  - stream_fill sends the static g_fill_word;
//  - stream_pixels copies the caller's run (it may live on the stack) into one of two driver buffers (ping-pong): the copy
//    goes into the buffer that is NOT on the wire, then the call waits for the previous transfer and starts its own;
//  - stream_begin only records the window; the first pixels or fill of the stream send CASET / RASET / RAMWR (open_window),
//    so the window's own setup and the caller's first run are composed while the previous stream is still on the wire;
//  - stream_end leaves the frame open (CS low, 16-bit frames, the last transfer running); whatever touches SPI3, DC or CS
//    next closes it first (close_frame: dma_wait, BF clear, CS high, 8-bit frames).
// dma_wait() is where usb::service() and the idle hook keep running while the CPU has nothing else to do.
constexpr std::uint32_t kPingPx = 256;   // a text chunk is 80 px, a band row at most 240: one copy per call
std::uint16_t g_ping[2][kPingPx];
unsigned g_ping_next;                    // the buffer the next run is copied into (the other one may be on the wire)
bool g_win_pending;                      // stream_begin recorded a window that is not on the wire yet
std::uint16_t g_win[4];
bool g_frame_open;                       // a pixel stream's frame is still open on the wire (stream_end left it)
volatile std::uint16_t g_fill_word;
const std::uint16_t* g_dma_src;   // nullptr: fixed source word (fill)
std::uint32_t g_dma_n;            // units in the in-flight transfer
bool g_dma_busy;
bool g_dma_ok = kLcdDma;          // cleared for good by a stalled transfer
bool g_dma_proven;                // first completed transfer logged
std::uint32_t g_dma_done, g_dma_stalls;   // completed / abandoned transfers since boot (dma_info)

K2_HOT void dma_start(const std::uint16_t* src, std::uint32_t n) {
    DmaRegs& d = dma2();
    DmaChannel& ch = d.ch[kDmaCh];
    ch.CTRL = 0;
    d.CLR = kDmaClearCh;
    ch.PADDR = reinterpret_cast<std::uintptr_t>(&spi3().DT);
    ch.MADDR = src ? reinterpret_cast<std::uintptr_t>(src) : reinterpret_cast<std::uintptr_t>(&g_fill_word);
    ch.DTCNT = n;
    spi3().CTRL2 = spi3().CTRL2 | Spi::CTRL2_TXDMAEN;
    __asm volatile("dsb" ::: "memory");  // source words are in RAM before the channel reads them
    ch.CTRL = DmaRegs::CTRL_EN | DmaRegs::CTRL_DIR_MEM2PER | DmaRegs::CTRL_PSIZE16 | DmaRegs::CTRL_MSIZE16 |
              DmaRegs::CTRL_PRIO_HIGH | (src ? DmaRegs::CTRL_MINC : 0u);
    g_dma_src = src;
    g_dma_n = n;
    g_dma_busy = true;
}

// A stalled channel: stop it, send what is left polled, and keep DMA off from now on.
K2_HOT void dma_abandon(std::uint32_t left) {
    dma2().ch[kDmaCh].CTRL = 0;
    spi3().CTRL2 = spi3().CTRL2 & ~Spi::CTRL2_TXDMAEN;
    g_dma_ok = false;
    g_dma_stalls++;
    K2LOG("[LCD] DMA stalled left=%u of %u STS=%04x, polled path from now on\n", unsigned(left), unsigned(g_dma_n),
          unsigned(spi3().STS));
    for (std::uint32_t i = g_dma_n - left; i < g_dma_n; i++) push_pixel(g_dma_src ? g_dma_src[i] : g_fill_word);
}

K2_HOT void dma_wait() {
    if (!g_dma_busy) return;
    DmaChannel& ch = dma2().ch[kDmaCh];
    const std::uint32_t total = g_dma_n;
    std::uint32_t left = ch.DTCNT, last = left;
    std::uint32_t moved_ms = systick::millis();
    bool stalled = false;
    while (left) {
        if (left != last) { last = left; moved_ms = systick::millis(); }
        else if (systick::millis() - moved_ms >= kDmaStallMs) { stalled = true; break; }
        stream_service();   // the CPU only waits here: serve on the same elapsed-time cadence
        left = ch.DTCNT;
    }
    g_dma_busy = false;
    if (stalled) {
        dma_abandon(left);
        return;
    }
    ch.CTRL = 0;
    dma2().CLR = kDmaClearCh;
    spi3().CTRL2 = spi3().CTRL2 & ~Spi::CTRL2_TXDMAEN;
    g_dma_done++;
    if (!g_dma_proven) {
        g_dma_proven = true;
        K2LOG("[LCD] DMA ok n=%u\n", unsigned(total));
    }
    stream_service();
}

// A fill of `count` g_fill_word units through DMA in <=65535-unit pieces; the last piece stays in flight.
K2_HOT void dma_fill(std::uint32_t count) {
    while (count) {
        dma_wait();
        if (!g_dma_ok) {   // a piece stalled and DMA is off: finish the rest polled
            for (std::uint32_t i = 0; i < count; i++) push_pixel(g_fill_word);
            return;
        }
        const std::uint32_t n = dma_chunk(count);
        dma_start(nullptr, n);
        count -= n;
    }
}

// A pixel run through the ping-pong buffers: each piece is copied while the previous transfer is still on the wire, then
// started; the last piece stays in flight.
K2_HOT void dma_pixels(const std::uint16_t* src, std::uint32_t count) {
    while (count) {
        const std::uint32_t n = count < kPingPx ? count : kPingPx;
        std::uint16_t* buf = g_ping[g_ping_next];
        g_ping_next ^= 1u;
        std::copy(src, src + n, buf);   // never the buffer on the wire: one transfer in flight, the other buffer's
        dma_wait();
        if (!g_dma_ok) {   // stalled (now or before): the rest goes out polled
            for (std::uint32_t i = 0; i < count; i++) push_pixel(src[i]);
            return;
        }
        dma_start(buf, n);
        src += n;
        count -= n;
    }
}

K2_HOT void close_frame() {
    dma_wait();
    if (!g_frame_open) return;
    spi_wait_idle();     // BF clear: the last frame has left the shift register before CS rises
    p::lcd_cs.set();
    spi_frame_16bit(false);  // back to 8-bit frames for commands
    g_frame_open = false;
}

// The recorded window goes on the wire (closing the previous frame first) and the pixel frames start.
K2_HOT void open_window() {
    if (!g_win_pending) return;
    g_win_pending = false;
    set_window(g_win[0], g_win[1], g_win[2], g_win[3]);   // closes the previous frame; leaves CS low, DC high
    spi_frame_16bit(true);   // 16-bit pixel frames until the frame is closed
    g_frame_open = true;
}

}  // namespace

void init() {
    Crm& c = crm();
    // Only the ports of the LCD pins (A15, B3..B7) and AFIO for the SWJ release below.
    c.APB2EN |= Crm::APB2EN_AFIO | Crm::APB2EN_GPIOA | Crm::APB2EN_GPIOB;
    c.APB1EN |= Crm::APB1EN_SPI3;
    if (kLcdDma) c.AHBEN |= kAhbenDma2;

    // PA13/PA14/PA15/PB3/PB4 double as the SWJ debug port at reset. Disable it (no debugger on
    // this board) so they become the buttons and LCD lines.
    afio().MAP = (afio().MAP & ~Afio::MAP_SWJ_MASK) | Afio::MAP_SWJ_DISABLED;

    p::lcd_cs.mode(Mode::OutPp50);
    p::lcd_cs.set();
    p::lcd_sck.mode(Mode::AfPp50);
    p::lcd_dc.mode(Mode::OutPp50);
    p::lcd_mosi.mode(Mode::AfPp50);
    p::lcd_reset.mode(Mode::OutPp50);
    p::lcd_bl.mode(Mode::OutPp50);  // plain GPIO, driven high below: full brightness, no PWM
    p::lcd_dc.set();
    p::lcd_reset.set();

    spi3().CTRL1 = g_ctrl1;

    p::lcd_reset.clear();           // hardware reset pulse
    systick::delay_ms(15);
    p::lcd_reset.set();
    systick::delay_ms(120);         // controller needs 120 ms before accepting commands

    p::lcd_bl.set();

    command(Cmd::SLPOUT);
    systick::delay_ms(120);
    command(Cmd::MADCTL, 0x00);     // normal orientation
    command(Cmd::COLMOD, 0x55);     // 16 bit/pixel RGB565
    command(Cmd::INVON);            // IPS panel is wired inverted
    command(Cmd::NORON);
    set_window(0, 0, kWidth - 1, kHeight - 1);
    command(Cmd::DISPON);
    systick::delay_ms(20);
}

void park() {
    command(Cmd::DISPOFF);
    command(Cmd::SLPIN);
    p::lcd_bl.clear();
    systick::delay_ms(5);   // SLPIN needs 5 ms before the next command
}

void set_backlight(bool on) { p::lcd_bl.write(on); }

void set_rotation(std::uint8_t rotation) {
    rotation &= 3;
    g_x_off = rotation == 3 ? kRamOffset : 0;
    g_y_off = rotation == 2 ? kRamOffset : 0;
    command(Cmd::MADCTL, kMadctl[rotation]);
}

void set_sink(const Sink* sink) { g_sink = sink; }
const Sink* get_sink() { return g_sink; }
K2_HOT bool sink_band(std::uint16_t& first, std::uint16_t& last) {
    if (!g_sink || !g_sink->band) return false;
    g_sink->band(&first, &last);
    return true;
}
void set_idle_hook(void (*hook)()) { g_idle_hook = hook; }

K2_HOT void pump() {
    if (g_pacer.force()) run_round();
}

DmaInfo dma_info() { return {kLcdDma, kLcdDma && g_dma_ok, g_dma_done, g_dma_stalls}; }

unsigned spi_div() { return static_cast<unsigned>(g_div); }

bool set_spi_div(unsigned divider) {
    SpiDiv d;
    if (!lab_spi_div(divider, d) || g_stream_open || g_sink) return false;
    close_frame();      // an in-flight transfer and the last stream's open frame still own the wire
    spi_wait_idle();
    g_div = d;
    g_ctrl1 = ctrl1_for(d);
    spi3().CTRL1 = g_ctrl1 & ~Spi::CTRL1_SPIEN;   // BR only changes with SPE clear
    spi3().CTRL1 = g_ctrl1;
    K2LOG("[LCD] SPI3 divider /%u (CTRL1=%04x)\n", divider, unsigned(g_ctrl1));
    return true;
}

bool g_erase = false;
void set_erase(bool erase) { g_erase = erase; }
bool get_erase() { return g_erase; }

namespace {

// Paint cost (k2/core/paint_meter.hpp): the core cycles spent inside one outermost stream call, wire waits and the
// service rounds served inside them included. stream_pixels calls stream_fill in erase mode, so only the outermost
// call of a nest is timed and counted. Sink (screenshot) calls return before the timer is made: they never touch the panel.
int g_paint_depth = 0;
class PaintTimer {
public:
    explicit PaintTimer(std::uint32_t px) : outer_(g_paint_depth++ == 0), t0_(outer_ ? systick::ticks() : 0) {
        if (outer_) paint::meter.add_px(px);
    }
    ~PaintTimer() {
        g_paint_depth--;
        if (outer_) paint::meter.add_lcd(systick::ticks() - t0_);
    }
    PaintTimer(const PaintTimer&) = delete;
    PaintTimer& operator=(const PaintTimer&) = delete;

private:
    bool outer_;
    std::uint32_t t0_;
};

}  // namespace

K2_HOT void stream_begin(std::uint16_t x0, std::uint16_t y0, std::uint16_t x1, std::uint16_t y1) {
    if (g_sink) return g_sink->begin(x0, y0, x1, y1);
    // Recorded only: the first pixels or fill send it (open_window), so the previous stream's last transfer keeps the wire
    // while the caller composes this one's first run.
    g_win[0] = x0; g_win[1] = y0; g_win[2] = x1; g_win[3] = y1;
    g_win_pending = true;
    g_stream_open = true;   // the service cadence carries on from the previous window (elapsed time, not reset here)
}

K2_HOT void stream_pixels(std::span<const std::uint16_t> px) {
    if (g_sink) return g_sink->pixels(px.data(), px.size());
    const PaintTimer timer(static_cast<std::uint32_t>(px.size()));
    if (g_erase) return stream_fill(0, static_cast<std::uint32_t>(px.size()));
    if (px.empty()) return;
    open_window();
    if (kLcdDma && g_dma_ok && use_dma(static_cast<std::uint32_t>(px.size()))) {
        dma_pixels(px.data(), static_cast<std::uint32_t>(px.size()));   // left in flight; services inside its waits
        return;
    }
    dma_wait();   // the previous run or fill may still be on the wire
    for (std::uint16_t c : px) push_pixel(c);
    stream_service();
}

K2_HOT void stream_fill(std::uint16_t color, std::uint32_t count) {
    if (g_sink) return g_sink->fill(color, count);
    const PaintTimer timer(count);
    if (g_erase) color = 0;
    if (count == 0) return;
    open_window();
    if (kLcdDma && g_dma_ok && use_dma(count)) {
        dma_wait();                 // the source word is shared: the previous fill must be done
        g_fill_word = color;
        dma_fill(count);            // the last piece is left running; the next call or close_frame waits
        return;
    }
    dma_wait();
    while (count) {
        const std::uint32_t n = count < 256 ? count : 256;
        for (std::uint32_t i = 0; i < n; i++) push_pixel(color);
        count -= n;
        stream_service();
    }
}

K2_HOT void stream_end() {
    if (g_sink) return;  // the panel CS was never asserted
    // A window that got no pixels never went on the wire; otherwise the frame stays open, its last transfer running, until
    // the next window or command closes it (close_frame).
    g_win_pending = false;
    g_stream_open = false;
}

K2_HOT void flush() {
    if (g_sink) return;
    close_frame();
}

K2_HOT void fill_rect(std::uint16_t x, std::uint16_t y, std::uint16_t w, std::uint16_t h, std::uint16_t color) {
    if (x >= kWidth || y >= kHeight || w == 0 || h == 0) return;
    if (x + w > kWidth) w = kWidth - x;
    if (y + h > kHeight) h = kHeight - y;
    if (!clip_to_band(y, h)) return;   // a strip capture keeps 4 rows: the rest of the rectangle is O(1)
    stream_begin(x, y, x + w - 1, y + h - 1);
    stream_fill(color, std::uint32_t{w} * h);
    stream_end();
}

void fill(std::uint16_t color) { fill_rect(0, 0, kWidth, kHeight, color); }
void clear_strip(std::uint16_t y, std::uint16_t h, std::uint16_t color) { fill_rect(0, y, kWidth, h, color); }

}  // namespace k2::lcd
