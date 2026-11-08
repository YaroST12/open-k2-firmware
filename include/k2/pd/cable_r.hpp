// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Cable and connector resistance, pure maths (host-tested in tests/test_cable_r.cpp). No hardware.
//
// Two methods:
//  * Meter: PASSIVE two-point differential. It only watches VBUS and bus current from the power
//    monitor. A light-load reading (< 200 mA) is the reference; once the user's load draws at least
//    1 A more at the same negotiated voltage, R = (V_idle - V_load) / (I_load - I_idle)
//    (FEATURE_PROTOTYPES prototype 6). The figure covers everything between the source's regulation
//    point and the K2's VBUS tap: cable, both connectors and the K2's own path before the shunt tap.
//  * kelvin(): 8-wire formulas. It needs a
//    pass-through fixture that routes the far-end VBUS / GND sense points onto CC1, CC2 and D-. The
//    K2 reads those three pins with its ADC and the bus current from the power monitor, so the
//    result is the drop BETWEEN the sense points only: the K2's own path is not in it, which is
//    the difference to the passive Meter (Mode::Kelvin in the UI, the CABLE_R mode op in the remote).
// Nothing here changes VBUS or the load.
#pragma once
#include <climits>
#include <cstdint>
#include "k2/drivers/power_calc.hpp"
#include <optional>
#include <span>

namespace k2::cable_r {

inline constexpr std::int32_t kIdleMaxMa = 200;      // at or below this the bus counts as idle
inline constexpr std::int32_t kMinStepMa = 1000;     // load increase over the idle point for an estimate
inline constexpr std::int32_t kPresentMv = 4000;     // below this VBUS is absent: everything resets
inline constexpr std::int32_t kIdleMinMv = 4500;     // an idle reference needs at least this VBUS
inline constexpr std::int32_t kMaxDropMv = 2500;     // more than this is not a cable
inline constexpr std::int32_t kContractStepMv = 1500;  // a VBUS jump between samples = another PDO: reference void
inline constexpr std::int32_t kKelvinMinMa = 450;    // minimum bus current for the 8-wire measurement

struct Estimate {
    std::uint16_t mohm = 0;
    std::int32_t idle_mv = 0, idle_ma = 0, load_mv = 0, load_ma = 0;   // currents are magnitudes
};

enum class Grade : std::uint8_t { Waiting, Excellent, Good, Fair, Poor };
enum class Mode : std::uint8_t { Passive, Kelvin };

class Meter {
public:
    // Feed one reading (millivolts, signed milliamps); call every power-monitor update.
    void sample(std::int32_t vbus_mv, std::int32_t ibus_ma) {
        // |ibus| saturated at INT32_MAX: negating INT32_MIN is UB.
        const std::int32_t i = ibus_ma >= 0 ? ibus_ma : ibus_ma == INT32_MIN ? INT32_MAX : -ibus_ma;
        if (vbus_mv < kPresentMv) {
            reset();
            return;
        }
        const std::int32_t jump = vbus_mv > last_mv_ ? vbus_mv - last_mv_ : last_mv_ - vbus_mv;
        if (have_last_ && has_idle_ && jump > kContractStepMv) reset();
        last_mv_ = vbus_mv;
        have_last_ = true;

        if (i < kIdleMaxMa && vbus_mv >= kIdleMinMv) {   // the latest light-load reading is the reference
            has_idle_ = true;
            idle_mv_ = vbus_mv;
            idle_ma_ = i;
            return;
        }
        if (!has_idle_ || i - idle_ma_ < kMinStepMa) return;
        const std::int32_t dv = idle_mv_ - vbus_mv, di = i - idle_ma_;
        if (dv <= 0 || dv > kMaxDropMv) return;
        const std::int32_t r = (dv * 1000 + di / 2) / di;
        est_ = Estimate{static_cast<std::uint16_t>(r), idle_mv_, idle_ma_, vbus_mv, i};
        has_est_ = true;
    }
    void reset() {
        has_idle_ = has_est_ = have_last_ = false;
    }
    bool has_idle() const { return has_idle_; }
    std::int32_t idle_mv() const { return idle_mv_; }
    std::int32_t idle_ma() const { return idle_ma_; }
    std::optional<Estimate> estimate() const {
        return has_est_ ? std::optional<Estimate>(est_) : std::nullopt;
    }

private:
    bool has_idle_ = false, has_est_ = false, have_last_ = false;
    std::int32_t idle_mv_ = 0, idle_ma_ = 0, last_mv_ = 0;
    Estimate est_{};
};

constexpr Grade grade_mohm(std::int32_t mohm) {
    return mohm < 150 ? Grade::Excellent : mohm < 300 ? Grade::Good : mohm < 500 ? Grade::Fair : Grade::Poor;
}
constexpr Grade grade(const Estimate &e) { return grade_mohm(e.mohm); }
constexpr Grade grade(const std::optional<Estimate> &e) { return e ? grade(*e) : Grade::Waiting; }

constexpr const char *grade_name(Grade g) {
    switch (g) {
    case Grade::Excellent: return "EXCELLENT";
    case Grade::Good:      return "GOOD";
    case Grade::Fair:      return "FAIR";
    case Grade::Poor:      return "POOR";
    default:               return "WAITING FOR LOAD";
    }
}

struct Kelvin {
    std::int32_t total_mohm = 0, pos_mohm = 0, gnd_mohm = 0;
};

// 8-wire formulas, sense voltages in mV and the bus current in mA (sign ignored):
// R_total = (V_cc2 - V_cc1) / I, R_pos = (V_cc2 - V_dm) / I, R_gnd = (V_dm - V_cc1) / I, in milliohms.
// Nothing below the 0.45 A floor, and nothing when the pins do not look like sense wires:
// no drop or a negative leg, or more than kMaxDropMv between CC2 and CC1 (no fixture fitted reads 0 V /
// floating: that is invalid, not a 0 mohm cable).
inline std::optional<Kelvin> kelvin(std::int32_t cc1_mv, std::int32_t cc2_mv, std::int32_t dm_mv,
                                    std::int32_t ibus_ma) {
    const std::int32_t i = power::abs_sat(ibus_ma);
    if (i < kKelvinMinMa) return std::nullopt;
    if (cc2_mv <= cc1_mv || cc2_mv < dm_mv || dm_mv < cc1_mv || cc2_mv - cc1_mv > kMaxDropMv) return std::nullopt;
    auto r = [i](std::int32_t dv) { return (dv * 1000 + (dv >= 0 ? i / 2 : -i / 2)) / i; };
    return Kelvin{r(cc2_mv - cc1_mv), r(cc2_mv - dm_mv), r(dm_mv - cc1_mv)};
}

constexpr Grade grade(const std::optional<Kelvin> &k) { return k ? grade_mohm(k->total_mohm) : Grade::Waiting; }

// The two small-print lines under the readings, at most 21 characters each (16 px font, 240 px).
// Passive: the figure still contains the K2's own path. Kelvin: the sense pairs leave it out.
constexpr const char *note(Mode m, int line) {
    if (m == Mode::Kelvin) return line == 0 ? "Sense wires: K2's own" : "path not included.";
    return line == 0 ? "Passive: includes the" : "K2's own path.";
}

// "200 m<Omega>" below one ohm, "1.25 <Omega>" up to 10, "12.3 <Omega>" above (UTF-8 U+03A9, in the 16 and
// 24 px fonts). NUL-terminated, truncated to fit; returns the length written.
inline int fmt_mohm(std::span<char> out, std::uint32_t mohm) {
    char t[16];
    int n = 0;
    auto num = [&](std::uint32_t v) {
        char d[10];
        int k = 0;
        do { d[k++] = static_cast<char>('0' + v % 10); v /= 10; } while (v);
        while (k) t[n++] = d[--k];
    };
    const char *unit;
    if (mohm < 1000) {
        num(mohm);
        unit = " m\xCE\xA9";
    } else if (mohm < 10000) {
        const std::uint32_t h = (mohm + 5) / 10;   // hundredths of an ohm
        num(h / 100); t[n++] = '.'; t[n++] = static_cast<char>('0' + h / 10 % 10); t[n++] = static_cast<char>('0' + h % 10);
        unit = " \xCE\xA9";
    } else {
        const std::uint32_t d = (mohm + 50) / 100;   // tenths
        num(d / 10); t[n++] = '.'; t[n++] = static_cast<char>('0' + d % 10);
        unit = " \xCE\xA9";
    }
    t[n] = '\0';
    std::size_t w = 0;
    auto put = [&](const char *s) { for (; *s && w + 1 < out.size(); ++s) out[w++] = *s; };
    put(t);
    put(unit);
    if (!out.empty()) out[w] = '\0';
    return static_cast<int>(w);
}

}  // namespace k2::cable_r
