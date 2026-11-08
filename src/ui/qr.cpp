// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#include "k2/ui/qr.hpp"

#include <cstring>

#include "qrcodegen.h"

namespace k2::qr {

int encode(std::string_view text, Workspace& w) {
    if (text.empty() || text.size() > kMaxBytes) return 0;
    std::memcpy(w.temp, text.data(), text.size());
    if (!qrcodegen_encodeBinary(w.temp, text.size(), w.code, qrcodegen_Ecc_LOW, qrcodegen_VERSION_MIN,
                                kMaxVersion, qrcodegen_Mask_AUTO, false))
        return 0;
    return qrcodegen_getSize(w.code);
}

int encode_text(const char* text, Workspace& w, int max_version) {
    if (!text || !*text || max_version < 1 || max_version > kMaxVersion) return 0;
    if (!qrcodegen_encodeText(text, w.temp, w.code, qrcodegen_Ecc_LOW, qrcodegen_VERSION_MIN, max_version,
                              qrcodegen_Mask_AUTO, false))
        return 0;
    return qrcodegen_getSize(w.code);
}

bool dark(const Workspace& w, int x, int y) {
    const int n = qrcodegen_getSize(w.code);
    if (x < 0 || y < 0 || x >= n || y >= n) return false;
    return qrcodegen_getModule(w.code, x, y);
}

}  // namespace k2::qr
