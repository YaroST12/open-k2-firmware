// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// k2::eeprom::locate: where a linear EEPROM offset lands on the I2C wire (7-bit device address and the word-address
// bytes) for the 24Cxx family, and which reads are refused before they reach the bus. Expected values are literals from the
// 24Cxx convention: 8-bit address 0xA0 = 7-bit 0x50; the 24C01..24C16 take a one-byte word address and carry address bits
// 8..10 in the low bits of the device address (24C16: 8 blocks 0x50..0x57); the 24C32 and larger take two word-address bytes.
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

int main() {
    test_one_byte_blocks();
    test_one_byte_refusals();
    test_one_byte_end_of_array();
    test_select_pins();
    test_two_byte();
    test_length_and_mode();
    return test_report("test_eeprom_calc");
}
