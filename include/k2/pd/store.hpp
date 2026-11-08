// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Native e-marker pages: read-only access to the single copied-e-marker slot and the 10-slot
// library already on the unit. Nothing here ever writes; saved cables live in k2/pd/emlib.hpp on the littlefs volume.
// Templated over a view() seam so tests/test_store.cpp runs it over a fake flash; src/store.cpp binds the
// real one.
//
//   0x080E3000  1 sector   native single copied-e-marker slot: READ ONLY, outside every writable range
//   0x080E3800  1 sector   native e-marker library: READ ONLY (listed on the drive as NATIVE/)
#pragma once
#include <array>
#include <cstdint>
#include <concepts>
#include <cstring>
#include <span>
#include <string_view>

#include "k2/pd/emrec.hpp"
#include "k2/drivers/flash.hpp"

namespace k2::store {

inline constexpr std::uint32_t kSingleAddr = 0x080E3000;
inline constexpr std::uint32_t kLibraryAddr = 0x080E3800;


static_assert(emrec::kLibraryMaxSize <= flash::kSectorSize, "the library fits its sector");

enum class Err : std::uint8_t {
    Ok,
    Full,              // library has 10 records
    BadSlot,           // slot id out of range
    Empty,             // nothing at that slot
    Invalid,           // record fails the record validator
    TooBig,            // text longer than the space
    Foreign,           // library page holds data this firmware did not write
    FlashRefused,
    FlashBusy,
    FlashKey,
    FlashProgram,
    FlashWriteProtect,
    FlashVerify,
};

constexpr Err err_from(flash::Status s) {
    switch (s) {
    case flash::Status::Ok: return Err::Ok;
    case flash::Status::Refused: return Err::FlashRefused;
    case flash::Status::Busy: return Err::FlashBusy;
    case flash::Status::KeyFail: return Err::FlashKey;
    case flash::Status::ProgramError: return Err::FlashProgram;
    case flash::Status::WriteProtect: return Err::FlashWriteProtect;
    case flash::Status::VerifyFailed: return Err::FlashVerify;
    }
    return Err::FlashRefused;
}

constexpr const char* err_name(Err e) {
    switch (e) {
    case Err::Ok: return "ok";
    case Err::Full: return "library full";
    case Err::BadSlot: return "bad slot";
    case Err::Empty: return "slot empty";
    case Err::Invalid: return "invalid record";
    case Err::TooBig: return "too big";
    case Err::Foreign: return "library page not written by this firmware";
    case Err::FlashRefused: return "flash refused";
    case Err::FlashBusy: return "flash busy";
    case Err::FlashKey: return "flash unlock failed";
    case Err::FlashProgram: return "flash program error";
    case Err::FlashWriteProtect: return "flash write protected";
    case Err::FlashVerify: return "flash verify failed";
    }
    return "?";
}

struct Info {
    emrec::Library lib;
    bool has_single = false;
    emrec::Record single{};
    bool library_foreign = false;
    bool has_native = false;                   // the foreign page parses as a native library
    emrec::NativeLibrary native{};
    std::span<const std::uint8_t> library_page{};   // the raw 2 KB page when it is not blank
    std::uint32_t log_len = 0;                      // size of the saved log (/log.txt), filled by storage.cpp
};

// The flags and sizes of Info without its ~1.5 KB of decoded tables (the remote and the drive only need these).
struct Brief {
    bool has_single = false;
    bool library_foreign = false;
    std::uint8_t native_count = 0;                  // records of the foreign page when it parses as a native library
    std::span<const std::uint8_t> library_page{};   // the raw 2 KB page when it is not blank
    std::uint32_t log_len = 0;                      // filled by storage.cpp
};

template <class Hw>
    requires requires(const Hw& hw, std::uint32_t a) { { hw.view(a) } -> std::same_as<const std::uint8_t*>; }
class Store {
public:
    explicit Store(Hw& hw) : hw_(hw) {}

    Brief brief() const {
        Brief b;
        const std::uint8_t* lib = hw_.view(kLibraryAddr);
        const std::span<const std::uint8_t> page{lib, flash::kSectorSize};
        if (!emrec::library_valid(page)) b.library_foreign = !blank(kLibraryAddr);
        if (b.library_foreign) {
            b.library_page = page;
            b.native_count = static_cast<std::uint8_t>(emrec::native_library_count(page));
        }
        b.has_single = emrec::valid({hw_.view(kSingleAddr), emrec::kRecordSize});
        return b;
    }

    Info load() const {
        Info i;
        const std::uint8_t* lib = hw_.view(kLibraryAddr);
        if (!emrec::decode_library({lib, flash::kSectorSize}, i.lib)) i.library_foreign = !blank(kLibraryAddr);
        if (i.library_foreign) {
            i.library_page = {lib, flash::kSectorSize};
            i.has_native = emrec::parse_native_library(i.library_page, i.native);
        }
        const std::uint8_t* one = hw_.view(kSingleAddr);
        if (emrec::valid({one, emrec::kRecordSize})) {
            i.has_single = true;
            std::memcpy(i.single.data(), one, emrec::kRecordSize);
        }
        return i;
    }

private:
    Hw& hw_;

    bool blank(std::uint32_t addr) const {
        const std::uint8_t* p = hw_.view(addr);
        for (std::uint32_t k = 0; k < flash::kSectorSize; k++)
            if (p[k] != 0xFF) return false;
        return true;
    }
};

// ---- Real flash (src/store.cpp) -------------------------------------------------------------
// Memory-mapped flash as a store seam (read only).
struct RealHw {
    const std::uint8_t* view(std::uint32_t addr) const { return flash::store_view(addr); }
};

}  // namespace k2::store
