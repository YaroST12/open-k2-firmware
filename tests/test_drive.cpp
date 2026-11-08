// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Host tests for the drive content (k2::drive): file table, text rendering, and the volume built
// from it. Dumps $K2_DUMP/drive.img for tests/test_drive_image.py.
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "k2/drivers/drive.hpp"
#include "k2/pd/pdtrace_view.hpp"
#include "k2/pd/emjson.hpp"
#include "k2/core/fmt.hpp"
#include "k2/chargertest/report_card_json.hpp"
#include "test_util.h"

using namespace k2;

static std::size_t status_text(void*, std::span<char> out) {
    return fmt::format(out, "V: 5.02 V\nI: 1.250 A\nP: 6.28 W\n");
}
static std::size_t log_text(void*, std::span<char> out) {
    const char s[] = "[BOOT] hello\n[SYS] up\n";
    std::memcpy(out.data(), s, sizeof s - 1);
    return sizeof s - 1;
}
static const char kSaved[] = "[OLD] saved line\n";

static drive::Env env(bool saved) {
    drive::Env e{};
    e.version = "9.9";
    e.git = "abc1234-dirty";
    e.status = status_text;
    e.log = log_text;
    if (saved) e.saved_log = std::span<const char>(kSaved, sizeof kSaved - 1);
    return e;
}

static emrec::Record rec(int n, const char* name) {
    std::array<std::uint32_t, 5> v = {0xFF00A041, 0x18002B01, 0, 0x40510000u + n, 0x000A2640};
    return emrec::make(v, 2, name);
}

static std::string file(drive::Drive& d, const char* name) {
    for (std::size_t i = 0; i < d.files().size(); i++) {
        const auto& f = d.files()[i];
        if (std::memcmp(f.name.data(), vfat::name83(name).data(), 11) == 0 && (f.parent < 0 ? true : true)) {
            std::string s(f.size, 0);
            d.read_file(i, 0, std::span<std::uint8_t>(reinterpret_cast<std::uint8_t*>(s.data()), s.size()));
            return s;
        }
    }
    return "<missing>";
}
static bool has(const drive::Drive& d, const char* name) {
    for (const auto& f : d.files())
        if (std::memcmp(f.name.data(), vfat::name83(name).data(), 11) == 0) return true;
    return false;
}

static void test_base_files() {
    drive::Drive d;
    d.init(env(false));
    for (const char* n : {"README.TXT", "STATUS.TXT", "LOG.TXT", "RIPPLE.CSV", "BLACKBOX"}) CHECK_EQ(has(d, n), 1);
    CHECK_EQ(has(d, "EMARKERS"), 0);                                    // the old writer folder is gone
    CHECK_EQ(has(d, "SAVEDLOG.TXT"), 0);                                // nothing saved yet
    // a log saved later appears after set_saved_log(), and goes away again when it is cleared
    d.set_saved_log(std::span<const char>(kSaved, sizeof kSaved - 1));
    CHECK_EQ(has(d, "SAVEDLOG.TXT"), 1);
    CHECK_EQ(file(d, "SAVEDLOG.TXT") == std::string(kSaved), 1);
    d.set_saved_log({});
    CHECK_EQ(has(d, "SAVEDLOG.TXT"), 0);
    const std::string readme = file(d, "README.TXT");
    CHECK_EQ(readme.find("9.9") != std::string::npos, 1);
    CHECK_EQ(readme.find("abc1234-dirty") != std::string::npos, 1);
    CHECK_EQ(readme.find("read-only") != std::string::npos, 1);
    CHECK_EQ(readme.find("emarker save") != std::string::npos, 1);

    const std::string st = file(d, "STATUS.TXT");
    CHECK_EQ(st.size(), 1024u);
    CHECK_EQ(st.compare(0, 9, "V: 5.02 V"), 0);
    CHECK_EQ(st.back(), '\n');
    CHECK_EQ(st[500], ' ');                                             // space padded

    const std::string lg = file(d, "LOG.TXT");
    CHECK_EQ(lg.size(), 3072u);
    CHECK_EQ(lg.compare(0, 12, "[BOOT] hello"), 0);
    CHECK_EQ(lg.back(), '\n');

    const std::string rip = file(d, "RIPPLE.CSV");
    CHECK_EQ(rip.find("no capture") != std::string::npos, 1);
}

static std::array<std::uint8_t, 18> g_cap = {12, 14, 0xE8, 3, 0, 0, 1, 2, 3, 4, 5, 6, 6, 1, 0, 0, 0, 0};
static std::span<const std::uint8_t> cap_cb(void*, unsigned slot) {
    return slot == 1 ? std::span<const std::uint8_t>(g_cap) : std::span<const std::uint8_t>{};
}

static void test_blackbox_files() {
    drive::Drive d;
    drive::Env e = env(false);
    e.capture = cap_cb;
    e.capture_slots = 3;
    d.init(e);
    CHECK_EQ(has(d, "CAP0.JSN"), 0);
    CHECK_EQ(has(d, "CAP1.JSN"), 1);
    const std::string f = file(d, "CAP1.JSN");
    CHECK_EQ(f.size(), pdtrace::view::kJsonSize);
    CHECK_EQ(f.compare(0, 10, "{\"meta\":{\""), 0);              // line 0: identity of the capture
    CHECK_EQ(f.find("\"key\":\"?\"") < pdtrace::view::kJsonLine, true);
    const std::size_t L = pdtrace::view::kJsonLine;
    CHECK_EQ(f.compare(L, 9, "{\"link\":{"), 0);               // line 1: the negotiated link
    CHECK_STR(f.substr(2 * L, 36).c_str(), "{\"t\":1000,\"y\":14,\"d\":\"010203040506\"}");
    CHECK_EQ(f[3 * L - 1], '\n');
    CHECK_EQ(f[L * 4], ' ');                             // blank row past the records
    CHECK_EQ(d.ok(), 1);
}

static std::array<std::uint8_t, 2048> native_page() {
    std::array<std::uint8_t, 2048> pg;
    pg.fill(0xFF);
    pg[0] = 0xAB; pg[1] = 0xCD; pg[2] = 0xEF; pg[3] = 0x01; pg[4] = 0; pg[5] = 2;
    for (unsigned n = 0; n < 2; n++) std::memcpy(&pg[6 + n * 66], rec(20 + n, "[native]").data(), 66);
    std::uint8_t s = 0xA5;
    for (unsigned k = 0; k < 6 + 2 * 66; k++) s = static_cast<std::uint8_t>(s + pg[k]);
    pg[6 + 2 * 66] = s;
    return pg;
}

static void test_native_folder() {
    static const auto pg = native_page();
    drive::Drive d;
    d.init(env(false));
    CHECK_EQ(has(d, "NATIVE"), 0);                                  // nothing to show when the page is ours or blank
    d.set_native_page(pg);
    CHECK_EQ(has(d, "NATIVE"), 1);
    CHECK_EQ(has(d, "LIBRARY.BIN"), 1);
    const std::string bin = file(d, "LIBRARY.BIN");
    CHECK_EQ(bin.size(), 2048u);
    CHECK_EQ(std::memcmp(bin.data(), pg.data(), 2048), 0);          // the raw page, byte for byte
    const std::string txt = file(d, "LIBRARY.TXT");
    CHECK_EQ(txt.find("header: AB CD EF 01\n") != std::string::npos, 1);
    CHECK_EQ(txt.find("records: 2\n") != std::string::npos, 1);
    CHECK_EQ(txt.find("[native]") != std::string::npos, 1);
    CHECK_EQ(d.ok(), 1);
    d.set_native_page({});
    CHECK_EQ(has(d, "NATIVE"), 0);

    std::array<std::uint8_t, 2048> junk;                           // a page that does not parse
    junk.fill(0x5A);
    d.set_native_page(junk);
    CHECK_EQ(file(d, "LIBRARY.TXT").find("does not parse") != std::string::npos, 1);
    CHECK_EQ(file(d, "LIBRARY.BIN").size(), 2048u);

    emrec::Library lib;                                             // worst case for the entry table
    for (int i = 0; i < 10; i++) emrec::library_add(lib, rec(i, "slot"));
    d.set_saved_log(std::span<const char>(kSaved, sizeof kSaved - 1));
    d.set_native_page(pg);
    CHECK_EQ(d.files().size() <= vfat::kMaxEntries, 1);
    CHECK_EQ(d.ok(), 1);
}

static bool card_cb(void*, unsigned index, report_card::Card& c);

static void test_dump() {
    const char* dir = std::getenv("K2_DUMP");
    if (!dir) return;
    drive::Drive d;
    drive::Env e = env(true);
    e.card = card_cb;
    d.init(e);
    d.set_cards(2);
    char path[256];
    std::snprintf(path, sizeof path, "%s/drive.img", dir);
    FILE* f = std::fopen(path, "wb");
    if (!f) return;
    std::array<std::uint8_t, 512> s;
    for (std::uint32_t lba = 0; lba < d.sectors(); lba++) {
        d.read(lba, s);
        std::fwrite(s.data(), 1, 512, f);
    }
    std::fclose(f);
}


// Saved cables: CABLES/<SHORT>.JSN with a long name, content = emjson::render of the entry.
static bool g_cable_gone = false;
static unsigned g_cable_max = 3;
static bool cable_cb(void*, std::uint16_t seq, emlib::Entry& e) {
    if (g_cable_gone || seq < 1 || seq > g_cable_max) return false;
    e.seq = seq;
    e.version = 1;
    const char* names[] = {"", "Anker-USB2-60W", "Apple-USB4-240W", "Apple-USB4-240W-2 extra long"};
    if (seq <= 3) std::snprintf(e.name.data(), e.name.size(), "%s", names[seq]);
    else std::snprintf(e.name.data(), e.name.size(), "Cable-%u-with-a-long-name", seq);
    std::array<std::uint32_t, 5> v = {0xFF00A041, 0x180005AC, 0, 0x12340101, 0x00004643};
    e.rec = emrec::make(v, 2, "x");
    return true;
}

static void test_cables_folder() {
    drive::Drive d;
    drive::Env e = env(false);
    e.cable = cable_cb;
    d.init(e);
    CHECK_EQ(has(d, "CABLES"), 0);                       // none saved: no folder
    const std::array<std::uint16_t, 3> seqs = {1, 2, 3};
    d.set_cables(seqs);
    CHECK_EQ(d.ok(), 1);
    CHECK_EQ(has(d, "CABLES"), 1);
    // short names: "ANKER-US.JSN"-style 8.3, collisions get ~N
    int found = 0;
    char nm[256];
    for (std::size_t i = 0; i < d.files().size(); i++) {
        const auto& f = d.files()[i];
        if (f.parent < 0 || f.lfn_len == 0) continue;
        found++;
        CHECK_EQ(std::memcmp(f.name.data() + 8, "JSN", 3), 0);
        CHECK_EQ(f.lfn_len, d.long_name(i, nm));
    }
    CHECK_EQ(found, 3);
    const std::size_t first = d.files().size() - 3;
    const std::size_t ln = d.long_name(first, nm);
    CHECK_EQ(std::string(nm, ln) == "Anker-USB2-60W.json", 1);
    // contents are the JSON of the entry
    emlib::Entry en;
    cable_cb(nullptr, 2, en);
    std::array<char, emjson::kMaxSize> want;
    const std::size_t n = emjson::render(en, want);
    const auto& spec = d.files()[d.files().size() - 2];
    CHECK_EQ(spec.size, n);
    std::string got(n, 0);
    d.read_file(d.files().size() - 2, 0, std::span<std::uint8_t>(reinterpret_cast<std::uint8_t*>(got.data()), n));
    CHECK_EQ(std::memcmp(got.data(), want.data(), n), 0);
    CHECK_EQ(got.find("\"name\": \"Apple-USB4-240W\"") != std::string::npos, 1);
    // all short names differ
    for (std::size_t i = 0; i + 1 < 3; i++)
        CHECK_EQ(std::memcmp(d.files()[d.files().size() - 3 + i].name.data(), d.files()[d.files().size() - 2 + i].name.data(), 11) != 0, 1);
    // removing them all removes the folder
    d.set_cables({});
    CHECK_EQ(has(d, "CABLES"), 0);
}

static void test_cables_full() {
    drive::Drive d;
    drive::Env e = env(false);
    e.cable = [](void*, std::uint16_t seq, emlib::Entry& en) {
        en.seq = seq; en.version = 1;
        std::snprintf(en.name.data(), en.name.size(), "Cable-number-%04u-long", seq);
        std::array<std::uint32_t, 5> v = {0xFF00A041, 0x180005AC, 0, 0x12340101u + seq, 0x00004643};
        en.rec = emrec::make(v, 2, "x");
        return true;
    };
    d.init(e);
    std::vector<std::uint16_t> seqs;
    for (unsigned i = 1; i <= emlib::kCapacity; i++) seqs.push_back(static_cast<std::uint16_t>(i));
    d.set_cables(seqs);
    CHECK_EQ(d.ok(), 1);
    CHECK_EQ(d.files().size() > emlib::kCapacity, 1);
}

// The worst case for the entry table: every library slot holds a cable, plus the log and native-library files.
static void test_full_cable_library_fits() {
    drive::Drive d;
    drive::Env e = env(true);
    e.cable = cable_cb;
    g_cable_max = emlib::kCapacity;
    d.init(e);
    std::array<std::uint16_t, emlib::kCapacity> seqs;
    for (unsigned i = 0; i < seqs.size(); i++) seqs[i] = static_cast<std::uint16_t>(i + 1);
    d.set_saved_log(std::span<const char>(kSaved, sizeof kSaved - 1));
    d.set_cables(seqs);
    CHECK_EQ(d.files().size() <= vfat::kMaxEntries, 1);
    CHECK_EQ(d.ok(), 1);
    g_cable_max = 3;
}

static bool card_cb(void*, unsigned index, report_card::Card& c) {
    if (index >= 2) return false;
    c = report_card::Card{};
    c.id.fp = 0x1000 + index; c.card_no = static_cast<std::uint16_t>(index + 1); c.runs = 1;
    c.group[0] = {report_card::GV::Pass, 9000, 0, 0};
    if (index == 1) report_card::set_name(c, "Second");
    return true;
}

static void test_cards_file() {
    drive::Drive d;
    drive::Env e = env(false);
    e.card = card_cb;
    d.init(e);
    CHECK_EQ(has(d, "CARDS.JSN"), 0);                   // none saved: no file
    d.set_cards(2);
    CHECK_EQ(d.ok(), 1);
    CHECK_EQ(has(d, "CARDS.JSN"), 1);
    const std::string t = file(d, "CARDS.JSN");
    CHECK_EQ(t.size(), 2 + 2 * drive::kCardSlot + 2);
    CHECK_EQ(t.compare(0, 2, "[\n"), 0);
    CHECK_EQ(t.compare(t.size() - 3, 3, " \n]") == 0 || t.compare(t.size() - 2, 2, "]\n") == 0, 1);
    // each card is the JSON of render_json, then padding, a comma after all but the last one
    for (unsigned i = 0; i < 2; i++) {
        report_card::Card c;
        card_cb(nullptr, i, c);
        std::array<char, report_card::kJsonMax> want;
        const std::size_t n = report_card::render_json(c, want);
        const std::size_t at = 2 + i * drive::kCardSlot;
        CHECK_EQ(t.compare(at, n, want.data(), n), 0);
        CHECK_EQ(t[at + n], ' ');
        CHECK_EQ(t[at + drive::kCardSlot - 2], i == 0 ? ',' : ' ');
        CHECK_EQ(t[at + drive::kCardSlot - 1], '\n');
    }
    char nm[64];
    for (std::size_t i = 0; i < d.files().size(); i++)
        if (d.files()[i].lfn_len) {
            const std::string ln(nm, d.long_name(i, nm));
            CHECK_EQ(ln == "CARDS.JSON" || ln == "SUMMARY.md", 1);
        }
    // SUMMARY.md: one kSummarySlot block per card, the render_summary text, padding, a closing newline
    CHECK_EQ(has(d, "SUMMARY.MD"), 1);
    const std::string sm = file(d, "SUMMARY.MD");
    CHECK_EQ(sm.size(), 2 * drive::kSummarySlot);
    for (unsigned i = 0; i < 2; i++) {
        report_card::Card c;
        card_cb(nullptr, i, c);
        std::array<char, drive::kSummarySlot> want;
        const std::size_t n = report_card::render_summary(c, std::span<char>(want.data(), want.size() - 1));
        CHECK_EQ(n > 20, 1);
        CHECK_EQ(sm.compare(i * drive::kSummarySlot, n, want.data(), n), 0);
        CHECK_EQ(sm[i * drive::kSummarySlot + drive::kSummarySlot - 1], '\n');
    }
    CHECK_EQ(sm.compare(0, 9, "Card 1 \xC2\xB7"), 0);
    d.set_cards(0);
    CHECK_EQ(has(d, "CARDS.JSN"), 0);
    CHECK_EQ(has(d, "SUMMARY.MD"), 0);
    d.set_cards(drive::kMaxCards);                       // the most cards still fit the volume
    CHECK_EQ(d.ok(), 1);
    CHECK_EQ(file(d, "CARDS.JSN").size(), 2 + drive::kMaxCards * drive::kCardSlot + 2);
    d.set_cards(0);
}

int main() {
    test_base_files();
    test_full_cable_library_fits();
    test_native_folder();
    test_blackbox_files();
    test_cables_folder();
    test_cables_full();
    test_cards_file();
    test_dump();
    return test_report("test_drive");
}
