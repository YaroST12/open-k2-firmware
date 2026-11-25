// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The Battery place, pure half: see k2/ui/battery_view.hpp.
#include "k2/ui/battery_view.hpp"
#include "k2/core/strpack.hpp"
#include "k2/core/math_opt.hpp"

namespace k2::battery::view {

namespace {
void app(Num &x, std::string_view s) {
    K2_EXPECT_PLAIN(s);   // hot: readout text stays plain
    for (char c : s)
        if (x.n + 1u < sizeof x.b) x.b[x.n++] = c;
}
void app_u(Num &x, uint32_t v, int width = 0) {
    char d[10];
    int n = 0;
    do { d[n++] = static_cast<char>('0' + v % 10); v /= 10; } while (v);
    while (n < width) d[n++] = '0';
    while (n) app(x, std::string_view(&d[--n], 1));
}
}  // namespace

namespace {
uint32_t milli(uint64_t micro) {
    const uint64_t m = math::div_u64_u32(micro, 1000u);
    return m > 99'999'999u ? 99'999'999u : static_cast<uint32_t>(m);
}
}  // namespace

Num wh(uint64_t uwh) {
    Num x;
    const uint32_t mwh = milli(uwh);
    if (mwh < 10'000u) { app_u(x, mwh / 1000u); app(x, "."); app_u(x, mwh % 1000u, 3); }
    else if (mwh < 100'000u) { app_u(x, mwh / 1000u); app(x, "."); app_u(x, mwh % 1000u / 10u, 2); }
    else if (mwh < 1'000'000u) { app_u(x, mwh / 1000u); app(x, "."); app_u(x, mwh % 1000u / 100u); }
    else app_u(x, mwh / 1000u);
    app(x, "Wh");
    return x;
}

Num mah(uint64_t uah) {
    Num x;
    const uint32_t m = milli(uah);
    if (m <= 99'999u) { app_u(x, m); app(x, "mAh"); return x; }
    app_u(x, m / 1000u);
    app(x, ".");
    app_u(x, m % 1000u / 100u);
    app(x, "Ah");
    return x;
}

Num hms(uint32_t s) {
    Num x;
    const uint32_t h = s / 3600u, m = s / 60u % 60u, sec = s % 60u;
    if (h) { app_u(x, h); app(x, "h"); app_u(x, m, 2); app(x, "m"); app_u(x, sec, 2); app(x, "s"); }
    else if (m) { app_u(x, m); app(x, "m"); app_u(x, sec, 2); app(x, "s"); }
    else { app_u(x, sec); app(x, "s"); }
    return x;
}

Num hm(uint32_t s) {
    Num x;
    const uint32_t h = s / 3600u, m = s / 60u % 60u;
    if (h) { app_u(x, h); app(x, "h"); app_u(x, m, 2); }
    else app_u(x, m);
    app(x, "m");
    return x;
}

Num pct(uint16_t permille) {
    Num x;
    app_u(x, permille / 10u);
    app(x, ".");
    app_u(x, permille % 10u);
    app(x, "%");
    return x;
}

Shown shown(const Record &r, bool any) {
    Shown s;
    s.any = any && r.phases > 0;
    if (!s.any) return s;
    s.state = r.state;
    s.mode = r.mode;
    s.wiring = r.wiring;
    s.phases = r.phases;
    s.cfg = r.cfg;
    const PhaseRec &last = r.ph[r.phases - 1];
    s.kind = last.kind;
    s.end = last.end;
    s.id = r.id;
    for (int i = 0; i < r.phases; i++) {
        s.lost = s.lost || r.ph[i].end == End::Lost;
        s.gaps = static_cast<uint16_t>(s.gaps + r.ph[i].gaps);
    }
    s.sum = sums(r);
    s.secs = s.sum.secs;
    s.phase_secs = last.secs;
    s.phase_uwh = last.kind == Kind::Charge ? last.uwh_in : last.uwh_out;
    if (r.mode == Mode::RoundTrip && r.phases == 2) s.eff = ratio_permille(s.sum.uwh_out, s.sum.uwh_in);
    s.eff_final = r.state == RunState::Done && has_efficiency(r);
    return s;
}

Pair pair(const Shown *runs, int n) {
    Pair p;
    uint32_t sum[2] = {};
    Mode m[2] = {};
    int k = -1;   // the stretch being summed: 0 the newest, 1 the one before it
    for (int i = 0; i < n; i++) {
        const Shown &s = runs[i];
        if (!s.any || empty(s.sum)) continue;
        if (s.state != RunState::Done) return {};   // a run still measuring (saved every 10 s) would pair a part
        const bool next = k < 0 || s.mode != m[k];
        if (s.mode == Mode::RoundTrip || (next && k == 1)) break;
        if (s.lost) return {};   // part of a stretch, short by what it lost
        if (next) m[++k] = s.mode;
        sum[k] += static_cast<uint32_t>(s.mode == Mode::Discharge ? s.sum.uwh_out : s.sum.uwh_in);
        if (!p.hi) p.hi = s.id;
        p.lo = s.id;
    }
    if (k < 1) return {};
    const bool dis = m[0] == Mode::Discharge;
    p.uwh_out = sum[dis ? 0 : 1];
    p.uwh_in = sum[dis ? 1 : 0];
    p.eff = ratio_permille(p.uwh_out, p.uwh_in);
    return p;
}

bool is_fact(Act a) { return a == Act::In || a == Act::Out || a == Act::Time || a == Act::Eff; }

Rows rows(const Shown &s, bool stored, bool cont) {
    Rows r;
    const auto add = [&](Act a) { if (r.n < kMaxRows) r.a[r.n++] = a; };
    if (s.any) {
        add(Act::In);
        add(Act::Out);
        add(Act::Time);
        if (s.mode == Mode::RoundTrip && s.phases == 2) add(Act::Eff);
    }
    r.facts = r.n;
    if (cont || (!stored && s.any && s.state == RunState::Done)) add(Act::Continue);
    if (stored) {
        add(Act::Share);
        return r;
    }
    if (!s.any || s.state == RunState::Done) {
        add(Act::StartCharge);
        add(Act::StartDischarge);
        add(Act::StartRoundTrip);
        add(Act::Wiring);
        add(Act::EndMa);
        add(Act::EndS);
        add(Act::Runs);
        if (s.any) add(Act::Share);
        return r;
    }
    if (s.state == RunState::Measuring) {
        add(Act::EndPhase);
        if (s.mode == Mode::RoundTrip && s.phases == 1) add(Act::Finish);
    } else {   // between the round trip's phases
        add(Act::Next);
        add(Act::Finish);
    }
    add(Act::EndMa);
    add(Act::EndS);
    return r;
}

const char *act_name(Act a, const Shown &s) {
    switch (a) {
    case Act::In: return "In";
    case Act::Out: return "Out";
    case Act::Time: return "Time";
    case Act::Eff: return K2_P("Efficiency");
    case Act::StartCharge: return K2_P("Start charge");
    case Act::StartDischarge: return K2_P("Start discharge");
    case Act::StartRoundTrip: return K2_P("Start round trip");
    case Act::EndPhase: return s.kind == Kind::Charge ? K2_P("End charge") : K2_P("End discharge");
    case Act::Next: return K2_P("Start discharge");
    case Act::Finish: return K2_P("Finish run");
    case Act::Wiring: return K2_P("Wiring");
    case Act::EndMa: return K2_P("End below");
    case Act::EndS: return K2_P("End after");
    case Act::Runs: return K2_P("Saved runs");
    case Act::Share: return K2_P("Share as QR");
    case Act::Continue: return K2_P("Continue run");
    }
    return "";
}

sl::Id act_verb(Act a) {
    switch (a) {
    case Act::StartCharge: case Act::StartDischarge: case Act::StartRoundTrip: case Act::Next: return sl::Start;
    case Act::EndPhase: case Act::Finish: return sl::Stop;
    case Act::Wiring: return sl::Toggle;
    case Act::EndMa: case Act::EndS: return sl::Cycle;
    case Act::Runs: return sl::Open;
    case Act::Share: return sl::Share;
    case Act::Continue: return sl::Resume;
    case Act::In: case Act::Out: case Act::Time: case Act::Eff: break;
    }
    return sl::None;
}

uint16_t next_step(const uint16_t *steps, int n, uint16_t cur) {
    for (int i = 0; i + 1 < n; i++)
        if (steps[i] == cur) return steps[i + 1];
    return steps[0];
}

namespace {
const char *wiring_word(Wiring w) { return w == Wiring::Moved ? K2_P("Moved") : K2_P("Same port"); }
}  // namespace

void bench_status(const Shown &s, StrBuf<29> &out) {
    if (!s.any) return;
    switch (s.state) {
    case RunState::Measuring:
        out.put(s.kind == Kind::Charge ? K2_P("Charging ") : K2_P("Discharging ")).put(hm(s.phase_secs).view()).put(", ");
        out.put(wh(s.phase_uwh).view());
        return;
    case RunState::Between:
        out.put(K2_P("Charged ")).put(wh(s.sum.uwh_in).view()).put(K2_P(", waiting"));
        return;
    case RunState::Done:
        break;
    }
    if (s.eff_final) { out.put(K2_P("Round trip ")).put(pct(s.eff).view()); return; }
    const uint64_t e = s.mode == Mode::Discharge ? s.sum.uwh_out : s.sum.uwh_in;
    if (s.lost) out.put(K2_P("Cut short, "));
    else out.put(s.mode == Mode::Discharge ? K2_P("Discharged ") : K2_P("Charged "));
    out.put(wh(e).view());
}

void ident(const Shown &s, kit::Ident &out) {
    using K = kit::Ident;
    if (!s.any) {
        out.head.put(K2_P("No run"));
        out.dim = true;
        out.line.put(K2_P("Charge, discharge or round trip"));
        return;
    }
    const bool trip = s.mode == Mode::RoundTrip;
    switch (s.state) {
    case RunState::Measuring:
        out.head.put(s.kind == Kind::Charge ? K2_P("Charging") : K2_P("Discharging"));
        if (trip) out.add(s.phases == 1 ? K2_P("1 of 2") : K2_P("2 of 2"), K::Solid);
        // no "in"/"out": the head says it; "Same port, 99h59m59s, 123.4Wh" stays inside kit::kIdLineCells
        out.line.put(wiring_word(s.wiring)).put(", ").put(hms(s.phase_secs).view()).put(", ").put(wh(s.phase_uwh).view());
        return;
    case RunState::Between:
        out.head.put(K2_P("Charged"));
        out.add(K2_P("1 of 2"), K::Solid);
        out.line.put(s.wiring == Wiring::Moved ? K2_P("Move the bank to the input port") : K2_P("Unplug charger, load the bank"));
        return;
    case RunState::Done:
        break;
    }
    if (s.eff_final) {
        out.head.put(pct(s.eff).view());
        out.add(K2_P("Round trip"), K::Ok);
    } else {
        out.head.put(wh(s.mode == Mode::Discharge ? s.sum.uwh_out : s.sum.uwh_in).view());
        if (s.lost) out.add(K2_P("Cut short"), K::Outline);
        else out.add(s.mode == Mode::Charge ? K2_P("Charged") : s.mode == Mode::Discharge ? K2_P("Discharged") : K2_P("Round trip"), K::Solid);
    }
    out.line.put("Run ").put_u32(s.id).put(", ").put(hms(s.secs).view()).put(", ").put(wiring_word(s.wiring));
}

FooterTone footer(const Shown &s, bool monitor, bool write_failed, bool unwritten, bool discarded, StrBuf<28> &out) {
    if (!monitor) { out.put(K2_P("No power monitor")); return FooterTone::Error; }
    if (write_failed) { out.put(K2_P("Run not saved to flash")); return FooterTone::Caution; }
    if (!s.any) { out.put(K2_P("Nothing measuring")); return FooterTone::Info; }
    switch (s.state) {
    case RunState::Measuring:
        out.put(K2_P("Ends under ")).put_u32(s.cfg.end_ma).put(K2_P("mA for ")).put_u32(s.cfg.end_s).put_char('s');
        return FooterTone::Info;
    case RunState::Between:
        out.put(K2_P("Discharge starts by itself"));
        return FooterTone::Info;
    case RunState::Done:
        break;
    }
    if (discarded) { out.put(K2_P("Empty run, not saved")); return FooterTone::Caution; }
    if (s.lost) { out.put(K2_P("Phase cut by a power loss")); return FooterTone::Caution; }
    if (s.eff_final) { out.put(K2_P("Round trip ")).put(pct(s.eff).view()).put(K2_P(" efficient")); return FooterTone::Ok; }
    out.put("Run ").put_u32(s.id).put(unwritten ? K2_P(" done, not saved") : K2_P(" saved"));
    return FooterTone::Ok;
}

}  // namespace k2::battery::view
