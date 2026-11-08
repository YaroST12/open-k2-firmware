// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// ST7789V driver, see k2/drivers/lcd.hpp. Commands and their arguments are polled 8-bit SPI3 writes;
// the pixel stream switches SPI3 to 16-bit data frames (one store per pixel). The panel
// wiring is in k2/drivers/pins.hpp.
#include "k2/drivers/lcd.hpp"
#include "k2/drivers/lcd_timing.hpp"
#include "k2/core/log.hpp"

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
constexpr std::uint32_t kServicePixels = 2048;  // ~4 KB = ~4.4 ms at 7.5 MHz between usb::service()

// SPI3 clock divider: DIV8 (7.5 MHz) ships; DIV4 (15 MHz) and DIV2 (30 MHz) are only for a
// hardware signal-integrity check on a specific board.
constexpr SpiDiv kLcdSpiDiv = SpiDiv::Div8;
constexpr std::uint16_t ctrl1_for(SpiDiv div) { return (Spi::CTRL1_LCD & ~Spi::CTRL1_BR_MASK) | br_field(div); }
constexpr std::uint16_t kLcdCtrl1 = ctrl1_for(kLcdSpiDiv);
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
// While a transfer runs the CPU serves usb::service() and the idle hook about once per
// millisecond of wire time at any divider (60000 px/ms-of-APB1 / (16 * divider)).
constexpr std::uint32_t kDmaServicePx = 3750 / static_cast<unsigned>(kLcdSpiDiv);
// A transfer whose count does not move for this long (SPI3 stuck, wrong request mapping) is
// abandoned: the remainder goes out polled and DMA stays off. One pixel takes 2.1 us at DIV8.
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
std::uint32_t g_stream_px;
const Sink* g_sink;  // non-null while a screenshot is being rendered

// Command path: the data register must be written as a byte or the SPI would send a 16-bit
// frame (DSIZE is 0 outside a pixel stream). The pixel path uses spi_write16 instead.
// Bounded waits: one frame takes ~1.1..2.1 us (~256..512 CPU cycles); kSpiSpins polls are well
// over 100 us. A stuck SPI3 (clock off, peripheral wedged) drops the frame and logs once instead
// of hanging the main loop with no debugger to see it.
constexpr std::uint32_t kSpiSpins = 20000;
std::uint32_t g_spi_timeouts;

void spi_timeout(const char* where) {
    if (g_spi_timeouts++ == 0) K2LOG("[LCD] FAULT SPI3 %s timeout STS=%04x\n", where, unsigned(spi3().STS));
}
void spi_write8(std::uint8_t byte) {
    Spi& spi = spi3();
    for (std::uint32_t n = kSpiSpins; !(spi.STS & Spi::STS_TDBE);)
        if (--n == 0) { spi_timeout("TDBE"); return; }
    *reinterpret_cast<volatile std::uint8_t*>(&spi.DT) = byte;
}
// 16-bit data frame (DSIZE=1): one store shifts out both bytes of a pixel, MSB first -- the
// same wire order as the old two 8-bit writes, with half the TDBE spin-waits and bus frames.
void spi_write16(std::uint16_t word) {
    Spi& spi = spi3();
    for (std::uint32_t n = kSpiSpins; !(spi.STS & Spi::STS_TDBE);)
        if (--n == 0) { spi_timeout("TDBE"); return; }
    *reinterpret_cast<volatile std::uint16_t*>(&spi.DT) = word;
}
void spi_wait_idle() {
    for (std::uint32_t n = kSpiSpins; spi3().STS & Spi::STS_BF;)
        if (--n == 0) { spi_timeout("BF"); return; }
}

// ST7789 command frames must stay 8-bit, so a pixel stream flips DSIZE on for its duration and
// back off afterwards. DSIZE is only changed while the bus is idle (BF clear, CS high) and with
// SPE cleared, as the AT32F403A requires; the switch happens once per stream, never per pixel.
void spi_frame_16bit(bool on) {
    Spi& spi = spi3();
    spi_wait_idle();
    spi.CTRL1 = kLcdCtrl1 & ~Spi::CTRL1_SPIEN;  // SPE=0 while DSIZE changes
    spi.CTRL1 = on ? (kLcdCtrl1 | Spi::CTRL1_DSIZE) : kLcdCtrl1;
}

void dma_wait();  // below: finishes an in-flight pixel transfer, serving PD/USB meanwhile

// One chip-select frame: DC low for a command byte, then (separately framed) DC high data.
void command(Cmd cmd) {
    dma_wait();
    p::lcd_dc.clear();
    p::lcd_cs.clear();
    spi_write8(static_cast<std::uint8_t>(cmd));
    spi_wait_idle();
    p::lcd_cs.set();
}
void data(std::span<const std::uint8_t> bytes) {
    dma_wait();
    p::lcd_dc.set();
    p::lcd_cs.clear();
    for (std::uint8_t b : bytes) spi_write8(b);
    spi_wait_idle();
    p::lcd_cs.set();
}
void command(Cmd cmd, std::span<const std::uint8_t> args) {
    command(cmd);
    data(args);
}
void command(Cmd cmd, std::uint8_t arg) { command(cmd, {&arg, 1}); }

// CASET / RASET take big-endian start and end, RAMWR then opens the pixel stream. All three go
// out under ONE chip-select assertion (CS low once) instead of three frames with a CS toggle +
// idle wait each; CS stays low and DC ends high so the pixel stream continues the same frame.
void set_window(std::uint16_t x0, std::uint16_t y0, std::uint16_t x1, std::uint16_t y1) {
    x0 += g_x_off; x1 += g_x_off;
    y0 += g_y_off; y1 += g_y_off;
    const std::uint8_t cols[4] = {std::uint8_t(x0 >> 8), std::uint8_t(x0), std::uint8_t(x1 >> 8), std::uint8_t(x1)};
    const std::uint8_t rows[4] = {std::uint8_t(y0 >> 8), std::uint8_t(y0), std::uint8_t(y1 >> 8), std::uint8_t(y1)};
    dma_wait();
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

void stream_service(std::uint32_t count) {
    g_stream_px += count;
    if (g_stream_px >= kServicePixels) {
        g_stream_px = 0;
        usb::service();
        if (g_idle_hook) g_idle_hook();
    }
}

void push_pixel(std::uint16_t c) { spi_write16(c); }

// ---- DMA pixel transfer ----------------------------------------------------------------------
// One transfer is in flight at most. stream_fill starts it and returns (the source is the static
// g_fill_word, so the caller's CPU work overlaps the wire time); stream_pixels waits for its own
// transfer because the caller's buffer may live on the stack. Everything that touches SPI3, DC or
// CS first calls dma_wait(), which is also where usb::service() and the idle hook keep running.
volatile std::uint16_t g_fill_word;
const std::uint16_t* g_dma_src;   // nullptr: fixed source word (fill)
std::uint32_t g_dma_n;            // units in the in-flight transfer
bool g_dma_busy;
bool g_dma_ok = kLcdDma;          // cleared for good by a stalled transfer
bool g_dma_proven;                // first completed transfer logged
std::uint32_t g_dma_done, g_dma_stalls;   // completed / abandoned transfers since boot (dma_info)

void serve() {
    usb::service();
    if (g_idle_hook) g_idle_hook();
}

void dma_start(const std::uint16_t* src, std::uint32_t n) {
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
void dma_abandon(std::uint32_t left) {
    dma2().ch[kDmaCh].CTRL = 0;
    spi3().CTRL2 = spi3().CTRL2 & ~Spi::CTRL2_TXDMAEN;
    g_dma_ok = false;
    g_dma_stalls++;
    K2LOG("[LCD] DMA stalled left=%u of %u STS=%04x, polled path from now on\n", unsigned(left), unsigned(g_dma_n),
          unsigned(spi3().STS));
    for (std::uint32_t i = g_dma_n - left; i < g_dma_n; i++) push_pixel(g_dma_src ? g_dma_src[i] : g_fill_word);
}

void dma_wait() {
    if (!g_dma_busy) return;
    DmaChannel& ch = dma2().ch[kDmaCh];
    const std::uint32_t total = g_dma_n;
    std::uint32_t left = ch.DTCNT, last = left, served = 0;
    std::uint32_t moved_ms = systick::millis();
    bool stalled = false;
    while (left) {
        if (left != last) { last = left; moved_ms = systick::millis(); }
        else if (systick::millis() - moved_ms >= kDmaStallMs) { stalled = true; break; }
        if (total - left - served >= kDmaServicePx) {
            served = total - left;
            g_stream_px = 0;
            serve();
        }
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
    stream_service(total - served);   // pixels since the last in-wait service count toward the next
}

// Runs the whole count through DMA in <=65535-unit pieces. src == nullptr is a fill of
// g_fill_word (the last piece stays in flight); a pixel buffer is waited for piece by piece.
void dma_send(const std::uint16_t* src, std::uint32_t count) {
    while (count) {
        dma_wait();
        if (!g_dma_ok) {   // a piece stalled and DMA is off: finish the rest polled
            for (std::uint32_t i = 0; i < count; i++) push_pixel(src ? src[i] : g_fill_word);
            return;
        }
        const std::uint32_t n = dma_chunk(count);
        dma_start(src, n);
        count -= n;
        if (src) { src += n; dma_wait(); }
    }
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

    spi3().CTRL1 = kLcdCtrl1;

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

void set_rotation(std::uint8_t rotation) {
    rotation &= 3;
    g_x_off = rotation == 3 ? kRamOffset : 0;
    g_y_off = rotation == 2 ? kRamOffset : 0;
    command(Cmd::MADCTL, kMadctl[rotation]);
}

void set_sink(const Sink* sink) { g_sink = sink; }
const Sink* get_sink() { return g_sink; }
void set_idle_hook(void (*hook)()) { g_idle_hook = hook; }

DmaInfo dma_info() { return {kLcdDma, kLcdDma && g_dma_ok, g_dma_done, g_dma_stalls}; }

bool g_erase = false;
void set_erase(bool erase) { g_erase = erase; }
bool get_erase() { return g_erase; }

void stream_begin(std::uint16_t x0, std::uint16_t y0, std::uint16_t x1, std::uint16_t y1) {
    if (g_sink) return g_sink->begin(x0, y0, x1, y1);
    set_window(x0, y0, x1, y1);  // leaves CS low, DC high: the pixel stream continues the frame
    spi_frame_16bit(true);  // 16-bit pixel frames from here until stream_end()
    g_stream_px = 0;
}

void stream_pixels(std::span<const std::uint16_t> px) {
    if (g_sink) return g_sink->pixels(px.data(), px.size());
    if (g_erase) return stream_fill(0, static_cast<std::uint32_t>(px.size()));
    if (kLcdDma && g_dma_ok && use_dma(static_cast<std::uint32_t>(px.size()))) {
        dma_send(px.data(), static_cast<std::uint32_t>(px.size()));   // services inside its waits
        return;
    }
    dma_wait();   // a fill started by the caller may still be on the wire
    for (std::uint16_t c : px) push_pixel(c);
    stream_service(px.size());
}

void stream_fill(std::uint16_t color, std::uint32_t count) {
    if (g_sink) return g_sink->fill(color, count);
    if (g_erase) color = 0;
    if (kLcdDma && g_dma_ok && use_dma(count)) {
        dma_wait();                 // the source word is shared: the previous fill must be done
        g_fill_word = color;
        dma_send(nullptr, count);   // the last piece is left running; stream_end or the next call waits
        return;
    }
    dma_wait();
    while (count) {
        const std::uint32_t n = count < 256 ? count : 256;
        for (std::uint32_t i = 0; i < n; i++) push_pixel(color);
        count -= n;
        stream_service(n);
    }
}

void stream_end() {
    if (g_sink) return;  // the panel CS was never asserted
    dma_wait();          // the last fill piece may still be running
    spi_wait_idle();     // BF clear: the last frame has left the shift register before CS rises
    p::lcd_cs.set();
    spi_frame_16bit(false);  // back to 8-bit frames for commands
}

void fill_rect(std::uint16_t x, std::uint16_t y, std::uint16_t w, std::uint16_t h, std::uint16_t color) {
    if (x >= kWidth || y >= kHeight || w == 0 || h == 0) return;
    if (x + w > kWidth) w = kWidth - x;
    if (y + h > kHeight) h = kHeight - y;
    stream_begin(x, y, x + w - 1, y + h - 1);
    stream_fill(color, std::uint32_t{w} * h);
    stream_end();
}

void fill(std::uint16_t color) { fill_rect(0, 0, kWidth, kHeight, color); }

}  // namespace k2::lcd
