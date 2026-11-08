// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// FUSB302 register profile, as pure data (host-tested in tests/test_pd_phy.cpp).
//
// The chip stays passive on the meter screen:
// Auto-CRC off, no Rd change, SOP/SOP'/SOP'' (and the debug variants) enabled for receive, only
// "good CRC received" and hard/soft reset interrupts reach INT_N. Auto-CRC and transmit are
// switched on only while an active operation runs. This header holds the register addresses, the
// bit values and the two profiles; src/drivers/fusb302.cpp only writes them.
#pragma once
#include <array>
#include <cstdint>

namespace k2::pd::phy {

enum Reg : std::uint8_t {
    kDeviceId = 0x01, kSwitches0 = 0x02, kSwitches1 = 0x03, kMeasure = 0x04, kControl0 = 0x06,
    kControl1 = 0x07, kControl3 = 0x09, kMask = 0x0A, kPower = 0x0B, kReset = 0x0C, kMaskA = 0x0E,
    kMaskB = 0x0F, kInterruptA = 0x3E, kInterruptB = 0x3F, kStatus0 = 0x40, kStatus1 = 0x41,
    kInterrupt = 0x42, kFifos = 0x43,
};

// SWITCHES0: pull-downs and CC measure select.
inline constexpr std::uint8_t kPdwn1 = 1u << 0, kPdwn2 = 1u << 1, kMeasCc1 = 1u << 2, kMeasCc2 = 1u << 3;
inline constexpr std::uint8_t kVconnCc1 = 1u << 4, kVconnCc2 = 1u << 5;   // the chip's VCONN supply onto CC1 / CC2
inline constexpr std::uint8_t kSw0PullDowns = kPdwn1 | kPdwn2;
// SWITCHES1: BMC transmit routing, automatic GoodCRC and the spec revision (01 = PD 2.0) the
// chip puts in its own GoodCRC headers. Power role (bit 7) and data role (bit 4) stay 0: sink, UFP.
inline constexpr std::uint8_t kTxCc1 = 1u << 0, kTxCc2 = 1u << 1, kAutoCrc = 1u << 2, kSpecRev1 = 1u << 5;
inline constexpr std::uint8_t kTxStart = 1u << 0, kTxFlush = 1u << 6;      // CONTROL0
inline constexpr std::uint8_t kEnSop = 3u << 0, kRxFlush = 1u << 2;        // CONTROL1: ENSOP1 | ENSOP2
inline constexpr std::uint8_t kEnSopDebug = 3u << 5;                       // CONTROL1: ENSOP1DB | ENSOP2DB
inline constexpr std::uint8_t kPwrAll = 0x0F, kSwReset = 1u << 0, kPdReset = 1u << 1;
inline constexpr std::uint8_t kSendHardReset = 1u << 6;                    // CONTROL3
inline constexpr std::uint8_t kRetries3Auto = (3u << 1) | 1u;              // CONTROL3
inline constexpr std::uint8_t kMdacVconnMv = 62;                           // MEASURE: 62 * 42 mV = 2.6 V
inline constexpr std::uint8_t kMdac210mV = 5;                              // MEASURE: 5 * 42 mV

// Interrupt masks of the passive profile (a set bit masks the source).
inline constexpr std::uint8_t kMaskPassive = 0xEF;   // only M_CRC_CHK (bit 4) open: a good packet came in
inline constexpr std::uint8_t kMaskAPassive = 0xFC;  // only HARDRST and SOFTRST open
inline constexpr std::uint8_t kMaskBPassive = 0x01;  // GCRCSENT masked
inline constexpr std::uint8_t kControl0Passive = 0x00;   // INT_MASK clear: INT_N enabled
inline constexpr std::uint8_t kControl1Passive = kEnSop | kRxFlush | kEnSopDebug;  // 0x67

constexpr std::uint8_t sw0_measure(std::uint8_t cc) { return cc == 1 ? kMeasCc1 : cc == 2 ? kMeasCc2 : 0; }
constexpr std::uint8_t tx_route(std::uint8_t cc) { return cc == 1 ? kTxCc1 : cc == 2 ? kTxCc2 : 0; }

// SWITCHES0 of the passive profile: no Rd (plain boot), `cc` measured. Rd is the driver's
// decision (pd_ccmode.hpp).
constexpr std::uint8_t sw0(std::uint8_t cc) { return sw0_measure(cc); }
// SWITCHES1 for the active line `cc` (0 = none yet), with or without Auto-CRC.
// CONTROL1: passive also receives SOP' and SOP'' and the debug SOPs (0x67; SOP itself is always on).
// With Auto-CRC on the chip would GoodCRC a cable's SOP' message and pose as the e-marker, so the
// SOP'/SOP'' enables go off (0x04 = RX_FLUSH only; flush self-clears).
// `sop_prime` (the e-marker spoof is armed): SOP' stays enabled with Auto-CRC on, so the chip itself GoodCRCs the
// source's Discover Identity (and Soft_Reset) to the cable inside the 195 us; SOP'' stays off.
// `cable_read` (our own e-marker read, VCONN up, Auto-CRC on so the chip GoodCRCs the cable's reply): the chip listens on
// SOP' and SOP'' like the passive profile, whatever Auto-CRC says.
constexpr std::uint8_t control1(bool auto_crc, bool sop_prime = false, bool cable_read = false) {
    return auto_crc && !cable_read ? std::uint8_t(kRxFlush | (sop_prime ? 1u : 0u)) : kControl1Passive;
}
constexpr std::uint8_t sw1(std::uint8_t cc, bool auto_crc) {
    return std::uint8_t(kSpecRev1 | tx_route(cc) | (auto_crc ? kAutoCrc : 0));
}

struct RegWrite { Reg reg; std::uint8_t value; };

// The full passive configuration, in the order the driver writes it: power, CC routing, masks,
// SOP enables, then PD_RESET so the PD logic starts from the new setup. Used after SW_RES at boot
// and after every detach. Auto-CRC is off.
constexpr std::array<RegWrite, 11> passive_config(std::uint8_t cc) {
    return {{
        {kPower, kPwrAll},
        {kSwitches0, sw0(cc)},
        {kSwitches1, sw1(cc, false)},
        {kControl3, kRetries3Auto},
        {kMaskA, kMaskAPassive},
        {kMaskB, kMaskBPassive},
        {kMask, kMaskPassive},
        {kControl0, kControl0Passive},
        {kControl1, kControl1Passive},
        {kMeasure, kMdac210mV},
        {kReset, kPdReset},
    }};
}

}  // namespace k2::pd::phy
