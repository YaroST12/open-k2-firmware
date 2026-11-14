// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Loop probe "Host report", USB half (k2/loopprobe/usb_fp.hpp): the enumeration recorder, the features and the host
// classifier over SYNTHETIC traces. The traces follow commonly described enumeration habits, not captures: they pin the
// rules as written (all unverified guesses), so a rule change after real captures shows up here.
#include "test_util.h"

#include <initializer_list>

#include "k2/loopprobe/usb_fp.hpp"

using namespace k2::loopprobe::usbfp;

namespace {

struct Builder {
    Recorder r;
    std::uint32_t t = 1000;
    Builder& reset() { r.bus_reset(t += 10); return *this; }
    Builder& setup(std::uint8_t type, std::uint8_t req, std::uint16_t value, std::uint16_t index, std::uint16_t len) {
        const std::uint8_t p[8] = {type, req, std::uint8_t(value), std::uint8_t(value >> 8), std::uint8_t(index),
                                   std::uint8_t(index >> 8), std::uint8_t(len), std::uint8_t(len >> 8)};
        r.setup(p, t += 3);
        return *this;
    }
    Builder& desc(std::uint8_t dtype, std::uint8_t idx, std::uint16_t len, std::uint16_t langid = 0) {
        return setup(0x80, kReqGetDescriptor, std::uint16_t(dtype << 8 | idx), langid, len);
    }
    Builder& addr(std::uint8_t a) { return setup(0x00, kReqSetAddress, a, 0, 0); }
    Builder& config() { return setup(0x00, kReqSetConfiguration, 1, 0, 0); }
    Builder& strings(std::initializer_list<std::uint8_t> idx, std::uint16_t len) {
        for (auto i : idx) desc(kDescString, i, len, 0x0409);
        return *this;
    }
    Builder& sof() { r.flag(kFlagSof); return *this; }
};

Os os_of(const Builder& b) { return classify(b.r.trace()); }

Builder windows() {
    Builder b;
    b.sof().reset().desc(kDescDevice, 0, 64).reset().addr(5).desc(kDescDevice, 0, 18).desc(kDescConfig, 0, 255)
        .desc(kDescQualifier, 0, 10).desc(kDescString, 0xEE, 18).desc(kDescString, 0, 255).strings({2}, 255).config();
    return b;
}
Builder linux_host() {
    Builder b;
    b.sof().reset().desc(kDescDevice, 0, 64).reset().addr(7).desc(kDescDevice, 0, 18).desc(kDescConfig, 0, 9)
        .desc(kDescConfig, 0, 34).desc(kDescString, 0, 255).strings({2, 1, 3}, 255).config();
    return b;
}
Builder apple() {
    Builder b;
    b.sof().reset().desc(kDescDevice, 0, 8).addr(3).desc(kDescDevice, 0, 18).desc(kDescConfig, 0, 9)
        .desc(kDescConfig, 0, 34).desc(kDescString, 0, 2).desc(kDescString, 0, 4).strings({2}, 2).strings({2}, 26);
    return b;
}

void test_recorder() {
    Recorder r;
    const std::uint8_t p[8] = {0x80, 6, 0x00, 0x01, 0, 0, 64, 0};
    r.bus_reset(5000);
    r.setup(p, 5012);
    const Trace& t = r.trace();
    CHECK_EQ(t.n, 2);
    CHECK_EQ(t.ev[0].kind, std::uint8_t(Ev::Reset));
    CHECK_EQ(t.ev[0].t_ms, 0);
    CHECK_EQ(t.ev[1].t_ms, 12);
    CHECK_EQ(t.ev[1].req_type, 0x80);
    CHECK_EQ(t.ev[1].req, 6);
    CHECK_EQ(t.ev[1].value, 0x0100);
    CHECK_EQ(t.ev[1].length, 64);
    CHECK_EQ(r.last_ms(), 5012u);
    for (int i = 0; i < 30; i++) r.suspend(5100 + std::uint32_t(i));
    CHECK_EQ(r.trace().n, kEvents);
    CHECK_EQ(r.trace().total, 32);
    CHECK_COND(r.trace().flags & kFlagOverflow);
    r.bus_reset(5000 + 70000);   // times saturate at 0xFFFF ms
    CHECK_EQ(r.trace().total, 33);
    r.clear();
    CHECK_EQ(r.trace().n, 0);
}

void test_features() {
    const Features w = features(windows().r.trace());
    CHECK_EQ(w.first_dev_len, 64);
    CHECK_EQ(w.resets_before_addr, 2);
    CHECK_COND(w.addr_ms != 0xFFFF);
    CHECK_COND(w.ms_os);
    CHECK_COND(w.qualifier);
    CHECK_COND(!w.bos);
    CHECK_COND(w.configured);
    CHECK_COND(w.sof);
    CHECK_EQ(w.str_len, 255);
    const Features a = features(apple().r.trace());
    CHECK_EQ(a.first_dev_len, 8);
    CHECK_EQ(a.resets_before_addr, 1);
    CHECK_COND(a.str_short_first);
    CHECK_COND(!a.configured);
    CHECK_EQ(a.str_len, 2);
    Builder l = linux_host();
    l.strings({1, 2, 3}, 255);
    const Features lf = features(l.r.trace());
    CHECK_EQ(lf.strings_after_config, 3);
    CHECK_COND(!lf.str_short_first);
}

void test_classify() {
    CHECK_EQ(int(os_of(windows())), int(Os::Windows));
    CHECK_EQ(int(os_of(linux_host())), int(Os::Linux));
    Builder an = linux_host();
    an.strings({1, 2}, 255);
    CHECK_EQ(int(os_of(an)), int(Os::Android));
    Builder cr = linux_host();
    cr.r.suspend(cr.t += 2000);
    CHECK_EQ(int(os_of(cr)), int(Os::ChromeOs));
    Builder mac = apple();
    mac.config();
    CHECK_EQ(int(os_of(mac)), int(Os::MacOs));
    CHECK_EQ(int(os_of(apple())), int(Os::Ios));
    // Windows without the cached 0xEE read still matches on the second reset plus the qualifier.
    Builder w2;
    w2.sof().reset().desc(kDescDevice, 0, 64).reset().addr(5).desc(kDescDevice, 0, 18).desc(kDescQualifier, 0, 10).config();
    CHECK_EQ(int(os_of(w2)), int(Os::Windows));
    // No requests at all: no SOF = charge-only port; SOFs but no requests = unknown.
    Builder none;
    CHECK_EQ(int(os_of(none)), int(Os::ChargeOnly));
    Builder sof_only;
    sof_only.sof();
    CHECK_EQ(int(os_of(sof_only)), int(Os::Unknown));
    // Something else: a single odd request.
    Builder odd;
    odd.sof().reset().desc(kDescDevice, 0, 18);
    const char* why = nullptr;
    CHECK_EQ(int(classify(odd.r.trace(), &why)), int(Os::Unknown));
    CHECK_COND(why != nullptr);
    CHECK_STR(os_name(Os::ChromeOs), "ChromeOS");
}

}  // namespace

int main() {
    test_recorder();
    test_features();
    test_classify();
    return test_report("test_usb_fp");
}
