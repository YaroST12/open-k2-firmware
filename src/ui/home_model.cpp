// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#include "k2/ui/home_model.hpp"

namespace k2::home {
namespace {

int put_text(std::span<char> out, std::string_view s) {
    size_t n = 0;
    for (char c : s) {
        if (n + 1 >= out.size()) break;
        out[n++] = c;
    }
    if (!out.empty()) out[n] = '\0';
    return static_cast<int>(n);
}

int32_t magnitude(int32_t v) { return v < 0 ? (v == INT32_MIN ? INT32_MAX : -v) : v; }

// fmt_fit right-aligns in its width; the fields align themselves, so the text is returned trimmed.
int fit_trimmed(std::span<char> out, int32_t milli, int width, int max_decimals) {
    char t[16];
    const int n = fmt_fit(t, milli, width, max_decimals);
    int lead = 0;
    while (lead < n && t[lead] == ' ') lead++;
    return put_text(out, {t + lead, static_cast<size_t>(n - lead)});
}

void put_2d(StrBuf<10> &s, uint32_t v) { s.put_char(static_cast<char>('0' + v / 10)).put_char(static_cast<char>('0' + v % 10)); }

}  // namespace

int fmt_hero(std::span<char> out, int32_t milli) { return fit_trimmed(out, magnitude(milli), 5, 3); }

int fmt_watts(std::span<char> out, int32_t milli) { return fit_trimmed(out, magnitude(milli), 4, 2); }

int fmt_energy(std::span<char> out, int32_t milli) {
    const int32_t m = magnitude(milli);
    if (m >= 999500) {   // kilo-units, three characters: "1.2k" while a single digit, "12k" above
        char t[8];
        const int n = fit_trimmed({t, sizeof t}, m / 1000, 3, 1);
        StrBuf<8> s;
        s.put({t, static_cast<size_t>(n)}).put_char('k');
        return put_text(out, s.view());
    }
    return fit_trimmed(out, m, m >= 100000 ? 3 : 4, m >= 10000 ? 1 : 2);
}

int fmt_elapsed(std::span<char> out, uint32_t secs) {
    StrBuf<10> s;
    if (secs >= 3600) {
        uint32_t h = secs / 3600;
        uint32_t m = (secs / 60) % 60;
        if (h > 99) { h = 99; m = 59; }
        if (h >= 10) {   // "10h05": the field is five characters wide
            s.put_u32(h).put_char('h');
            put_2d(s, m);
        } else {
            s.put_u32(h).put_char(':');
            put_2d(s, m);
            s.put_char('h');
        }
    } else {
        s.put_u32(secs / 60).put_char(':');
        put_2d(s, secs % 60);
    }
    return put_text(out, s.view());
}

int fmt_level(std::span<char> out, uint32_t milli) {
    const uint32_t h = (milli + 5) / 10;   // hundredths
    StrBuf<12> s;
    s.put_u32(h / 100);
    if (h % 100 != 0) {
        s.put_char('.').put_char(static_cast<char>('0' + (h % 100) / 10));
        if (h % 10 != 0) s.put_char(static_cast<char>('0' + h % 10));
    }
    return put_text(out, s.view());
}

namespace {

void level(StrBuf<20> &s, uint32_t milli, char unit) {
    char t[12];
    const int n = fmt_level(t, milli);
    s.put({t, static_cast<size_t>(n)});
    if (unit) s.put_char(unit);
}
// "20V", "20V5A": the voltage, then the current when the rating is known.
void fixed(StrBuf<20> &s, const ProtoInfo &p) {
    level(s, p.mv, 'V');
    if (p.ma) level(s, p.ma, 'A');
}
// "3.3-21V", "3.3-21V 5A": a range, then one space and the current when known. Equal bounds are one
// level ("9V"): a PPS keep-alive labelled from the measured VBUS has no range without caps.
void range(StrBuf<20> &s, const ProtoInfo &p) {
    if (p.lo_mv == p.hi_mv) {
        level(s, p.hi_mv, 'V');
        if (p.ma) {
            s.put_char(' ');
            level(s, p.ma, 'A');
        }
        return;
    }
    level(s, p.lo_mv, 0);
    s.put_char('-');
    level(s, p.hi_mv, 'V');
    if (p.ma) {
        s.put_char(' ');
        level(s, p.ma, 'A');
    }
}

}  // namespace

void format_proto(const ProtoInfo &p, ProtoText &out) {
    out.tag.clear();
    out.value.clear();
    switch (p.kind) {
    case ProtoKind::PdFixed: out.tag.put("PD"); fixed(out.value, p); break;
    case ProtoKind::PdEpr: out.tag.put("EPR"); fixed(out.value, p); break;
    case ProtoKind::Pps: out.tag.put("PPS"); range(out.value, p); break;
    case ProtoKind::Avs:
        out.tag.put("AVS");
        if (p.lo_mv == p.hi_mv) level(out.value, p.hi_mv, 'V');   // one level (an own contract): "AVS 12.6V"
        else {
            level(out.value, p.lo_mv, 0);
            out.value.put_char('-');
            level(out.value, p.hi_mv, 'V');
        }
        if (p.mw) {
            out.value.put_char(' ');
            out.value.put_u32(p.mw / 1000).put_char('W');
        }
        break;
    case ProtoKind::Qc2: out.tag.put("QC2"); fixed(out.value, p); break;
    case ProtoKind::Qc3: out.tag.put("QC3"); range(out.value, p); break;
    case ProtoKind::Fcp: out.tag.put("FCP"); fixed(out.value, p); break;
    case ProtoKind::Scp: out.tag.put("SCP"); fixed(out.value, p); break;
    case ProtoKind::Ufcs: out.tag.put("UFCS"); range(out.value, p); break;
    case ProtoKind::Svooc: out.tag.put("SVOOC"); fixed(out.value, p); break;
    case ProtoKind::TypeC: out.tag.put("CC"); fixed(out.value, p); break;
    case ProtoKind::Dcp: out.tag.put("DCP"); fixed(out.value, p); break;
    case ProtoKind::Detecting: out.tag.put(".."); out.value.put("Detecting"); break;
    case ProtoKind::Unknown: out.tag.put("?"); out.value.put("Unknown source"); break;
    case ProtoKind::None: out.tag.put("--"); out.value.put("No source"); break;
    }
}

namespace {

bool max_text(Mode m, uint32_t milli, char unit, StrBuf<12> &out) {
    out.clear();
    if (m != Mode::Live || milli == 0) return false;
    char t[12];
    const int n = fmt_level(t, milli);
    out.put("max ").put({t, static_cast<size_t>(n)}).put_char(unit);
    return true;
}

}  // namespace

bool max_volts_text(Mode m, const ProtoInfo &p, StrBuf<12> &out) { return max_text(m, p.max_mv, 'V', out); }
bool max_amps_text(Mode m, const ProtoInfo &p, StrBuf<12> &out) { return max_text(m, p.ma, 'A', out); }

void alarm_text(Alarm a, uint32_t abs_ma, uint32_t max_ma, StrBuf<16> &out) {
    out.clear();
    if (a == Alarm::None) return;
    char t[12];
    const int n = fmt_level(t, max_ma);
    const std::string_view lim{t, static_cast<size_t>(n)};
    if (a == Alarm::Over) {
        out.put("OVER ").put(lim).put("A LIMIT");
    } else {
        out.put_u32(pct_of(abs_ma, max_ma)).put("% OF ").put(lim).put_char('A');
    }
}

}  // namespace k2::home
