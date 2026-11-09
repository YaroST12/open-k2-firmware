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
// The vendored generator's own tables cover every version up to 40 (its header caps qrcodegen_VERSION_MAX at 10, but
// the asserts that enforce the cap are compiled out, NDEBUG, and the capacity tool tools/qr_capacity.c relies on the
// same); only a workspace's size limits a call above kMaxVersion. The QR scan-test path (k2/ui/ui_qrtest.hpp) uses this.
inline constexpr int kMaxVersionAny = 40;

// The buffer length the generator needs for `version` (its qrcodegen_BUFFER_LEN_FOR_VERSION, spelled out so this header
// does not include the vendored one): ceil(modules^2 / 8) + 1.
constexpr std::size_t buf_len_for(int version) {
    const int m = version * 4 + 17;
    return (static_cast<std::size_t>(m) * m + 7) / 8 + 1;
}
static_assert(kBufLen == buf_len_for(kMaxVersion), "kBufLen is the version-12 buffer length");

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

// Byte-mode encode into caller-owned buffers, for versions above kMaxVersion: `data_temp` must hold the payload at its
// front on entry (the generator uses the rest as working storage) and `code` receives the symbol; each buffer must be
// at least buf_len_for(max_version) bytes. Returns the modules per side, or 0 when the payload is empty, the buffers
// are too small or the payload does not fit `max_version` at level L.
int encode_placed(std::uint8_t* code, std::uint8_t* data_temp, std::size_t len, int max_version);
// The same with the payload copied in from `text` first: `buf_len` is the size of each of `code` / `data_temp`.
int encode_version(std::string_view text, std::uint8_t* code, std::uint8_t* data_temp, std::size_t buf_len, int max_version);

// encode_text() into caller-owned buffers (`code` and `temp`, each at least buf_len_for(max_version) bytes, max_version at
// most kMaxVersion: the text modes pack up to the version-12 alphanumeric capacity). Returns the modules per side, or 0.
int encode_text_placed(const char* text, std::uint8_t* code, std::uint8_t* temp, int max_version = kMaxVersion);

// A hook the generator calls at the start of each of its eight mask candidates, so a caller can keep servicing USB and the
// PD engine through the slow encode of a large symbol. nullptr (the default) removes it; the hook must not encode.
void set_poll(void (*poll)());

// Module (x, y) of the symbol encode() just made; false outside it.
bool dark(const Workspace& w, int x, int y);
// The same for a symbol in a caller buffer (a workspace above kMaxVersion): `code` is the packed bitmap encode_placed wrote.
bool dark_code(const std::uint8_t* code, int x, int y);

}  // namespace k2::qr
