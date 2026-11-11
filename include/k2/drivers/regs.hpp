// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Typed register map for the AT32F403A (STM32F1-style peripherals at Artery addresses).
//
// Each peripheral is a struct laid over its base address; bit constants live inside the
// struct as `static constexpr`, so `Crm::CTRL_HEXTEN` reads as "CRM.CTRL.HEXTEN".
// Board wiring (which pin does what) is in k2/drivers/pins.hpp, not here.
#pragma once
#include <cstddef>
#include <cstdint>
#include "k2/core/critical.hpp"

namespace k2::reg {

using R32 = volatile uint32_t;
using R16 = volatile uint16_t;

// Overlay a register struct on a fixed address. The reinterpret_cast is inlined away.
template <typename T>
inline T& at(uintptr_t addr) { return *reinterpret_cast<T*>(addr); }

// ---- Cortex-M4 core peripherals -------------------------------------------------------

struct Scb {
    R32 CPUID, ICSR, VTOR, AIRCR, SCR, CCR, SHPR[3], SHCSR, CFSR, HFSR, DFSR, MMFAR, BFAR, AFSR;
    uint32_t reserved0[18];
    R32 CPACR;

    static constexpr uint32_t AIRCR_SYSRESET = 0x05FA0004u;  // VECTKEY 0x5FA | SYSRESETREQ
    static constexpr uint32_t CPACR_FPU_FULL = (3u << 20) | (3u << 22);  // CP10 + CP11 full access
};
static_assert(offsetof(Scb, VTOR) == 0x08 && offsetof(Scb, AIRCR) == 0x0C && offsetof(Scb, CPACR) == 0x88);
inline Scb& scb() { return at<Scb>(0xE000ED00); }

// Memory protection unit (ARMv7-M): RNR selects a region, RBAR/RASR describe it (k2/drivers/mpu_calc.hpp has the values).
struct Mpu {
    R32 TYPE, CTRL, RNR, RBAR, RASR;
};
static_assert(offsetof(Mpu, CTRL) == 0x04 && offsetof(Mpu, RBAR) == 0x0C && offsetof(Mpu, RASR) == 0x10);
inline Mpu& mpu() { return at<Mpu>(0xE000ED90); }

// NVIC: ICER/ICPR clear enable / pending for 8 x 32 interrupt lines.
struct Nvic {
    R32 ISER[8];  uint32_t r0[24];
    R32 ICER[8];  uint32_t r1[24];
    R32 ISPR[8];  uint32_t r2[24];
    R32 ICPR[8];
};
static_assert(offsetof(Nvic, ICER) == 0x80 && offsetof(Nvic, ICPR) == 0x180);
inline Nvic& nvic() { return at<Nvic>(0xE000E100); }

struct SysTick {
    R32 CTRL, LOAD, VAL;
    static constexpr uint32_t CTRL_ENABLE = 1u << 0, CTRL_TICKINT = 1u << 1, CTRL_CLKSOURCE_CORE = 1u << 2;
};
inline SysTick& systick() { return at<SysTick>(0xE000E010); }

// ---- Clock and reset management ------------------------------------------------------

struct Crm {
    R32 CTRL, CFG, INT, APB2RST, APB1RST, AHBEN, APB2EN, APB1EN, BPDC, CTRLSTS, AHBRST;
    uint32_t r0;
    R32 MISC1;
    uint32_t r1[8];
    R32 MISC2;

    static constexpr uint32_t CTRL_HICKEN = 1u << 0, CTRL_HICKSTBL = 1u << 1;
    static constexpr uint32_t CTRL_TRIM_MASK = 0xFFFC;   // bits 15:2, HICK trim + calibration
    static constexpr uint32_t CTRL_HEXTEN = 1u << 16, CTRL_HEXTSTBL = 1u << 17, CTRL_HEXTBYPS = 1u << 18;
    static constexpr uint32_t CTRL_PLLEN = 1u << 24, CTRL_PLLSTBL = 1u << 25;

    static constexpr uint32_t CFG_SCLKSEL_MASK = 0x3, CFG_SCLKSEL_PLL = 0x2, CFG_SCLKSTS_SHIFT = 2;
    static constexpr uint32_t CFG_APB1DIV_MASK = 0x0700, CFG_APB1DIV_2 = 0x0400;   // field 100b = /2 (same code as APB2DIV_2)
    static constexpr uint32_t CFG_APB2DIV_MASK = 0x3800, CFG_APB2DIV_2 = 0x2000;

    static constexpr uint32_t APB2EN_AFIO = 1u << 0, APB2EN_GPIOA = 1u << 2, APB2EN_GPIOB = 1u << 3,
                              APB2EN_GPIOC = 1u << 4, APB2EN_ACC = 1u << 22;
    // CTRLSTS reset-cause flags (sticky until RSTFC; sysreset::capture_flags() reads and clears them at every boot).
    static constexpr uint32_t CTRLSTS_RSTFC = 1u << 24, CTRLSTS_PINRSTF = 1u << 26,
                              CTRLSTS_PORRSTF = 1u << 27, CTRLSTS_SWRSTF = 1u << 28,
                              CTRLSTS_IWDGRSTF = 1u << 29, CTRLSTS_WWDGRSTF = 1u << 30,
                              CTRLSTS_LPWRRSTF = 1u << 31;
    static constexpr uint32_t APB1EN_SPI3 = 1u << 15, APB1EN_USB = 1u << 23;
};
static_assert(offsetof(Crm, MISC1) == 0x30 && offsetof(Crm, MISC2) == 0x54);
inline Crm& crm() { return at<Crm>(0x40021000); }

// Independent watchdog on the ~40 kHz LSI. KR: 0xCCCC start, 0x5555 unlock PR/RLR, 0xAAAA reload.
struct Iwdg {
    R32 KR, PR, RLR, SR;
    static constexpr uint32_t KR_START = 0xCCCC, KR_UNLOCK = 0x5555, KR_RELOAD = 0xAAAA;
};
static_assert(offsetof(Iwdg, SR) == 0x0C);
inline Iwdg& iwdg() { return at<Iwdg>(0x40003000); }

// Automatic clock calibration: trims HICK to 48 MHz against the USB SOF (used as USB clock).
struct Acc {
    R32 STS, CTRL1, CTRL2, C1, C2, C3;
    static constexpr uint32_t CTRL1_CALON = 1u << 0, CTRL1_ENTRIM = 1u << 1;
    static constexpr uint32_t CTRL1_CALRSEL_HICK = 1u << 4, CTRL1_SOFSEL = 1u << 5;
};
inline Acc& acc() { return at<Acc>(0x40015800); }

// ---- GPIO and AFIO -------------------------------------------------------------------

struct Afio {
    R32 EVCR, MAP;
    R32 EXTICR[4];  // EXTICR[n] routes EXTI lines 4n..4n+3, 4 bits each (0 = port A, 1 = B, 2 = C)
    static constexpr uint32_t MAP_SWJ_MASK = 7u << 24;
    static constexpr uint32_t MAP_SWJ_DISABLED = 4u << 24;  // frees PA13/PA14/PA15/PB3/PB4
};
static_assert(offsetof(Afio, EXTICR) == 0x08);
inline Afio& afio() { return at<Afio>(0x40010000); }

// External interrupt controller, lines 0..19.
struct Exti {
    R32 IMR, EMR, RTSR, FTSR, SWIER, PR;  // PR: write 1 to clear
};
static_assert(offsetof(Exti, PR) == 0x14);
inline Exti& exti() { return at<Exti>(0x40010400); }

// 4-bit pin configuration (CNF[1:0] << 2 | MODE[1:0]) as written to CRL/CRH.
enum class Mode : uint8_t {
    Analog      = 0x0,
    OutPp50     = 0x3,  // general-purpose push-pull, 50 MHz
    InFloating  = 0x4,
    OutOd50     = 0x7,  // general-purpose open-drain, 50 MHz
    InPull      = 0x8,  // input with pull; ODR bit picks up (1) or down (0)
    AfPp50      = 0xB,  // alternate-function push-pull, 50 MHz
};

struct Gpio {
    R32 CRL, CRH, IDR, ODR, BSRR, BRR, LCKR;
};

enum class Port : uintptr_t { A = 0x40010800, B = 0x40010C00, C = 0x40011000 };
inline Gpio& gpio(Port p) { return at<Gpio>(static_cast<uintptr_t>(p)); }

#ifdef K2_HOST_PINS
enum class HostPin : uint8_t { Clear, Set, Read, Mode };
bool host_pin(Port p, uint8_t n, HostPin op, unsigned arg = 0);
#endif

struct Pin {
    Port port;
    uint8_t n;

    constexpr uint32_t mask() const { return 1u << n; }
#ifdef K2_HOST_PINS
    // Host tests: pins are events in a log instead of writes to the GPIO block (tests/fake_pins.cpp).
    void set() const { host_pin(port, n, HostPin::Set); }
    void clear() const { host_pin(port, n, HostPin::Clear); }
    void write(bool high) const { high ? set() : clear(); }
    bool read() const { return host_pin(port, n, HostPin::Read); }
    void mode(Mode m) const { host_pin(port, n, HostPin::Mode, static_cast<unsigned>(m)); }
#else
    void set() const { gpio(port).BSRR = mask(); }
    void clear() const { gpio(port).BRR = mask(); }
    void write(bool high) const { high ? set() : clear(); }
    bool read() const { return gpio(port).IDR & mask(); }

    // CRL/CRH hold 8 pins each and are shared with ISR code (load.cpp hw_off from SysTick):
    // the read-modify-write runs with interrupts masked.
    void mode(Mode m) const {
        const Critical cs;
        R32& cr = n < 8 ? gpio(port).CRL : gpio(port).CRH;
        const unsigned shift = (n & 7u) * 4;
        cr = (cr & ~(0xFu << shift)) | (static_cast<uint32_t>(m) << shift);
    }
#endif
    // Input with pull-up: ODR bit = 1 selects the pull-up. ODR first, so a pin that was a
    // high output never dips low while it changes direction.
    void pull_up() const { set(); mode(Mode::InPull); }
};

// ---- SPI3 (LCD) ----------------------------------------------------------------------

struct Spi {
    R16 CTRL1; uint16_t r0;
    R16 CTRL2; uint16_t r1;
    R16 STS;   uint16_t r2;
    R16 DT;    uint16_t r3;

    static constexpr uint16_t CTRL1_CPHA = 1u << 0, CTRL1_CPOL = 1u << 1, CTRL1_MSTEN = 1u << 2;
    static constexpr uint16_t CTRL1_SPIEN = 1u << 6, CTRL1_SSI = 1u << 8, CTRL1_SSM = 1u << 9;
    static constexpr uint16_t CTRL1_BIDIOEN = 1u << 14, CTRL1_BIDIEN = 1u << 15;
    static constexpr uint16_t CTRL1_BR_MASK = 7u << 3;
    static constexpr uint16_t CTRL1_BR_DIV2 = 0u << 3;  // BR = 0b000: APB1 / 2 = 60 MHz at 120 MHz
    static constexpr uint16_t CTRL1_BR_DIV4 = 1u << 3;  // BR = 0b001: APB1 / 4 = 30 MHz at 120 MHz
    static constexpr uint16_t CTRL1_BR_DIV8 = 1u << 4;  // BR = 0b010: APB1 / 8 = 15 MHz at 120 MHz
    // DSIZE (DFF on STM32F1 clones): 0 = 8-bit data frames, 1 = 16-bit. Only changed while the
    // bus is idle and SPE is cleared (see spi_frame_16bit in src/drivers/lcd.cpp).
    static constexpr uint16_t CTRL1_DSIZE = 1u << 11;
    static constexpr uint16_t STS_TDBE = 1u << 1, STS_BF = 1u << 7;
    static constexpr uint16_t CTRL2_TXDMAEN = 1u << 1;  // TDBE raises a DMA request

    // LCD link: master, mode 3 (CPOL=1 CPHA=1), 8-bit frames, MSB first, software NSS,
    // 1-line TX-only (BIDI), APB1/8.
    static constexpr uint16_t CTRL1_LCD = CTRL1_BIDIEN | CTRL1_BIDIOEN | CTRL1_SSM | CTRL1_SSI |
                                          CTRL1_SPIEN | CTRL1_BR_DIV8 | CTRL1_MSTEN | CTRL1_CPOL | CTRL1_CPHA;
    // Same link with 16-bit data frames, used for the pixel stream only: ST7789 commands and
    // their arguments must stay 8-bit frames, so the driver switches between the two.
    static constexpr uint16_t CTRL1_LCD_16 = CTRL1_LCD | CTRL1_DSIZE;
};
static_assert(Spi::CTRL1_LCD == 0xC357);
static_assert(Spi::CTRL1_LCD_16 == 0xCB57);
inline Spi& spi3() { return at<Spi>(0x40003C00); }

// ---- USB full-speed device core (driven by TinyUSB; only the interrupt status is read here) ----

inline R16& usb_istr() { return at<R16>(0x40005C00 + 0x44); }
constexpr uint16_t kUsbIstrReset = 1u << 10;
// Frame number register: the frame counter of the last SOF and the PHY's D+ / D- receiver levels (read-only).
inline R16& usb_fnr() { return at<R16>(0x40005C00 + 0x48); }
constexpr uint16_t kUsbFnrFrame = 0x7FF, kUsbFnrRxdm = 1u << 14, kUsbFnrRxdp = 1u << 15;
// The core's configuration word: bit 1 set = the internal D+ 1.5k pull-up off (TinyUSB's connect / disconnect write it).
inline R32& usb_cfg() { return at<R32>(0x40005C00 + 0x60); }
constexpr uint32_t kUsbCfgPullupOff = 1u << 1;

}  // namespace k2::reg
