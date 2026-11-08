// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Build identification, filled in by the Makefile (-DK2_FW_VERSION / -DK2_GIT_HASH on version.cpp).
#pragma once

namespace k2 {

extern const char kFwVersion[];   // e.g. "1.0"
extern const char kGitHash[];     // `git describe --always --dirty` of the build tree
extern const char kBuildDate[];   // __DATE__ of the last version.cpp compile

}  // namespace k2
