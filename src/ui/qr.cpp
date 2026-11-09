// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#include "k2/ui/qr.hpp"

#include <cstring>

#include "qrcodegen.h"

namespace k2::qr {

int encode(std::string_view text, Workspace& w) {
    return encode_version(text, w.code, w.temp, kBufLen, kMaxVersion);
}

int encode_placed(std::uint8_t* code, std::uint8_t* data_temp, std::size_t len, int max_version) {
    if (len == 0 || max_version < 1 || max_version > kMaxVersionAny) return 0;
    if (!qrcodegen_encodeBinary(data_temp, len, code, qrcodegen_Ecc_LOW, qrcodegen_VERSION_MIN,
                                max_version, qrcodegen_Mask_AUTO, false))
        return 0;
    return qrcodegen_getSize(code);
}

int encode_version(std::string_view text, std::uint8_t* code, std::uint8_t* data_temp, std::size_t buf_len, int max_version) {
    if (text.empty() || max_version < 1 || max_version > kMaxVersionAny) return 0;
    if (buf_len < buf_len_for(max_version)) return 0;   // the payload plus the working storage must fit
    std::memcpy(data_temp, text.data(), text.size());
    return encode_placed(code, data_temp, text.size(), max_version);
}

int encode_text(const char* text, Workspace& w, int max_version) {
    if (!text || !*text || max_version < 1 || max_version > kMaxVersion) return 0;
    if (!qrcodegen_encodeText(text, w.temp, w.code, qrcodegen_Ecc_LOW, qrcodegen_VERSION_MIN, max_version,
                              qrcodegen_Mask_AUTO, false))
        return 0;
    return qrcodegen_getSize(w.code);
}

bool dark(const Workspace& w, int x, int y) { return dark_code(w.code, x, y); }

bool dark_code(const std::uint8_t* code, int x, int y) {
    const int n = qrcodegen_getSize(code);
    if (x < 0 || y < 0 || x >= n || y >= n) return false;
    return qrcodegen_getModule(code, x, y);
}

}  // namespace k2::qr
