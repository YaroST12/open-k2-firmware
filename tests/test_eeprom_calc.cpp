// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// k2::eeprom::locate: where a linear EEPROM offset lands on the I2C wire (7-bit device address and the word-address
// bytes) for the 24Cxx family, and which reads are refused before they reach the bus. Expected values are literals from the
// 24Cxx convention: 8-bit address 0xA0 = 7-bit 0x50; the 24C01..24C16 take a one-byte word address and carry address bits
// 8..10 in the low bits of the device address (24C16: 8 blocks 0x50..0x57); the 24C32 and larger take two word-address bytes.
#include <array>
#include <span>

#include "k2/drivers/eeprom_calc.hpp"
#include "test_util.h"

using namespace k2::eeprom;

static Addr at(std::uint8_t abytes, std::uint8_t sel, std::uint16_t off, std::size_t len, bool expect_ok = true) {
    Addr a;
    CHECK_EQ(int(locate(abytes, sel, off, len, a)), int(expect_ok));
    return a;
}

static void test_one_byte_blocks() {
    Addr a = at(1, 0, 0x000, 16);
    CHECK_EQ(a.dev7, 0x50); CHECK_EQ(a.nword, 1); CHECK_EQ(a.word[0], 0x00);
    a = at(1, 0, 0x0F0, 16);                       // last 16 bytes of block 0
    CHECK_EQ(a.dev7, 0x50); CHECK_EQ(a.word[0], 0xF0);
    a = at(1, 0, 0x400, 32);                       // 0x400 is the first byte of block 4
    CHECK_EQ(a.dev7, 0x54); CHECK_EQ(a.nword, 1); CHECK_EQ(a.word[0], 0x00);
    a = at(1, 0, 0x4A0, 32);
    CHECK_EQ(a.dev7, 0x54); CHECK_EQ(a.word[0], 0xA0);
    a = at(1, 0, 0x7E0, 32);                       // the last 32 bytes of a 24C16
    CHECK_EQ(a.dev7, 0x57); CHECK_EQ(a.word[0], 0xE0);
}

static void test_one_byte_refusals() {
    at(1, 0, 0x0F0, 32, false);                    // 0xF0 + 32 would cross the 256 byte block boundary
    at(1, 0, 0x0FF, 2, false);
    at(1, 0, 0x0F0, 16);                           // ending exactly on the boundary is fine
}

static void test_one_byte_end_of_array() {
    at(1, 0, 0x7E1, 31);                           // ends exactly at 2 KB
    at(1, 0, 0x7E1, 32, false);                    // one byte past the array
    at(1, 0, 0x800, 1, false);                     // a 9th block does not exist
}

static void test_select_pins() {
    Addr a = at(1, 2, 0x010, 8);                   // 24C02 with A1 high: 0x52
    CHECK_EQ(a.dev7, 0x52); CHECK_EQ(a.word[0], 0x10);
    a = at(1, 3, 0x200, 8);                        // sel adds to the block: 0x50 + 3 + 2
    CHECK_EQ(a.dev7, 0x55);
    at(1, 6, 0x200, 8, false);                     // 6 + 2 = 8 leaves 0x50..0x57
    at(1, 8, 0x000, 8, false);                     // sel is 0..7
}

static void test_two_byte() {
    Addr a = at(2, 0, 0x1234, 32);
    CHECK_EQ(a.dev7, 0x50); CHECK_EQ(a.nword, 2); CHECK_EQ(a.word[0], 0x12); CHECK_EQ(a.word[1], 0x34);
    a = at(2, 1, 0x0000, 1);
    CHECK_EQ(a.dev7, 0x51); CHECK_EQ(a.word[0], 0x00); CHECK_EQ(a.word[1], 0x00);
    a = at(2, 0, 0xFFE0, 32);                      // ends exactly at 64 KB
    CHECK_EQ(a.word[0], 0xFF); CHECK_EQ(a.word[1], 0xE0);
    at(2, 0, 0xFFE1, 32, false);
    a = at(2, 0, 0x0100, 32);                      // two-byte addressing has no 256 byte block rule
    CHECK_EQ(a.word[0], 0x01);
    a = at(2, 0, 0x01F0, 32);
    CHECK_EQ(a.word[0], 0x01); CHECK_EQ(a.word[1], 0xF0);
    at(2, 8, 0, 1, false);
}

static void test_length_and_mode() {
    at(1, 0, 0, 0, false);
    at(2, 0, 0, 0, false);
    at(1, 0, 0, kMaxChunk);
    at(1, 0, 0, kMaxChunk + 1, false);
    at(0, 0, 0, 8, false);
    at(3, 0, 0, 8, false);
    CHECK_EQ(int(kMaxChunk), 32);                  // divides 256: a host that steps by 32 never crosses a block
}

// ---- cal_clear: the one compare-and-swap write of the calibration repair ----

// A 24C16 page 0 (16 bytes) behind the three operations the sequence may use. Everything the sequence does lands in the counters.
struct FakeChip {
    std::uint8_t mem[16] = {0x11, 0x22, 0x33, 0x44, 0x00, 0x60, 0xF0, 0xFF, 0xCF, 0xFE, 0xFF, 0xFF, 0xA2, 0x4E, 0xFB, 0xFF};
    bool read_ok = true, write_ok = true, ready = true, ignore_write = false;
    int reads = 0, writes = 0, waits = 0;
    std::uint8_t write_word = 0xEE;
    std::uint8_t write_data[kCalLen];
    bool read(std::uint8_t word, std::span<std::uint8_t> out) {
        reads++;
        if (!read_ok) return false;
        for (std::size_t i = 0; i < out.size(); i++) out[i] = mem[word + i];
        return true;
    }
    bool write(std::uint8_t word, std::span<const std::uint8_t> data) {
        writes++; write_word = word;
        for (std::size_t i = 0; i < data.size() && i < kCalLen; i++) write_data[i] = data[i];
        if (!write_ok) return false;
        if (!ignore_write) for (std::size_t i = 0; i < data.size(); i++) mem[word + i] = data[i];
        return true;
    }
    bool wait_ready() { waits++; return ready; }
};

// The corrupt trims of the unit to repair (literals from the brief): -1024000, -305, -307550 as int32 little endian.
static constexpr std::array<std::uint8_t, kCalLen> kCorrupt = {0x00, 0x60, 0xF0, 0xFF, 0xCF, 0xFE, 0xFF, 0xFF, 0xA2, 0x4E, 0xFB, 0xFF};

static CalResult clear(FakeChip &c, const std::array<std::uint8_t, kCalLen> &expect) {
    return cal_clear(c, std::span<const std::uint8_t, kCalLen>(expect));
}

static void test_cal_constants() {
    CHECK_EQ(int(kCalDev7), 0x50);
    CHECK_EQ(int(kCalWord), 0x04);
    CHECK_EQ(int(kCalLen), 12);
    CHECK_EQ(int(kCalWord + kCalLen), 16);         // ends on the 16-byte page boundary of a 24C16: a page write cannot wrap
    for (std::uint8_t b : kCalZero) CHECK_EQ(int(b), 0);
}

static void test_cal_match_writes_zeros_once() {
    FakeChip c;
    CalResult r = clear(c, kCorrupt);
    CHECK_EQ(int(r.err), int(CalErr::Ok));
    CHECK_EQ(c.writes, 1);
    CHECK_EQ(int(c.write_word), 0x04);
    for (std::uint8_t b : c.write_data) CHECK_EQ(int(b), 0);
    CHECK_EQ(c.waits, 1);
    CHECK_EQ(c.reads, 2);                          // before and readback
    for (std::size_t i = 0; i < kCalLen; i++) { CHECK_EQ(int(r.before[i]), int(kCorrupt[i])); CHECK_EQ(int(r.after[i]), 0); }
    CHECK_COND(r.before_valid); CHECK_COND(r.after_valid);
    CHECK_EQ(int(c.mem[0]), 0x11); CHECK_EQ(int(c.mem[3]), 0x44);   // bytes below 0x04 untouched
    CHECK_EQ(int(c.mem[4]), 0); CHECK_EQ(int(c.mem[15]), 0);
}

static void test_cal_mismatch_never_writes() {
    for (std::size_t i = 0; i < kCalLen; i++) {    // any single wrong byte of the expectation refuses
        FakeChip c;
        auto e = kCorrupt;
        e[i] ^= 0x01;
        CalResult r = clear(c, e);
        CHECK_EQ(int(r.err), int(CalErr::Mismatch));
        CHECK_EQ(c.writes, 0); CHECK_EQ(c.waits, 0); CHECK_EQ(c.reads, 1);
        CHECK_COND(r.before_valid); CHECK_COND(!r.after_valid);
        for (std::size_t k = 0; k < kCalLen; k++) CHECK_EQ(int(r.before[k]), int(kCorrupt[k]));   // reports what it read
        CHECK_EQ(int(c.mem[4]), 0x00); CHECK_EQ(int(c.mem[5]), 0x60);                              // chip untouched
    }
    FakeChip z;                                    // an already-zero area is a mismatch for the corrupt expectation
    for (auto &b : z.mem) b = 0;
    CHECK_EQ(int(clear(z, kCorrupt).err), int(CalErr::Mismatch));
    CHECK_EQ(z.writes, 0);
}

static void test_cal_failed_read_never_compares_or_writes() {
    FakeChip c;
    c.read_ok = false;
    std::array<std::uint8_t, kCalLen> zero{};      // a zeroed result buffer must not pass for "the chip holds zeros"
    CalResult r = clear(c, zero);
    CHECK_EQ(int(r.err), int(CalErr::ReadFail));
    CHECK_EQ(c.writes, 0); CHECK_COND(!r.before_valid); CHECK_COND(!r.after_valid);
}

static void test_cal_write_failures_still_read_back() {
    FakeChip c;
    c.write_ok = false;                            // a NACK part way: the chip may have taken some bytes, so look again
    CalResult r = clear(c, kCorrupt);
    CHECK_EQ(int(r.err), int(CalErr::WriteFail));
    CHECK_EQ(c.writes, 1); CHECK_EQ(c.waits, 1); CHECK_EQ(c.reads, 2); CHECK_COND(r.after_valid);

    FakeChip t;
    t.ready = false;                               // the write cycle never ends
    r = clear(t, kCorrupt);
    CHECK_EQ(int(r.err), int(CalErr::Timeout));
    CHECK_EQ(t.writes, 1); CHECK_EQ(t.reads, 2);

    FakeChip v;
    v.ignore_write = true;                         // accepted but not stored (write protect): the readback is the judge
    r = clear(v, kCorrupt);
    CHECK_EQ(int(r.err), int(CalErr::Verify));
    CHECK_COND(r.after_valid); CHECK_EQ(int(r.after[1]), 0x60);
}

static std::array<std::uint8_t, kCalLen> trim_bytes(std::int32_t voltage_gain, std::int32_t current_zero,
                                                     std::int32_t current_gain) {
    std::array<std::uint8_t, kCalLen> b{};
    auto put = [&b](std::size_t off, std::int32_t v) {
        const auto u = static_cast<std::uint32_t>(v);
        for (std::size_t i = 0; i < 4; i++) b[off + i] = static_cast<std::uint8_t>(u >> (8 * i));
    };
    put(0, voltage_gain);
    put(4, current_zero);
    put(8, current_gain);
    return b;
}

static void check_trim(k2::power::Chip chip, const std::array<std::uint8_t, kCalLen> &b, CalState state,
                       std::int32_t voltage_gain, std::int32_t current_zero, std::int32_t current_gain) {
    const CalInfo c = classify_cal(chip, true, b);
    CHECK_EQ(int(c.state), int(state));
    CHECK_EQ(c.voltage_gain, voltage_gain);
    CHECK_EQ(c.current_zero, current_zero);
    CHECK_EQ(c.current_gain, current_gain);
}

static void test_cal_classifier_healthy() {
    const auto zero = trim_bytes(0, 0, 0);
    check_trim(k2::power::Chip::Ina228, zero, CalState::Ok, 0, 0, 0);
    check_trim(k2::power::Chip::Ina226, zero, CalState::Ok, 0, 0, 0);
    check_trim(k2::power::Chip::Ina237, zero, CalState::Ok, 0, 0, 0);

    const auto small = trim_bytes(-37, 21, 94);
    for (const k2::power::Chip chip : {k2::power::Chip::Ina228, k2::power::Chip::Ina226, k2::power::Chip::Ina237})
        check_trim(chip, small, CalState::Ok, -37, 21, 94);

    check_trim(k2::power::Chip::Ina228, trim_bytes(20'480, 3'072, -30'720), CalState::Ok,
               20'480, 3'072, -30'720);   // exactly 10% gain and 1% zero are allowed
}

static void test_cal_classifier_corrupt_examples() {
    const auto unit = trim_bytes(-1'024'000, -305, -307'550);
    check_trim(k2::power::Chip::Ina226, unit, CalState::Corrupt, -1'024'000, -305, -307'550);
    check_trim(k2::power::Chip::Ina228, trim_bytes(20'481, 0, 0), CalState::Corrupt, 20'481, 0, 0);
    check_trim(k2::power::Chip::Ina226, trim_bytes(0, 3'073, 0), CalState::Corrupt, 0, 3'073, 0);
    check_trim(k2::power::Chip::Ina237, trim_bytes(0, 0, -61'441), CalState::Corrupt, 0, 0, -61'441);
    check_trim(k2::power::Chip::Ina226, trim_bytes(INT32_MAX, 0, 0), CalState::Corrupt, INT32_MAX, 0, 0);
    check_trim(k2::power::Chip::Ina226, trim_bytes(INT32_MIN, 0, 0), CalState::Corrupt, INT32_MIN, 0, 0);
}

static void test_cal_classifier_absent_and_unknown() {
    const auto values = trim_bytes(1, -2, 3);
    check_trim(k2::power::Chip::None, values, CalState::Unknown, 1, -2, 3);
    const CalInfo absent = classify_cal(k2::power::Chip::Ina226, false, values);
    CHECK_EQ(int(absent.state), int(CalState::Absent));
    CHECK_EQ(absent.voltage_gain, 0);
    CHECK_EQ(absent.current_zero, 0);
    CHECK_EQ(absent.current_gain, 0);
}

int main() {
    test_cal_constants();
    test_cal_match_writes_zeros_once();
    test_cal_mismatch_never_writes();
    test_cal_failed_read_never_compares_or_writes();
    test_cal_write_failures_still_read_back();
    test_cal_classifier_healthy();
    test_cal_classifier_corrupt_examples();
    test_cal_classifier_absent_and_unknown();
    test_one_byte_blocks();
    test_one_byte_refusals();
    test_one_byte_end_of_array();
    test_select_pins();
    test_two_byte();
    test_length_and_mode();
    return test_report("test_eeprom_calc");
}
