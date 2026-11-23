// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The PC-link icon's state: nothing / power only / PC using the port / PC and the shell at work (k2/core/pc_link.hpp).
#include "test_util.h"
#include "k2/core/pc_link.hpp"

using namespace k2;
using pclink::State;

static State step(pclink::Tracker& t, int32_t aux, bool host, bool shell = false) { return t.update({aux, host, shell}); }

static void test_empty_port_shows_nothing() {
    pclink::Tracker t;
    CHECK_EQ(int(step(t, 0, false)), int(State::None));
    CHECK_EQ(int(step(t, 3400, false)), int(State::None));
    CHECK_EQ(int(step(t, 3900, false)), int(State::None));   // below the 4.0 V rise: not up yet
}

static void test_power_only_then_host_then_shell() {
    pclink::Tracker t;
    CHECK_EQ(int(step(t, 5000, false)), int(State::Power));
    CHECK_EQ(int(step(t, 5000, true)), int(State::Host));
    CHECK_EQ(int(step(t, 5000, true, true)), int(State::Shell));
    CHECK_EQ(int(step(t, 5000, true, false)), int(State::Host));
    CHECK_EQ(int(step(t, 5000, false, false)), int(State::Power));   // suspended or unmounted: the port is only powered
}

static void test_shell_without_a_host_is_not_shown() {
    pclink::Tracker t;
    CHECK_EQ(int(step(t, 5000, false, true)), int(State::Power));
    CHECK_EQ(int(step(t, 0, false, true)), int(State::None));
}

static void test_aux_hysteresis() {
    pclink::Tracker t;
    CHECK_EQ(int(step(t, 4000, false)), int(State::Power));   // at or above 4.0 V: up
    CHECK_EQ(int(step(t, 3600, false)), int(State::Power));   // between 3.5 and 4.0 V: stays up
    CHECK_EQ(int(step(t, 3500, false)), int(State::Power));   // gone is strictly below 3.5 V
    CHECK_EQ(int(step(t, 3499, false)), int(State::None));
    CHECK_EQ(int(step(t, 3800, false)), int(State::None));    // and stays down until 4.0 V
}

static void test_unknown_aux_follows_the_host() {
    pclink::Tracker t;
    CHECK_EQ(int(step(t, -1, false)), int(State::None));      // no reading, no host: nothing to show
    CHECK_EQ(int(step(t, -1, true)), int(State::Host));       // a mounted host is proof of a PC on its own
    CHECK_EQ(int(step(t, -1, true, true)), int(State::Shell));
    CHECK_EQ(int(step(t, 5000, false)), int(State::Power));
    CHECK_EQ(int(step(t, -1, false)), int(State::Power));     // a lost reading keeps the last port state
}

static void test_a_host_holds_the_port_up_through_a_sagging_aux() {
    pclink::Tracker t;
    step(t, 5000, true);
    CHECK_EQ(int(step(t, 3000, true)), int(State::Host));     // a mounted PC is there whatever the divider reads
    CHECK_EQ(int(step(t, 3000, false)), int(State::None));
}

static void test_names_are_the_wire_names() {
    CHECK_STR(pclink::name(State::None), "none");
    CHECK_STR(pclink::name(State::Power), "power");
    CHECK_STR(pclink::name(State::Host), "host");
    CHECK_STR(pclink::name(State::Shell), "shell");
    CHECK_EQ(int(State::Shell), 3);   // the wire byte (session part, offset 31)
}

int main() {
    test_empty_port_shows_nothing();
    test_power_only_then_host_then_shell();
    test_shell_without_a_host_is_not_shown();
    test_aux_hysteresis();
    test_unknown_aux_follows_the_host();
    test_a_host_holds_the_port_up_through_a_sagging_aux();
    test_names_are_the_wire_names();
    return test_report("test_pc_link");
}
