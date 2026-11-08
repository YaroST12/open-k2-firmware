// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#include "k2/drivers/board.hpp"

#include <initializer_list>

#include "k2/drivers/pins.hpp"
#include "k2/drivers/regs.hpp"

namespace k2::board {

using namespace reg;
namespace p = pins;

void init() {
    crm().APB2EN |= Crm::APB2EN_GPIOB;
    p::sw_pb13.clear();
    p::sw_pb13.mode(Mode::OutPp50);
    // PB15 is left floating (analog): driving it low let a self-powered boot die when PLUS was released.
    p::sw_pb15.mode(Mode::Analog);
}

}  // namespace k2::board
