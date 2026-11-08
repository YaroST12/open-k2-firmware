// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// OPPO VOOC / SuperVOOC phone-side slave, pure. The adapter is the clock master on D+: 9 low
// pulses (25-40 us, 0.46 / 0.54 ms apart) per frame; D- carries one bit per pulse, latched just before the
// next D+ fall (the last bit 300 us after the last pulse). An adapter command is 8 bits + a trailing 0;
// the phone answers about 9 ms later with a 9 bit word in the clock of the next frame, driving each D- bit
// about 50 us after the D+ rise of its pulse. The reply words come from the real OnePlus 15 session
// (tests/fixtures/oneplus15_svooc_init.edges, the accepted attempt at 61.57 s), replayed per command.
#pragma once
#include <array>
#include <cstdint>
#include <optional>

namespace k2::vooc {

inline constexpr int kFrameBits = 9;
inline constexpr std::uint16_t kCableOk = 0x04D;   // reply to the 3rd 0x88 that makes the adapter ramp (0x00D rejects)
inline constexpr std::uint32_t kFrameGapUs = 700;      // D+ high this long (> the 0.54 ms in-frame gap): the next fall starts a frame
inline constexpr std::uint32_t kLastBitDelayUs = 300;  // last command bit: this long after the last pulse rises
inline constexpr std::uint32_t kReplyDelayUs = 50;     // phone drives D- this long after the D+ rise
inline constexpr std::uint32_t kReplyWaitUs = 12000;   // the reply slot starts about 9 ms after the command

enum class Mode : std::uint8_t { Vooc, SuperVooc5A, SuperVooc65A };

constexpr std::uint8_t frame_cmd(std::uint16_t frame) { return static_cast<std::uint8_t>(frame >> 1); }   // bit 0 is the trailing 0

// Hardware seam of the slave: the firmware implements it over D+ (PA7 or PB0) and D- (PB2 / PB1 drive),
// the host tests over a fake adapter clock.
//   wait_frame(t)  D+ has been high for kFrameGapUs, then falls: true on that fall, false after t us
//   wait_fall(t) / wait_rise(t)   next D+ edge, false after t us; wait_fall also latches dm_before_fall()
//   dm_before_fall()  D- level sampled the last time before the fall wait_fall returned on
//   dm()  D- now; delay_us(n); drive_dm(level) takes D- (phone side); release_dm() lets go of it
template <class Hw>
int rx_command(Hw& hw, std::uint32_t first_timeout_us) {
    if (!hw.wait_frame(first_timeout_us)) return -1;
    unsigned v = 0;
    for (int k = 1; k < kFrameBits; ++k) {
        if (!hw.wait_fall(1000)) return -1;
        v = v << 1 | (hw.dm_before_fall() ? 1u : 0u);
    }
    if (!hw.wait_rise(1000)) return -1;
    hw.delay_us(kLastBitDelayUs);
    v = v << 1 | (hw.dm() ? 1u : 0u);
    return static_cast<int>(v);
}

// Reply in the next frame's clock. D- goes high (the phone-side idle) right away, then each bit follows its
// pulse. False when no clock came within kReplyWaitUs or it stopped mid-frame; D- is released either way.
template <class Hw>
bool tx_reply(Hw& hw, std::uint16_t word) {
    hw.drive_dm(true);
    bool ok = hw.wait_frame(kReplyWaitUs);
    for (int i = kFrameBits - 1; i >= 0 && ok; --i) {
        if (i != kFrameBits - 1) ok = hw.wait_fall(1000);
        if (ok) ok = hw.wait_rise(1000);
        if (!ok) break;
        hw.delay_us(kReplyDelayUs);
        hw.drive_dm(((word >> i) & 1) != 0);
    }
    if (ok) hw.delay_us(kLastBitDelayUs + 100);   // the adapter latches the last bit after the last pulse
    hw.release_dm();
    return ok;
}

class Responder {
public:
    explicit Responder(Mode m) : mode_(m) {}
    void reset() { seen_ = {}; }
    Mode mode() const { return mode_; }

    // Reply word for an adapter command, or nullopt when the phone stays silent. The n-th occurrence of a
    // command gets the n-th word of its list (the last one repeats): the real phone's data words change.
    std::optional<std::uint16_t> reply(std::uint8_t cmd) {
        if (mode_ == Mode::Vooc) {
            if (cmd == 0xA8 || cmd == 0xAC || cmd == 0xA6) return std::uint16_t{0x00D};
            return std::nullopt;
        }
        const Entry* e = find(cmd);
        if (!e) return std::nullopt;
        int& n = seen_[cmd & 0x3F];
        const std::uint16_t w = e->words[n < e->count ? n : e->count - 1];
        if (n < 255) ++n;
        return w;
    }
    // A command only an SVOOC adapter sends (the ident 0xA8 is shared with VOOC, 0xAC / 0xA6 belong to VOOC alone).
    static constexpr bool svooc_only(std::uint8_t cmd) {
        for (const auto& e : kTable) if (e.cmd == cmd) return cmd != 0xA8;
        return false;
    }
    // True when the word is the accepting answer of the cable check.
    static constexpr bool accepts(std::uint8_t cmd, std::uint16_t word) { return cmd == 0x88 && word == kCableOk; }

private:
    struct Entry { std::uint8_t cmd, count; std::uint16_t words[4]; };
    static constexpr std::array<Entry, 13> kTable{{
        {0xA8, 1, {0x00D}},
        {0x98, 4, {0x073, 0x023, 0x010, 0x010}},
        {0x9E, 1, {0x010}},
        {0x80, 1, {0x010}},
        {0x94, 1, {0x010}},
        {0x8A, 3, {0x028, 0x048, 0x052}},
        {0x8E, 1, {0x010}},
        {0x9A, 1, {0x030}},
        {0x82, 3, {0x074, 0x00B, 0x035}},   // then the steady-state poll value
        {0x84, 2, {0x071, 0x031}},
        {0x86, 2, {0x06B, 0x03A}},
        {0x88, 3, {0x05D, 0x042, kCableOk}},
        {0x8C, 2, {0x046, 0x066}},
    }};
    static const Entry* find(std::uint8_t cmd) {
        for (const auto& e : kTable) if (e.cmd == cmd) return &e;
        return nullptr;
    }
    Mode mode_;
    std::array<int, 64> seen_{};
};

}  // namespace k2::vooc
