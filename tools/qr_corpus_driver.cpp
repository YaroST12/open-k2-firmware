// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Host driver behind tools/qr_corpus.py (measurement only: not part of the firmware, not a test).
//
// Builds the QR-share record corpus — the golden vectors of tests/fixtures/qrshare.tsv and tests/fixtures/qrlink.tsv,
// plus the built extras (a cable with all eight quirk rows, the all-limits report card, a 10-PDO EPR source) — and for
// every record prints one TSV row with the sizes the firmware's own code produces: the raw bytes, the LZ4 block
// (lz::compress), what qrshare::pack would send, and the link text of qrshare::link::encode for both kind letters,
// with the QR modules qr::encode picks for it. tools/qr_corpus.py turns this into the measurement table.
//
// Usage: qr_corpus_driver <repo root> [extra-hex file]   (extra rows: one record hex per line, type 4 = card)
// Output: "# key=value" header lines, then "name type raw_len raw_hex lz4_len pack_len plain_len packed_len plain_link packed_link plain_mod packed_mod".
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "k2/chargertest/report_card.hpp"
#include "k2/core/lz.hpp"
#include "k2/pd/emk_quirk.hpp"
#include "k2/pd/emrec.hpp"
#include "k2/pd/pd.hpp"
#include "k2/ui/qr.hpp"
#include "k2/ui/qrshare.hpp"
#include "k2/ui/qrshare_link.hpp"
#include "k2/ui/qrshare_records.hpp"
#include "qrshare_fixtures.h"

namespace qs = k2::qrshare;
namespace lk = k2::qrshare::link;
namespace rc = k2::report_card;
namespace ct = k2::charger_test;
using Bytes = std::vector<std::uint8_t>;

static Bytes unhex(const std::string& s) {
    Bytes b;
    for (size_t i = 0; i + 1 < s.size(); i += 2)
        b.push_back(std::uint8_t(std::stoul(s.substr(i, 2), nullptr, 16)));
    return b;
}
static std::string hex(const Bytes& b) {
    static const char* d = "0123456789abcdef";
    std::string s;
    for (auto x : b) { s += d[x >> 4]; s += d[x & 15]; }
    return s;
}
static std::vector<std::string> split(const std::string& line, char sep) {
    std::vector<std::string> f;
    std::stringstream ss(line);
    std::string c;
    while (std::getline(ss, c, sep)) f.push_back(c);
    return f;
}

struct Entry {
    std::string name;
    qs::Type type;
    Bytes raw;
};

static void add(std::map<std::string, Entry>& by_hex, std::vector<Entry>& list, std::string name, qs::Type t, Bytes raw) {
    if (raw.empty()) return;
    auto it = by_hex.find(hex(raw));
    if (it != by_hex.end()) return;   // the same record already in (a packed golden inflates to its plain twin)
    by_hex[hex(raw)] = {name, t, raw};
    list.push_back({std::move(name), t, std::move(raw)});
}

// Inflate a packed golden row back to the record it carries (the LZ4 block), when it is one.
static Bytes maybe_inflate(const Bytes& rec) {
    Bytes out(qs::kMaxRecord);
    const std::size_t n = k2::lz::decompress(rec, out);
    if (n != k2::lz::kError && n != 0) { out.resize(n); return out; }
    return rec;
}

static qs::Type type_of_name(const std::string& n) {
    if (n.rfind("emk", 0) == 0) return qs::Type::Emarker;
    if (n.rfind("pdcaps", 0) == 0) return qs::Type::PdCaps;
    if (n.rfind("identity", 0) == 0) return qs::Type::Identity;
    if (n.rfind("boot", 0) == 0) return qs::Type::BootInfo;
    if (n.rfind("log", 0) == 0) return qs::Type::Log;
    return qs::Type::Card;
}

static void read_fixtures(const std::string& root, std::map<std::string, Entry>& by_hex, std::vector<Entry>& list) {
    {
        std::ifstream in(root + "/tests/fixtures/qrshare.tsv");
        std::string line;
        while (std::getline(in, line)) {
            if (line.empty() || line[0] == '#') continue;
            const auto f = split(line, '\t');
            if (f.size() < 4) continue;
            Bytes rec = unhex(f[3]);
            if (f[0].size() > 3 && f[0].substr(f[0].size() - 3) == "_lz") rec = maybe_inflate(rec);
            add(by_hex, list, f[0], qs::Type(std::stoi(f[1])), std::move(rec));
        }
    }
    {
        std::ifstream in(root + "/tests/fixtures/qrlink.tsv");
        std::string line;
        while (std::getline(in, line)) {
            if (line.empty() || line[0] == '#') continue;
            const auto f = split(line, '\t');
            if (f.size() < 4) continue;
            Bytes rec = unhex(f[2]);
            if (f[1] == "1") rec = maybe_inflate(rec);   // the row carries the LZ4 block; measure the record itself
            add(by_hex, list, f[0], type_of_name(f[0]), std::move(rec));
        }
    }
}

// A cable with all eight quirk rows: the biggest record the share feature builds (510 B, no link today).
static Bytes emk_rows8() {
    Bytes out(qs::records::kMaxEmarker);
    const std::size_t n = qs::records::emarker(qsfx::oneplus_mag_clone(), "OnePlus 100W mag", qsfx::quirk_rows(8), out);
    out.resize(n);
    return out;
}

// The report card with every field at its limit: the worst case for any packing (202 B, incompressible-ish).
static Bytes worst_card() {
    rc::Card c{};
    c.id = {0xFFFFFFFFu, 0xFFFF, 0xFFFF, 255};
    c.card_no = 0xFFFF; c.runs = 0xFFFF;
    c.boots = 0xFFFFFFFFu; c.uptime_s = 0xFFFFFFFFu; c.fw = 0xFFFFFFFFu;
    c.handover_ds = 0xFFFF;
    c.spec_rev = 2; c.num_pdos = 11;
    c.handover = ct::HandoverOutcome::InContract; c.state = ct::State::Attach; c.reason = ct::Reason::Capped;
    c.n_ranges = rc::kMaxRanges; c.ev_flags = 0xFF;
    std::strcpy(c.name, "12345678901234567890");
    c.pd_ma = 6350;
    for (auto& g : c.group) g = {rc::GV::Fail, 51100, 255, 63, 51100};
    for (auto& r : c.range) r = {uint8_t(ct::Algorithm::SprAvs), 255, 511, 511, 511, 511, 6350};
    for (auto& e : c.epr) e = {480, {rc::GV::Fail, 51100, 255, 63, 51100}};
    c.cable = {0xFFFFFFFFu, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFF, 0xFF};
    c.resumed = 0xFF; c.src_flags = 0xFF;
    for (auto& u : c.ufcs) u = {255, 511, 6350};
    c.times = 255; c.last_boots = 0xFFFF; c.last_uptime_s = 0xFFFFFFu;
    Bytes out(rc::kMaxRecord);
    const std::size_t n = rc::encode_card(out, c);
    out.resize(n);
    return out;
}

// A 10-PDO EPR source, the hardware shape (fixed 5/9/12/15/20 V, PPS, two zeros, 28 V, 36 V, SPR AVS).
static Bytes caps_epr_real() {
    k2::pd::SourceCaps c{};
    c.valid = true;
    c.num_pdos = 10;
    c.active_cc = 2;
    c.spec_rev = 2;
    c.epr = true;
    c.raw_header = 0x5BB1;
    const std::uint32_t w[10] = {0x2A01912Cu | (1u << 23), 0x0002D12Cu, 0x0003C12Cu, 0x0004B1F4u, 3u << 30 | 210u << 17 | 33u << 8 | 100u,
                                 0, 0, 560u << 10 | 500u, 720u << 10 | 400u, 3u << 30 | 1u << 28 | 280u << 17 | 150u << 8 | 140u};
    for (unsigned i = 0; i < 10; i++) c.pdos[i].raw = w[i];
    Bytes out(qs::records::kMaxPdCaps);
    const std::size_t n = qs::records::pdcaps(c, out);
    out.resize(n);
    return out;
}

int main(int argc, char** argv) {
    const std::string root = argc > 1 ? argv[1] : ".";
    std::map<std::string, Entry> by_hex;
    std::vector<Entry> list;
    read_fixtures(root, by_hex, list);
    add(by_hex, list, "emk_rows8", qs::Type::Emarker, emk_rows8());
    add(by_hex, list, "card_worst", qs::Type::Card, worst_card());
    add(by_hex, list, "pdcaps_epr_real", qs::Type::PdCaps, caps_epr_real());
    if (argc > 2) {   // extra record hex lines (type 4) from the caller, e.g. captures re-encoded by qr_corpus.py
        std::ifstream in(argv[2]);
        std::string line, name = "extra";
        unsigned k = 0;
        while (std::getline(in, line)) {
            if (line.empty() || line[0] == '#') continue;
            add(by_hex, list, "extra" + std::to_string(k++), qs::Type::Card, unhex(line));
        }
        (void)name;
    }

    std::printf("# kBase=%.*s\n", int(lk::kBase.size()), lk::kBase.data());
    std::printf("# kHead=%zu kMaxRecord=%zu kQrBytesV11=%zu kMaxText=%zu b64_per_byte=4/3\n", lk::kHead, lk::kMaxRecord,
                lk::kQrBytesV11, lk::kMaxText);
    std::printf("# name\ttype\traw_len\traw_hex\tlz4_len\tpack_len\tplain_len\tpacked_len\tplain_link\tpacked_link\tplain_mod\tpacked_mod\n");
    static k2::qr::Workspace w;
    for (const Entry& e : list) {
        Bytes z(k2::lz::bound(e.raw.size()));
        const std::size_t lz4 = k2::lz::compress(e.raw, z);
        z.resize(lz4);
        Bytes pk(k2::lz::bound(e.raw.size()));
        const std::size_t pack = qs::pack(e.raw, pk);
        pk.resize(pack);
        char lp[qs::kTextBuf] = {}, lq[qs::kTextBuf] = {};
        const std::size_t nl = lk::encode(e.type, e.raw, false, lp);
        const std::size_t nq = pack ? lk::encode(e.type, pk, true, lq) : 0;
        const int mp = nl ? k2::qr::encode(std::string_view(lp, nl), w) : 0;
        const int mq = nq ? k2::qr::encode(std::string_view(lq, nq), w) : 0;
        std::printf("%s\t%u\t%zu\t%s\t%zu\t%zu\t%zu\t%zu\t%s\t%s\t%d\t%d\n", e.name.c_str(), unsigned(e.type), e.raw.size(),
                    hex(e.raw).c_str(), lz4, pack, nl, nq, nl ? lp : "-", nq ? lq : "-", mp, mq);
    }
    return 0;
}
