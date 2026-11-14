// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Host test for k2::selftest::run_math and shell `selftest` dispatch.
#include "k2/core/math_selftest.hpp"
#include "k2/core/shell.hpp"
#include "fake_lcd.h"
#include "test_util.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

namespace {

void collect(std::string_view text, void* ctx) {
    static_cast<std::string*>(ctx)->append(text);
}

struct TestBackend final : k2::shell::Backend {
    std::string out;
    bool selftest_called = false;

    void write(std::string_view s) override { out.append(s); }
    void state() override {}
    void log_stream(bool) override {}
    const char* drive(bool, k2::dpdm::Drive) override { return nullptr; }
    const char* cc_open(std::uint32_t) override { return nullptr; }
    const char* pd_silent(std::uint32_t) override { return nullptr; }
    const char* pd_reset() override { return nullptr; }
    const char* pd_rescan() override { return nullptr; }
    const char* fc_trigger(k2::fc::Proto, int, bool) override { return nullptr; }
    const char* fc_reset() override { return nullptr; }
    const char* stop() override { return nullptr; }
    const char* sniff(int, std::uint32_t) override { return nullptr; }
    const char* selftest() override {
        selftest_called = true;
        return k2::selftest::run_math(collect, &out) ? nullptr : "math selftest failed";
    }
};

// math_opt is built only where k2/core/math_opt.hpp exists (the bench-pre image predates it and reports [SKIP]).
#if __has_include("k2/core/math_opt.hpp")
#define K2_TEST_MATH_OPT 1
#else
#define K2_TEST_MATH_OPT 0
#endif

// The checks every build has, in report order (the on-device build adds tu_fifo after pdtrace_ring).
constexpr const char* kNames[] = {
    "scale_div", "adc_calc", "power_calc",
#if K2_TEST_MATH_OPT
    "math_opt",
#endif
    "readout_fmt", "fpu_sqrt", "one_euro", "blend565",
    "text_aa", "text_big", "gfx_fill", "mem_ops", "fmt_line", "pd_codec", "pd_sink", "pdtrace_ring", "ws_fast", "ws_slow",
};

// The line of a check: "  [PASS] name ... sum=0xXXXXXXXX  cyc=N zw=Z". Returns the text after the tag, or "" when absent.
std::string line_of(const std::string& report, const char* name) {
    const std::string tag = std::string("] ") + name;
    const std::size_t at = report.find(tag);
    if (at == std::string::npos) return {};
    const std::size_t end = report.find_first_of("\r\n", at);
    return report.substr(at, end == std::string::npos ? std::string::npos : end - at);
}

std::string sum_of(const std::string& line) {
    const std::size_t at = line.find("sum=0x");
    return at == std::string::npos ? std::string() : line.substr(at + 6, 8);
}

}  // namespace

int main() {
    std::string report;
    k2::selftest::Summary sum{};
    const unsigned px_before = g_lcd_pixels;
    const bool ok = k2::selftest::run_math(collect, &report, &sum);
    // K2_SELFTEST_REPORT=1 prints the report: how the goldens in math_selftest.cpp are harvested after a check changes.
    if (std::getenv("K2_SELFTEST_REPORT")) std::fputs(report.c_str(), stdout);
    CHECK_COND(ok);
    CHECK_COND(sum.total >= std::size(kNames));
    CHECK_EQ(sum.passed, sum.total);
    char pass_line[48];
    std::snprintf(pass_line, sizeof pass_line, "PASS %u/%u checks", unsigned(sum.passed), unsigned(sum.total));
    CHECK_COND(report.find(pass_line) != std::string::npos);
    CHECK_COND(report.find("[FAIL]") == std::string::npos);
    CHECK_COND(report.find("[PASS] scale_div") != std::string::npos);
    CHECK_COND(report.find("[PASS] fpu_sqrt") != std::string::npos);
    CHECK_COND(report.find("[PASS] blend565") != std::string::npos);

    // Every check is reported, with a sum, a cycle count and the zero-wait flag.
    for (const char* name : kNames) {
        const std::string line = line_of(report, name);
        CHECK_COND(!line.empty());
        CHECK_COND(line.find("sum=0x") != std::string::npos);
        CHECK_COND(line.find("cyc=") != std::string::npos);
        const bool zw = line.size() > 5 && (line.compare(line.size() - 5, 5, " zw=0") == 0 || line.compare(line.size() - 5, 5, " zw=1") == 0);
        CHECK_COND(zw);
        CHECK_COND(line.find("PASS") == std::string::npos);   // the tag is before the matched "] name"
    }
    for (const char* name : kNames) CHECK_COND(report.find(std::string("[PASS] ") + name) != std::string::npos);

    // Golden sums of the deterministic checks (the same constants as the table in math_selftest.cpp, kept here on purpose so a
    // change to either side is a visible decision). The two float checks are judged by tolerance instead: fpu_sqrt's qsum is
    // 16 x the float sum rounded, one_euro's sum is not compared at all (its range is asserted inside the check).
    struct Gold {
        const char* name;
        const char* sum;
    };
    static constexpr Gold kGold[] = {
        {"scale_div", "7EF9EC51"}, {"adc_calc", "EE89B01D"},  {"power_calc", "B2F07723"},
#if K2_TEST_MATH_OPT
        {"math_opt", "74B742B7"},
#endif
        {"readout_fmt", "2E7FF5D7"}, {"blend565", "2B0E04E3"}, {"text_aa", "447FE66E"},    {"text_big", "3C2A04BF"},
        {"gfx_fill", "041E7430"},    {"mem_ops", "8B0882C5"},  {"fmt_line", "0F171019"},   {"pd_codec", "DBF8DC4D"},
        {"pd_sink", "5FEDCFA6"},     {"pdtrace_ring", "D2BDBEB5"}, {"ws_fast", "7B1CE946"}, {"ws_slow", "7B1CE946"},
    };
    for (const Gold& g : kGold) CHECK_COND(sum_of(line_of(report, g.name)) == g.sum);
    {
        const unsigned q = static_cast<unsigned>(std::strtoul(sum_of(line_of(report, "fpu_sqrt")).c_str(), nullptr, 16));
        CHECK_COND(q >= 8203u - 3u && q <= 8203u + 3u);
    }
    CHECK_COND(report.find("want=") == std::string::npos);
    // The header carries the placement switch and the firmware version (tools/gate.py perf step reads fw=).
    CHECK_COND(report.find(" hot=") != std::string::npos);
    CHECK_COND(report.find(" fw=host):") != std::string::npos);
#if K2_TEST_MATH_OPT
    CHECK_COND(report.find("[SKIP]") == std::string::npos);
#else
    CHECK_COND(report.find("[SKIP] math_opt") != std::string::npos);   // reported, counted in neither passed nor total
    CHECK_COND(report.find("(1 skipped)") != std::string::npos);
#endif

    // The placement canary runs the same loop twice, from two places in flash: identical results.
    const std::string fast = sum_of(line_of(report, "ws_fast")), slow = sum_of(line_of(report, "ws_slow"));
    CHECK_COND(fast.size() == 8);
    CHECK_COND(fast == slow);

    // No panel traffic and no sink left behind: the drawing checks go to a RAM sink and put the previous one back.
    CHECK_EQ(g_lcd_pixels, px_before);
    CHECK_COND(k2::lcd::get_sink() == nullptr);
    {
        static const k2::lcd::Sink kOther{[](std::uint16_t, std::uint16_t, std::uint16_t, std::uint16_t) {},
                                          [](const std::uint16_t*, std::uint32_t) {}, [](std::uint16_t, std::uint32_t) {}};
        k2::lcd::set_sink(&kOther);
        std::string again;
        CHECK_COND(k2::selftest::run_math(collect, &again, nullptr));
        CHECK_COND(k2::lcd::get_sink() == &kOther);
        k2::lcd::set_sink(nullptr);
        CHECK_EQ(g_lcd_pixels, px_before);
        // Deterministic: the host reads no cycle counter, so a second run prints the same report byte for byte.
        CHECK_COND(again == report);
    }

    TestBackend b;
    k2::shell::Shell sh(b);
    sh.execute("selftest");
    CHECK_COND(b.selftest_called);
    CHECK_COND(b.out.find(pass_line) != std::string::npos);
    CHECK_COND(b.out.find("err:") == std::string::npos);

    b.out.clear();
    sh.execute("selftest extra");
    CHECK_COND(b.out.find("err: usage: selftest") != std::string::npos);

    return test_report("test_math_selftest");
}
