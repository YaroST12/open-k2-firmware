// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// See k2/drivers/pin_mark.hpp. BPR at 0x40006C00, DR11.. at 0x40 + 4 * (n - 11): DR21 = 0x68 (16 bits).
#include "k2/drivers/pin_mark.hpp"

#include "k2/drivers/regs.hpp"

namespace k2::pinmark {
namespace {

constexpr std::uint32_t kBpr = 0x40006C00;
constexpr std::uint32_t kPwr = 0x40007000;
constexpr std::uint32_t kDr21 = kBpr + 0x68;
constexpr std::uint32_t kApb1enPwr = 1u << 28, kApb1enBpr = 1u << 27;
constexpr std::uint32_t kPwrDbp = 1u << 8;            // disable backup-domain write protection

void enable() {
    reg::crm().APB1EN |= kApb1enPwr | kApb1enBpr;
    *reinterpret_cast<volatile std::uint32_t*>(kPwr) |= kPwrDbp;
}

}  // namespace

std::uint16_t read() {
    enable();
    return std::uint16_t(*reinterpret_cast<volatile std::uint32_t*>(kDr21));
}

void write(std::uint16_t raw) {
    enable();
    volatile std::uint32_t& r = *reinterpret_cast<volatile std::uint32_t*>(kDr21);
    if (std::uint16_t(r) != raw) r = raw;   // a backup-domain write only when it changes
}

}  // namespace k2::pinmark
