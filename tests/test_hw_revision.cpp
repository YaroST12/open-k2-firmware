// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#include "k2/core/hw_revision.hpp"

#include <array>
#include <cstdint>

#include "test_util.h"

namespace {

using k2::hw_revision::decode_minor;

std::array<std::uint8_t, 256> synthetic_table() {
    std::array<std::uint8_t, 256> table{};
    for (std::size_t i = 0; i < table.size(); ++i)
        table[i] = static_cast<std::uint8_t>(i * 73u + 41u);
    return table;
}

std::uint8_t raw_for(const std::array<std::uint8_t, 256> &table, std::uint8_t plain) {
    for (std::size_t i = 0; i < table.size(); ++i)
        if (table[i] == plain) return static_cast<std::uint8_t>(i);
    CHECK_EQ(false, true);
    return 0;
}

void put_serial(std::array<std::uint8_t, 1024> &page,
                const std::array<std::uint8_t, 256> &table,
                std::uint32_t serial) {
    for (std::size_t i = 0; i < 6; ++i) {
        page[0xFA + i] = raw_for(table, static_cast<std::uint8_t>('0' + serial % 10));
        serial /= 10;
    }
}

void test_minor_ranges() {
    const auto table = synthetic_table();
    std::array<std::uint8_t, 1024> page{};
    put_serial(page, table, 100199);
    page[0xE4] = raw_for(table, 7);
    auto d = decode_minor(table, page);
    CHECK_EQ(d.serial_valid && d.minor == 1, true);

    put_serial(page, table, 200020);
    page[0xE4] = raw_for(table, 99);
    d = decode_minor(table, page);
    CHECK_EQ(d.serial_valid && d.minor == 3, true);

    put_serial(page, table, 200041);
    page[0xE4] = raw_for(table, 254);
    d = decode_minor(table, page);
    CHECK_EQ(d.serial_valid && d.minor == 0, true);
}

void test_invalid_page() {
    const auto table = synthetic_table();
    std::array<std::uint8_t, 1024> page{};
    CHECK_EQ(decode_minor(table, page).serial_valid, false);

    page.fill(0xFF);
    CHECK_EQ(decode_minor(table, page).serial_valid, false);

    put_serial(page, table, 200041);
    page[0xFF - 2] = raw_for(table, 'X');
    CHECK_EQ(decode_minor(table, page).serial_valid, false);
}

}  // namespace

void test_hw_revision() {
    test_minor_ranges();
    test_invalid_page();
}

int main() {
    test_hw_revision();
    return 0;
}
