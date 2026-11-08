// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// QR share record builders (see k2/ui/qrshare_records.hpp).
#include "k2/ui/qrshare_records.hpp"

#include <algorithm>
#include <cstring>

#include "k2/core/fmt.hpp"

namespace k2::qrshare::records {

std::size_t emarker(const emrec::Record& rec, std::string_view name, const emk_quirk::Quirks& quirks, std::span<std::uint8_t> out) {
    const std::size_t qlen = quirks.empty() ? 0 : emk_quirk::block_size(quirks.count);
    if (out.size() < kEmarkerHead + qlen) return 0;
    out[0] = qlen ? kEmarkerQuirks : 0;
    const std::size_t n = std::min(name.size(), kEmarkerName);
    std::memset(out.data() + 1, 0, kEmarkerName);
    std::memcpy(out.data() + 1, name.data(), n);
    std::memcpy(out.data() + 1 + kEmarkerName, rec.data(), rec.size());
    if (qlen && emk_quirk::encode(quirks, out.subspan(kEmarkerHead, qlen)) != qlen) return 0;
    return kEmarkerHead + qlen;
}

std::size_t pdcaps(const pd::SourceCaps& caps, std::span<std::uint8_t> out) {
    if (!caps.valid || caps.num_pdos == 0) return 0;
    const std::size_t n = std::min<std::size_t>(caps.num_pdos, pd::kMaxPdos);
    if (out.size() < kPdCapsHead + 4 * n) return 0;
    out[0] = static_cast<std::uint8_t>(n);
    out[1] = static_cast<std::uint8_t>((caps.epr ? kPdCapsEpr : 0) | (caps.epr_partial ? kPdCapsEprPartial : 0));
    out[2] = caps.spec_rev;
    out[3] = caps.active_cc;
    out[4] = static_cast<std::uint8_t>(caps.raw_header);
    out[5] = static_cast<std::uint8_t>(caps.raw_header >> 8);
    for (std::size_t i = 0; i < n; i++) {
        const std::uint32_t w = caps.pdos[i].raw;
        for (unsigned b = 0; b < 4; b++) out[kPdCapsHead + 4 * i + b] = static_cast<std::uint8_t>(w >> (8 * b));
    }
    return kPdCapsHead + 4 * n;
}

std::size_t card(const report_card::Card& c, std::span<std::uint8_t> out) {
    if (out.size() < kMaxCard) return 0;
    return report_card::encode_card(out, c);
}

std::size_t identity(std::string_view fw, std::string_view git, unsigned proto, std::span<std::uint8_t> out) {
    char num[12];
    const std::size_t nl = fmt::format(num, "%u", proto);
    fw = fw.substr(0, 15);
    git = git.substr(0, 39);
    const std::size_t n = 3 + fw.size() + 3 + git.size() + 3 + nl;   // "fw=" fw " g=" git " p=" proto
    if (out.size() < n) return 0;
    std::uint8_t* p = out.data();
    const auto put = [&p](std::string_view s) { std::memcpy(p, s.data(), s.size()); p += s.size(); };
    put("fw=");
    put(fw);
    put(" g=");
    put(git);
    put(" p=");
    put(std::string_view(num, nl));
    return n;
}

std::size_t boot(const bootinfo::Snapshot& s, std::span<std::uint8_t> out) {
    char tmp[bootinfo::kQrMax + 1];
    const std::size_t n = bootinfo::format(s, tmp);
    if (n == 0 || out.size() < n) return 0;
    std::memcpy(out.data(), tmp, n);
    return n;
}

std::size_t log(std::string_view git, std::uint32_t now_ms, std::uint32_t base_ms, bool older, std::size_t entries, std::span<std::uint8_t> out) {
    if (entries == 0 || out.size() < kLogHeadMax + entries) return 0;
    git = git.substr(0, kLogGit);
    const std::size_t head = 1 + git.size() + 1 + 8;
    std::uint8_t* p = out.data();
    std::memmove(p + head, p + kLogHeadMax, entries);
    *p++ = static_cast<std::uint8_t>(git.size());
    std::memcpy(p, git.data(), git.size());
    p += git.size();
    *p++ = older ? kLogOlder : 0;
    for (const std::uint32_t v : {base_ms, now_ms})
        for (unsigned b = 0; b < 4; b++) *p++ = static_cast<std::uint8_t>(v >> (8 * b));
    return head + entries;
}

}  // namespace k2::qrshare::records
