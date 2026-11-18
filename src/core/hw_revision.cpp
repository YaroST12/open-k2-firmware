// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#include "k2/core/hw_revision.hpp"

#include <string_view>

namespace k2::hw_revision {
namespace {

constexpr std::uintptr_t kSubstitutionTable = 0x08000C00;
constexpr std::uintptr_t kInfoPage = 0x08003000;
Info g_info;
char g_text[7];
std::uint8_t g_text_len;

}  // namespace

void init(std::uint8_t accelerometer_id, power::Chip monitor) {
    const auto table = std::span(reinterpret_cast<const std::uint8_t *>(kSubstitutionTable), kTableBytes);
    const auto page = std::span(reinterpret_cast<const std::uint8_t *>(kInfoPage), kPageBytes);
    const DecodedMinor decoded = decode_minor(table, page);
    g_info = {accelerometer_id, monitor, decoded.serial_valid, decoded.minor};
    std::size_t n = 0;
    g_text[n++] = g_info.accelerometer_id == 0x11 ? 'Q' : 'N';
    g_text[n++] = power::model_badge(g_info.monitor);
    g_text[n++] = '.';
    if (!g_info.minor_valid) {
        g_text[n++] = '-';
        g_text[n++] = '-';
    } else if (g_info.minor >= 100) {
        g_text[n++] = static_cast<char>('0' + g_info.minor / 100u);
        g_text[n++] = static_cast<char>('0' + (g_info.minor / 10u) % 10u);
        g_text[n++] = static_cast<char>('0' + g_info.minor % 10u);
    } else {
        g_text[n++] = static_cast<char>('0' + g_info.minor / 10u);
        g_text[n++] = static_cast<char>('0' + g_info.minor % 10u);
    }
    g_text[n] = '\0';
    g_text_len = static_cast<std::uint8_t>(n);
}

Info current() { return g_info; }
std::string_view text() { return {g_text, g_text_len}; }

}  // namespace k2::hw_revision
