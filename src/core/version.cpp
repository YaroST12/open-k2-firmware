// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#include "k2/core/version.hpp"

#include <cstddef>

#ifndef K2_FW_VERSION
#define K2_FW_VERSION "0.0.0-0-unknown"
#endif
#ifndef K2_GIT_HASH
#define K2_GIT_HASH "unknown"
#endif

namespace k2 {

const char kFwVersion[] = K2_FW_VERSION;
const char kGitHash[] = K2_GIT_HASH;
const char kBuildDate[] = __DATE__;

// PING carries the version as the tail of a 58-byte payload (proto 1 + uptime 4 + git 24 = 29 bytes before it), so
// 29 characters is the most the wire holds; the boot-info field holds 31 and the record below 43. "1.0.0-99999-26b245ac-dirty"
// is 26.
static_assert(sizeof(K2_FW_VERSION) - 1 <= 29, "firmware version string longer than PING can carry");

// The firmware identity record: "K2FW" plus the version text, NUL padded to 48 bytes. link.ld puts section .fwid
// first in .text, at 0x08004404, right behind the bootloader's own meta words, so a flasher can read the full
// version from a package or from the device with no firmware running. It lies in code space: the bootloader never
// looks at it, and the one-byte major.minor version at 0x08004000 stays exactly as the bootloader expects.
struct FwRecord {
    char magic[4];
    char text[44];
};
static_assert(sizeof(FwRecord) == 48, "the flasher reads 48 bytes at app offset 3076");

constexpr FwRecord make_record() {
    FwRecord r{{'K', '2', 'F', 'W'}, {}};
    for (std::size_t i = 0; i + 1 < sizeof r.text && K2_FW_VERSION[i] != '\0'; i++) r.text[i] = K2_FW_VERSION[i];
    return r;
}

extern const FwRecord kFwRecord;
__attribute__((section(".fwid"), used)) const FwRecord kFwRecord = make_record();

}  // namespace k2
