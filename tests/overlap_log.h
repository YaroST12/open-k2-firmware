// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Opt-in overlap guard for the UI host tests (include/k2/ui/trunc.hpp "Overlap guard"). A test calls overlap_enable() once;
// from then on every trunc::context() change (and test_report) checks the frame that just ended.
#pragma once
void overlap_enable();
// Forget every recorded draw and who owns each panel pixel: a test that blanks g_panel itself starts a fresh frame with it.
void overlap_reset();
