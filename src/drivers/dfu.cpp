// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// DFU entry on the real hardware: USB logs, flash marker clear, NVIC system reset.
#include "k2/core/log.hpp"
#include "k2/drivers/dfu.hpp"

#include <cstdint>
#include <string_view>

#include "k2/drivers/crm.hpp"
#include "k2/drivers/flash.hpp"
#include "k2/drivers/fusb302.hpp"
#include "k2/drivers/lcd.hpp"
#include "k2/drivers/regs.hpp"
#include "k2/drivers/sysreset.hpp"
#include "k2/drivers/systick.hpp"
#include "k2/drivers/usb.hpp"

namespace k2::dfu {

namespace {

struct RealEnv {
    void say(std::string_view m) { usb::log(m); }
    // delay_ms() services USB every millisecond, so queued log reports reach the host.
    void flush() {
        usb::flush();
        systick::delay_ms(60);
    }
    bool clear_marker() {
        const flash::Status st = flash::clear_marker();
        if (st != flash::Status::Ok) K2LOG("[DFU] flash status=%d\n", static_cast<int>(st));
        return st == flash::Status::Ok;
    }
    bool marker_cleared() { return !flash::marker_present(); }
    void detach() { usb::detach(); }
    // Three 16-word SRAM windows, 4 words per log line. The bootloader's mailbox, if it has one,
    // is not documented, so look at
    // the start of SRAM, the area just above it and the end of the 96 KB. Nothing above 96 KB:
    // that region is not mapped by default and a read there bus-faults (hung the first build
    // of this dump). Read before the wipe destroys them.
    void dump_ram() {
        static constexpr std::uint32_t kWindows[] = {0x20000000, 0x200001C0, 0x20017FC0};
        for (const std::uint32_t base : kWindows) {
            for (std::uint32_t a = base; a < base + 64; a += 16) {
                const auto* w = reinterpret_cast<const volatile std::uint32_t*>(a);
                K2LOG("[RAM] %08X: %08X %08X %08X %08X\n", static_cast<unsigned>(a),
                             static_cast<unsigned>(w[0]), static_cast<unsigned>(w[1]),
                             static_cast<unsigned>(w[2]), static_cast<unsigned>(w[3]));
            }
            flush();
        }
    }
    // Put what a power cycle would reset into its reset state: FUSB302 software reset, LCD
    // display off and sleep, backlight off. The bootloader brings them up again itself.
    void park() {
        fusb302::park();
        lcd::park();
    }
    // Hand the bootloader power-on clock state: a SYSRESETREQ does not clear everything the app
    // set up. Nothing may use the clocks or interrupts after this point. Then wipe SRAM and
    // reset (sys or watchdog); neither returns.
    void reset(ResetMode mode) {
        __asm volatile("cpsid i" ::: "memory");
        crm::deinit();
        sysreset::wipe_and_reset(mode);
    }
};

}  // namespace

bool enter(ResetMode mode) {
    RealEnv env;
    return enter_sequence(env, mode);
}

}  // namespace k2::dfu
