// SPDX-License-Identifier: GPL-3.0-or-later
// Host test for k2::selftest::run_math and shell `selftest` dispatch.
#include "k2/core/math_selftest.hpp"
#include "k2/core/shell.hpp"
#include "test_util.h"

#include <string>

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

}  // namespace

int main() {
    std::string report;
    k2::selftest::Summary sum{};
    const bool ok = k2::selftest::run_math(collect, &report, &sum);
    CHECK_COND(ok);
    CHECK_EQ(sum.total, 7u);
    CHECK_EQ(sum.passed, 7u);
    CHECK_COND(report.find("PASS 7/7 checks") != std::string::npos);
    CHECK_COND(report.find("[PASS] scale_div") != std::string::npos);
    CHECK_COND(report.find("[PASS] fpu_sqrt") != std::string::npos);
    CHECK_COND(report.find("[PASS] blend565") != std::string::npos);

    TestBackend b;
    k2::shell::Shell sh(b);
    sh.execute("selftest");
    CHECK_COND(b.selftest_called);
    CHECK_COND(b.out.find("PASS 7/7 checks") != std::string::npos);
    CHECK_COND(b.out.find("err:") == std::string::npos);

    b.out.clear();
    sh.execute("selftest extra");
    CHECK_COND(b.out.find("err: usage: selftest") != std::string::npos);

    return test_report("test_math_selftest");
}
