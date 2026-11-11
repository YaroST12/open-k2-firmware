// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Black Box timestamps from the RX interrupt edge: stamp_rx() gives the next Poll / Rx / RxBad records the edge time instead of "now",
// never moves the ring's time backwards, and applies to one frame only.
#include <array>
#include <cstdint>

#include "k2/pd/pdtrace.hpp"
#include "test_util.h"

using namespace k2::pdtrace;

namespace {

// ts of the newest record of `type` pushed since `from`, 0 when none.
std::uint32_t last_ts(std::uint32_t from, Type type) {
    std::array<std::uint8_t, 600> buf;
    const auto r = live_ring().read(from, buf);
    std::uint32_t ts = 0;
    for (std::size_t i = 0; i + kHeader <= r.bytes && buf[i] >= kHeader; i += buf[i])
        if (buf[i + 1] == std::uint8_t(type))
            ts = buf[i + 2] | buf[i + 3] << 8 | buf[i + 4] << 16 | std::uint32_t(buf[i + 5]) << 24;
    return ts;
}

void fl() { freeze(true); freeze(false); }   // freezing writes out what Collapse holds back

void rx_frame(std::uint8_t id) {
    const std::uint8_t crc[4] = {id, 0, 0, 0};
    rx(0xE0, std::uint16_t(0x0001 | id << 9), crc);   // a GoodCRC-sized frame, MessageID varies so Collapse sees a new frame
}

}  // namespace

int main() {
    init();
    fl();
    {   // a stamp ahead of the ring's clock is used as it is (the host sees the edge time)
        const std::uint32_t h = live_ring().head();
        mark(1);
        fl();
        const std::uint32_t t_mark = last_ts(h, Type::Mark);
        stamp_rx(t_mark + 5000);
        rx_frame(1);
        fl();
        CHECK_EQ(int(last_ts(h, Type::Rx)), int(t_mark + 5000));
    }
    {   // the stamp is for one frame: the next one is stamped now
        const std::uint32_t h = live_ring().head();
        stamp_rx(0x7FFFFFF0u);
        rx_frame(2);
        fl();
        const std::uint32_t t1 = last_ts(h, Type::Rx);
        CHECK_EQ(int(t1), 0x7FFFFFF0);
        const std::uint32_t h2 = live_ring().head();
        rx_frame(3);
        fl();
        // unstamped: the clock, but never before the previous record (the ring's time stays monotone)
        CHECK_COND(std::int32_t(last_ts(h2, Type::Rx) - t1) >= 0);
    }
    {   // a stamp older than a record already in the ring is clamped up to it
        const std::uint32_t h = live_ring().head();
        mark(2);
        fl();
        const std::uint32_t t_mark = last_ts(h, Type::Mark);
        stamp_rx(t_mark - 700);
        rx_frame(4);
        fl();
        CHECK_COND(std::int32_t(last_ts(h, Type::Rx) - t_mark) >= 0);
    }
    {   // a bad frame takes and spends the stamp too
        const std::uint32_t h = live_ring().head();
        mark(3);
        fl();
        const std::uint32_t t_mark = last_ts(h, Type::Mark);
        stamp_rx(t_mark + 40);
        rx_bad(0x00, 1, 2);
        fl();
        CHECK_EQ(int(last_ts(h, Type::RxBad)), int(t_mark + 40));
    }
    return test_report("test_pdtrace_stamp");
}
