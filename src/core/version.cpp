// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#include "k2/core/version.hpp"

#ifndef K2_FW_VERSION
#define K2_FW_VERSION "dev"
#endif
#ifndef K2_GIT_HASH
#define K2_GIT_HASH "unknown"
#endif

namespace k2 {

const char kFwVersion[] = K2_FW_VERSION;
const char kGitHash[] = K2_GIT_HASH;
const char kBuildDate[] = __DATE__;

}  // namespace k2
