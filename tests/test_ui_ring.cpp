// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The page ring (include/k2/ui/ui_ring.hpp): nine pages in a loop, view-only keys in the ring, one total owner function.
#include "k2/ui/ui_ring.hpp"
#include <cstdio>
#include <cstring>
#include "k2/ui/ui_labels.hpp"
using namespace k2;
using namespace k2::ring;
static int g_n = 0, g_bad = 0;
#define CHECK(c) do { g_n++; if (!(c)) { g_bad++; std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)

static Event ev(Button b, Press p) { return Event{b, p}; }

int main() {
    // The loop: next / prev wrap over all ten, in the user's order.
    static_assert(kCount == 10);
    CHECK(next(Pos::Home) == Pos::Lines && next(Pos::Lines) == Pos::Numbers && next(Pos::Numbers) == Pos::Graph);
    CHECK(next(Pos::Graph) == Pos::Ripple && next(Pos::Ripple) == Pos::Link && next(Pos::Link) == Pos::Tools);
    CHECK(next(Pos::Tools) == Pos::Battery && next(Pos::Battery) == Pos::Log && next(Pos::Log) == Pos::K2 && next(Pos::K2) == Pos::Home);
    CHECK(prev(Pos::Home) == Pos::K2);
    {
        Pos p = Pos::Home;
        for (int i = 0; i < kCount; ++i) { CHECK(prev(next(p)) == p); p = next(p); }
        CHECK(p == Pos::Home);
    }

    // Ring view keys: + next, - prev, OK enters (when the page has anything to do), Back short = Home, Back long = the target.
    {
        Step s = view_key(Pos::Lines, ev(Button::Plus, Press::Short), Pos::Home);
        CHECK(s.what == Do::Show && s.to == Pos::Numbers);
        s = view_key(Pos::Home, ev(Button::Minus, Press::Short), Pos::Home);
        CHECK(s.what == Do::Show && s.to == Pos::K2);
        s = view_key(Pos::Graph, ev(Button::Ok, Press::Short), Pos::Home);
        CHECK(s.what == Do::Enter);
        s = view_key(Pos::Lines, ev(Button::Ok, Press::Short), Pos::Home);   // view-only: nothing to enter
        CHECK(s.what == Do::None);
        s = view_key(Pos::Battery, ev(Button::Back, Press::Short), Pos::Home);
        CHECK(s.what == Do::Show && s.to == Pos::Home);
        s = view_key(Pos::Home, ev(Button::Back, Press::Short), Pos::Home);
        CHECK(s.what == Do::None);
        s = view_key(Pos::Battery, ev(Button::Back, Press::Long), Pos::Link);
        CHECK(s.what == Do::Show && s.to == Pos::Link);
        s = view_key(Pos::Link, ev(Button::Back, Press::Long), Pos::Link);   // already there
        CHECK(s.what == Do::None);
        // Long OK / long +/- / holds are consumed: no long Plus in the ring, no page action leaks out of the view.
        for (Button b : {Button::Minus, Button::Ok, Button::Plus}) {
            CHECK(view_key(Pos::Graph, ev(b, Press::Long), Pos::Home).what == Do::None);
            CHECK(view_key(Pos::Graph, ev(b, Press::Hold), Pos::Home).what == Do::None);
        }
        CHECK(view_key(Pos::Graph, ev(Button::Back, Press::Hold), Pos::Home).what == Do::None);
    }
    // Only the pages with something to do can be entered.
    for (int i = 0; i < kCount; ++i) {
        const Pos p = static_cast<Pos>(i);
        CHECK(can_enter(p) == (p != Pos::Lines && p != Pos::Numbers));
    }

    // Entered: a Back the page does not take leaves to the ring; a pop above the top level is an ordinary pop.
    CHECK(entered_back(Press::Short, true) == Do::Leave);
    CHECK(entered_back(Press::Long, true) == Do::Leave);
    CHECK(entered_back(Press::Long, false) == Do::Leave);
    CHECK(entered_back(Press::Short, false) == Do::None);

    // The owner is total: every base page, first menu and tab maps into the ring.
    for (int pg = 0; pg < static_cast<int>(Page::Count); ++pg)
        for (int m = 0; m <= static_cast<int>(Menu::Count); ++m)
            for (int t = 0; t < 3; ++t) {
                const Pos o = owner(static_cast<Page>(pg), static_cast<Menu>(m), static_cast<uint8_t>(t));
                CHECK(static_cast<int>(o) < kCount);
            }
    CHECK(owner(Page::Home, Menu::Count, 0) == Pos::Home);
    CHECK(owner(Page::Detail, Menu::Count, 0) == Pos::Numbers);
    CHECK(owner(Page::Lines, Menu::Count, 0) == Pos::Lines);
    CHECK(owner(Page::Graphs, Menu::Count, 0) == Pos::Graph);
    CHECK(owner(Page::Ripple, Menu::Count, 0) == Pos::Ripple);
    CHECK(owner(Page::Settings, Menu::Count, 0) == Pos::K2);
    CHECK(owner(Page::ChargerTest, Menu::Count, 0) == Pos::Link);
    CHECK(owner(Page::Lines, Menu::Connection, 0) == Pos::Link);
    CHECK(owner(Page::Lines, Menu::Connection, 1) == Pos::Link);
    CHECK(owner(Page::Lines, Menu::Connection, 2) == Pos::Link);
    // Tool pages belong to the place that opens them; Share and the QR test follow the page they sit on.
    CHECK(owner(Page::Lines, Menu::Trigger, 0) == Pos::Tools && owner(Page::Lines, Menu::PdTrigger, 0) == Pos::Tools);
    CHECK(owner(Page::Lines, Menu::Tools, 0) == Pos::Tools);
    CHECK(owner(Page::Lines, Menu::Cards, 0) == Pos::Link && owner(Page::Lines, Menu::PdStress, 0) == Pos::Link);
    CHECK(owner(Page::Lines, Menu::Emarker, 0) == Pos::Link && owner(Page::Lines, Menu::CableR, 0) == Pos::Link);
    CHECK(owner(Page::Lines, Menu::Spoof, 0) == Pos::Link && owner(Page::Lines, Menu::LoopProbe, 0) == Pos::Link);
    CHECK(owner(Page::Lines, Menu::Twin, 0) == Pos::Link);
    CHECK(owner(Page::Lines, Menu::Battery, 0) == Pos::Battery);
    CHECK(owner(Page::Lines, Menu::BlackBox, 0) == Pos::Log && owner(Page::Lines, Menu::Diagnosis, 0) == Pos::Log);
    for (Menu m : {Menu::Diag, Menu::UsbLink, Menu::Autorotate, Menu::Memory, Menu::Display}) CHECK(owner(Page::Home, m, 0) == Pos::K2);
    CHECK(owner(Page::Graphs, Menu::Share, 0) == Pos::Graph && owner(Page::Home, Menu::QrTest, 0) == Pos::Home);

    // Every ring position has a target, and showing it lands on that position (owner round trip).
    for (int i = 0; i < kCount; ++i) {
        const Pos p = static_cast<Pos>(i);
        const Target t = target(p);
        const Page base = t.menu == Menu::Count ? t.page : Page::Lines;   // a menu target sits over any host page
        CHECK(owner(base, t.menu, t.tab) == p);
    }
    CHECK(target(Pos::Link).menu == Menu::Connection && target(Pos::Link).tab == 0);
    CHECK(target(Pos::Tools).menu == Menu::Tools && target(Pos::Tools).tab == 0);
    CHECK(target(Pos::Log).menu == Menu::Diagnosis && target(Pos::Battery).menu == Menu::Battery);
    CHECK(target(Pos::K2).menu == Menu::Count && target(Pos::K2).page == Page::Settings);

    // The masthead names are 1-based and fit the rail's 9 characters.
    for (int i = 0; i < kCount; ++i) CHECK(mast_name(static_cast<Pos>(i))[0] != 0 && std::strlen(mast_name(static_cast<Pos>(i))) <= 9);

    // One name per page: the masthead is the plain name in capitals behind its number ("4 GRAPHS" = "Graphs"), the page's own rail says GRAPHS.
    for (int i = 0; i < kCount; ++i) {
        const char *m = mast_name(static_cast<Pos>(i)), *n = name(static_cast<Pos>(i));
        while (*m >= '0' && *m <= '9') ++m;
        ++m;
        bool same = std::strlen(m) == std::strlen(n);
        for (size_t k = 0; same && n[k]; ++k) same = m[k] == (n[k] >= 'a' && n[k] <= 'z' ? n[k] - 'a' + 'A' : n[k]);
        CHECK(same);
    }
    CHECK(std::strcmp(mast_name(Pos::Graph), "4 GRAPHS") == 0);
    CHECK(std::strcmp(mast_name(Pos::Link), "6 LINK") == 0);
    CHECK(std::strcmp(mast_name(Pos::Tools), "7 TOOLS") == 0);

    // The persisted long-back target: out of range falls back to Home.
    CHECK(sanitize_target(0) == Pos::Home && sanitize_target(9) == Pos::K2);
    CHECK(sanitize_target(10) == Pos::Home && sanitize_target(255) == Pos::Home);

    // The ring strip: Prev / Next move, Enter dims where there is nothing to enter, Back goes Home (Jump names the long press).
    {
        const Strip s = view_strip(Pos::Graph, Pos::Home);
        CHECK(s[Button::Back] == sl::Home && s[Button::Minus] == sl::Prev && s[Button::Ok] == sl::Enter && s[Button::Plus] == sl::Next);
        CHECK(view_strip(Pos::Graph, Pos::Link)[Button::Back] == sl::HomeJump);
        CHECK(view_strip(Pos::Link, Pos::Link)[Button::Back] == sl::Home);   // already the jump target: nothing to name
        CHECK(view_strip(Pos::Home, Pos::Home)[Button::Back] == sl::None);          // nowhere to go
        CHECK(view_strip(Pos::Home, Pos::Link)[Button::Back] == sl::Jump);
        CHECK(view_strip(Pos::Lines, Pos::Home)[Button::Ok] == sl::EnterDim && view_strip(Pos::Numbers, Pos::Home)[Button::Ok] == sl::EnterDim);
        for (int i = 0; i < kCount; ++i)
            for (int j = 0; j < kCount; ++j) {
                const PanelLabels pl = resolve(view_strip(static_cast<Pos>(i), static_cast<Pos>(j)));
                CHECK(label_violation(pl) == nullptr);
            }
    }

    std::printf("%d checks, %d failed\n", g_n, g_bad);
    return g_bad != 0;
}
