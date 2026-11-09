// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The QR scan-test rig, firmware side (the pure model is k2/ui/ui_qrtest.hpp): the bridge from the remote backend
// (src/core/remote.cpp, Cmd::QrTest) to the UI. Only this file's symbols are referenced outside the host-tested
// library sources, so the page itself stays in ui_pages.cpp where the host tests link it.
#include "k2/ui/ui_qrtest.hpp"

#include "k2/ui/ui.hpp"
#include "k2/ui/ui_types.hpp"

namespace k2::qrtest {

bool show(Ui& ui, std::uint8_t version, std::uint8_t px, std::uint8_t quiet, std::uint32_t now_ms) {
    ui.context().qrtest = {version, px, quiet};   // QrTestPage::enter() consumes it
    // The page carries its v25 encode workspace as a member (~3.4 KB), so the page arena fits it only over a base
    // page small enough (the static_assert in ui.hpp pins Tools); over Home / Detail / ChargerTest it would not fit.
    // goto_page refuses Menu::QrTest (no params), so close any open menu / land on the small base, then push.
    const Page cur = ui.page();
    const bool small_base = cur == Page::Lines || cur == Page::Tools || cur == Page::Settings;
    if (ui.menu_depth() > 0 || !small_base) ui.goto_page(static_cast<std::uint8_t>(Page::Tools), now_ms);
    return ui.push_menu(Menu::QrTest, now_ms);
}

bool stop(Ui& ui, std::uint32_t now_ms) {
    if (ui.menu() != Menu::QrTest) return false;
    return ui.pop_menu(now_ms);
}

}  // namespace k2::qrtest
