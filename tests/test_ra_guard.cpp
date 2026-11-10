// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#include "k2/pd/ra_guard.hpp"
#include <cstdio>
#include <string>
using namespace k2::raguard;
static int g_n = 0, g_bad = 0;
#define CHECK(c) do { g_n++; if (!(c)) { g_bad++; std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)

// A passive K2, no source attached to anything yet (VBUS down): the readings the dialog classifies.
static Topology seen(int cc1, int cc2, int vbus = 0, bool passive = true) {
    return classify({.cc1_mv = cc1, .cc2_mv = cc2, .vbus_mv = vbus, .passive = passive});
}

int main() {
    // The measured levels (hardware 2026-11-06): a receptacle charger shows Rp on BOTH lines.
    CHECK(seen(3300, 3300) == Topology::DirectReceptacle);   // saturated at the 3.3 V rail (Baseus, CUKTECH, OnePlus)
    CHECK(seen(3200, 3250) == Topology::DirectReceptacle);
    CHECK(seen(kRpMinMv, kRpMinMv) == Topology::DirectReceptacle);   // the threshold itself is Rp

    // A cable carries ONE CC wire: Rp on one line, the other idle (0 V), Ra-level (~0.3 V) or the PLUS-coupled ~1.7 V.
    CHECK(seen(3300, 0) == Topology::CableSingleCc);
    CHECK(seen(0, 3300) == Topology::CableSingleCc);
    CHECK(seen(3300, 320) == Topology::CableSingleCc);       // Ra of an e-marked cable on the far line
    CHECK(seen(3300, 1707) == Topology::CableSingleCc);      // CC2 idles at ~1.7 V and is NOT Rp
    CHECK(seen(1707, 3300) == Topology::CableSingleCc);
    CHECK(seen(kRpMinMv, kRpMinMv - 1) == Topology::CableSingleCc);   // one below the threshold on one line

    // Nothing waiting: both lines idle or Ra-level.
    CHECK(seen(0, 0) == Topology::NoSource);
    CHECK(seen(0, 1707) == Topology::NoSource);              // the idle CC2 alone is no source
    CHECK(seen(320, 320) == Topology::NoSource);
    CHECK(seen(kRpMinMv - 1, kRpMinMv - 1) == Topology::NoSource);

    // Unknown never blocks: a missing reading, a K2 that is not passive, a source already powering VBUS.
    CHECK(seen(-1, 3300) == Topology::Unknown && seen(3300, -1) == Topology::Unknown && seen(-1, -1) == Topology::Unknown);
    CHECK(seen(3300, 3300, 0, false) == Topology::Unknown);   // our own Rd / Ra / an inline sink on the lines
    CHECK(seen(3300, 0, 0, false) == Topology::Unknown);
    CHECK(seen(3300, 0, 5000) == Topology::Unknown);          // VBUS up: a source is already attached to something
    CHECK(seen(3300, 0, kVbusAttachedMv - 1) == Topology::CableSingleCc);
    CHECK(seen(3300, 0, kVbusAttachedMv) == Topology::Unknown);
    CHECK(seen(3300, 0, -1) == Topology::CableSingleCc);      // no power monitor reading: a missing VBUS is not proof of a source

    // What the dialog does with it: only a receptacle charger (or an unknown) may run the spoof.
    CHECK(spoof_allowed(Topology::DirectReceptacle) && spoof_allowed(Topology::Unknown));
    CHECK(!spoof_allowed(Topology::CableSingleCc) && !spoof_allowed(Topology::NoSource));
    CHECK(std::string(name(Topology::DirectReceptacle)) == "direct" && std::string(name(Topology::CableSingleCc)) == "cable");
    CHECK(std::string(name(Topology::NoSource)) == "none" && std::string(name(Topology::Unknown)) == "unknown");

    // The peak window: a PLUS press sags a line for a few samples; the peak of the last kSamples keeps the verdict.
    PeakWindow w;
    CHECK(w.count() == 0 && w.cc1() == -1 && w.cc2() == -1);
    w.add(3300, 3300);
    CHECK(w.cc1() == 3300 && w.cc2() == 3300 && w.count() == 1);
    w.add(3300, 1700);   // CC2 sags while PLUS is down
    w.add(3300, 1700);
    CHECK(w.cc1() == 3300 && w.cc2() == 3300);   // the earlier sample still holds the peak
    CHECK(classify({.cc1_mv = w.cc1(), .cc2_mv = w.cc2(), .vbus_mv = 0, .passive = true}) == Topology::DirectReceptacle);
    for (std::size_t n = 0; n < PeakWindow::kSamples; ++n) w.add(3300, 1700);   // the sag outlasts the window: the cable verdict
    CHECK(w.count() == PeakWindow::kSamples && w.cc2() == 1700);
    CHECK(classify({.cc1_mv = w.cc1(), .cc2_mv = w.cc2(), .vbus_mv = 0, .passive = true}) == Topology::CableSingleCc);
    // an unplugged charger leaves the window after kSamples samples too (no stale Direct)
    for (std::size_t n = 0; n < PeakWindow::kSamples; ++n) w.add(0, 0);
    CHECK(w.cc1() == 0 && w.cc2() == 0);
    w.clear();
    CHECK(w.count() == 0 && w.cc1() == -1);
    w.add(-1, -1);   // an unreadable sample is no reading
    CHECK(w.cc1() == -1 && w.cc2() == -1);
    w.add(3300, 0);
    CHECK(w.cc1() == 3300 && w.cc2() == 0);

    // The refusal text: the full message, one row per line, nothing wider than the 27 characters one row fits.
    CHECK(kRefusalLines.size() == 4);
    for (const char* s : kRefusalLines) CHECK(s && std::string(s).size() <= 27);
    CHECK(std::string(kRefusalLines[0]).find("Spoof needs the charger") == 0);

    // The waiting classifier over an Rp trace (samples every 20 ms, as the UI tick feeds it).
    RpTrace tr;
    CHECK(tr.classify(0, false) == WaitKind::Unknown);   // empty trace: no verdict
    // A charger's Rp is steady: from kSteadyMs of history on, it is "charger waiting".
    for (std::uint32_t t = 0; t <= 280; t += 20) tr.add(t, true);
    CHECK(tr.classify(280, false) == WaitKind::Unknown);          // 280 ms of history: not yet
    tr.add(300, true);
    CHECK(tr.classify(300, false) == WaitKind::ChargerWaiting);   // steady for 300 ms
    for (std::uint32_t t = 320; t <= 800; t += 20) tr.add(t, true);
    CHECK(tr.classify(800, false) == WaitKind::ChargerWaiting);
    CHECK(tr.classify(800, true) == WaitKind::Unknown);           // VBUS up: a source is already attached
    // Nothing on the lines at all: no source.
    RpTrace none;
    for (std::uint32_t t = 0; t <= 800; t += 20) none.add(t, false);
    CHECK(none.classify(800, false) == WaitKind::NoSource);
    // A dual-role port toggles its Rp open about every 50-100 ms (a phone looking for a partner): not a charger.
    RpTrace phone;
    for (std::uint32_t t = 0; t <= 800; t += 20) phone.add(t, (t / 50) % 2 == 0);    // 50 ms on/off
    CHECK(phone.classify(800, false) == WaitKind::DualRolePort);
    RpTrace phone2;
    for (std::uint32_t t = 0; t <= 800; t += 20) phone2.add(t, (t / 100) % 2 == 0);  // 100 ms on/off
    CHECK(phone2.classify(800, false) == WaitKind::DualRolePort);
    // An unplug is a single edge, not a toggle: after it, no Rp in the recent span = no source.
    RpTrace unplugged;
    for (std::uint32_t t = 0; t <= 800; t += 20) unplugged.add(t, t <= 400);
    CHECK(unplugged.classify(800, false) == WaitKind::NoSource);
    // A charger plugged in 200 ms ago: Rp seen recently but not steady yet = no verdict, not "no source".
    RpTrace fresh;
    for (std::uint32_t t = 600; t <= 800; t += 20) fresh.add(t, true);
    CHECK(fresh.classify(800, false) == WaitKind::Unknown);
    // The ring wraps: history older than the window is forgotten.
    RpTrace wrap;
    for (std::uint32_t t = 0; t < 2000; t += 20) wrap.add(t, t >= 1200);   // Rp only from t=1200 on
    CHECK(wrap.classify(1999, false) == WaitKind::ChargerWaiting);         // 799 ms of steady Rp inside the window

    std::printf("%d checks, %d failed\n", g_n, g_bad);
    return g_bad != 0;
}
