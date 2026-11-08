// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Report card store bound to the real flash (src/chargertest/report_card_hw.cpp). Every call runs from the main loop
// (never from an ISR); a save is one small littlefs file commit.
#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "k2/chargertest/report_card.hpp"
#include "k2/chargertest/report_card_store.hpp"

namespace k2::report_card::hw {

std::size_t list(std::span<Card> out);          // newest first
std::size_t numbers(std::span<std::uint16_t> out);   // card numbers, newest first
std::uint32_t generation();                      // bumped by every successful write (the USB drive re-lists on it)
bool find(std::uint32_t fp, Card& c);
bool get(std::uint16_t card_no, Card& c);
Err put(Card& c);                                // see Store::put
Err rename(std::uint16_t card_no, std::string_view name);
Err remove(std::uint16_t card_no);
Context context();                               // firmware hash, boot counter and uptime for a new card

}  // namespace k2::report_card::hw
