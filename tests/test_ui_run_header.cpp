// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The charger-test Running header (plain row): the family label and the step name stand left of the 4-cell VBUS hero and must
// stay kLeftGap clear of its first digit cell, whatever the family, the step name, the reading and the rotation (include/k2/ui/run_layout.hpp).
// A text cell keeps its native size in the landscape mapping and only its origin moves (runlay::painted).
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>
#include "test_util.h"
#include "k2/ui/run_layout.hpp"
#include "k2/ui/trunc.hpp"

using namespace k2;
namespace {

// The cells a label of `cells` glyphs paints: the name field (the 12 px font when it outgrows the 16 px field) clips at its width.
runlay::Box name_box(int cells) {
    const bool small = runlay::name_small(cells);
    const int adv = small ? runlay::kAdv12 : runlay::kAdv16, max = small ? runlay::kNameCellsSmall : runlay::kNameCells;
    return {"name", runlay::kLeftX, runlay::kPlainRun.name_y, int16_t(max * adv), int16_t(small ? runlay::kH12 : runlay::kH16), true};   // a Field blanks its whole width
}
runlay::Box family_box(int cells) {
    (void)cells;   // the Field is kFamCells wide whatever the text
    return {"family", runlay::kLeftX, 32, int16_t(runlay::kFamCells * runlay::kAdv12), runlay::kH12, true};
}
bool fits_field(int cells, int field_cells) { return cells <= field_cells; }
runlay::Box hero_box(int cells) {   // right-aligned in the 4-cell field: the field blanks its whole width on a full repaint
    (void)cells;
    return {"hero", runlay::kPlainHeroLeft, runlay::kPlainRun.hero_y, int16_t(runlay::kPlainRun.hero_chars * runlay::kAdv48), runlay::kH48, true};
}
// The widest the left column's boxes may paint: its right edge plus the gap must not reach the hero.
bool clear_of_hero(const runlay::Box &b, int ext_w, int ext_h) {
    runlay::Box padded = b;
    padded.w = int16_t(b.w + runlay::kLeftGap);
    return !runlay::overlap(padded, hero_box(4), ext_w, ext_h);
}

std::vector<std::string> golden_labels() {
    std::vector<std::string> out;
    std::ifstream in("tests/fixtures/step_labels.txt");
    for (std::string line; std::getline(in, line);) {
        if (line.empty() || line[0] == '#') continue;
        const size_t bar = line.rfind('|');
        if (bar == std::string::npos) continue;
        std::string l = line.substr(bar + 1);
        while (!l.empty() && l[0] == ' ') l.erase(0, 1);
        out.push_back(l);
    }
    return out;
}

void test_the_left_column_clears_the_hero() {
    // the longest names of each family's steps, as the plan words them
    std::vector<std::string> names = {"QC2B 20V", "AVS 15\xe2\x86\x92" "12V", "UFCS 5.5V", "QC3+ 11.8V", "PPS 20V 5A", "EPR 28V 5A", "SVOOC65 10V", "SCP 10V 2.25A"};
    for (const auto &g : golden_labels()) names.push_back(g);
    CHECK_EQ(names.size() > 40, true);
    const int exts[2][2] = {{208, 240}, {240, 208}};
    for (const auto &e : exts) {
        for (const char *f : runlay::kFamilyNames) {
            const int fc = text_cells(f);
            CHECK_EQ(clear_of_hero(family_box(fc), e[0], e[1]), true);
            CHECK_EQ(fits_field(fc, runlay::kFamCells), true);   // and the name fits it whole
        }
        for (const auto &n : names) {
            CHECK_EQ(clear_of_hero(name_box(text_cells(n)), e[0], e[1]), true);
        }
    }
    // a label that fits the field whole keeps the 16 px font; only the long ones (a ramp, SCP) shrink
    CHECK_EQ(runlay::name_small(text_cells("PPS 20V 5A")), false);
    CHECK_EQ(runlay::name_small(text_cells("SCP 10V 2.25A")), true);
}

}  // namespace

int main() {
    test_the_left_column_clears_the_hero();
    return test_report("test_ui_run_header");
}
