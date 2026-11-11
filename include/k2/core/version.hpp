// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Build identification, filled in by the Makefile (-DK2_FW_VERSION / -DK2_GIT_HASH on version.cpp).
// The version is ONE string, `<major>.<minor>.<patch>-<iteration>-<hash>[-dirty]` (tools/fw_version.py), shown
// everywhere: PING, boot screen, K2B1 line, flasher messages. kGitHash is the raw describe, kept for traceability.
#pragma once

namespace k2 {

extern const char kFwVersion[];   // e.g. "1.0.0-435-26b245ac" ("-dirty" appended on a dirty tree), at most 29 characters
extern const char kGitHash[];     // `git describe --always --dirty` of the build tree, e.g. "v1.0-435-g26b245ac"
extern const char kBuildDate[];   // __DATE__ of the last version.cpp compile

}  // namespace k2
