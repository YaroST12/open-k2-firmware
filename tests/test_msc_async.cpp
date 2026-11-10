// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// AsyncRead (k2/drivers/msc_async.hpp): the READ10 callback only stashes the request, the main loop takes it,
// does the sector reads and completes it. A request that was dropped (bus reset, unmount, a new command) or
// replaced while the main loop was reading must not be completed into the new transfer.
#include <array>
#include <cstdint>

#include "k2/drivers/msc_async.hpp"
#include "test_util.h"

using k2::msc::AsyncRead;

int main() {
    std::array<std::uint8_t, 512> buf{};
    {   // nothing pending until a request is submitted; take() hands it over once
        AsyncRead<512> a;
        CHECK_COND(!a.take().has_value());
        CHECK_COND(a.submit(7, 0, buf.data(), 512));
        const auto t = a.take();
        CHECK_COND(t.has_value());
        CHECK_EQ(t->req.lba, 7);
        CHECK_EQ(t->req.offset, 0);
        CHECK_EQ(t->req.size, 512);
        CHECK_COND(t->req.buf == buf.data());
        CHECK_COND(a.valid(*t));
        CHECK_COND(!a.take().has_value());
    }
    {   // a request that is not whole sectors is an error for the callback (no stash)
        AsyncRead<512> a;
        CHECK_COND(!a.submit(0, 100, buf.data(), 512));
        CHECK_COND(!a.submit(0, 0, buf.data(), 100));
        CHECK_COND(!a.submit(0, 0, buf.data(), 0));
        CHECK_COND(!a.take().has_value());
    }
    {   // dropped before the main loop got to it: nothing to do
        AsyncRead<512> a;
        CHECK_COND(a.submit(1, 0, buf.data(), 512));
        a.drop();
        CHECK_COND(!a.take().has_value());
    }
    {   // dropped while the main loop was reading: the ticket is stale, the completion must be skipped
        AsyncRead<512> a;
        CHECK_COND(a.submit(1, 0, buf.data(), 512));
        const auto t = a.take();
        CHECK_COND(a.valid(*t));
        a.drop();
        CHECK_COND(!a.valid(*t));
    }
    {   // replaced while the main loop was reading (a nested tud_task took a new command): old ticket is stale
        AsyncRead<512> a;
        CHECK_COND(a.submit(1, 0, buf.data(), 512));
        const auto old = a.take();
        CHECK_COND(a.submit(2, 0, buf.data(), 512));
        CHECK_COND(!a.valid(*old));
        const auto fresh = a.take();
        CHECK_COND(a.valid(*fresh));
        CHECK_EQ(fresh->req.lba, 2);
    }
    {   // a second submit before take() replaces the first (the earlier one was reset away)
        AsyncRead<512> a;
        CHECK_COND(a.submit(1, 0, buf.data(), 512));
        CHECK_COND(a.submit(9, 0, buf.data(), 512));
        const auto t = a.take();
        CHECK_EQ(t->req.lba, 9);
        CHECK_COND(!a.take().has_value());
    }
    {   // several sectors per request (a larger endpoint buffer) keep their size
        AsyncRead<512> a;
        std::array<std::uint8_t, 2048> big{};
        CHECK_COND(a.submit(4, 512, big.data(), 2048));
        CHECK_EQ(a.take()->req.size, 2048);
    }
    return test_report("test_msc_async");
}
