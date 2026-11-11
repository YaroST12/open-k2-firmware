// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The query variants a cable read tries: which PD header revision and which structured-VDM version a Discover
// Identity request to SOP' carries. Cables differ in what they acknowledge: most answer the PD 3.x header with
// SVDM 2.0, some old ones only PD 2.0 with SVDM 1.0, and a 100 W C-to-C cable answers only a PD 3.x header that
// carries SVDM 1.0. The e-marker scan (src/drivers/fusb302.cpp) and the learn plan (emk_learn.hpp) walk this one
// table in order and then keep the variant that was answered for every later request of the session.
// Pure constexpr code, host-tested in tests/test_emk_variant.cpp.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace k2::emk_variant {

// `spec_rev` is the PD header specification-revision field (1 = PD 2.0, 2 = PD 3.x), `svdm` the structured VDM
// major version (1 or 2).
struct Variant {
    std::uint8_t spec_rev = 2;
    std::uint8_t svdm = 2;
    constexpr bool operator==(const Variant&) const = default;
};

// Per orientation, in this order. Each attempt costs one request window (about 80 ms of waiting plus the 2 ms
// FIFO flush, ~235 ms measured on hardware), so a silent line costs kAttempts windows per orientation.
inline constexpr std::size_t kAttempts = 3;
inline constexpr std::array<Variant, kAttempts> kTable{{
    {2, 2},   // PD 3.x header, SVDM 2.0: what a current source sends
    {2, 1},   // PD 3.x header, SVDM 1.0: the Baseus 100 W C-to-C cable answers only this
    {1, 1},   // PD 2.0 header, SVDM 1.0: old cables
}};

// The first variant, used where no cable has answered yet.
inline constexpr Variant kFirst = kTable[0];

// Structured VDM request header: SVID, structured bit, version (SVDM 2.0 = major 1 in bits 14:13), command,
// object position 0.
constexpr std::uint32_t vdm_header(std::uint16_t svid, std::uint8_t cmd, Variant v) {
    return (std::uint32_t(svid) << 16) | 0x8000u | (v.svdm >= 2 ? 0x2000u : 0u) | (cmd & 0x1Fu);
}

// PD header of a one-object VDM to SOP' (message id 0, cable-plug role bits clear).
constexpr std::uint16_t vdm_pd_header(Variant v) {
    return static_cast<std::uint16_t>(0x0Fu | (unsigned(v.spec_rev & 3u) << 6) | (1u << 12));
}

// Discover Identity request to SOP' as a FIFO token stream: SOP1 SOP1 SOP3 SOP3, PACKSYM|6, PD header, VDM header
// (little endian), JAM_CRC, EOP, TXOFF.
constexpr std::array<std::uint8_t, 14> discover_identity_frame(Variant v) {
    const std::uint16_t h = vdm_pd_header(v);
    const std::uint32_t w = vdm_header(0xFF00, 1, v);
    return {0x12, 0x12, 0x1B, 0x1B, 0x86,
            std::uint8_t(h & 0xFF), std::uint8_t(h >> 8),
            std::uint8_t(w & 0xFF), std::uint8_t((w >> 8) & 0xFF), std::uint8_t((w >> 16) & 0xFF), std::uint8_t(w >> 24),
            0xFF, 0x14, 0xFE};
}

// The variant as the report words: "PD 3.x" / "PD 2.0", and "2.0" / "1.0".
constexpr std::string_view header_text(Variant v) { return v.spec_rev == 1 ? "PD 2.0" : "PD 3.x"; }
constexpr std::string_view svdm_text(Variant v) { return v.svdm >= 2 ? "2.0" : "1.0"; }

// What a cable answered: the orientation of the scan and the table row. One byte each on the wire (remote
// GET_EMARKER part 2 tail, the learn summary tail), in this order; all zero = nothing answered / not known.
struct Answered {
    std::uint8_t orientation = 0;   // 0 none, 1 = A (CC1 BMC, CC2 VCONN), 2 = B (CC2 BMC, CC1 VCONN)
    std::uint8_t spec_rev = 0;      // 0 none, 1 = PD 2.0 header, 2 = PD 3.x header
    std::uint8_t svdm = 0;          // 0 none, 1 = SVDM 1.0, 2 = SVDM 2.0
    std::uint8_t attempt = 0;       // 1-based position in the table within the orientation, 0 none
    constexpr bool valid() const { return orientation != 0; }
    constexpr bool operator==(const Answered&) const = default;
};

// `attempt` is the 0-based table index (an index off the table gives "nothing answered").
constexpr Answered answered_from(std::uint8_t orientation, std::size_t attempt) {
    if (orientation == 0 || orientation > 2 || attempt >= kAttempts) return {};
    const Variant v = kTable[attempt];
    return {orientation, v.spec_rev, v.svdm, std::uint8_t(attempt + 1)};
}

// A fixed small text, built without the heap so it can be checked at compile time and logged with %s.
struct Text {
    std::array<char, 72> s{};
    std::uint8_t n = 0;
    constexpr std::string_view view() const { return {s.data(), n}; }
    constexpr const char* c_str() const { return s.data(); }
    constexpr void add(std::string_view t) {
        for (const char c : t) if (n + 1u < s.size()) s[n++] = c;
    }
    constexpr void add(char c) { add(std::string_view(&c, 1)); }
};

constexpr char orientation_letter(const Answered& a) { return a.orientation == 2 ? 'B' : 'A'; }

// "A (CC1 BMC, CC2 VCONN)" / "B (CC2 BMC, CC1 VCONN)"; empty when nothing answered.
constexpr Text orientation_text(const Answered& a) {
    Text t;
    if (!a.valid()) return t;
    t.add(orientation_letter(a));
    t.add(a.orientation == 2 ? " (CC2 BMC, CC1 VCONN)" : " (CC1 BMC, CC2 VCONN)");
    return t;
}

// "PD 3.x header, SVDM 1.0, attempt 2"; empty when nothing answered.
constexpr Text combo_text(const Answered& a) {
    Text t;
    if (!a.valid()) return t;
    const Variant v{a.spec_rev, a.svdm};
    t.add(header_text(v));
    t.add(" header, SVDM ");
    t.add(svdm_text(v));
    t.add(", attempt ");
    t.add(char('0' + (a.attempt % 10)));
    return t;
}

// The one-row form for the cable screen: "A, PD3, SVDM 1.0"; empty when nothing answered.
constexpr Text short_text(const Answered& a) {
    Text t;
    if (!a.valid()) return t;
    t.add(orientation_letter(a));
    t.add(a.spec_rev == 1 ? ", PD2, SVDM " : ", PD3, SVDM ");
    t.add(svdm_text(Variant{a.spec_rev, a.svdm}));
    return t;
}

}  // namespace k2::emk_variant
