// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// USB data-link classifier for a K2 sitting inline between a host and a USB / Thunderbolt dock.
//
// WHAT IS OBSERVABLE (passive, from the CC line and D+/D-): everything the two ends say in PD
// messages on SOP (partner) and SOP' / SOP'' (cable plugs): the UFP/DFP product-type VDOs of
// Discover Identity (USB highest speed, device/host capability, TBT3 alt-mode flag), the cable VDOs,
// Discover SVIDs / Modes (0x8087 Thunderbolt, 0xFF01 DisplayPort), Enter_USB (USB2 / USB3.2 / USB4
// mode and the cable speed the host chose), Enter_Mode / DP Configure, Data_Reset.
//
// WHAT IS NOT: the SuperSpeed / USB4 lanes are not wired to the K2 (it only owns CC1/CC2, D+/D-, VBUS,
// SBU is not read), so the result is what the ends NEGOTIATED or ADVERTISED over PD, never a measured
// link rate. USB 1.1 vs 2.0 vs the 480 Mbit/s high-speed chirp cannot be told from the D+/D- idle
// levels (the ADC sees a pull-up, not the chirp); 3.2 Gen2x2 (20G) is not distinguishable from Gen2x1
// by any PD field; Thunderbolt 4 and 5 are certification levels, reported as USB4 + the 0x8087 alt
// mode at 40G / 80G. A dock that never runs PD data messages (USB3 over a plain Type-C cable with only
// Rp/Rd) leaves just the D+ pull-up.
//
// Pure logic, no heap, host-tested in tests/test_usb_link.cpp. Feed every received PD frame to
// on_message() (all SOP types, both directions), the idle D+/D- levels to on_lines() and a
// detach / new source to on_reset(); read link().
#pragma once
#include <array>
#include <cstdint>
#include <span>

namespace k2::usblink {

enum class Sop : std::uint8_t { Sop, SopP, SopPP };  // same order as k2::pd::Sop

enum class Family : std::uint8_t {
    Unknown,
    LowSpeed,   // D- pull-up: USB 1.0/1.1 low speed (1.5 Mbit/s)
    FullSpeed,  // D+ pull-up only: USB 1.1 full speed or a 2.0 device before chirp, undecidable
    Usb2,       // PD says "USB 2.0 only" (480 Mbit/s capable)
    Usb32,      // USB 3.2 Gen1 (5G) / Gen2 (10G)
    Usb4,       // USB4 Gen2 20G / Gen3 40G
    Usb4v2,     // USB4 Gen4 80G
};
enum class Tbt : std::uint8_t { None, Tbt3, Tbt4, Tbt5 };
// What backs the answer. Negotiated = the host's Enter_USB / Enter_Mode was Accepted/ACKed since the last
// Data_Reset; Advertised = the dock's own Discover Identity / Modes; Hint = a single weak indicator.
enum class Confidence : std::uint8_t { None, Hint, Advertised, Negotiated };
enum class Source : std::uint8_t {
    None, DpDm, Identity, Cable, EnterUsb, EnterTbt, DpAlt, DataReset,
};

struct Link {
    Family family = Family::Unknown;
    std::uint32_t mbps = 0;        // nominal rate at the negotiated or best advertised speed, 0 = unknown (low speed: 2 for 1.5)
    Tbt tbt = Tbt::None;
    Confidence conf = Confidence::None;
    Source source = Source::None;  // the deciding input
    bool cable_limited = false;    // the cable VDO is what holds the speed below the partner's
    bool dp_4lane = false;         // DP alt mode on 4 lanes: SuperSpeed lanes are gone, link is USB 2.0
    bool dp = false;               // DisplayPort alt mode entered (Enter_Mode ACKed); TBT is only claimed from a TBT Enter_Mode
    bool billboard = false;        // partner is a Billboard device (alt mode not entered)
    bool dp_2lane = false;         // DP entered with a 2-lane pin assignment (B, D, F): USB 3 stays alongside
    bool dp_adv = false, tbt_adv = false;  // SVIDs the partner advertised (Discover SVIDs / UFP alt-mode flag)
};

// Display text, e.g. "USB4 40G TBT4", "USB 3.2 Gen2 10G", "USB 1.1/2.0 FS". Returns a static string.
const char* family_name(Family f);
const char* tbt_name(Tbt t);
const char* source_name(Source s);
const char* confidence_name(Confidence c);
// One short line for the UI / remote: "USB4 40G TBT4 (neg)", "USB 3.2 Gen2 10G (adv)", "USB 1.1/2.0 FS (hint)",
// "--" when nothing is known. NUL-terminated, truncated to out.size() - 1; returns the length.
std::size_t format(const Link& l, std::span<char> out);

// Compact UI form for a column already labelled "USB", at most kUiMaxChars cells, no confidence text (the UI
// colours by Link::conf): "3.2 Gen2 10G +DP", "USB4 40G TB4", "TB3/4 40G", "2.0 480M", "FS", "--".
constexpr std::size_t kUiMaxChars = 24;
std::size_t format_ui(const Link& l, std::span<char> out);

// Split readout for the Detail page and the PD tabs: "USB" = the data speed alone ("3.2 Gen2 10G", "USB4 40G",
// "Unkn"), "Alt" = the alternate mode. Alt: modes ENTERED ("DP 2-lane", "DP 4-lane", "DP", "TB3/4") ->
// Entered; none entered but some advertised ("DP, TB (adv)", the tag only when `tag`) -> Advertised;
// "-" -> None. Only real alternate modes count (DisplayPort, Thunderbolt): a vendor SVID (a charging protocol such
// as 0x2717) is not an alt mode and never appears here; it is a protocol of the source or sink (conn::protocol_text).
// The UI colours by the state.
enum class AltState : std::uint8_t { None, Advertised, Entered };
std::size_t format_speed(const Link& l, std::span<char> out);
AltState format_alt(const Link& l, std::span<char> out, bool tag = true);

// The words that decided a negotiated link, for a caller that stores them (the device twin): the Enter_USB data object
// the host sent, the Thunderbolt Enter_Mode VDO, the ACKed DP Configure pin assignment. All zero = nothing negotiated.
struct Raw {
    bool eu_ok = false;           // Enter_USB was Accepted since the last Data_Reset
    std::uint32_t eu_vdo = 0;
    bool tbt_active = false;      // Thunderbolt Enter_Mode ACKed
    std::uint32_t tbt_enter_vdo = 0;
    bool dp_active = false;       // DisplayPort Enter_Mode ACKed
    std::uint8_t dp_pin = 0;      // ACKed pin assignment, A = b0 .. F = b5
};

class Classifier {
public:
    // `payload` = the data objects, little endian, VDM header first for a VDM; control messages have none.
    void on_message(Sop sop, std::uint16_t header, std::span<const std::uint8_t> payload);
    // Idle D+ / D- levels in mV with nothing else known; only used when no PD data decided the link.
    void on_lines(std::int32_t dp_mv, std::int32_t dm_mv);
    void on_reset() { *this = Classifier{}; }
    Link link() const;
    Raw raw() const { return {eu_ok_, eu_vdo_, tbt_active_, tbt_enter_vdo_, dp_active_, dp_pin_}; }

private:
    Link link_core() const;
    // Partner (SOP) Discover Identity.
    bool id_ = false;
    bool id_usb_host_ = false, id_usb_dev_ = false;
    bool ufp_ = false;            // a UFP VDO was parsed
    std::uint8_t ufp_cap_ = 0;    // device capability bits: b0 USB2, b1 Billboard, b2 USB3.2, b3 USB4
    std::uint8_t ufp_alt_ = 0;    // alt modes: b0 TBT3
    std::uint8_t ufp_speed_ = 0;  // USB highest speed 0..4
    // Alternate Mode Adapter VDO (UFP type 5): rank + 1 (rank 0 USB2 .. 2 Gen2), 0 none. Zero means "nothing seen" so a default
    // Classifier is all-zero: the Engine that holds one then lives in .bss, not in the flash-backed .data.
    std::uint8_t ama_rank1_ = 0;
    bool dfp_ = false;
    std::uint8_t dfp_cap_ = 0;    // host capability: b0 USB2, b1 USB3.2, b2 USB4
    // Cable (SOP' / SOP'').
    bool cable_ = false;
    bool cable_active_ = false, cable_intel_ = false;
    std::uint8_t cable_speed_ = 0;  // USB highest speed 0..4
    bool cable_no_usb3_ = false, cable_no_usb4_ = false;  // active cable VDO2
    // Alt modes.
    bool tbt_svid_ = false, dp_svid_ = false;
    bool tbt_mode_ = false;       // Discover Modes ACK for 0x8087
    std::uint32_t tbt_enter_vdo_ = 0;
    bool tbt_pend_ = false, tbt_active_ = false;
    bool dp_pend_ = false, dp_active_ = false;
    std::uint8_t dp_pin_ = 0;     // ACKed DP Configure pin assignment bits (A=b0 .. F=b5), 0 = none
    std::uint8_t dp_cfg_pin_ = 0; // the Configure REQ in flight
    // Enter_USB.
    bool eu_pend_ = false, eu_ok_ = false;
    std::uint32_t eu_vdo_ = 0;
    bool reset_ = false;          // a Data_Reset was seen after the last negotiated state
    std::int32_t dp_mv_ = 0, dm_mv_ = 0;   // idle line levels; only compared with >= 2000, so 0 reads as "unknown" like -1 did

    void on_vdm(Sop sop, std::span<const std::uint8_t> payload);
    int partner_rank() const;     // -1 unknown
    int cable_rank(int partner) const;
    void on_identity(Sop sop, std::span<const std::uint32_t> v, std::size_t n);
};

}  // namespace k2::usblink
