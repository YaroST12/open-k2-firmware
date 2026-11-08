// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Host tests for the flash write allow-list, the guarded programmer and the DFU entry sequence.
// The hardware sits behind two small fakes, so what is checked is policy: which addresses ever
// reach the flash controller, in which order, and when the reset is (not) taken.
#include <array>
#include <string>
#include <string_view>
#include <vector>

#include "k2/drivers/dfu.hpp"
#include "k2/drivers/flash.hpp"
#include "test_util.h"

using namespace k2;

namespace {

// A flash array that records every program request and models "program can only clear bits".
struct FakeFlash {
    std::vector<std::pair<uint32_t, uint16_t>> programmed;
    uint32_t word_at_marker = 0x30303032 + 0;  // overwritten in main()
    uint16_t mem[4] = {0x324B, 0x3030, 0xFFFE, 0xFFFF};  // 0x08004010, 12, 0x08004400, 02
    flash::Status result = flash::Status::Ok;
    bool stuck = false;  // a controller that reports Ok but changes nothing

    static int slot(uint32_t a) {
        switch (a) {
        case 0x08004010: return 0;
        case 0x08004012: return 1;
        case 0x08004400: return 2;
        case 0x08004402: return 3;
        default: return -1;
        }
    }
    flash::Status program(uint32_t addr, uint16_t v) {
        programmed.emplace_back(addr, v);
        if (result != flash::Status::Ok) return result;
        const int s = slot(addr);
        if (s >= 0 && !stuck) mem[s] &= v;
        return flash::Status::Ok;
    }
    uint16_t read(uint32_t addr) const {
        const int s = slot(addr);
        return s >= 0 ? mem[s] : 0xFFFF;
    }
};

struct FakeEnv {
    std::string log;
    int flushes = 0, resets = 0, clears = 0, detaches = 0;
    bool clear_ok = true, marker_gone = true;
    std::vector<std::string> order;
    void say(std::string_view m) { log += m; order.push_back("log"); }
    void flush() { flushes++; order.push_back("flush"); }
    bool clear_marker() { clears++; order.push_back("clear"); return clear_ok; }
    bool marker_cleared() { order.push_back("verify"); return marker_gone; }
    void detach() { detaches++; order.push_back("detach"); }
    void dump_ram() { dumps++; order.push_back("dump"); }
    void park() { parks++; order.push_back("park"); }
    void reset(dfu::ResetMode m) { resets++; mode = m; order.push_back("reset"); }
    int dumps = 0, parks = 0;
    dfu::ResetMode mode = dfu::ResetMode::Sys;
};

}  // namespace

int main() {
    // ---- allow-list: exactly the two half-words of the chosen marker word, value 0 only ------
    CHECK_EQ(flash::allowed(0x08004010, 0), 1);
    CHECK_EQ(flash::allowed(0x08004012, 0), 1);
    CHECK_EQ(flash::kDfuMarkerAddr, 0x08004010u);
    CHECK_EQ(flash::allowed(0x08004010, 0x0001), 0);      // not a pure clear
    CHECK_EQ(flash::allowed(0x08004010, 0xFFFF), 0);
    CHECK_EQ(flash::allowed(0x08004011, 0), 0);           // misaligned
    CHECK_EQ(flash::allowed(0x08004014, 0), 0);           // next word of the signature slot
    CHECK_EQ(flash::allowed(0x08004000, 0), 0);           // version word
    CHECK_EQ(flash::allowed(0x08004400, 0), 0);           // commit marker (not the chosen one)
    CHECK_EQ(flash::allowed(0x08004404, 0), 0);           // first code word
    CHECK_EQ(flash::allowed(0x08003800, 0), 0);           // vector table
    CHECK_EQ(flash::allowed(0x08003000, 0), 0);           // bootloader region
    CHECK_EQ(flash::allowed(0x08000000, 0), 0);           // bootloader
    CHECK_EQ(flash::allowed(0x080E3000, 0), 0);           // settings page
    CHECK_EQ(flash::allowed(0x20000000, 0), 0);           // RAM
    CHECK_EQ(flash::allowed(0x00000000, 0), 0);
    CHECK_EQ(flash::allowed(0xFFFFFFFF, 0), 0);

    // Every address below the app start is refused whatever the value.
    for (uint32_t a = 0x08000000; a < 0x08003800; a += 0x40)
        for (uint16_t v : {uint16_t{0}, uint16_t{0xFFFF}}) CHECK_EQ(flash::allowed(a, v), 0);

    // ---- guarded programmer ------------------------------------------------------------------
    {
        FakeFlash f;
        CHECK_EQ(static_cast<int>(flash::program_guarded(f, 0x08004400, 0)), static_cast<int>(flash::Status::Refused));
        CHECK_EQ(static_cast<int>(flash::program_guarded(f, 0x08004010, 7)), static_cast<int>(flash::Status::Refused));
        CHECK_EQ(static_cast<int>(flash::program_guarded(f, 0x08003800, 0)), static_cast<int>(flash::Status::Refused));
        CHECK_EQ(f.programmed.size(), 0u);  // the controller never saw a refused request
        CHECK_EQ(f.mem[2], 0xFFFE);
    }
    {
        FakeFlash f;
        CHECK_EQ(static_cast<int>(flash::clear_marker(f)), static_cast<int>(flash::Status::Ok));
        CHECK_EQ(f.programmed.size(), 2u);
        CHECK_EQ(f.programmed[0].first, 0x08004010u);
        CHECK_EQ(f.programmed[1].first, 0x08004012u);
        CHECK_EQ(f.programmed[0].second, 0);
        CHECK_EQ(f.programmed[1].second, 0);
        CHECK_EQ(f.mem[0], 0);
        CHECK_EQ(f.mem[1], 0);
        CHECK_EQ(f.mem[2], 0xFFFE);  // the commit marker is untouched
        CHECK_EQ(f.mem[3], 0xFFFF);
        // Clearing again is idempotent.
        CHECK_EQ(static_cast<int>(flash::clear_marker(f)), static_cast<int>(flash::Status::Ok));
    }
    {   // controller error: reported, second half-word not attempted
        FakeFlash f;
        f.result = flash::Status::ProgramError;
        CHECK_EQ(static_cast<int>(flash::clear_marker(f)), static_cast<int>(flash::Status::ProgramError));
        CHECK_EQ(f.programmed.size(), 1u);
    }
    {   // controller says Ok but the readback differs: VerifyFailed
        FakeFlash f;
        f.stuck = true;
        CHECK_EQ(static_cast<int>(flash::clear_marker(f)), static_cast<int>(flash::Status::VerifyFailed));
    }

    // ---- DFU entry sequence ------------------------------------------------------------------
    {
        FakeEnv e;
        CHECK_EQ(dfu::enter_sequence(e, dfu::ResetMode::Sys), 1);
        CHECK_EQ(e.resets, 1);
        CHECK_EQ(static_cast<int>(e.mode), static_cast<int>(dfu::ResetMode::Sys));
        CHECK_EQ(e.clears, 1);
        CHECK_EQ(e.flushes >= 1, 1);
        CHECK_EQ(e.log.find("[DFU] entering bootloader") != std::string::npos, 1);
        // order: announce, flush, clear, verify, reset (reset is last)
        CHECK_EQ(e.order.front() == "log", 1);
        CHECK_EQ(e.order.back() == "reset", 1);
        size_t fl = 0, cl = 0, vf = 0, rs = 0, dt = 0;
        for (size_t i = 0; i < e.order.size(); i++) {
            if (e.order[i] == "flush" && fl == 0) fl = i;
            if (e.order[i] == "clear") cl = i;
            if (e.order[i] == "verify") vf = i;
            if (e.order[i] == "reset") rs = i;
            if (e.order[i] == "detach") dt = i;
        }
        CHECK_EQ(fl < cl && cl < vf && vf < rs, 1);
        // the host must see a USB detach before the reset, after the last log line left
        CHECK_EQ(e.detaches, 1);
        CHECK_EQ(vf < dt && dt < rs, 1);
        CHECK_EQ(e.order[dt - 1] == "flush", 1);
        // RAM windows are logged, then the peripherals are parked, both before the detach
        size_t dm = 0, pk = 0;
        for (size_t i = 0; i < e.order.size(); i++) {
            if (e.order[i] == "dump") dm = i;
            if (e.order[i] == "park") pk = i;
        }
        CHECK_EQ(e.dumps, 1);
        CHECK_EQ(e.parks, 1);
        CHECK_EQ(vf < dm && dm < pk && pk < dt, 1);
    }
    {   // the watchdog mode reaches the reset step and is announced
        FakeEnv e;
        CHECK_EQ(dfu::enter_sequence(e, dfu::ResetMode::Iwdg), 1);
        CHECK_EQ(static_cast<int>(e.mode), static_cast<int>(dfu::ResetMode::Iwdg));
        CHECK_EQ(e.log.find("iwdg") != std::string::npos, 1);
    }
    {   // no dump, no park, no detach when the marker was not cleared
        FakeEnv e;
        e.clear_ok = false;
        dfu::enter_sequence(e, dfu::ResetMode::Sys);
        CHECK_EQ(e.dumps + e.parks, 0);
    }
    {   // the clear itself fails: no reset
        FakeEnv e;
        e.clear_ok = false;
        CHECK_EQ(dfu::enter_sequence(e, dfu::ResetMode::Sys), 0);
        CHECK_EQ(e.resets, 0);
        CHECK_EQ(e.detaches, 0);                          // USB stays up so the failure is visible
        CHECK_EQ(e.log.find("[DFU]") != std::string::npos, 1);
    }
    {   // the clear says fine but the readback still shows a valid signature: no reset
        FakeEnv e;
        e.marker_gone = false;
        CHECK_EQ(dfu::enter_sequence(e, dfu::ResetMode::Sys), 0);
        CHECK_EQ(e.resets, 0);
        CHECK_EQ(e.detaches, 0);
        CHECK_EQ(e.log.find("readback") != std::string::npos, 1);
    }
    return test_report("test_flash");
}
