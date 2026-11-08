// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Host tests for the Wants vs Offers match logic (pure: source caps, sniffed contract, sink caps).
#include "test_util.h"
#include "k2/ui/ui_match.hpp"
#include "k2/pd/pd.hpp"
#include <vector>

using namespace k2;

namespace {
constexpr uint32_t fixed(int mv, int ma) { return (uint32_t(mv / 50) << 10) | uint32_t(ma / 10); }
constexpr uint32_t pps(int min_mv, int max_mv, int ma) {
    return 0xC0000000u | (uint32_t(max_mv / 100) << 17) | (uint32_t(min_mv / 100) << 8) | uint32_t(ma / 50);
}

pd::SourceCaps caps_of(std::initializer_list<uint32_t> raws) {
    pd::SourceCaps c;
    c.valid = true;
    for (uint32_t r : raws) c.pdos[c.num_pdos++] = pd::decode_pdo(r);
    return c;
}

pd::Contract want(int pos, int mv, int op_ma, int max_ma) {
    pd::Contract c;
    c.valid = true; c.pos = uint8_t(pos); c.mv = uint16_t(mv); c.op_ma = uint16_t(op_ma); c.max_ma = uint16_t(max_ma);
    return c;
}

const pd::Contract kNoRequest{};

void test_missing_data() {
    pd::SourceCaps none;
    MatchResult r = match_compute({none, kNoRequest, Tri::Unknown, {}});
    CHECK_EQ(r.rows, 0);
    CHECK_EQ((int)r.verdict, (int)Verdict::NoCaps);
    CHECK_EQ(r.caps_seen, 0);

    auto caps = caps_of({fixed(5000, 3000), fixed(9000, 3000)});
    r = match_compute({caps, kNoRequest, Tri::Unknown, {}});
    CHECK_EQ((int)r.verdict, (int)Verdict::NoRequest);
    CHECK_EQ(r.rows, 2);
    CHECK_EQ(r.request_seen, 0);
    for (int i = 0; i < 2; i++) CHECK_EQ((int)r.row[i], (int)RowState::Unknown);
}

void test_request_only() {
    auto caps = caps_of({fixed(5000, 3000), fixed(9000, 3000), fixed(20000, 3250)});
    MatchResult r = match_compute({caps, want(3, 20000, 3250, 3250), Tri::Unknown, {}});
    CHECK_EQ((int)r.verdict, (int)Verdict::SinkUnknown);
    CHECK_EQ((int)r.row[0], (int)RowState::Unknown);
    CHECK_EQ((int)r.row[2], (int)RowState::Selected);
    CHECK_EQ(r.want_pos, 3);
    CHECK_EQ(r.want_mv, 20000);
    CHECK_EQ(r.want_mw, 65000);   // 20 V * 3.25 A
    CHECK_EQ(r.src_max_mv, 20000);
    CHECK_EQ(r.src_max_mw, 65000);
}

void test_match_ok() {
    auto caps = caps_of({fixed(5000, 3000), fixed(9000, 3000), fixed(15000, 3000), fixed(20000, 3250),
                         pps(3300, 11000, 5000), pps(3300, 21000, 3250)});
    const uint32_t sink[] = {fixed(5000, 900), fixed(9000, 2000), fixed(15000, 3000), fixed(20000, 3000)};
    MatchResult r = match_compute({caps, want(4, 20000, 3000, 3000), Tri::No, sink});
    CHECK_EQ(r.rows, 6);
    CHECK_EQ((int)r.row[0], (int)RowState::Ok);
    CHECK_EQ((int)r.row[1], (int)RowState::Ok);
    CHECK_EQ((int)r.row[2], (int)RowState::Ok);
    CHECK_EQ((int)r.row[3], (int)RowState::Selected);
    CHECK_EQ((int)r.row[4], (int)RowState::No);   // a fixed-only sink cannot use an APDO
    CHECK_EQ((int)r.row[5], (int)RowState::No);
    CHECK_EQ((int)r.verdict, (int)Verdict::Matches);
    CHECK_EQ(r.flags, 0);
    CHECK_EQ(r.sink_max_mv, 20000);
}

void test_wants_higher_voltage() {
    auto caps = caps_of({fixed(5000, 3000), fixed(9000, 3000), fixed(12000, 3000), fixed(15000, 3000)});
    const uint32_t sink[] = {fixed(5000, 900), fixed(9000, 2000), fixed(15000, 3000), fixed(20000, 3000)};
    MatchResult r = match_compute({caps, want(4, 15000, 3000, 3000), Tri::Yes, sink});
    CHECK_EQ((int)r.row[0], (int)RowState::Ok);
    CHECK_EQ((int)r.row[2], (int)RowState::No);   // 12 V: the sink has no such PDO
    CHECK_EQ((int)r.row[3], (int)RowState::Selected);
    CHECK_EQ(r.sink_max_mv, 20000);
    CHECK_EQ(r.src_max_mv, 15000);
    CHECK_EQ((r.flags & kFlagWantsHigherV) != 0, 1);
    CHECK_EQ((r.flags & kFlagMismatchBit) != 0, 1);
    CHECK_EQ((int)r.verdict, (int)Verdict::WantsHigherV);   // the specific cause beats the generic bit
    StrBuf<32> s;
    match_footer(r, s);
    CHECK_STR(s.c_str(), "Wants 20V, charger max 15V");
}

void test_current_limited() {
    auto caps = caps_of({fixed(5000, 3000), fixed(20000, 2250)});
    const uint32_t sink[] = {fixed(5000, 900), fixed(20000, 3000)};
    MatchResult r = match_compute({caps, want(2, 20000, 2250, 2250), Tri::No, sink});
    CHECK_EQ((int)r.row[1], (int)RowState::SelectedLow);
    CHECK_EQ((r.flags & kFlagCurrentLimited) != 0, 1);
    CHECK_EQ((int)r.verdict, (int)Verdict::CurrentLimited);
    CHECK_EQ(r.need_ma, 3000);
    CHECK_EQ(r.have_ma, 2250);
    StrBuf<32> s;
    match_footer(r, s);
    CHECK_STR(s.c_str(), "Charger gives 2.25A, wants 3A");

    // An unselected PDO that is too weak is marked Low.
    auto caps2 = caps_of({fixed(5000, 3000), fixed(20000, 2250)});
    r = match_compute({caps2, want(1, 5000, 900, 900), Tri::No, sink});
    CHECK_EQ((int)r.row[0], (int)RowState::Selected);
    CHECK_EQ((int)r.row[1], (int)RowState::Low);
}

void test_pps() {
    auto caps = caps_of({fixed(5000, 3000), pps(3300, 11000, 5000)});
    const uint32_t sink[] = {fixed(5000, 900), pps(3300, 11000, 3000)};
    MatchResult r = match_compute({caps, want(2, 9000, 2000, 2000), Tri::No, sink});
    CHECK_EQ((int)r.row[0], (int)RowState::Ok);
    CHECK_EQ((int)r.row[1], (int)RowState::Selected);
    CHECK_EQ(r.want_mv, 9000);
    CHECK_EQ((int)r.verdict, (int)Verdict::Matches);
    // A PPS sink range outside the APDO does not match.
    const uint32_t sink2[] = {pps(15000, 21000, 3000)};
    r = match_compute({caps, want(1, 5000, 900, 900), Tri::No, sink2});
    CHECK_EQ((int)r.row[1], (int)RowState::No);
}

void test_outside_caps_and_mismatch_bit() {
    auto caps = caps_of({fixed(5000, 3000), fixed(9000, 3000)});
    MatchResult r = match_compute({caps, want(6, 20000, 3000, 3000), Tri::Unknown, {}});
    CHECK_EQ((r.flags & kFlagOutsideCaps) != 0, 1);
    CHECK_EQ((int)r.verdict, (int)Verdict::OutsideCaps);
    for (int i = 0; i < 2; i++) CHECK_EQ((int)r.row[i] == (int)RowState::Selected, 0);
    StrBuf<32> s;
    match_footer(r, s);
    CHECK_STR(s.c_str(), "Request PDO6 not in caps");

    const uint32_t sink[] = {fixed(5000, 900), fixed(9000, 2000)};
    r = match_compute({caps, want(2, 9000, 2000, 3000), Tri::Yes, sink});
    CHECK_EQ((int)r.verdict, (int)Verdict::MismatchBit);
}

void test_footer_and_summary() {
    pd::SourceCaps none;
    StrBuf<32> s;
    match_footer(match_compute({none, kNoRequest, Tri::Unknown, {}}), s);
    CHECK_STR(s.c_str(), "Waiting for Source_Caps");
    auto caps = caps_of({fixed(5000, 3000)});
    StrBuf<32> s2;
    match_footer(match_compute({caps, kNoRequest, Tri::Unknown, {}}), s2);
    CHECK_STR(s2.c_str(), "Replug device on the charger");
    StrBuf<32> s3;
    match_footer(match_compute({caps, want(1, 5000, 500, 500), Tri::Unknown, {}}), s3);
    CHECK_STR(s3.c_str(), "Sink caps not seen");
    const uint32_t sink[] = {fixed(5000, 900), fixed(9000, 2000), fixed(15000, 3000), fixed(20000, 3000)};
    StrBuf<32> s4;
    match_footer(match_compute({caps_of({fixed(5000, 3000)}), want(1, 5000, 500, 500), Tri::No, sink}), s4);
    CHECK_STR(s4.c_str(), "Wants 20V, charger max 5V");

    StrBuf<32> sum;
    sink_summary(sink, sum);
    CHECK_STR(sum.c_str(), "Sink caps 5 9 15 20V");
    const uint32_t sink2[] = {fixed(5000, 900), fixed(9000, 2000), pps(3300, 11000, 3000)};
    StrBuf<32> sum2;
    sink_summary(sink2, sum2);
    CHECK_STR(sum2.c_str(), "Sink caps 5 9V +PPS");
    StrBuf<32> sum3;
    sink_summary({}, sum3);
    CHECK_STR(sum3.c_str(), "Sink caps not seen");
    // Fractional volts keep one decimal (12.5 V is not a USB PD fixed voltage, 5.1 V is a variable one).
    const uint32_t sink3[] = {fixed(5100, 900)};
    StrBuf<32> sum4;
    sink_summary(sink3, sum4);
    CHECK_STR(sum4.c_str(), "Sink caps 5.1V");
}

// A real charger: Fixed 5/9/12/15 V at 3 A, 20 V at 3.35 A, PPS 5-11 V at 5 A, PPS 5-21 V at 3.2 A.
pd::SourceCaps real_charger() {
    return caps_of({fixed(5000, 3000), fixed(9000, 3000), fixed(12000, 3000), fixed(15000, 3000), fixed(20000, 3350),
                    pps(5000, 11000, 5000), pps(5000, 21000, 3200)});
}

void test_real_charger_offers_only() {
    const pd::SourceCaps caps = real_charger();
    MatchResult r = match_compute({caps, kNoRequest, Tri::Unknown, {}});
    CHECK_EQ(r.rows, 7);
    CHECK_EQ(r.caps_seen, 1);
    CHECK_EQ((int)r.verdict, (int)Verdict::NoRequest);
    CHECK_EQ(r.src_max_mv, 21000);
    CHECK_EQ(r.src_max_mw, 67200);
    // The PDO order is the charger's: nothing is reordered, nothing is marked.
    const uint32_t mv[7] = {5000, 9000, 12000, 15000, 20000, 11000, 21000};
    for (int i = 0; i < 7; i++) {
        CHECK_EQ(caps.pdos[i].max_mv, mv[i]);
        CHECK_EQ((int)r.row[i], (int)RowState::Unknown);
    }
    CHECK_EQ(caps.pdos[4].max_ma, 3350);
    CHECK_EQ(caps.pdos[5].min_mv, 5000);
    CHECK_EQ(caps.pdos[6].max_ma, 3200);
    StrBuf<32> s;
    match_footer(r, s);
    CHECK_STR(s.c_str(), "Replug device on the charger");
}

void test_real_charger_rdo_position_2() {
    const pd::SourceCaps caps = real_charger();
    MatchResult r = match_compute({caps, want(2, 9000, 2000, 3000), Tri::Unknown, {}});
    CHECK_EQ(r.rows, 7);
    CHECK_EQ(r.want_pos, 2);
    CHECK_EQ(r.want_mv, 9000);
    CHECK_EQ(r.want_ma, 3000);
    CHECK_EQ(r.want_mw, 27000);
    CHECK_EQ((int)r.verdict, (int)Verdict::SinkUnknown);
    for (int i = 0; i < 7; i++)
        CHECK_EQ((int)r.row[i], (int)(i == 1 ? RowState::Selected : RowState::Unknown));
    CHECK_EQ(r.flags, 0);
    // With a fixed-only sink the same request is judged against the sink: the PPS rows are unusable.
    const uint32_t sink[] = {fixed(5000, 900), fixed(9000, 2000), fixed(15000, 3000)};
    r = match_compute({caps, want(2, 9000, 2000, 3000), Tri::No, sink});
    CHECK_EQ((int)r.row[0], (int)RowState::Ok);
    CHECK_EQ((int)r.row[1], (int)RowState::Selected);
    CHECK_EQ((int)r.row[2], (int)RowState::No);   // 12 V: the sink has no such PDO
    CHECK_EQ((int)r.row[3], (int)RowState::Ok);
    CHECK_EQ((int)r.row[4], (int)RowState::No);   // 20 V: nothing for it in the sink
    CHECK_EQ((int)r.row[5], (int)RowState::No);
    CHECK_EQ((int)r.row[6], (int)RowState::No);
    CHECK_EQ((int)r.verdict, (int)Verdict::Matches);
}

// The offer list is append-only per source: a charger that re-sends fewer PDOs (PPS dropped after a hard
// reset) keeps its earlier offers on screen, flagged as not offered now. Cleared on a new source.
void test_offer_book_append_only() {
    OfferBook book;
    book.merge(real_charger());
    CHECK_EQ(book.caps.num_pdos, 7);
    CHECK_EQ(book.offered, 0x7Fu);
    // Same charger, no detach, now only five fixed PDOs.
    book.merge(caps_of({fixed(5000, 3000), fixed(9000, 3000), fixed(12000, 3000), fixed(15000, 3000), fixed(20000, 3350)}));
    CHECK_EQ(book.caps.num_pdos, 7);
    CHECK_EQ(book.offered, 0x1Fu);
    CHECK_EQ(book.caps.pdos[5].type == pd::PdoType::Pps, 1);   // the PPS rows are still there
    CHECK_EQ(book.caps.pdos[6].max_mv, 21000);
    MatchResult r = match_compute({book.caps, kNoRequest, Tri::Unknown, {}, book.offered});
    CHECK_EQ(r.rows, 7);
    CHECK_EQ(r.gone, 2);
    for (int i = 0; i < 5; i++) CHECK_EQ((int)r.row[i], (int)RowState::Unknown);
    CHECK_EQ((int)r.row[5], (int)RowState::Gone);
    CHECK_EQ((int)r.row[6], (int)RowState::Gone);
    CHECK_EQ(r.src_max_mv, 20000);   // the dropped 21 V PPS does not count as on offer
    CHECK_EQ(r.src_max_mw, 67000);
    StrBuf<32> s;
    CHECK_EQ(gone_footer(r, s), 1);
    CHECK_STR(s.c_str(), "2 offers not sent now");
    // A request on a live row still works; one on a dropped row is outside the current caps.
    r = match_compute({book.caps, want(2, 9000, 2000, 3000), Tri::No, {}, book.offered});
    CHECK_EQ((int)r.row[1], (int)RowState::Selected);
    CHECK_EQ((int)r.row[6], (int)RowState::Gone);
    r = match_compute({book.caps, want(6, 9000, 2000, 3000), Tri::No, {}, book.offered});
    CHECK_EQ((int)r.verdict, (int)Verdict::OutsideCaps);
    // The PPS comes back: everything is offered again.
    book.merge(real_charger());
    CHECK_EQ(book.offered, 0x7Fu);
    r = match_compute({book.caps, kNoRequest, Tri::Unknown, {}, book.offered});
    CHECK_EQ(r.gone, 0);
    CHECK_EQ(gone_footer(r, s), 0);
    // A new source (or a detach) starts from nothing.
    book.clear();
    CHECK_EQ(book.caps.num_pdos, 0);
    book.merge(caps_of({fixed(5000, 1500)}));
    CHECK_EQ(book.caps.num_pdos, 1);
    CHECK_EQ(book.offered, 1u);
    // Invalid or empty caps never disturb the book.
    book.merge(pd::SourceCaps{});
    CHECK_EQ(book.caps.num_pdos, 1);
    CHECK_EQ(book.offered, 1u);
}

// Merging is only for one source: caps that are not a subset or superset of the book (position by
// position, same PDO1 raw word) belong to another charger and replace it. A missed detach must not
// leak charger A's offers into charger B.
void test_offer_book_replaces_other_source() {
    OfferBook book;
    book.merge(real_charger());                                   // charger A, 7 PDOs
    book.merge(caps_of({fixed(5000, 1500), fixed(9000, 1500)}));   // B: other PDO1 (1.5 A), no detach seen
    CHECK_EQ(book.caps.num_pdos, 2);
    CHECK_EQ(book.offered, 0x3u);
    CHECK_EQ(int(book.caps.pdos[0].max_ma), 1500);
    // Same PDO1 but a list that is neither prefix nor extension: another charger as well.
    book.clear();
    book.merge(caps_of({fixed(5000, 3000), fixed(9000, 3000), pps(3300, 11000, 3000)}));
    book.merge(caps_of({fixed(5000, 3000), fixed(12000, 3000)}));
    CHECK_EQ(book.caps.num_pdos, 2);
    CHECK_EQ(book.offered, 0x3u);
    CHECK_EQ(int(book.caps.pdos[1].max_mv), 12000);
    // A superset of the book (the source adds PPS) stays merged.
    book.merge(caps_of({fixed(5000, 3000), fixed(12000, 3000), pps(3300, 11000, 3000)}));
    CHECK_EQ(book.caps.num_pdos, 3);
    CHECK_EQ(book.offered, 0x7u);
    // PPS dropped and re-added: merged all along.
    book.merge(caps_of({fixed(5000, 3000), fixed(12000, 3000)}));
    CHECK_EQ(book.caps.num_pdos, 3);
    CHECK_EQ(book.offered, 0x3u);
    book.merge(caps_of({fixed(5000, 3000), fixed(12000, 3000), pps(3300, 11000, 3000)}));
    CHECK_EQ(book.offered, 0x7u);
}
}  // namespace

void test_request_state_and_flags() {
    pd::Contract c;
    CHECK_STR(request_state_name(c), "");
    c.valid = true;
    CHECK_STR(request_state_name(c), "Requested");
    c.accepted = true;
    CHECK_STR(request_state_name(c), "Accepted");
    c.ready = true;
    CHECK_STR(request_state_name(c), "Ready");
    c = pd::Contract{};
    c.valid = true; c.rejected = true;
    CHECK_STR(request_state_name(c), "Rejected");

    StrBuf<32> t;
    rdo_flags_text(0, 19, t);
    CHECK_STR(t.c_str(), "");
    rdo_flags_text(pd::kRdoFUsbComm | pd::kRdoFNoSuspend, 30, t);
    CHECK_STR(t.c_str(), "USB comm, No suspend");
    rdo_flags_text(pd::kRdoFGiveBack | pd::kRdoFMismatch, 30, t);
    CHECK_STR(t.c_str(), "Mismatch, Give-back");   // the abnormal one first
    // Whole names only: what does not fit becomes " +", never a cut word.
    const uint8_t all = pd::kRdoFGiveBack | pd::kRdoFMismatch | pd::kRdoFUsbComm | pd::kRdoFNoSuspend | pd::kRdoFEpr;
    rdo_flags_text(all, 13, t);
    CHECK_STR(t.c_str(), "Mismatch +");              // "Mismatch, Give-back +" would be 21
    rdo_flags_text(all, 19, t);
    CHECK_STR(t.c_str(), "Mismatch +");              // " +" counts against the room, so the second name waits
    rdo_flags_text(all, 21, t);
    CHECK_STR(t.c_str(), "Mismatch, Give-back +");
    rdo_flags_text(pd::kRdoFMismatch | pd::kRdoFEpr, 19, t);
    CHECK_STR(t.c_str(), "Mismatch, EPR");           // the last name needs no " +" room
    rdo_flags_text(pd::kRdoFMismatch, 3, t);
    CHECK_STR(t.c_str(), "+");                       // nothing fits: just the marker
}

void test_typec_only_source() {
    // A source with no PD (Rp only): no caps, Type-C current from Rp. Badge "Type-C only", never
    // the Waiting/No-caps look.
    MatchResult r;
    r.typec_ma = 3000;
    CHECK_EQ((int)agreement(r), (int)Agreement::TypeCOnly);
    StrBuf<32> s;
    match_footer(r, s);
    CHECK_STR(s.c_str(), "No PD: Type-C 3A source");
    r.typec_ma = 1500;
    match_footer(r, s);
    CHECK_STR(s.c_str(), "No PD: Type-C 1.5A source");
    // PD caps seen: a PD source whatever the Rp says
    MatchResult p = match_compute({caps_of({fixed(5000, 3000)}), kNoRequest, Tri::Unknown, {}});
    p.typec_ma = 3000;
    CHECK_EQ((int)agreement(p), (int)Agreement::NoContract);
}

void test_agreement_badge() {
    auto caps = caps_of({fixed(5000, 3000), fixed(9000, 3000), fixed(20000, 3250)});
    // No Request on the wire: no contract, whatever caps were seen.
    CHECK_EQ((int)agreement(match_compute({caps, kNoRequest, Tri::Unknown, {}})), (int)Agreement::NoContract);
    CHECK_EQ((int)agreement(MatchResult{}), (int)Agreement::NoContract);
    // Request with nothing against it: full (sink caps unknown does not make it partial).
    CHECK_EQ((int)agreement(match_compute({caps, want(3, 20000, 3250, 3250), Tri::No, {}})), (int)Agreement::Full);
    const uint32_t sink[] = {fixed(5000, 900), fixed(20000, 3000)};
    CHECK_EQ((int)agreement(match_compute({caps, want(3, 20000, 3000, 3000), Tri::No, sink})), (int)Agreement::Full);
    // The Capability Mismatch bit makes it partial.
    CHECK_EQ((int)agreement(match_compute({caps, want(3, 20000, 3250, 3250), Tri::Yes, {}})), (int)Agreement::Partial);
    // Less current than the sink wants.
    const uint32_t hungry[] = {fixed(9000, 4000)};
    CHECK_EQ((int)agreement(match_compute({caps, want(2, 9000, 4000, 4000), Tri::No, hungry})), (int)Agreement::Partial);
    // The sink wants a higher voltage than the charger has.
    const uint32_t high[] = {fixed(28000, 1000)};
    CHECK_EQ((int)agreement(match_compute({caps, want(3, 20000, 3000, 3000), Tri::No, high})), (int)Agreement::Partial);
    // A Request beyond the offer.
    CHECK_EQ((int)agreement(match_compute({caps, want(9, 20000, 3000, 3000), Tri::No, {}})), (int)Agreement::Partial);
}

int main() {
    test_agreement_badge();
    test_typec_only_source();
    test_missing_data();
    test_request_only();
    test_match_ok();
    test_wants_higher_voltage();
    test_current_limited();
    test_pps();
    test_outside_caps_and_mismatch_bit();
    test_footer_and_summary();
    test_real_charger_offers_only();
    test_real_charger_rdo_position_2();
    test_offer_book_append_only();
    test_offer_book_replaces_other_source();
    test_request_state_and_flags();
    return test_report("test_ui_match");
}
