// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Shared button meaning of the charger-test wizard pages (Tests with its Custom sub-list and Group pages, Cable, Review,
// then the Results). The look (top bar, rows, footer) is the kit's; this header keeps the side-panel label set only.
// Pure function over values, so the host tests cover it without a page.
#pragma once
#include <cstdint>
#include "k2/ui/ui_labels.hpp"

namespace k2::setup {

enum class Step : uint8_t { Tests, Cable, Review, Results };

// Side panel of a wizard page: [+]/[-] move the cursor one row, OK acts on the row (`ok`), a long OK does `ok_long`
// (the group toggle on the Custom list), BACK goes to the previous step ("Back" leaves the wizard at step 1).
// The long [+]/[-] page switch stays global and is kept from `base`.
inline PanelLabels labels(PanelLabels base, Step s, const char* ok, const char* ok_long = "") {
    auto at = [&](Button b) -> SlotLabel& { return base.b[static_cast<int>(b)]; };
    at(Button::Minus).shrt = "Up";
    at(Button::Minus).lng_alt = "Prv";
    at(Button::Plus).shrt = "Dn";
    at(Button::Ok) = {ok, ok_long, "", false};
    (void)s;
    at(Button::Back).shrt = "Back";   // BACK reads "Back" on every step (label vocabulary, ui_labels.hpp)
    return base;
}

}  // namespace k2::setup
