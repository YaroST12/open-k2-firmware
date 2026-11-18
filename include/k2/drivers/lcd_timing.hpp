// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// LCD link timing and DMA chunk planning: pure constexpr code over values, so host tests can
// cover it (the driver in src/drivers/lcd.cpp owns the registers).
#pragma once
#include <cstdint>

namespace k2::lcd {

// SPI3 clock divider off APB1 (120 MHz): /4 = 30 MHz is the boot value and ships (the user checked the panel at 30 MHz
// on 2026-11-12: clean); /8 = 15 MHz and /2 = 60 MHz are runtime lab values (`lcd spi <n>`, RAM only, never persisted).
// 60 MHz sits near the ST7789 write-clock limit: it is only ever judged by a visual check of the panel by the user.
enum class SpiDiv : std::uint8_t { Div2 = 2, Div4 = 4, Div8 = 8 };

inline constexpr SpiDiv kBootSpiDiv = SpiDiv::Div4;

inline constexpr std::uint32_t kApb1Hz = 120'000'000;

// CTRL1 BR field (bits 5:3) for a divider: 0b000 = /2, 0b001 = /4, 0b010 = /8.
constexpr std::uint16_t br_field(SpiDiv div) {
    switch (div) {
    case SpiDiv::Div2: return 0u << 3;
    case SpiDiv::Div4: return 1u << 3;
    default:           return 2u << 3;
    }
}

// Time on the wire for `px` RGB565 pixels, in microseconds: one pixel is one 16-bit SPI frame
// (16 * divider APB1 cycles). Command and window overhead is not included.
constexpr std::uint32_t wire_us(std::uint32_t px, SpiDiv div) {
    return static_cast<std::uint32_t>(std::uint64_t{px} * 16u * static_cast<unsigned>(div) * 1'000'000u /
                                      (std::uint64_t{kApb1Hz}));
}

// The service cadence of a paint (docs/ARCH_REWORK.md E1): one usb::service() + idle-hook round once kServiceUs has passed
// since the last round ended. lcd.cpp checks after every stream call's pixels (each polled run, each 256-px fill piece, the
// end of a DMA wait) and while it spins in a DMA wait. Elapsed time, not pixels: a paint of many short windows (text fields)
// is served like one long fill, whatever the divider. The longest stretch without a round is kServiceUs plus the round
// itself plus the work between two checks (one stream call and the rendering of its run, well under 1 ms).
inline constexpr std::uint32_t kServiceUs = 1000;

// The pacer behind that rule, over any free-running unsigned clock (core cycles on the device, microseconds on the host);
// the interval is in the clock's units. Differences are unsigned, so the clock may wrap; after a silence longer than its
// period (~17.9 s of cycles at 240 MHz) the first round can come up to one interval late. A round stays open from start()
// or force() to finish(): no nested round starts meanwhile, so neither the idle hook nor anything it calls ever runs the
// hook again.
class ServicePacer {
public:
    explicit constexpr ServicePacer(std::uint32_t interval) : interval_(interval) {}
    // True when a round is due and none is open; the round is then open.
    constexpr bool start(std::uint32_t now) {
        if (open_ || now - last_ < interval_) return false;
        open_ = true;
        return true;
    }
    // A round on demand (lcd::pump), still never nested.
    constexpr bool force() {
        if (open_) return false;
        open_ = true;
        return true;
    }
    // The round ended at `now`: the next one is due an interval later.
    constexpr void finish(std::uint32_t now) {
        open_ = false;
        last_ = now;
    }

private:
    std::uint32_t interval_;
    std::uint32_t last_ = 0;
    bool open_ = false;
};

// The runtime lab switch (`lcd spi <n>`) offers /4 (shipped), /8 (the old default) and /2 (60 MHz, near the ST7789
// write-clock limit: a visual check by the user only). RAM only; every boot comes up at kBootSpiDiv.
constexpr bool lab_spi_div(unsigned value, SpiDiv& out) {
    if (value == 2) { out = SpiDiv::Div2; return true; }
    if (value == 4) { out = SpiDiv::Div4; return true; }
    if (value == 8) { out = SpiDiv::Div8; return true; }
    return false;
}

// DMA transfer size: the count register is 16 bit, so a longer run goes out in pieces.
inline constexpr std::uint32_t kDmaMaxCount = 0xFFFF;
constexpr std::uint32_t dma_chunk(std::uint32_t remaining) {
    return remaining < kDmaMaxCount ? remaining : kDmaMaxCount;
}

// Below this many pixels the DMA setup (about ten register writes) costs as much as pushing the
// pixels by hand, so short runs stay polled.
inline constexpr std::uint32_t kDmaMinPixels = 32;
constexpr bool use_dma(std::uint32_t px) { return px >= kDmaMinPixels; }

// ---- Backlight PWM -----------------------------------------------------------------------------------------------------
// The backlight pin runs off a general timer at 24 kHz (above hearing, far below anything the driver chip cares about);
// the period is counts of the timer clock, so it follows the AHB divider (240 MHz: 10000 counts, 60 MHz: 2500). The
// 16-bit counter limits it to 65536 counts.
inline constexpr std::uint32_t kBacklightHz = 24'000;
constexpr std::uint32_t backlight_period(std::uint32_t timer_hz) {
    const std::uint32_t n = timer_hz / kBacklightHz;
    return n == 0 ? 1u : (n > 65536u ? 65536u : n);
}
// The auto-reload value for a period of that many counts.
constexpr std::uint32_t backlight_arr(std::uint32_t period) { return period - 1; }
// The compare value for a brightness of pct percent (0..100, more is 100): 0 never high, 100 = the whole period, which
// the PWM mode 1 output holds high all the time (the compare value is above the auto-reload value). 32-bit: the period
// is at most 65536 and pct below 100.
constexpr std::uint32_t backlight_ccr(unsigned pct, std::uint32_t period) {
    return pct >= 100 ? period : period * pct / 100u;
}

// ---- Panel sleep and wake ----------------------------------------------------------------------------------------------
// Sleep In and Sleep Out may not follow each other (or the other way round) within 120 ms, and Display On waits out the
// 120 ms after Sleep Out. The sequencer turns "should the panel be awake" into the next command to send, polled with the
// millisecond clock, so a wake never blocks the main loop. It holds the state only; lcd.cpp sends the commands.
class PanelSeq {
public:
    enum class State : std::uint8_t { Awake, Asleep, Waking };   // Waking: Sleep Out sent, Display On still to come
    enum class Action : std::uint8_t { None, Sleep, SlpOut, DispOn };   // Sleep = Display Off + Sleep In
    static constexpr std::uint32_t kGapMs = 120;

    constexpr void request(bool awake) { want_ = awake; }
    constexpr Action poll(std::uint32_t now_ms) {
        switch (state_) {
        case State::Awake:
            if (want_) return Action::None;
            state_ = State::Asleep;
            since_ = now_ms;
            return Action::Sleep;
        case State::Asleep:
            if (!want_ || now_ms - since_ < kGapMs) return Action::None;
            state_ = State::Waking;
            since_ = now_ms;
            return Action::SlpOut;
        case State::Waking:
            if (now_ms - since_ < kGapMs) return Action::None;
            if (want_) {
                state_ = State::Awake;
                return Action::DispOn;
            }
            state_ = State::Asleep;
            since_ = now_ms;
            return Action::Sleep;
        }
        return Action::None;
    }
    constexpr State state() const { return state_; }
    constexpr bool awake() const { return state_ == State::Awake; }

private:
    State state_ = State::Awake;
    bool want_ = true;
    std::uint32_t since_ = 0;
};

}  // namespace k2::lcd
