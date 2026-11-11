// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// PD Stress journal on the littlefs volume, see k2/pdstress/journal_hw.hpp.
#include "k2/pdstress/journal_hw.hpp"

#include "k2/core/log.hpp"
#include "k2/drivers/fs_flash.hpp"
#include "k2/drivers/systick.hpp"

namespace k2::pdstress::journal_hw {
namespace {
// A write that held the main loop longer than this is named: a source resets the link after ~100 ms of silence.
constexpr std::uint32_t kSlowMs = 150;

class FsStore final : public journal::Store {
public:
    bool read(std::span<std::uint8_t> out, std::size_t& n) override {
        n = 0;
        return fs::instance().read_file(kFile, out, n) == fs::Err::Ok;
    }
    bool write(std::span<const std::uint8_t> in) override {
        auto& f = fs::instance();
        const std::uint32_t t0 = systick::millis();
        f.mkdir(kDir);   // an existing directory is fine
        const fs::Err e = f.write_file(kFile, in);
        const std::uint32_t ms = systick::millis() - t0;
        if (e != fs::Err::Ok) {
            K2LOG("[PS] journal write failed fs=%s ms=%u\n", fs::err_name(e), static_cast<unsigned>(ms));
            return false;
        }
        if (ms > kSlowMs) K2LOG("[PS] slow journal write ms=%u\n", static_cast<unsigned>(ms));
        return true;
    }
    void remove() override { fs::instance().remove(kFile); }
};
FsStore g_store;
}  // namespace

journal::Store& store() { return g_store; }

}  // namespace k2::pdstress::journal_hw
