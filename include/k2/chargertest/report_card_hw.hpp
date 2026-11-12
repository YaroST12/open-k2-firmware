// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Report card store bound to the real flash (src/chargertest/report_card_hw.cpp): the report-card ring log
// (k2::fs::card_ring()). Every call runs from the main loop (never from an ISR); a save is one ring record (no erase).
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
Err put(Card& c, Card& prev, bool& had);         // same, with the caller's buffer for the card it replaces (Store::put)
Err rename(std::uint16_t card_no, std::string_view name);
Err remove(std::uint16_t card_no);
Context context();                               // firmware hash, boot counter and uptime for a new card
// Main loop: one maintenance erase of the ring when it needs one and `may_erase` (no charger test, no PD Stress run, a
// quiet PD link), at most one per second (Store::maintain).
void poll(bool may_erase);

}  // namespace k2::report_card::hw
