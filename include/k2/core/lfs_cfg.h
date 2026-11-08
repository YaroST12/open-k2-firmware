// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
/* littlefs build configuration, force-included into lfs.c and by include/k2/core/fs.hpp before lfs.h.
 * No malloc, no asserts, no printing: static buffers only. */
#ifndef K2_LFS_CFG_H
#define K2_LFS_CFG_H
#define LFS_NO_MALLOC
#define LFS_NO_ASSERT
#define LFS_NO_DEBUG
#define LFS_NO_WARN
#define LFS_NO_ERROR
#define LFS_NAME_MAX 40     /* lfs_info lives on the stack: 41 bytes instead of 256 */
#endif
