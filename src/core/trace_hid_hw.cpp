// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#include "k2/core/trace_hid_hw.hpp"

#include <array>

#include "k2/core/remote_proto.hpp"
#include "k2/core/trace_hid.hpp"
#include "k2/drivers/systick.hpp"
#include "k2/drivers/usb.hpp"
#include "k2/pd/pdtrace.hpp"

namespace k2::trace_hid_hw {
namespace {

trace_hid::Session g_session;

constexpr std::uint8_t kKeepFree = 3;        // IN queue slots left for command responses (the queue holds 16)
constexpr unsigned kMaxReportsPerPass = 8;   // bounds the work of one main-loop pass; the pipe takes one report per ms

void put32(std::uint8_t* p, std::uint32_t v) {
    for (int i = 0; i < 4; i++) p[i] = static_cast<std::uint8_t>(v >> (8 * i));
}

}  // namespace

bool active() { return g_session.active(); }

std::size_t command(std::span<const std::uint8_t> req, std::span<std::uint8_t> out) {
    if (req.empty() || out.size() < remote::kThRspBytes) return 0;
    const std::uint32_t now = systick::millis();
    switch (req[0]) {
    case remote::kThStart:
        g_session.start(pdtrace::live_ring(), (req[1] & remote::kThFlagAll) != 0, now,
                        static_cast<std::uint32_t>(req[2] | req[3] << 8));
        usb::set_hid_quiet(true);
        break;
    case remote::kThStop:
        g_session.stop();
        usb::set_hid_quiet(false);
        break;
    case remote::kThKeepalive:
        g_session.keepalive(now);
        break;
    default:   // kThStatus
        break;
    }
    const auto st = g_session.stats();
    out[0] = g_session.active() ? 1 : 0;
    put32(&out[1], st.reports);
    put32(&out[5], st.records);
    put32(&out[9], st.lost);
    return remote::kThRspBytes;
}

void service() {
    if (!g_session.active()) return;
    const std::uint32_t now = systick::millis();
    std::array<std::uint8_t, trace_hid::kReportBytes> f;
    unsigned n = 0;
    while (n < kMaxReportsPerPass && usb::tx_free() > kKeepFree) {
        if (!g_session.next(pdtrace::live_ring(), now, f)) break;
        usb::send_report(f.data());
        n++;
    }
    // A host that stopped reading leaves the queue full, so next() is never asked for the End report: end it here.
    if (g_session.expired(now)) g_session.stop();
    if (!g_session.active()) usb::set_hid_quiet(false);
}

}  // namespace k2::trace_hid_hw
