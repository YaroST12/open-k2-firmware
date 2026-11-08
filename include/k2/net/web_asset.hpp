// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The web dashboard served at http://192.168.7.1/: one HTML file, gzip-compressed at build time
// and linked into flash (tools/embed_web.py, build/web_asset.cpp). Nothing is copied to RAM.
#pragma once
#include <cstddef>
#include <cstdint>

namespace k2 {

struct WebAsset {
    const std::uint8_t* data;
    std::size_t len;
    bool gzip;   // send Content-Encoding: gzip
};

WebAsset web_asset();

}  // namespace k2
