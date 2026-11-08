// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Bit-banged I2C, see k2/drivers/i2c.hpp. Timing is by busy loops, not peripherals, because the
// board routes both buses to pins without a hardware I2C function.
#include "k2/drivers/i2c.hpp"

#include "k2/drivers/pins.hpp"
#include "k2/drivers/regs.hpp"
#include "k2/drivers/systick.hpp"

namespace k2::i2c {

namespace {

using namespace reg;
namespace p = pins;

struct Lines {
    Pin scl, sda;
    bool open_drain;  // true: Fusb (slave may stretch SCL, SDA never leaves output mode)
};
constexpr Lines kLines[] = {
    {p::fusb_scl, p::fusb_sda, true},
    {p::ina_scl, p::ina_sda, false},
};
constexpr const Lines& lines(Bus b) { return kLines[static_cast<unsigned>(b)]; }

// Half-bit delay. Ina: SysTick-timed 2 us (~200 kHz). Fusb: ~30 nops, about 400 ns.
void half_bit(Bus bus) {
    if (bus == Bus::Ina) {
        const std::uint32_t start = systick::ticks();
        constexpr std::uint32_t kHalfBitTicks = 2 * (systick::kSysclkHz / 1'000'000);
        while (systick::ticks() - start < kHalfBitTicks) {}
        return;
    }
    for (volatile int i = 0; i < 30; i++) __asm volatile("nop");
}

void scl_high_stretch(Bus bus) {
    const Lines& l = lines(bus);
    l.scl.set();
    if (!l.open_drain) return;  // push-pull SCL cannot be stretched
    for (std::uint32_t timeout = 1000; !l.scl.read() && --timeout;) __asm volatile("nop");
}

// Ina only: SDA is a push-pull output; to read or to receive an ACK it becomes an input with
// pull-up (ODR=1 selects it, set first so the line never glitches low). Back to output keeps
// ODR=1, so the line stays high.
void sda_to_input(Bus bus) {
    if (bus != Bus::Ina) return;
    lines(bus).sda.pull_up();
}
void sda_to_output(Bus bus) {
    if (bus != Bus::Ina) return;
    lines(bus).sda.mode(Mode::OutPp50);
}

void start(Bus bus) {
    const Lines& l = lines(bus);
    l.sda.set();
    scl_high_stretch(bus);
    half_bit(bus);
    l.sda.clear();
    half_bit(bus);
    l.scl.clear();
    half_bit(bus);
}

void stop(Bus bus) {
    const Lines& l = lines(bus);
    l.sda.clear();
    half_bit(bus);
    scl_high_stretch(bus);
    half_bit(bus);
    l.sda.set();
    half_bit(bus);
}

// Clock one byte out MSB first; returns true when the slave ACKs.
bool write_byte(Bus bus, std::uint8_t byte) {
    const Lines& l = lines(bus);
    for (int i = 7; i >= 0; i--) {
        l.sda.write(byte & (1 << i));
        half_bit(bus);
        scl_high_stretch(bus);
        half_bit(bus);
        l.scl.clear();
    }
    l.sda.set();  // release SDA for the ACK bit
    sda_to_input(bus);
    half_bit(bus);
    scl_high_stretch(bus);
    half_bit(bus);
    const bool ack = !l.sda.read();
    l.scl.clear();
    half_bit(bus);
    sda_to_output(bus);
    return ack;
}

std::uint8_t read_byte(Bus bus, bool ack) {
    const Lines& l = lines(bus);
    std::uint8_t byte = 0;
    l.sda.set();
    sda_to_input(bus);
    for (int i = 7; i >= 0; i--) {
        half_bit(bus);
        scl_high_stretch(bus);
        half_bit(bus);
        if (l.sda.read()) byte |= 1 << i;
        l.scl.clear();
    }
    sda_to_output(bus);
    l.sda.write(!ack);  // ACK = SDA low, NACK = high
    half_bit(bus);
    scl_high_stretch(bus);
    half_bit(bus);
    l.scl.clear();
    l.sda.set();
    half_bit(bus);
    return byte;
}

// Send "addr+W, reg" after a START. Caller issues STOP on failure.
bool select_register(Bus bus, std::uint8_t dev, std::uint8_t reg) {
    start(bus);
    return write_byte(bus, dev << 1) && write_byte(bus, reg);
}

// Bus recovery: 9 SCL pulses with SDA released, then START-less STOP.
void recover(Bus bus) {
    const Lines& l = lines(bus);
    if (bus == Bus::Fusb) l.sda.set(); else sda_to_input(bus);
    for (int i = 0; i < 9; i++) {
        l.scl.clear();
        half_bit(bus);
        l.scl.set();
        half_bit(bus);
    }
    l.scl.clear();
    if (bus == Bus::Ina) sda_to_output(bus); else half_bit(bus);
    l.sda.clear();
    half_bit(bus);
    l.scl.set();
    half_bit(bus);
    l.sda.set();
    half_bit(bus);
}

}  // namespace

void init() {
    crm().APB2EN |= Crm::APB2EN_GPIOA | Crm::APB2EN_GPIOB;

    // PB14 (BACK button) is not touched here: Pin::mode only rewrites its own nibble.
    for (const Pin& pin : {p::ina_scl, p::ina_sda}) pin.mode(Mode::OutPp50);
    p::fusb_scl.mode(Mode::OutOd50);
    p::fusb_sda.mode(Mode::OutOd50);
    p::fusb_int.mode(Mode::InPull);

    // Idle levels: both buses high, FUSB INT_N pull-up (ODR=1).
    for (const Pin& pin : {p::ina_scl, p::ina_sda, p::fusb_scl, p::fusb_sda, p::fusb_int}) pin.set();

    recover(Bus::Fusb);
    recover(Bus::Ina);
}

bool write(Bus bus, std::uint8_t dev, std::uint8_t reg, std::span<const std::uint8_t> data) {
    bool ok = select_register(bus, dev, reg);
    for (std::size_t i = 0; ok && i < data.size(); i++) ok = write_byte(bus, data[i]);
    stop(bus);
    return ok;
}

bool read(Bus bus, std::uint8_t dev, std::uint8_t reg, std::span<std::uint8_t> out) {
    bool ok = select_register(bus, dev, reg);
    if (ok) {
        start(bus);  // repeated start
        ok = write_byte(bus, (dev << 1) | 1);
    }
    for (std::size_t i = 0; ok && i < out.size(); i++) out[i] = read_byte(bus, i + 1 < out.size());
    stop(bus);
    return ok;
}

}  // namespace k2::i2c
