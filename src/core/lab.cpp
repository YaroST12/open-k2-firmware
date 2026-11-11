// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Protocol lab grammar, see k2/core/lab.hpp. One table row per command: family, verb, id, the positional
// kinds and the rule that decides whether the command needs `confirm`.
#include "k2/core/lab.hpp"
#include "k2/drivers/lcd_timing.hpp"
#include "k2/fastcharge/ufcs_tune.hpp"

namespace k2::lab {
namespace {

// Positional kinds: b byte, B optional byte, n number, o optional number, v value, w word, x hex bytes.
// Numbers are decimal or 0x hex and at most kMaxMv (voltages, currents, pulse counts, PDO positions);
// a value ('v') is a required number without that cap (tuner fields such as the baud rate).
enum class Raise : std::uint8_t {
    Never,
    Always,    // can raise VBUS or drive the adapter in any case
    Mv0,       // positional 0 is mV: above 5000 needs confirm
    PdReq,     // positional 0 is the PDO position (> 1 is above vSafe5V), positional 1 optional mV
    Levels,    // dpdm: a raw 0.6 V / 3.3 V level
};

struct Entry {
    std::string_view fam, verb, name;
    Id id;
    const char* args;
    Raise raise;
};

constexpr Entry kTable[] = {
    {"accp", "ping", "accp.ping", Id::AccpPing, "", Raise::Never},
    {"accp", "read", "accp.read", Id::AccpRead, "bB", Raise::Never},
    {"accp", "write", "accp.write", Id::AccpWrite, "bb", Raise::Always},
    {"accp", "dump", "accp.dump", Id::AccpDump, "bb", Raise::Never},
    {"qc2", "", "qc2", Id::Qc2, "n", Raise::Mv0},
    {"qc3", "set", "qc3.set", Id::Qc3Set, "n", Raise::Mv0},
    {"qc3", "up", "qc3.up", Id::Qc3Up, "o", Raise::Always},
    {"qc3", "down", "qc3.down", Id::Qc3Down, "o", Raise::Always},
    {"qc3+", "set", "qc3+.set", Id::Qc3pSet, "n", Raise::Mv0},
    {"qc3+", "up", "qc3+.up", Id::Qc3pUp, "o", Raise::Always},
    {"qc3+", "down", "qc3+.down", Id::Qc3pDown, "o", Raise::Always},
    {"afc", "", "afc", Id::Afc, "no", Raise::Mv0},
    {"fcp", "", "fcp", Id::Fcp, "no", Raise::Mv0},
    {"scp", "", "scp", Id::Scp, "nn", Raise::Mv0},
    {"vooc", "wake", "vooc.wake", Id::VoocWake, "", Raise::Always},
    {"vooc", "listen", "vooc.listen", Id::VoocListen, "", Raise::Never},
    {"vooc", "answer", "vooc.answer", Id::VoocAnswer, "bb", Raise::Always},
    {"vooc", "sniff", "vooc.sniff", Id::VoocSniff, "", Raise::Never},
    {"ufcs", "ping", "ufcs.ping", Id::UfcsPing, "", Raise::Never},
    {"ufcs", "caps", "ufcs.caps", Id::UfcsCaps, "", Raise::Never},
    {"ufcs", "request", "ufcs.request", Id::UfcsRequest, "nn", Raise::Mv0},
    {"ufcs", "raw", "ufcs.raw", Id::UfcsRaw, "x", Raise::Always},
    {"ufcs", "set", "ufcs.set", Id::UfcsSet, "wv", Raise::Never},   // RAM tuner field; never touches the bus
    {"ufcs", "show", "ufcs.show", Id::UfcsShow, "", Raise::Never},
    {"ufcs", "reset", "ufcs.reset", Id::UfcsReset, "", Raise::Never},
    {"ufcs", "probe", "ufcs.probe", Id::UfcsProbe, "", Raise::Never},   // 5 V handshake + opener only
    {"ufcs", "result", "ufcs.result", Id::UfcsResult, "", Raise::Never},
    {"vivo", "raw", "vivo.raw", Id::VivoRaw, "x", Raise::Always},
    {"vivo", "request", "vivo.request", Id::VivoRequest, "nn", Raise::Mv0},
    {"dpdm", "", "dpdm", Id::Dpdm, "ww", Raise::Levels},
    {"cc", "rp", "cc.rp", Id::CcRp, "o", Raise::Never},
    {"cc", "rd", "cc.rd", Id::CcRd, "o", Raise::Never},
    {"vbus", "", "vbus", Id::Vbus, "", Raise::Never},
    {"pd", "caps", "pd.caps", Id::PdCaps, "", Raise::Never},
    {"pd", "request", "pd.request", Id::PdRequest, "noo", Raise::PdReq},
    {"pd", "pps", "pd.pps", Id::PdPps, "nn", Raise::Mv0},
    {"pd", "avs", "pd.avs", Id::PdAvs, "nn", Raise::Always},
    {"pd", "epr", "pd.epr", Id::PdEpr, "no", Raise::Always},
    {"pd", "vdm", "pd.vdm", Id::PdVdm, "w", Raise::Never},
    {"pd", "get", "pd.get", Id::PdGet, "w", Raise::Never},
    {"pd", "reread", "pd.reread", Id::PdReread, "o", Raise::Never},
    {"pd", "hardreset", "pd.hardreset", Id::PdHardReset, "", Raise::Never},
    {"pd", "softreset", "pd.softreset", Id::PdSoftReset, "", Raise::Never},
    {"pd", "raw", "pd.raw", Id::PdRaw, "x", Raise::Always},
    {"lcd", "show", "lcd.show", Id::LcdShow, "", Raise::Never},
    {"lcd", "spi", "lcd.spi", Id::LcdSpi, "n", Raise::Never},   // LCD SPI clock divider 4 or 8: RAM only, never touches VBUS
    // PD Stress (k2/pdstress): start renegotiates the charger's PDOs hard, so it needs confirm; the rest only reads or stops
    {"pdstress", "start", "pdstress.start", Id::PdStressStart, "", Raise::Always},
    {"pdstress", "status", "pdstress.status", Id::PdStressStatus, "", Raise::Never},
    {"pdstress", "result", "pdstress.result", Id::PdStressResult, "o", Raise::Never},
    {"pdstress", "stop", "pdstress.stop", Id::PdStressStop, "", Raise::Never},
};
static_assert(sizeof(kTable) / sizeof(kTable[0]) == static_cast<std::size_t>(Id::Count));

struct OptDef {
    std::string_view name;
    std::uint32_t Opts::*field;
    std::uint32_t lo, hi;
};

constexpr OptDef kOpts[] = {
    {"--retries", &Opts::retries, 1, 10},
    {"--timeout-ms", &Opts::timeout_ms, 1, 60000},
    {"--hs-ms", &Opts::hs_ms, 1, 5000},
    {"--hold-ms", &Opts::hold_ms, 1, 10000},
    {"--pulse-us", &Opts::pulse_us, 20, 20000},
    {"--gap-us", &Opts::gap_us, 20, 20000},
    {"--frame-gap-us", &Opts::frame_gap_us, 1, 100000},
    {"--watch", &Opts::watch_ms, 10, 10000},   // blocks the main loop: keep it short
    {"--sop", &Opts::sop, 0, 2},
    {"--mask", &Opts::mask, 1, 0xFFF},          // pdstress start: sequence bits P1..P12
    {"--cycles", &Opts::cycles, 1, 0xFFFF},     // percent of each sequence's cycle count
    {"--pace", &Opts::pace, 0, 2},
    {"--max-s", &Opts::max_s, 1, 0xFFFF},
    {"--max-trans", &Opts::max_trans, 1, 0xFFFF},
};

constexpr std::string_view kVdmWords[] = {"discover-identity", "svids", "modes"};

bool number(std::string_view s, std::uint32_t& out) {
    std::uint32_t v = 0;
    if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s.remove_prefix(2);
        if (s.empty() || s.size() > 6) return false;
        for (char c : s) {
            const int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
            if (d < 0) return false;
            v = v * 16 + static_cast<std::uint32_t>(d);
        }
    } else {
        if (s.empty() || s.size() > 6) return false;
        for (char c : s) {
            if (c < '0' || c > '9') return false;
            v = v * 10 + static_cast<std::uint32_t>(c - '0');
        }
    }
    out = v;
    return true;
}

bool hex_bytes(std::string_view s, Cmd& out) {
    if (s.empty() || s.size() % 2 != 0 || s.size() / 2 > kMaxBytes) return false;
    auto nib = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; };
    for (std::size_t i = 0; i < s.size(); i += 2) {
        const int h = nib(s[i]), l = nib(s[i + 1]);
        if (h < 0 || l < 0) return false;
        out.bytes[i / 2] = static_cast<std::uint8_t>(h << 4 | l);
    }
    out.nbytes = s.size() / 2;
    return true;
}

// dpdm level word -> dpdm::Drive value (0 HiZ, 1 V0, 2 V0p6, 4 V3p3); -1 when unknown.
int level(std::string_view w, bool& raises) {
    raises = false;
    if (w == "hiz") return 0;
    if (w == "0") return 1;
    if (w == "0.6") { raises = true; return 2; }
    if (w == "3.3") { raises = true; return 4; }
    return -1;
}

const Entry* find(std::string_view fam, std::string_view verb, bool& verbless) {
    verbless = false;
    const Entry* bare = nullptr;
    bool fam_seen = false;
    for (const Entry& e : kTable) {
        if (e.fam != fam) continue;
        fam_seen = true;
        if (e.verb.empty()) bare = &e;
        else if (e.verb == verb) return &e;
    }
    if (bare) { verbless = true; return bare; }
    return fam_seen ? nullptr : nullptr;
}

}  // namespace

bool is_family(std::string_view w) {
    for (const Entry& e : kTable) if (e.fam == w) return true;
    return false;
}

std::string_view name(Id id) {
    for (const Entry& e : kTable) if (e.id == id) return e.name;
    return {};
}

const char* parse(const std::string_view* t, std::size_t n, Cmd& out) {
    out = Cmd{};
    if (n == 0) return "empty";
    bool verbless;
    const Entry* e = find(t[0], n > 1 ? t[1] : std::string_view{}, verbless);
    if (!e) return "unknown verb (help lab)";
    out.id = e->id;
    const std::size_t first = verbless ? 1 : 2;
    const char* kinds = e->args;
    std::size_t want = 0, need = 0;
    for (const char* k = kinds; *k; k++) { want++; if (*k == 'b' || *k == 'n' || *k == 'v' || *k == 'w' || *k == 'x') need++; }
    std::size_t got = 0;
    std::string_view words[2];
    std::size_t nwords = 0;
    for (std::size_t i = first; i < n; i++) {
        const std::string_view tok = t[i];
        if (tok == "confirm" || tok == "--confirm") { out.o.confirm = true; continue; }
        if (tok == "--hold") { out.o.hold = true; continue; }
        if (tok == "--release") { out.o.release = true; continue; }
        if (tok == "--json") { out.o.json = true; continue; }
        if (tok == "--log") {
            if (i + 1 >= n || (t[i + 1] != "raw" && t[i + 1] != "decoded")) return "--log needs raw or decoded";
            out.o.raw_log = t[++i] == "raw";
            continue;
        }
        if (tok.size() >= 2 && tok[0] == '-' && tok[1] == '-') {
            const OptDef* d = nullptr;
            for (const OptDef& od : kOpts) if (od.name == tok) d = &od;
            if (!d) return "unknown option";
            std::uint32_t v;
            if (i + 1 >= n || !number(t[i + 1], v)) return "option needs a number";
            if (v < d->lo || v > d->hi) return "option out of range";
            out.o.*(d->field) = v;
            i++;
            continue;
        }
        if (got >= want) return "too many arguments";
        const char kind = kinds[got++];
        std::uint32_t v;
        switch (kind) {
        case 'b': case 'B':
            if (!number(tok, v) || v > 255) return "argument must be a byte (0..255 or 0x00..0xFF)";
            out.a[out.na++] = v;
            break;
        case 'n': case 'o':
            if (!number(tok, v) || v > static_cast<std::uint32_t>(kMaxMv)) return "argument must be a number (max 36000)";
            out.a[out.na++] = v;
            break;
        case 'v':
            if (!number(tok, v)) return "argument must be a number (decimal or 0x hex)";
            out.a[out.na++] = v;
            break;
        case 'w':
            if (nwords < 2) words[nwords++] = tok;
            break;
        default:   // 'x'
            if (!hex_bytes(tok, out)) return "argument must be an even-length hex string (max 32 bytes)";
            break;
        }
    }
    std::size_t needed_seen = 0;   // required positionals must all be present
    for (std::size_t i = 0; i < got; i++) if (kinds[i] == 'b' || kinds[i] == 'n' || kinds[i] == 'v' || kinds[i] == 'w' || kinds[i] == 'x') needed_seen++;
    if (needed_seen < need) return "missing argument";

    bool raises = false;
    switch (e->raise) {
    case Raise::Never: break;
    case Raise::Always: raises = true; break;
    case Raise::Mv0: raises = out.a[0] > 5000; break;
    case Raise::PdReq: raises = out.a[0] > 1 || (out.na > 1 && out.a[1] > 5000); break;
    case Raise::Levels: break;   // decided below
    }
    switch (e->id) {
    case Id::AccpDump:
        if (out.a[0] > out.a[1]) return "dump range is <from> <to>, from <= to";
        break;
    case Id::Dpdm: {
        bool r0, r1;
        const int d0 = level(words[0], r0), d1 = level(words[1], r1);
        if (d0 < 0 || d1 < 0) return "level is hiz, 0, 0.6 or 3.3";
        out.a[0] = static_cast<std::uint32_t>(d0);
        out.a[1] = static_cast<std::uint32_t>(d1);
        out.na = 2;
        raises = r0 || r1;
        break;
    }
    case Id::PdVdm: {
        bool known = false;
        for (std::string_view w : kVdmWords) if (w == words[0]) known = true;
        if (!known) return "vdm is discover-identity, svids or modes";
        out.word = words[0];
        break;
    }
    case Id::PdGet:
        out.word = words[0];
        break;
    case Id::LcdSpi: {
        lcd::SpiDiv d;
        if (!lcd::lab_spi_div(out.a[0], d)) return "divider is 4 (30 MHz) or 8 (15 MHz, the default)";
        break;
    }
    case Id::UfcsSet: {   // a[0] = value, a[1] = ufcs::Field index; the range is the field's own
        ufcs::Field f;
        if (!ufcs::find_field(words[0], f)) return "unknown tuner field (ufcs show lists them)";
        const ufcs::FieldDef& d = ufcs::kFields[static_cast<std::size_t>(f)];
        if (out.a[0] < d.lo || out.a[0] > d.hi) return "value out of range for this field (ufcs show lists ranges)";
        out.a[1] = static_cast<std::uint32_t>(f);
        out.na = 2;
        out.word = words[0];
        break;
    }
    case Id::PdReread:   // optional step mask: 1 caps-ext, 2 identity, 4 svids, 8 get-cap, 16 get-cap again
        if (out.na && (out.a[0] == 0 || out.a[0] > 31)) return "mask is 1..31 (1 caps-ext, 2 identity, 4 svids, 8 get-cap, 16 second get-cap)";
        break;
    default: break;
    }
    if (out.o.sop > 2) return "sop is 0 (SOP), 1 (SOP') or 2 (SOP'')";
    if (raises && !out.o.confirm) return "this can raise VBUS: add --confirm or a trailing confirm";
    return nullptr;
}

}  // namespace k2::lab
