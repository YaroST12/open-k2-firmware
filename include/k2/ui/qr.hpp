// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// QR code encoder: a thin C++ face on the vendored Nayuki generator (third_party/qrcodegen), fixed to what the QR screens
// need: error correction L, automatic mask, versions 1..12 (21..65 modules per side). encode_text() picks numeric,
// alphanumeric or byte mode by itself (the share frames of k2/ui/qrshare.hpp are all alphanumeric); encode() is byte mode.
// Pure code over a caller-owned workspace (no heap, no globals); the screens lease the workspace from the scratch arena.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace k2::qr {

inline constexpr int kMaxVersion = 12;
inline constexpr int kMaxModules = kMaxVersion * 4 + 17;                 // 65
inline constexpr std::size_t kBufLen = ((kMaxModules * kMaxModules) + 7) / 8 + 1;   // 530, the generator's buffer for version 12
inline constexpr std::size_t kMaxBytes = 367;                            // version 12, level L, byte mode
inline constexpr std::size_t kMaxAlnum = 535;                            // version 12, level L, alphanumeric mode

struct Workspace {
    std::uint8_t code[kBufLen];   // the finished symbol (the generator's packed module bitmap)
    std::uint8_t temp[kBufLen];   // the payload goes in here, the generator uses it as working storage
};

// Encodes `text` (1..kMaxBytes bytes) into `w`, byte mode. Returns the modules per side, or 0 when the text is empty or
// does not fit version 12 at level L.
int encode(std::string_view text, Workspace& w);

// Encodes the NUL-terminated `text` in the densest of numeric / alphanumeric / byte mode, versions 1..max_version (at most
// kMaxVersion). Returns the modules per side, or 0 when it is empty or does not fit.
int encode_text(const char* text, Workspace& w, int max_version = kMaxVersion);

// Module (x, y) of the symbol encode() just made; false outside it.
bool dark(const Workspace& w, int x, int y);

}  // namespace k2::qr
