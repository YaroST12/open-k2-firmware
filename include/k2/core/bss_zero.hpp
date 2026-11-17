// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Zero-initialization elision for ARM .bss singletons: k2_reset_main() zeroes _sbss.._ebss before
// static constructors run, so boot-once .bss objects do not need redundant zero stores or memsets
// in _sub_I_65535_0.0. Host builds keep explicit initialization for stack/heap test instances.
#pragma once

#ifdef __arm__
#define K2_BSS_ZERO
#define K2_BSS_INIT(val)
#else
#define K2_BSS_ZERO = {}
#define K2_BSS_INIT(val) = val
#endif
