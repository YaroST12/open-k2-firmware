// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The QR scan-test rig (remote Cmd::QrTest, tools range 0x50-0x5F): a full-screen test QR on the panel, filled with
// the longest link one code of the asked version carries, so a phone camera can be checked against every (version,
// px, quiet) the 240 px panel can show. Pure model (this header, host-tested): the QR capacity table, the fit table
// over the panel geometry, the show-argument validation and the max-length kind-'t' link builder. The page itself is
// QrTestPage (k2/ui/ui_pages.hpp); src/ui/ui_qrtest.cpp holds only the Ui bridge the remote backend calls.
//
// Geometry (the same maths as qrview::plan_in): a symbol of m modules with a quiet zone of q modules at p pixels per
// module takes box = (m + 2*q) * p pixels; it fits the panel when box <= 240. The largest version any accepted
// (px >= 2, quiet >= 1) combination can show is version 25 ((117 + 2) * 2 = 238 px), so the qrtest encode workspace
// is sized for version 25, not 40.
#pragma once
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>

#include "k2/core/fmt.hpp"
#include "k2/ui/qr.hpp"
#include "k2/ui/qrshare_link.hpp"

namespace k2 {
class Ui;
}

namespace k2::qrtest {

// The accepted ranges of the show op (Cmd::QrTest op 1).
inline constexpr int kVersionMin = 1, kVersionMax = 40;   // the vendored generator covers 1..40
inline constexpr int kPxMin = 2, kPxMax = 3;              // below 2 px a phone camera loses the code on this panel
inline constexpr int kQuietMin = 1, kQuietMax = 4;        // the QR specification asks for 4
inline constexpr int kPanelPx = 240;                     // qrview::kPanelPx
// The largest version the panel can show at px >= 2: (modules(25) + 2*1) * 2 = 238 <= 240, version 26 needs 246.
inline constexpr int kMaxShownVersion = 25;

// Byte-mode capacity at error correction L, in characters, per QR version 1..40 (index 0 unused). Measured with
// tools/qr_capacity.c over the vendored generator (binary search over the real encoder), not copied from a table:
// v11 = 321 (the share link's one-code limit), v13 = 425, v25 = 1273. tests/test_ui_qrtest.cpp compares this table
// against the tool's output.
inline constexpr std::array<std::uint16_t, kVersionMax + 1> kByteL = {
    0, 17, 32, 53, 78, 106, 134, 154, 192, 230, 271, 321, 367, 425, 458, 520, 586, 644, 718, 792, 858,
    929, 1003, 1091, 1171, 1273, 1367, 1465, 1528, 1628, 1732, 1840, 1952, 2068, 2188, 2303, 2431, 2563, 2699, 2809, 2953};

constexpr int modules_of(int version) { return version * 4 + 17; }
constexpr int byte_l(int version) { return version >= kVersionMin && version <= kVersionMax ? kByteL[version] : 0; }
// The most record bytes whose kind-'t' link (kHead + base64url) still fits `version` at level L in byte mode.
constexpr std::size_t max_record(int version) { return qrshare::link::max_record_for(byte_l(version)); }

// One row of the fit table: `version` drawn at (px, quiet) on the 240 px panel.
struct FitRow {
    int version, modules, px, quiet, box;
    bool fits;
    int chars;             // byte-L characters of the version
    std::size_t record;    // max record B of the version
};
constexpr FitRow fit_row(int version, int px, int quiet) {
    const int m = modules_of(version);
    const int box = (m + 2 * quiet) * px;
    return {version, m, px, quiet, box, box <= kPanelPx, byte_l(version), max_record(version)};
}

inline constexpr std::size_t kReasonMax = 40;   // check_show's reason buffer
int check_show(std::uint8_t version, std::uint8_t px, std::uint8_t quiet, char* reason);

// ---- the fit table as text (Cmd::QrTest op 0) ----
// One header line, then per (px, quiet) combo (px 3 then 2, quiet 4..1) two rows: the largest version that fits the
// panel and the first that does not, e.g. "v13 m69 p3 q4 box231 fit c425 r281" / "v14 m73 p3 q4 box243 no c458 r306".
inline constexpr int kListLines = 17;      // header + 8 combos x 2 rows
inline constexpr std::size_t kListChunk = 48;   // data bytes per response
// Formats line `index` (0 = header, 1..kListLines-1 = rows) into `out`; returns its length (0 = bad index).
int list_line(int index, std::span<char> out);
std::size_t list_total();                                        // the whole table's length in bytes
// One response chunk: off(2) total(2) n(1) data(n), n <= kListChunk. Returns 5 + n, or 0 when off > total.
std::size_t list_chunk(std::uint16_t off, std::span<std::uint8_t> out);

// ---- the max-length kind-'t' link of a version (what the test screen encodes) ----
// kBase + '1' + 't' + '.' + base64url(record), where record = the ASCII label "QR scan test v<NN> <N>px" NUL-padded to
// max_record(version) bytes — the same bytes qrshare::link::encode_kind('t', padded_record, out) writes, so the link
// is exactly the longest one a code of that version carries (it does not fit the version below). Returns the length,
// 0 when `out` is too small or the version is out of range.
std::size_t build_link(int version, int px, std::span<char> out);

// The qrtest encode workspace: sized for kMaxShownVersion (two buffers of qr::buf_len_for(25) = 1713 B). Owned as a
// member of QrTestPage, so it lives in the page arena, not in .bss and not in the scratch arena.
inline constexpr std::size_t kWsBufLen = qr::buf_len_for(kMaxShownVersion);
struct Workspace {
    std::uint8_t code[kWsBufLen];   // the finished symbol
    std::uint8_t temp[kWsBufLen];   // the link text goes in here; the generator uses it as working storage
};
static_assert(kWsBufLen == 1713, "version 25 buffer length");

// ---- the Ui bridge (src/ui/ui_qrtest.cpp, firmware side only; the remote backend calls these) ----
// Opens the QrTest page with (version, px, quiet); false when the arena or the ports refuse it now.
bool show(Ui& ui, std::uint8_t version, std::uint8_t px, std::uint8_t quiet, std::uint32_t now_ms);
// Pops the QrTest page when it is on the menu stack; returns whether it was showing.
bool stop(Ui& ui, std::uint32_t now_ms);

}  // namespace k2::qrtest

// ---- inline implementations of the pure model ----
namespace k2::qrtest {

inline int check_show(std::uint8_t version, std::uint8_t px, std::uint8_t quiet, char* reason) {
    const std::span<char> out(reason, kReasonMax);
    if (version < kVersionMin || version > kVersionMax)
        return static_cast<int>(fmt::format(out, "version %u outside %u..%u", unsigned(version),
                                            unsigned(kVersionMin), unsigned(kVersionMax)));
    if (px < kPxMin || px > kPxMax)
        return static_cast<int>(fmt::format(out, "px %u outside %u..%u", unsigned(px), unsigned(kPxMin), unsigned(kPxMax)));
    if (quiet < kQuietMin || quiet > kQuietMax)
        return static_cast<int>(fmt::format(out, "quiet %u outside %u..%u", unsigned(quiet), unsigned(kQuietMin),
                                            unsigned(kQuietMax)));
    const FitRow r = fit_row(version, px, quiet);
    if (!r.fits)
        return static_cast<int>(fmt::format(out, "box %u px over the %d px panel", unsigned(r.box), kPanelPx));
    return 0;   // accepted (and then version <= kMaxShownVersion: px >= 2 keeps box >= (m + 2) * 2 <= 240)
}

namespace detail {
// The (px, quiet) combos of the fit table, in order: px 3 then 2, quiet 4..1.
constexpr int kListCombos = 8;
constexpr void combo_at(int c, int& px, int& quiet) {
    px = c < 4 ? 3 : 2;
    quiet = 4 - (c % 4);
}
// The largest version that fits at (px, quiet) (the box grows with the version, so a scan up finds it); 0 = none.
constexpr int max_fit_version(int px, int quiet) {
    for (int v = kVersionMax; v >= kVersionMin; v--)
        if (fit_row(v, px, quiet).fits) return v;
    return 0;
}
}  // namespace detail

inline int list_line(int index, std::span<char> out) {
    if (index == 0) return static_cast<int>(fmt::format(out, "# v m p q box fit c r\n"));
    if (index < 1 || index >= kListLines) return 0;
    const int c = (index - 1) / 2;                       // combo index
    const bool boundary = (index - 1) % 2 == 1;         // second row of the combo: the first refused version
    int px, quiet;
    detail::combo_at(c, px, quiet);
    const int top = detail::max_fit_version(px, quiet);
    const int v = boundary ? top + 1 : top;
    if (v < kVersionMin || v > kVersionMax) return 0;
    const FitRow r = fit_row(v, px, quiet);
    return static_cast<int>(fmt::format(out, "v%u m%u p%u q%u box%u %s c%u r%u\n", unsigned(r.version), unsigned(r.modules),
                                        unsigned(r.px), unsigned(r.quiet), unsigned(r.box), r.fits ? "fit" : "no",
                                        unsigned(r.chars), unsigned(r.record)));
}

inline std::size_t list_total() {
    std::size_t total = 0;
    char line[48];
    for (int i = 0; i < kListLines; i++) total += static_cast<std::size_t>(list_line(i, line));
    return total;
}

inline std::size_t list_chunk(std::uint16_t off, std::span<std::uint8_t> out) {
    const std::size_t total = list_total();
    if (off > total || out.size() < 5) return 0;
    const std::size_t want = std::min({std::size_t(kListChunk), out.size() - 5, total - off});
    out[0] = static_cast<std::uint8_t>(off);
    out[1] = static_cast<std::uint8_t>(off >> 8);
    out[2] = static_cast<std::uint8_t>(total);
    out[3] = static_cast<std::uint8_t>(total >> 8);
    out[4] = static_cast<std::uint8_t>(want);
    std::size_t pos = 0, copied = 0;
    char line[48];
    for (int i = 0; i < kListLines && copied < want; i++) {
        const int n = list_line(i, line);
        if (n <= 0) continue;
        const std::size_t len = static_cast<std::size_t>(n);
        if (pos + len > off) {   // this line overlaps the window [off, off + want)
            const std::size_t skip = off > pos ? off - pos : 0;
            const std::size_t take = std::min(len - skip, want - copied);
            std::memcpy(out.data() + 5 + copied, line + skip, take);
            copied += take;
        }
        pos += len;
    }
    return 5 + copied;
}

inline std::size_t build_link(int version, int px, std::span<char> out) {
    if (version < kVersionMin || version > kVersionMax || px < kPxMin || px > kPxMax) return 0;
    const std::size_t rec = max_record(version);
    char label[24];
    const int ll = static_cast<int>(fmt::format(std::span<char>(label, sizeof label), "QR scan test v%u %upx",
                                                unsigned(version), unsigned(px)));
    if (ll <= 0) return 0;
    const std::size_t head = qrshare::link::kHead;   // kBase + the version digit + the kind letter + '.'
    const std::size_t need = head + qrshare::link::b64_len(rec);
    if (out.size() < need) return 0;
    std::memcpy(out.data(), qrshare::link::kBase.data(), qrshare::link::kBase.size());
    out[qrshare::link::kBase.size()] = qrshare::link::kVersion;      // '1': a plain text record, never packed
    out[qrshare::link::kBase.size() + 1] = qrshare::link::kKindText; // 't'
    out[qrshare::link::kBase.size() + 2] = '.';
    // base64url of the record: the label bytes, then NUL padding up to `rec` (group by group, no big record buffer)
    std::size_t o = head;
    for (std::size_t i = 0; i < rec;) {
        const std::size_t take = std::min<std::size_t>(3, rec - i);
        std::uint8_t grp[3] = {0, 0, 0};
        for (std::size_t k = 0; k < take; k++)
            grp[k] = i + k < static_cast<std::size_t>(ll) ? static_cast<std::uint8_t>(label[i + k]) : 0;
        o += qrshare::link::base64url_encode({grp, take}, out.subspan(o));
        i += take;
    }
    return o;
}

}  // namespace k2::qrtest
