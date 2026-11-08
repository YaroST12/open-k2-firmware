// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Transition capture, pure part (include/k2/pd/transcap_calc.hpp).
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "k2/pd/transcap_calc.hpp"

#include <algorithm>
#include <vector>

#include "test_util.h"

using namespace k2::transcap;

namespace {
using Rec = Recorder<16, 4>;   // small ring so wrap-around is cheap to reach

Sample smp(std::uint32_t t, std::uint16_t v = 5000, std::uint8_t c1 = 10, std::uint8_t c2 = 20) { return {t, v, c1, c2}; }
}  // namespace

static void test_ring_keeps_newest_in_order() {
    Rec r;
    r.arm();
    for (std::uint32_t i = 0; i < 40; i++) r.push(smp(i * 1000, std::uint16_t(i)));
    CHECK_EQ(r.sample_count(), 16);
    CHECK_EQ(r.lost_samples(), 24);
    CHECK_EQ(r.sample(0).vbus_mv, 24);      // oldest kept
    CHECK_EQ(r.sample(15).vbus_mv, 39);     // newest
}

static void test_not_armed_records_nothing() {
    Rec r;
    CHECK_EQ(r.push(smp(0)), false);
    r.mark(0, Mark::Accept);
    CHECK_EQ(r.sample_count(), 0);
    CHECK_EQ(r.marker_count(), 0);
    CHECK_EQ(int(r.state()), int(State::Off));
}

static void test_trigger_post_window_then_freeze() {
    Rec r;
    r.arm();
    for (std::uint32_t i = 0; i < 10; i++) r.push(smp(i * 1000));
    CHECK_EQ(r.trigger(10'000, Reason::ShutOff), true);
    CHECK_EQ(int(r.state()), int(State::Post));
    CHECK_EQ(r.trigger(11'000, Reason::Manual), false);   // the first reason sticks
    CHECK_EQ(int(r.reason()), int(Reason::ShutOff));
    CHECK_EQ(r.push(smp(10'000 + kPostUs - 1)), false);
    CHECK_EQ(int(r.state()), int(State::Post));
    CHECK_EQ(r.push(smp(10'000 + kPostUs)), true);        // the window has run: frozen
    CHECK_EQ(int(r.state()), int(State::Frozen));
    const auto n = r.sample_count();
    CHECK_EQ(r.push(smp(10'000 + kPostUs + 1000)), false);   // frozen: no more samples
    CHECK_EQ(r.sample_count(), n);
}

// triggered(): true from the trigger on (post window and frozen), false before and after arm/stop. A caller that
// finds the capture already frozen by an earlier event (a detach) still wants to keep it for the failed step.
static void test_triggered_covers_post_and_frozen() {
    Rec r;
    CHECK_EQ(r.triggered(), false);
    r.arm();
    CHECK_EQ(r.triggered(), false);
    r.push(smp(0));
    r.trigger(1000, Reason::Detach);
    CHECK_EQ(r.triggered(), true);                 // post window
    r.push(smp(1000 + kPostUs));
    CHECK_EQ(int(r.state()), int(State::Frozen));
    CHECK_EQ(r.triggered(), true);                 // frozen
    CHECK_EQ(r.trigger(2000, Reason::ShutOff), false);   // a second trigger is refused, which is why keep() must not depend on it
    r.arm();
    CHECK_EQ(r.triggered(), false);
}

static void test_trigger_survives_clock_wrap() {
    Rec r;
    r.arm();
    const std::uint32_t t0 = 0xFFFFFF00u;
    r.push(smp(t0));
    r.trigger(t0, Reason::Manual);
    CHECK_EQ(r.push(smp(t0 + 1000)), false);
    CHECK_EQ(r.push(smp(t0 + kPostUs)), true);   // wraps past 2^32
}

static void test_auto_trigger_rules() {
    CHECK_EQ(int(auto_reason(Mark::HardReset)), int(Reason::HardReset));
    CHECK_EQ(int(auto_reason(Mark::Detach)), int(Reason::Detach));
    CHECK_EQ(int(auto_reason(Mark::Accept)), int(Reason::None));
    CHECK_EQ(int(auto_reason(Mark::TxRequest)), int(Reason::None));
    Rec r;
    r.arm();
    r.mark(100, Mark::TxRequest, 3, 3300);
    r.mark(200, Mark::Accept, 3);
    CHECK_EQ(int(r.state()), int(State::Armed));   // ordinary events never trigger
    r.mark(300, Mark::HardReset);
    CHECK_EQ(int(r.state()), int(State::Post));
    CHECK_EQ(int(r.reason()), int(Reason::HardReset));
    CHECK_EQ(r.trigger_us(), 300);
    CHECK_EQ(r.marker(r.marker_count() - 1).code, int(Mark::Trigger));   // trigger is on the timeline too
    r.mark(350, Mark::Detach);   // already past the trigger: kept as a marker, does not retrigger
    CHECK_EQ(int(r.reason()), int(Reason::HardReset));
}

static void test_marker_ring_wraps() {
    Rec r;
    r.arm();
    for (std::uint32_t i = 0; i < 6; i++) r.mark(i, Mark::Rx, 0, std::uint16_t(i));
    CHECK_EQ(r.marker_count(), 4);
    CHECK_EQ(r.marker(0).arg, 2);
    CHECK_EQ(r.marker(3).arg, 5);
}

static void test_rearm_clears() {
    Rec r;
    r.arm();
    r.push(smp(0));
    r.trigger(0, Reason::Manual);
    r.arm();
    CHECK_EQ(r.sample_count(), 0);
    CHECK_EQ(r.marker_count(), 0);
    CHECK_EQ(int(r.state()), int(State::Armed));
    CHECK_EQ(int(r.reason()), int(Reason::None));
}

// A realistic trace: 1 kHz with a few jittered gaps, VBUS flat then falling.
static Recorder<1920, 64> make_trace(std::size_t n) {
    Recorder<1920, 64> r;
    r.arm();
    std::uint32_t t = 1'000'000;
    unsigned seed = 1;
    for (std::size_t i = 0; i < n; i++) {
        seed = seed * 1103515245u + 12345u;
        t += 1000 + ((seed >> 16) % 7 == 0 ? 250 : 0);   // occasional late sample
        std::uint16_t v = i < n / 2 ? 3300 : std::uint16_t(3300 - (i - n / 2) * 3);
        r.push(smp(t, v, std::uint8_t(100 + (i % 5 == 0)), 7));
    }
    r.mark(1'000'500, Mark::TxRequest, 2, 3300);
    r.mark(1'000'900, Mark::Accept, 2);
    return r;
}

static void test_encode_decode_roundtrip() {
    const auto r = make_trace(1500);
    std::vector<std::uint8_t> buf(16000);
    const std::size_t n = encode(r, buf);
    CHECK_EQ(n > 0, true);
    CHECK_EQ(n < 1500 * 8, true);   // smaller than the raw samples
    std::vector<Sample> sm(1500);
    std::vector<Marker> mk(8);
    Decoded d;
    CHECK_EQ(decode({buf.data(), n}, sm, mk, d), true);
    CHECK_EQ(d.n_samples, 1500);
    CHECK_EQ(d.n_markers, 2);
    for (std::size_t i = 0; i < 1500; i++) {
        const Sample a = r.sample(i);
        if (sm[i].t_us != a.t_us || sm[i].vbus_mv != a.vbus_mv || sm[i].cc1 != a.cc1 || sm[i].cc2 != a.cc2) {
            CHECK_EQ(i, -1);   // report the first mismatch and stop
            break;
        }
    }
    CHECK_EQ(mk[1].code, int(Mark::Accept));
    CHECK_EQ(mk[0].arg, 3300);
}

static void test_encode_flat_run_is_tiny() {
    Recorder<1920, 64> r;
    r.arm();
    for (std::uint32_t i = 0; i < 1000; i++) r.push(smp(i * 1000));   // identical every ms
    std::vector<std::uint8_t> buf(512);
    const std::size_t n = encode(r, buf);
    CHECK_EQ(n > 0 && n < 40, true);   // header + first sample + one dt flag + one run
    std::vector<Sample> sm(1000);
    std::vector<Marker> mk(1);
    Decoded d;
    CHECK_EQ(decode({buf.data(), n}, sm, mk, d), true);
    CHECK_EQ(sm[999].t_us, 999000);
}

static void test_encode_extremes_and_empty() {
    Recorder<16, 4> r;
    r.arm();
    r.push(smp(0, 0, 0, 0));
    r.push(smp(5, 65535, 255, 255));        // big jumps both ways
    r.push(smp(2'000'000, 0, 0, 0));        // a long gap
    std::vector<std::uint8_t> buf(64);
    const std::size_t n = encode(r, buf);
    CHECK_EQ(n > 0, true);
    std::vector<Sample> sm(3);
    std::vector<Marker> mk(1);
    Decoded d;
    CHECK_EQ(decode({buf.data(), n}, sm, mk, d), true);
    CHECK_EQ(sm[1].vbus_mv, 65535); CHECK_EQ(sm[1].cc1, 255);
    CHECK_EQ(sm[2].t_us, 2'000'000); CHECK_EQ(sm[2].vbus_mv, 0);
    Recorder<16, 4> e;
    e.arm();
    const std::size_t ne = encode(e, buf);
    CHECK_EQ(ne, kHeaderBytes);
    CHECK_EQ(decode({buf.data(), ne}, sm, mk, d), true);
    CHECK_EQ(d.n_samples, 0);
}

static void test_encoder_chunking_does_not_change_the_stream() {
    const auto r = make_trace(700);
    std::vector<std::uint8_t> whole(8000);
    const std::size_t n = encode(r, whole);
    CHECK_EQ(n > 0, true);
    for (std::size_t chunk : {1u, 7u, 64u, 1000u}) {
        Encoder<decltype(r)> e(r);
        std::vector<std::uint8_t> got;
        std::vector<std::uint8_t> piece(chunk);
        while (!e.done()) {
            const std::size_t k = e.read(piece);
            got.insert(got.end(), piece.begin(), piece.begin() + k);
            if (k == 0) break;
        }
        CHECK_EQ(got.size(), n);
        CHECK_EQ(std::equal(got.begin(), got.end(), whole.begin()), true);
    }
}

static void test_encode_small_output_fails_cleanly() {
    const auto r = make_trace(500);
    std::vector<std::uint8_t> small(100);
    CHECK_EQ(encode(r, small), 0);
}

static void test_decode_rejects_damage() {
    const auto r = make_trace(300);
    std::vector<std::uint8_t> buf(4000);
    const std::size_t n = encode(r, buf);
    std::vector<Sample> sm(300);
    std::vector<Marker> mk(8);
    Decoded d;
    CHECK_EQ(decode({buf.data(), n - 3}, sm, mk, d), false);   // truncated
    auto bad = buf;
    bad[0] = 'X';
    CHECK_EQ(decode({bad.data(), n}, sm, mk, d), false);       // magic
    std::vector<Sample> few(10);
    CHECK_EQ(decode({buf.data(), n}, few, mk, d), false);      // output too small
    CHECK_EQ(decode({buf.data(), 5}, sm, mk, d), false);       // shorter than the header
}

static void test_thumb_keeps_peaks_and_marks() {
    Recorder<1920, 64> r;
    r.arm();
    for (std::uint32_t i = 0; i < 1000; i++) r.push(smp(i * 1000, i == 500 ? 100 : 5000));   // one 1 ms dip
    r.mark(250'000, Mark::Accept, 1);
    r.trigger(990'000, Reason::ShutOff);
    const Thumb th = make_thumb(r);
    CHECK_EQ(th.vmax_mv, 5000);
    std::uint8_t lowest = 255;
    for (std::size_t c = 0; c < kThumbCols; c++) { if (th.lo[c] < lowest) lowest = th.lo[c]; CHECK_EQ(th.lo[c] <= th.hi[c], true); }
    CHECK_EQ(lowest, scale_u8(100, 5000));   // the dip is not averaged away
    CHECK_EQ(th.hi[0], 255);
    CHECK_EQ(th.accept_col, column_of(250'000, 0, 999'000));
    CHECK_EQ(th.trig_col, column_of(990'000, 0, 999'000));
    CHECK_EQ(th.accept_col > 0 && th.accept_col < kThumbCols, true);
}

static void test_thumb_gap_filled_and_empty() {
    Recorder<1920, 64> r;
    const Thumb none = make_thumb(r);
    CHECK_EQ(none.vmax_mv, 0);
    CHECK_EQ(none.accept_col, 0xFF);
    r.arm();
    r.push(smp(0, 3000));
    r.push(smp(1'000'000, 3000));   // 1 s hole: the columns in between take the neighbour's value
    const Thumb th = make_thumb(r);
    for (std::size_t c = 0; c < kThumbCols; c++) CHECK_EQ(th.hi[c], 255);
}

static void test_column_of_edges() {
    CHECK_EQ(column_of(0, 0, 1000), 0);
    CHECK_EQ(column_of(1000, 0, 1000), kThumbCols - 1);
    CHECK_EQ(column_of(5000, 0, 1000), kThumbCols - 1);
    CHECK_EQ(column_of(5, 10, 1000), 0);
    CHECK_EQ(column_of(7, 7, 7), 0);
}

static void test_view_works_with_thumb() {
    std::vector<Sample> s{smp(0, 100), smp(1000, 200)};
    std::vector<Marker> m{{500, std::uint8_t(Mark::Accept), 0, 0}};
    View v{s, m, Reason::ShutOff, 1000};
    const Thumb th = make_thumb(v);
    CHECK_EQ(th.vmax_mv, 200);
    CHECK_EQ(th.accept_col, column_of(500, 0, 1000));
}

static void test_classify_pd_messages() {
    // header: type 4:0, n_objects 14:12. Control messages have none.
    auto hdr = [](unsigned type, unsigned n) { return std::uint16_t(type | n << 12); };
    CHECK_EQ(int(classify_pd(false, hdr(2, 1), 3u << 28).code), int(Mark::TxRequest));
    CHECK_EQ(classify_pd(false, hdr(2, 1), 3u << 28).step, 3);
    CHECK_EQ(int(classify_pd(true, hdr(3, 0), 0).code), int(Mark::Accept));
    CHECK_EQ(int(classify_pd(true, hdr(6, 0), 0).code), int(Mark::PsRdy));
    CHECK_EQ(int(classify_pd(true, hdr(4, 0), 0).code), int(Mark::Reject));
    CHECK_EQ(int(classify_pd(true, hdr(12, 0), 0).code), int(Mark::Wait));
    CHECK_EQ(int(classify_pd(true, hdr(13, 0), 0).code), int(Mark::SoftReset));
    CHECK_EQ(int(classify_pd(false, hdr(13, 0), 0).code), int(Mark::SoftReset));
    // GoodCRC is noise; our own Accept (a source role message) is not what we watch.
    CHECK_EQ(int(classify_pd(true, hdr(1, 0), 0).code), int(Mark::None));
    CHECK_EQ(int(classify_pd(false, hdr(3, 0), 0).code), int(Mark::Tx));
    // A received data message (Source_Capabilities) and a sent Get_Source_Cap keep their header.
    CHECK_EQ(int(classify_pd(true, hdr(1, 4), 0).code), int(Mark::Rx));
    CHECK_EQ(classify_pd(true, hdr(1, 4), 0).arg, hdr(1, 4));
    CHECK_EQ(int(classify_pd(false, hdr(7, 0), 0).code), int(Mark::Tx));
    // An Accept that arrives as a data message type 3 is a different message (Battery_Status? no: Sink_Cap).
    CHECK_EQ(int(classify_pd(true, hdr(3, 2), 0).code), int(Mark::Rx));
    // Extended messages are not control messages whatever their low bits say.
    CHECK_EQ(int(classify_pd(true, std::uint16_t(0x8000 | 6), 0).code), int(Mark::Rx));
}

static void test_mark_of_reset_and_cc() {
    CHECK_EQ(int(mark_of_reset(true)), int(Mark::HardReset));
    CHECK_EQ(int(mark_of_reset(false)), int(Mark::SoftReset));
    CHECK_EQ(int(mark_of_cc(true)), int(Mark::Attach));
    CHECK_EQ(int(mark_of_cc(false)), int(Mark::Detach));
}

static void test_names() {
    CHECK_STR(reason_name(Reason::ShutOff), "shutoff");
    CHECK_STR(reason_name(Reason::None), "none");
    CHECK_STR(mark_name(Mark::PsRdy), "ps_rdy");
    CHECK_STR(mark_name(Mark::None), "none");
}

static void test_clear_zeroes_everything() {
    Recorder<8, 4> r;
    r.arm();
    for (unsigned i = 0; i < 12; i++) r.push({i + 1, 3300, 9, 9});
    r.mark(5, Mark::Accept, 1, 2);
    r.trigger(6, Reason::Manual);
    r.clear();
    CHECK_EQ(int(r.state()), int(State::Off));
    CHECK_EQ(r.sample_count(), 0);
    CHECK_EQ(r.marker_count(), 0);
    CHECK_EQ(r.trigger_us(), 0u);
    Recorder<8, 4> z;
    CHECK_EQ(std::memcmp(&r, &z, sizeof r), 0);   // byte for byte what a fresh one holds
}

// A C135-shaped capture for tests/test_transcap_py.py (the host decoder reads the same bytes): 5 V, a PPS 3.3 V
// request, Accept, the rail follows down in 30 steps, then it is cut and CC1 goes with it. Samples are 1 ms apart
// except for a 6 ms hole (a long redraw). The Python test recomputes the series from these formulas.
static void dump_for_python() {
    const char* dir = std::getenv("K2_DUMP");
    if (!dir) return;
    static Recorder<> r;   // static: the default ring is 16 KB
    r.arm();
    std::uint32_t t = 1'000'000;
    for (int i = 0; i < 300; i++) {
        t += (i == 150) ? 6000 : 1000;
        std::uint16_t v = i < 100 ? 5000 : i < 130 ? std::uint16_t(5000 - (i - 99) * 57) : i < 265 ? 3300 : 0;
        r.push({t, v, std::uint8_t(i < 265 ? 40 : 0), std::uint8_t(i % 50 == 0 ? 1 : 0)});
        if (i == 99) r.mark(t, Mark::TxRequest, 2, 3300);
        if (i == 100) r.mark(t, Mark::Accept);
        if (i == 112) r.mark(t, Mark::PsRdy);
        if (i == 265) r.trigger(t, Reason::ShutOff);
    }
    for (int i = 300; r.state() != State::Frozen; i++) { t += 1000; r.push({t, 0, 0, 0}); }
    std::vector<std::uint8_t> buf(8192);
    const std::size_t n = encode(r, buf);
    CHECK_EQ(n > 0, true);
    char path[512];
    std::snprintf(path, sizeof path, "%s/transcap.bin", dir);
    if (FILE* f = std::fopen(path, "wb")) { std::fwrite(buf.data(), 1, n, f); std::fclose(f); }
}


// Samples are paced from the moment a read starts, so a pass every millisecond yields a sample on every
// pass (1 kHz); stamping the end of the read pushed every other pass under the gap (2 ms spacing).
static void test_pacer_samples_every_millisecond_pass() {
    Pacer p;
    p.reset(0);
    int taken = 0;
    std::uint32_t last = 0;
    for (std::uint32_t pass = 1; pass <= 100; pass++) {
        const std::uint32_t now = pass * 1000;         // main-loop pass, wfi wakes on SysTick
        if (p.take(now)) {                             // the read itself costs ~250 us, which must not count
            if (taken > 0) CHECK_EQ(now - last, 1000);
            last = now;
            taken++;
        }
    }
    CHECK_EQ(taken, 100);
}

static void test_pacer_keeps_the_minimum_gap_and_survives_the_clock_wrap() {
    Pacer p;
    p.reset(0xFFFFFF00u);                              // micros() wraps every ~71 min
    int taken = 0;
    std::uint32_t last = 0;
    for (std::uint32_t i = 0; i < 400; i++) {
        const std::uint32_t now = 0xFFFFFF00u + i * 300;   // polled faster than the gap
        if (p.take(now)) {
            if (taken > 0) CHECK_EQ(now - last >= kSampleGapUs, true);
            last = now;
            taken++;
        }
    }
    CHECK_EQ(taken > 100, true);
    CHECK_EQ(taken < 160, true);
}

static void test_pacer_first_poll_after_reset_samples_at_once() {
    Pacer p;
    p.reset(5000);
    CHECK_EQ(p.take(5000), true);
    CHECK_EQ(p.take(5100), false);
    CHECK_EQ(p.take(5000 + kSampleGapUs), true);
}

int main() {
    dump_for_python();
    test_clear_zeroes_everything();
    test_names();
    test_classify_pd_messages();
    test_mark_of_reset_and_cc();
    test_ring_keeps_newest_in_order();
    test_not_armed_records_nothing();
    test_trigger_post_window_then_freeze();
    test_triggered_covers_post_and_frozen();
    test_trigger_survives_clock_wrap();
    test_auto_trigger_rules();
    test_marker_ring_wraps();
    test_rearm_clears();
    test_encode_decode_roundtrip();
    test_encode_flat_run_is_tiny();
    test_encode_extremes_and_empty();
    test_encoder_chunking_does_not_change_the_stream();
    test_encode_small_output_fails_cleanly();
    test_decode_rejects_damage();
    test_thumb_keeps_peaks_and_marks();
    test_thumb_gap_filled_and_empty();
    test_column_of_edges();
    test_view_works_with_thumb();
    test_pacer_samples_every_millisecond_pass();
    test_pacer_keeps_the_minimum_gap_and_survives_the_clock_wrap();
    test_pacer_first_poll_after_reset_samples_at_once();
    return test_report("test_transcap");
}
