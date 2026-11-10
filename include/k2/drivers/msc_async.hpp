// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The hand-off between TinyUSB's READ10 callback and the main loop (k2/drivers/msc.hpp).
//
// The callback runs inside tud_task(), and tud_task() is also called from every LCD wait and delay, so
// anything rendered there sits on top of whatever page is painting. The callback therefore only stashes the
// request (submit) and returns TUD_MSC_RET_ASYNC; the main loop takes it (take), reads the sectors and
// completes it with tud_msc_async_io_done(). Pure state, no TinyUSB, so the host tests cover the cases.
//
// A ticket carries the epoch it was taken in. drop() (bus reset, unmount, the host moved on to another
// command) and every submit() start a new epoch, so a read that was in progress while that happened is no
// longer valid and must not be completed into the transfer that replaced it.
#pragma once

#include <cstdint>
#include <optional>

namespace k2::msc {

template <std::uint32_t SectorSize>
class AsyncRead {
public:
    struct Req {
        std::uint32_t lba = 0, offset = 0, size = 0;
        std::uint8_t* buf = nullptr;
    };
    struct Ticket {
        Req req;
        std::uint32_t epoch = 0;
    };

    // Stash a request. False when it is not a whole number of sectors (the callback answers with an error).
    bool submit(std::uint32_t lba, std::uint32_t offset, std::uint8_t* buf, std::uint32_t size) {
        if (size == 0 || offset % SectorSize != 0 || size % SectorSize != 0) return false;
        req_ = {lba, offset, size, buf};
        pending_ = true;
        epoch_++;
        return true;
    }

    // Main loop: the pending request, once.
    std::optional<Ticket> take() {
        if (!pending_) return std::nullopt;
        pending_ = false;
        return Ticket{req_, epoch_};
    }

    // Still the request TinyUSB is waiting for (nothing dropped or submitted since take()).
    bool valid(const Ticket& t) const { return t.epoch == epoch_; }

    // The transfer is gone: forget a stashed request and invalidate a ticket that is being worked on.
    void drop() {
        pending_ = false;
        epoch_++;
    }

private:
    Req req_{};
    bool pending_ = false;
    std::uint32_t epoch_ = 0;
};

}  // namespace k2::msc
