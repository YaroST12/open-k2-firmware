// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// PD limiter Hard Reset comeback store (see k2/core/lim_comeback.hpp).
#include "k2/core/lim_comeback.hpp"

#include "k2/chargertest/session_store.hpp"
#include "k2/core/fs.hpp"
#include "k2/core/log.hpp"
#include "k2/drivers/fs_flash.hpp"

namespace k2::limcomeback {
namespace {

bool g_pending = false;
pd::LimConfig g_cfg{};

}  // namespace

bool save(const pd::LimConfig& cfg) {
    if (!pd::lim_config_valid(cfg)) return false;
    const auto raw = encode(cfg);
    return fs::instance().write_file(kPath, raw) == fs::Err::Ok;
}

void clear() {
    g_pending = false;
    fs::instance().remove(kPath);
}

bool begin(bootmode::Mode keys, bool warm) {
    g_pending = false;
    if (keys != bootmode::Mode::Plain || warm) return false;
    fs::Fs& f = fs::instance();
    if (f.mount() != fs::Err::Ok) return false;
    std::array<std::uint8_t, kSize> raw{};
    std::size_t n = 0;
    if (f.read_file(kPath, raw, n) != fs::Err::Ok || n != kSize) return false;
    const auto cfg = decode(raw);
    const bool canary = session::backup_alive();
    if (!should_restore(keys, warm, cfg.has_value(), canary)) return false;
    g_cfg = *cfg;
    g_pending = true;
    K2LOG("[LIM] comeback boot: re-arming saved limit\n");
    return true;
}

bool pending() { return g_pending; }
const pd::LimConfig& config() { return g_cfg; }

void consume() {
    g_pending = false;
    fs::instance().remove(kPath);
}

}  // namespace k2::limcomeback
