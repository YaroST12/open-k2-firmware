// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// FC input: Pick -> Edit -> Review -> Apply. Editing only changes a draft; each HV apply
// needs two short OK presses bound to the exact value and source session. BACK stops first
// while running, including a long BACK. Long +/- makes one coarse draft change, never repeats.
#pragma once
#include <array>
#include <cstdint>

#include "k2/core/math_opt.hpp"
#include "k2/fastcharge/fc_engine.hpp"
#include "k2/fastcharge/fc_scan.hpp"
#include "k2/ui/ui_calc.hpp"
#include "k2/ui/ui_events.hpp"
#include "k2/ui/ui_labels.hpp"

namespace k2::trig {

inline constexpr int kEntries = 10;            // SCAN + the nine protocols
inline constexpr uint32_t kArmWindowMs = 3000;
inline constexpr int kSafeMv = 5000;           // no confirmation up to and including this
inline constexpr int kNoCeilingMv = 20000;

enum class Run : uint8_t { Idle, Scan, Live, Negotiating };
enum class Stage : uint8_t { Pick, Edit, Review, Live };

enum class Kind : uint8_t { Scan, Levels, Range, Fixed };

struct Entry {
    const char *name;
    fc::Proto proto;
    Kind kind;
    int n;                       // Levels: number of levels
    std::array<int, 4> mv;       // Levels: the levels, ascending
    int lo, hi, step;            // Range: bounds and step
    int def;                     // default target
};

inline constexpr std::array<Entry, kEntries> kList = {{
    {"SCAN",    fc::Proto::Qc2,         Kind::Scan,   0, {0, 0, 0, 0},                 0, 0, 0,         0},
    {"QC2",     fc::Proto::Qc2,         Kind::Levels, 4, {5000, 9000, 12000, 20000},   0, 0, 0,         9000},
    {"QC3",     fc::Proto::Qc3,         Kind::Range,  0, {0, 0, 0, 0},                 3600, 20000, 200, 9000},
    {"QC3+",    fc::Proto::Qc3Plus,     Kind::Range,  0, {0, 0, 0, 0},                 3600, 12000, 200, 9000},
    {"AFC",     fc::Proto::Afc,         Kind::Levels, 2, {9000, 12000, 0, 0},          0, 0, 0,         9000},
    {"FCP",     fc::Proto::Fcp,         Kind::Levels, 3, {5000, 9000, 12000, 0},       0, 0, 0,         9000},
    {"SCP",     fc::Proto::Scp,         Kind::Range,  0, {0, 0, 0, 0},                 3000, 11000, 1000, 5000},
    {"VOOC",    fc::Proto::Vooc,        Kind::Fixed,  0, {0, 0, 0, 0},                 0, 0, 0,         5000},
    {"SVOOC",   fc::Proto::SuperVooc,   Kind::Fixed,  0, {0, 0, 0, 0},                 0, 0, 0,         5000},
    {"SVOOC65", fc::Proto::SuperVooc65, Kind::Fixed,  0, {0, 0, 0, 0},                 0, 0, 0,         5000},
}};

inline const Entry &entry(int i) { return kList[static_cast<std::size_t>(i < 0 ? 0 : i >= kEntries ? kEntries - 1 : i)]; }

class Model {
public:
    struct Req {
        enum Kind : uint8_t { None, Start, Scan, Stop, Retarget } kind = None;
        fc::Proto proto = fc::Proto::Qc2;
        int mv = 0;
        bool confirmed = false;
    };

    Model() { target_ = entry(sel_).def; }

    int sel() const { return sel_; }
    int target_mv() const { return scan_selected() ? 0 : target_; }
    int held_mv() const { return held_; }
    Stage stage() const { return stage_; }
    bool retargeting() const { return retarget_; }
    bool scan_selected() const { return sel_ == 0; }
    bool armed() const { return armed_; }
    uint32_t arm_left_s(uint32_t now) const {
        return armed_ && now - armed_ms_ < kArmWindowMs ? (kArmWindowMs - (now - armed_ms_) + 999) / 1000 : 0;
    }
    // The share of the confirm window still left, 1000 (just armed) .. 0: drives the draining bar.
    uint32_t arm_left_permille(uint32_t now) const {
        return armed_ && now - armed_ms_ < kArmWindowMs ? (kArmWindowMs - (now - armed_ms_)) * 1000u / kArmWindowMs : 0;
    }
    bool needs_confirm() const { return scan_selected() || target_ > kSafeMv; }
    bool can_step() const { const Entry &e = entry(sel_); return (e.kind == Kind::Levels && e.n > 1) || e.kind == Kind::Range; }
    bool available() const {
        const auto &e = entry(sel_);
        if (e.kind == Kind::Levels) {
            for (int i = 0; i < e.n; ++i) if (e.mv[i] <= top()) return true;
            return false;
        }
        return true;
    }

    // Clamp drafts to the monitor ceiling. Never rewrite an already-applied target; protocols
    // with no offered level under the ceiling remain visible but cannot apply.
    void set_ceiling(int mv) {
        if (ceil_ != mv) disarm();
        ceil_ = mv;
        if (stage_ != Stage::Live) clamp();
    }

    void tick(uint32_t now) {
        if (armed_ && now - armed_ms_ >= kArmWindowMs) disarm();
    }
    void session(uint32_t id) {
        if (id != session_) { disarm(); session_ = id; }
    }
    void cancel() { disarm(); retarget_ = false; held_ = 0; stage_ = Stage::Pick; }
    void applied() {
        held_ = target_;
        stage_ = Stage::Live;
        retarget_ = false;
        disarm();
    }
    void observe_live(fc::Proto proto, int mv) {
        if (stage_ != Stage::Pick) {
            if ((stage_ == Stage::Live || retarget_) && held_ != mv) {
                held_ = mv;
                if (stage_ == Stage::Live) target_ = mv;
                disarm();
            }
            return;
        }
        for (int i = 1; i < kEntries; ++i) if (entry(i).proto == proto) { sel_ = i; break; }
        target_ = held_ = mv;
        stage_ = Stage::Live;
    }
    void invalidate_confirmation() { disarm(); }

    // True when the event was consumed. `req` says what the page must do (cleared first).
    bool on_event(Event e, Run run, uint32_t now, Req &req) {
        req = Req{};
        tick(now);
        if (e.press == Press::Hold) { disarm(); return false; }
        if (e.button == Button::Back) {
            disarm();
            if (run != Run::Idle || retarget_) { stop(req); return true; }
            if (e.press != Press::Short) return false;
            if (stage_ == Stage::Pick) return false;
            if (stage_ == Stage::Review && can_step()) stage_ = Stage::Edit;
            else stage_ = Stage::Pick;
            return true;
        }
        const bool volt = e.button == Button::Plus || e.button == Button::Minus;
        const int dir = e.button == Button::Plus ? 1 : -1;
        if (run == Run::Scan || run == Run::Negotiating) {
            disarm();
            return true;   // BACK reads Stop (UI_SYSTEM.md 7.13.1); OK is free
        }
        if (stage_ == Stage::Live) {
            if (e.button == Button::Ok) return true;   // BACK reads Stop; OK is free
            if (run == Run::Live && volt && e.press == Press::Short && can_step()) {
                retarget_ = true;
                stage_ = Stage::Edit;
                target_ = held_;
                step(dir, false);
            }
            return true;
        }
        if (volt) {
            disarm();
            if (stage_ == Stage::Pick && e.press == Press::Short) cycle(dir);
            else if (stage_ == Stage::Edit) step(dir, e.press == Press::Long);
            return true;
        }
        if (e.button != Button::Ok || e.press != Press::Short) { disarm(); return false; }
        if (stage_ == Stage::Pick) {
            stage_ = can_step() ? Stage::Edit : Stage::Review;
            return true;
        }
        if (stage_ == Stage::Edit) { disarm(); stage_ = Stage::Review; return true; }
        if (!available()) { disarm(); return true; }
        if (needs_confirm() && !armed_) {
            armed_ = true;
            armed_ms_ = now;
            armed_session_ = session_;
            armed_target_ = target_;
            armed_sel_ = sel_;
            return true;
        }
        const bool confirmed = armed_ && armed_session_ == session_ &&
                               armed_target_ == target_ && armed_sel_ == sel_;
        if (needs_confirm() && !confirmed) { disarm(); return true; }
        disarm();
        if (scan_selected()) {
            req.kind = Req::Scan;
        } else {
            req.kind = retarget_ ? Req::Retarget : Req::Start;
            req.proto = entry(sel_).proto;
            req.mv = target_;
        }
        req.confirmed = confirmed;
        return true;
    }

private:
    void disarm() { armed_ = false; }
    void stop(Req &req) { req.kind = Req::Stop; cancel(); }
    int top() const {   // highest allowed target of the selected protocol
        const Entry &e = entry(sel_);
        const int c = ceil_ < kSafeMv ? kSafeMv : ceil_;
        if (e.kind == Kind::Range) { const int h = e.hi < c ? e.hi : c; return h < e.lo ? e.lo : h; }
        return c;
    }
    void clamp() {
        const Entry &e = entry(sel_);
        if (e.kind == Kind::Levels) {
            int best = e.mv[0];
            for (int i = 0; i < e.n; i++) if (e.mv[static_cast<std::size_t>(i)] <= top() && e.mv[static_cast<std::size_t>(i)] <= target_) best = e.mv[static_cast<std::size_t>(i)];
            target_ = best;
        } else if (e.kind == Kind::Range) {
            if (target_ < e.lo) target_ = e.lo;
            else if (target_ > top()) target_ = e.lo + (top() - e.lo) / e.step * e.step;   // stay on the step grid
        } else if (e.kind == Kind::Fixed) {
            target_ = e.def;
        }
    }
    void step(int d, bool coarse) {
        const Entry &e = entry(sel_);
        if (e.kind == Kind::Range) {
            target_ += d * (coarse && e.step < 1000 ? 1000 : e.step);
        } else if (e.kind == Kind::Levels) {
            int best = target_;
            for (int i = 0; i < e.n; i++) {
                const int v = e.mv[static_cast<std::size_t>(i)];
                if (v > top()) continue;
                if (d > 0 && v > target_ && (best == target_ || v < best)) best = v;
                if (d < 0 && v < target_ && (best == target_ || v > best)) best = v;
            }
            target_ = best;
            return;
        } else {
            return;
        }
        clamp();
    }
    void cycle(int d) {
        sel_ += d;
        if (sel_ < 0) sel_ = 0;
        if (sel_ >= kEntries) sel_ = kEntries - 1;
        target_ = entry(sel_).def;
        clamp();
    }

    int sel_ = 1;
    int target_ = 9000;
    int ceil_ = kNoCeilingMv;
    int held_ = 0, armed_target_ = 0, armed_sel_ = 0;
    Stage stage_ = Stage::Pick;
    bool armed_ = false, retarget_ = false;
    uint32_t armed_ms_ = 0, session_ = 0, armed_session_ = 0;
};

// Panel labels. Minus holds 6 characters, Plus 8, OK 6 (the slot lengths follow the physical buttons).
inline SlotLabel plus_label(const Model &m) {
    if (m.stage() == Stage::Pick) return {"Dn", "", "", m.sel() == kEntries - 1};
    if (m.stage() == Stage::Edit) return {"+", "Big*", "", false};
    if (m.stage() == Stage::Live && m.can_step()) return {"+", "", "", false};
    return {};
}
inline SlotLabel minus_label(const Model &m) {
    if (m.stage() == Stage::Pick) return {"Up", "", "", m.sel() == 0};
    if (m.stage() == Stage::Edit) return {"-", "Big*", "", false};
    if (m.stage() == Stage::Live && m.can_step()) return {"-", "", "", false};
    return {};
}
// OK advances the staged editor; only Review sends. While a scan, handshake or level runs, BACK reads Stop and OK is blank.
inline SlotLabel ok_label(const Model &m, Run run) {
    if (run == Run::Scan || run == Run::Negotiating || m.stage() == Stage::Live) return {};   // BACK reads Stop
    if (m.armed()) return {"Confirm", "", "", false};
    return {m.stage() == Stage::Review ? "Send" : "Next", "", "", !m.available()};
}

inline const char *fail_short(fc::Fail f) {
    switch (f) {
    case fc::Fail::None: return "";
    case fc::Fail::NeedConfirm: return "confirm";
    case fc::Fail::OverLimit: return "limit";
    case fc::Fail::NoMonitor: return "no mon";
    case fc::Fail::Busy: return "busy";
    case fc::Fail::Unsupported: return "unsupp";
    case fc::Fail::NoHandshake: return "no hs";
    case fc::Fail::NoResponse: return "no resp";
    case fc::Fail::NoBand: return "no volt";
    case fc::Fail::Overvoltage: return "overV";
    case fc::Fail::SourceLost: return "lost";
    case fc::Fail::Aborted: return "abort";
    case fc::Fail::Inline: return "load";
    case fc::Fail::NeedsLoad: return "needs load";
    case fc::Fail::NoVoltage: return "handshake ok";
    case fc::Fail::Detected: return "detected";
    }
    return "?";
}

inline void put_volts(StrBuf<24> &s, int mv) { s.put_fixed(mv, 1).put("V"); }

// "IDLE", "QC2 WAIT 9.0V", "QC2 ACTIVE 9.0V", "QC2 FAIL no hs"; "Confirm: 12.0V" /
// "Confirm: SCAN" while armed (target 0 = scan).
inline void state_text(fc::State st, fc::Proto p, fc::Fail f, int target_mv, bool armed, StrBuf<24> &s,
                       bool override_load = false) {
    if (armed && override_load) { s.put("OVERRIDE LOAD?"); return; }
    if (armed) {
        s.put("Confirm: ");
        if (target_mv <= 0) s.put("SCAN"); else put_volts(s, target_mv);
        return;
    }
    if (st == fc::State::Idle) { s.put("IDLE"); return; }
    s.put(fc::name(p));
    switch (st) {
    case fc::State::Negotiating: s.put(" WAIT "); put_volts(s, target_mv); break;
    case fc::State::Active:      s.put(" ACTIVE "); put_volts(s, target_mv); break;
    default:                     s.put((f == fc::Fail::Detected || f == fc::Fail::NoVoltage) ? " " : " FAIL ").put(fail_short(f)); break;
    }
}

inline void meter_text(int vbus_mv, int current_ma, StrBuf<24> &s) {
    s.put_fixed(vbus_mv, 2).put("V ").put_abs_fixed(current_ma, 2).put("A");
}

inline void scan_row_text(const fc::ScanResult &r, StrBuf<24> &s) {
    s.put(fc::name(r.proto)).put_char(' ');
    switch (r.outcome) {
    case fc::Outcome::Accepted: s.put("OK "); put_volts(s, (r.mv + 50) / 100 * 100); break;
    case fc::Outcome::Rejected: s.put("no"); break;
    case fc::Outcome::Skipped:  s.put("skipped"); break;
    default:                    s.put("-"); break;
    }
}

inline const char *fail_long(fc::Fail f) {
    switch (f) {
    case fc::Fail::Inline: return "Load/PD active: refused";
    case fc::Fail::NeedConfirm: return "Press again to confirm";
    case fc::Fail::OverLimit: return "Above the 20 V limit";
    case fc::Fail::Unsupported: return "Not supported here";
    case fc::Fail::NoHandshake: return "Charger did not answer";
    case fc::Fail::NoResponse: return "No response to request";
    case fc::Fail::NoBand: return "Voltage not offered";
    case fc::Fail::Overvoltage: return "VBUS too high: aborted";
    case fc::Fail::SourceLost: return "Charger lost";
    case fc::Fail::Aborted: return "Aborted";
    case fc::Fail::NeedsLoad: return "Needs a load to wake";
    case fc::Fail::NoVoltage: return "Handshake ok (no voltage)";
    case fc::Fail::Detected: return "Adapter detected (no load)";
    default: return "Refused";
    }
}

// ---- design v2 widgets: the gauges, the 10 s trace and the scan summary (pure) ----------------------
inline constexpr int kScaleMv = 20000;   // cover QC2 and Class-B QC3 targets without saturation

// Fill width of a gauge `w` px wide for a voltage on the 0..20 V scale.
inline int volt_fill_px(int mv, int w) {
    if (mv <= 0) return 0;
    if (mv >= kScaleMv) return w;
    return static_cast<int>(math::mul_div_i32(mv, w, kScaleMv));
}

// Trace sample height 0..h-1 for a voltage on the same scale.
inline int trace_level(int mv, int h) { return volt_fill_px(mv, h - 1); }

// Protocols that took the handshake, and the highest voltage reached (rounded to 100 mV like the rows).
inline int scan_accepted(const fc::ScanResult *r, std::size_t n) {
    int k = 0;
    for (std::size_t i = 0; i < n; i++) k += r[i].outcome == fc::Outcome::Accepted;
    return k;
}
inline int scan_max_mv(const fc::ScanResult *r, std::size_t n) {
    int m = 0;
    for (std::size_t i = 0; i < n; i++)
        if (r[i].outcome == fc::Outcome::Accepted && (r[i].mv + 50) / 100 * 100 > m) m = (r[i].mv + 50) / 100 * 100;
    return m;
}

// Refused start: the requirement checklist and the footer reason (UI design v2, fc_refused). A row
// text is the requirement; met rows get a check, open rows a cross.
struct RefusalView {
    const char *row[2];
    bool met[2];
    int n;
    const char *hint;     // S12 line under the rows
    const char *footer;
};
inline RefusalView refusal_view(fc::Fail f, bool vbus_ok) {
    switch (f) {
    case fc::Fail::Inline:    return {{"Other port empty", "D+/D- free"}, {false, true}, 2, "Disconnect the load", "Load/PD active: refused"};
    case fc::Fail::NoMonitor: return {{"Power monitor", "D+/D- free"}, {false, true}, 2, "needed to watch VBUS", "No power monitor found"};
    case fc::Fail::Busy:      return {{"VBUS present", "D+/D- free"}, {vbus_ok, false}, 2, "another trigger runs", "D+/D- busy: stop it first"};
    default:
        if (!vbus_ok) return {{"VBUS present", "D+/D- free"}, {false, true}, 2, "top or bottom port", "No VBUS on either port"};
        return {{"VBUS present", "D+/D- free"}, {true, true}, 2, "see the footer", fail_long(f)};
    }
}

}  // namespace k2::trig
