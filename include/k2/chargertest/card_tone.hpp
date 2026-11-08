// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The tones of a saved report card, by the Results grid's own table (result_layout::roll_up, docs/UI_DESIGN.md "Charger test"):
// green all passed, amber some passed or the source capped, red none passed, grey nothing offered or nothing ran.
// A card stores no per-step verdicts, so each row is turned back into the tally its steps would have had, then rolled up:
//   range   no failure and the top reached        -> pass; the top (within the 5 % VBUS tolerance) never exercised -> pass + skip
//           a failure with something reached      -> capped when the PD group's reason is Capped (the source plateaued below
//                                                    the target), else pass + fail; a failure with nothing reached -> fail
//   PD fixed a group that failed only through a range reads as its fixed levels: pass (the group's own fail_mv is 0 and a range
//           holds the failure); a fixed step that failed -> pass + fail when some level passed, else fail
//   legacy  a protocol that never answered (nothing passed) is "not offered": grey, never red. Red is a protocol that answered
//           and then failed with nothing passing, which a card cannot hold (the 5 V baseline would be a pass).
// The page (docs/site/r/card.js) carries the same table; tests/test_card_tone.cpp and tests/test_cardpage.mjs pin both to the
// FH22 card.
#pragma once
#include "k2/chargertest/report_card.hpp"
#include "k2/chargertest/result_layout.hpp"

namespace k2::report_card::tone {
using result_layout::Tone;
using result_layout::Tally;

// The source plateaued below a target (not a failure to follow): the PD group's first failing step says Capped.
inline bool capped(const Card& c) {
    return c.group[0].verdict == GV::Fail && c.group[0].reason == uint8_t(ct::Reason::Capped);
}

inline Tone range(const Card& c, const Range& r) {
    Tally t;
    if (r.fail_dv) {
        if (!r.max_dv) t.fail = 1;
        else if (capped(c)) t.capped = 1;
        else { t.pass = 1; t.fail = 1; }
    } else if (!r.max_dv) {
        t.skip = 1;
    } else {
        t.pass = 1;
        if (unsigned(r.max_dv) + r.hi_dv / 20u < r.hi_dv) t.skip = 1;   // the top of the range was never exercised
    }
    return result_layout::roll_up(t);
}

// True when the PD group's failure sits in a range and not in a fixed level.
inline bool fixed_ok_despite_group(const Card& c) {
    const GroupResult& g = c.group[0];
    if (g.verdict != GV::Fail || g.fail_mv || !g.max_mv) return false;
    for (unsigned i = 0; i < c.n_ranges; ++i)
        if (c.range[i].fail_dv) return true;
    return false;
}

inline Tone group(const Card& c, unsigned gi) {
    const GroupResult& g = c.group[gi];
    Tally t;
    switch (g.verdict) {
    case GV::Pass: t.pass = 1; break;
    case GV::Skip: t.skip = 1; break;
    case GV::Fail:
        if (gi == 0) {
            if (fixed_ok_despite_group(c)) t.pass = 1;
            else if (g.max_mv) { t.pass = 1; t.fail = 1; }
            else t.fail = 1;
        } else if (g.max_mv) { t.pass = 1; t.fail = 1; }   // answered, then failed a higher level
        else t.skip = 1;                                   // never answered: not offered
        break;
    default: break;
    }
    return result_layout::roll_up(t);
}

// The card's rows by tone (PD fixed, the ranges, the tested legacy groups). Grey rows are not counted as failures.
struct Counts { unsigned pass = 0, partial = 0, fail = 0, grey = 0; };
inline Counts counts(const Card& c) {
    Counts n;
    auto add = [&](Tone t) {
        switch (t) {
        case Tone::Pass: ++n.pass; break;
        case Tone::Partial: ++n.partial; break;
        case Tone::Fail: ++n.fail; break;
        default: ++n.grey; break;
        }
    };
    if (c.group[0].verdict != GV::None) add(group(c, 0));
    for (unsigned i = 0; i < c.n_ranges; ++i) add(range(c, c.range[i]));
    for (unsigned g = 1; g < kGroups; ++g)
        if (c.group[g].verdict != GV::None) add(group(c, g));
    return n;
}

// The card's one tone, the row summary and the head cell: green when everything offered passed, red when nothing did.
inline Tone card(const Card& c) {
    const Counts n = counts(c);
    Tally t;
    t.pass = n.pass; t.fail = n.fail; t.capped = n.partial;
    if (!t.pass && !t.fail && !t.capped) return n.grey ? Tone::Skip : Tone::Pending;
    return result_layout::roll_up(t);
}
}  // namespace k2::report_card::tone
