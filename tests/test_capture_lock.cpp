// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#include "k2/ui/capture.hpp"
#include "test_util.h"
#include "ui_stubs.h"

using namespace k2;

namespace {
Ui *active;
unsigned attempted, stolen;

void nested_http_capture() {
    attempted++;
    if (capture_begin(*active)) {
        stolen++;
        capture_end(*active, 1000);
    }
}
}

int main() {
    Ui ui;
    ui.init(1000);
    active = &ui;
    g_stub.on_report = nested_http_capture;
    CHECK_EQ(capture_screen(ui, 1, 1000), true);
    CHECK_EQ(attempted > 0, true);
    CHECK_EQ(stolen, 0u);
    g_stub.on_report = nullptr;
    CHECK_EQ(capture_begin(ui), true);
    CHECK_EQ(capture_begin(ui), false);
    CHECK_EQ(capture_screen(ui, 2, 1000), false);
    capture_end(ui, 1000);
    CHECK_EQ(capture_screen(ui, 3, 1000), true);
    return test_report("test_capture_lock");
}
