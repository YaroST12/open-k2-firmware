// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
/* Zero-wait flash placement markers (C and C++).
 *
 * The AT32F403A runs the first 256 KB of flash (0x08000000..0x0803FFFF) with zero wait states; everything above
 * it pays wait states on every fetch. The application is larger than the window, so the linker script
 * (link.ld) puts one curated "hot block" right after the firmware ID: every input section named
 * `.text.hot.*` / `.rodata.hot.*`, then the pinned libc helpers, ending at `__hot_end`. The gate
 * (tools/gate.py, step "hot") fails when `__hot_end` passes 0x08040000 and prints the headroom.
 *
 *   K2_HOT       before a function: `K2_HOT void f();` or `K2_HOT static int g() {...}`, also on templates,
 *                member functions and extern "C" ISRs; on a lambda it follows the parameter list:
 *                `[](int x) K2_HOT { ... }`.
 *   K2_HOT_DATA  before a small const table: `K2_HOT_DATA static const uint16_t kLut[16] = {...};`
 *
 * Why a per-use section name: GCC rejects one shared section name across a COMDAT (template, inline, in-class)
 * function and an ordinary one ("section type conflict"). `<file>:<line>` is unique per use and, being a
 * `.text.hot.` prefix, still matches the linker pattern. Do not mark whole files or init code: the block has
 * to stay a curated set (budget about 120 KB; the window is shared with everything that happens to link below it).
 *
 * Off the target (host tests) both markers expand to nothing. */
#ifndef K2_CORE_HOT_H_
#define K2_CORE_HOT_H_

#define K2_HOT_STR2_(x) #x
#define K2_HOT_STR_(x) K2_HOT_STR2_(x)

#if defined(__arm__) && defined(__GNUC__)
#define K2_HOT __attribute__((section(".text.hot." __FILE_NAME__ ":" K2_HOT_STR_(__LINE__))))
#define K2_HOT_DATA __attribute__((section(".rodata.hot." __FILE_NAME__ ":" K2_HOT_STR_(__LINE__))))
#else
#define K2_HOT
#define K2_HOT_DATA
#endif

#endif /* K2_CORE_HOT_H_ */
