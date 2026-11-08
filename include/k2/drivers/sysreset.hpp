// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Last-step system reset for the DFU entry: wipes SRAM, then resets either with SYSRESETREQ or
// through the independent watchdog. Also decodes the CRM CTRLSTS reset-cause flags.
//
// Why: after a soft reset the bootloader enumerates but its OUT endpoint stays dead
// (power-on works every time). Leftover SRAM (d) and the reset type (c) are the remaining
// suspects, so both are switchable per DFU_ENTER command.
#pragma once
#include <cstddef>
#include <cstdint>

namespace k2::sysreset {

enum class Mode : std::uint8_t { Sys = 0, Iwdg = 1 };

inline constexpr std::uint32_t kSramBase = 0x20000000;
// The AT32F403A maps 96 KB of SRAM unless EOPB0 selects 224 KB; anything above bus-faulted on
// the first hardware run. Only the default 96 KB (the whole of what the app links against) is touched.
inline constexpr std::uint32_t kSramEnd = 0x20018000;

// "PIN POR SW IWDG WWDG LPWR" for the set CTRLSTS flags (bits 26..31), "-" if none. Always NUL
// terminated, truncated to `n`; returns the length written.
inline std::size_t describe_flags(std::uint32_t ctrlsts, char* out, std::size_t n) {
    struct F { std::uint32_t bit; const char* name; };
    static constexpr F kFlags[] = {{1u << 26, "PIN"}, {1u << 27, "POR"}, {1u << 28, "SW"},
                                   {1u << 29, "IWDG"}, {1u << 30, "WWDG"}, {1u << 31, "LPWR"}};
    if (n == 0) return 0;
    std::size_t len = 0;
    auto put = [&](char c) { if (len + 1 < n) out[len++] = c; };
    for (const F& f : kFlags) {
        if (!(ctrlsts & f.bit)) continue;
        if (len) put(' ');
        for (const char* p = f.name; *p; ++p) put(*p);
    }
    if (len == 0) put('-');
    out[len] = '\0';
    return len;
}

// The one cause to report. PINRSTF is set by every reset (it follows the NRST line), so the more
// specific flags win; with several set (capture_flags() clears them after every boot, so normally
// one POR/PIN/SW/IWDG pair) the order is the severity
// of the surprise: LPWR, WWDG, IWDG, SW, POR, PIN. The F1-style core has no separate brown-out
// flag: a supply dip below the PDR level shows as POR.
enum class Cause : std::uint8_t { None, Pin, Por, Sw, Iwdg, Wwdg, Lpwr };

constexpr Cause cause(std::uint32_t ctrlsts) {
    if (ctrlsts & (1u << 31)) return Cause::Lpwr;
    if (ctrlsts & (1u << 30)) return Cause::Wwdg;
    if (ctrlsts & (1u << 29)) return Cause::Iwdg;
    if (ctrlsts & (1u << 28)) return Cause::Sw;
    if (ctrlsts & (1u << 27)) return Cause::Por;
    if (ctrlsts & (1u << 26)) return Cause::Pin;
    return Cause::None;
}

constexpr const char* cause_name(Cause c) {
    switch (c) {
        case Cause::Pin: return "PIN";
        case Cause::Por: return "POR";
        case Cause::Sw: return "SW";
        case Cause::Iwdg: return "IWDG";
        case Cause::Wwdg: return "WWDG";
        case Cause::Lpwr: return "LPWR";
        default: return "none";
    }
}

// A warm reset: the MCU restarted while the board stayed powered (no POR flag; a pin, software or watchdog reset), so the
// FUSB302 still holds what the previous run wrote. Standby wake and "no flag" count as cold: nothing is trusted there.
constexpr bool warm(std::uint32_t ctrlsts) {
    constexpr std::uint32_t kPor = 1u << 27, kLpwr = 1u << 31, kCauses = (1u << 26) | (1u << 28) | (1u << 29) | (1u << 30);
    return !(ctrlsts & (kPor | kLpwr)) && (ctrlsts & kCauses) != 0;
}

#ifndef K2_HOST_TEST  // firmware-only part (host tests define it)
// Reads CRM CTRLSTS once, keeps it for reset_flags() and clears the flags (RSTFC), so the next
// boot reports only its own cause. Call first thing in main; the DFU wipe and the bootloader
// do not depend on the flags.
void capture_flags();

// CTRLSTS as captured by capture_flags() (the live register if that was never called).
std::uint32_t reset_flags();

// Never returns. Interrupts off, bus faults ignored (FAULTMASK + CCR.BFHFNMIGN, in case the
// upper SRAM is not populated), SRAM kSramBase..kSramEnd (96 KB) zeroed by a register-only loop that
// uses no stack, then the reset: Sys = AIRCR SYSRESETREQ, Iwdg = start the watchdog with a
// ~0.2 ms timeout and spin.
[[noreturn]] void wipe_and_reset(Mode mode);
#endif

}  // namespace k2::sysreset
