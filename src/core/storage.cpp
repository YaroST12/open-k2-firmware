// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#include "k2/core/log.hpp"
#include "k2/core/storage.hpp"

#include <algorithm>
#include <array>
#include <cstring>

#include "k2/pd/emrec.hpp"
#include "k2/core/scratch.hpp"
#include "k2/drivers/flash_map.hpp"   // compile-time check that the flash regions do not overlap
#include "k2/drivers/flash.hpp"
#include "k2/drivers/fs_flash.hpp"
#include "k2/drivers/fusb302.hpp"
#include "k2/drivers/mode_ram.hpp"
#include "k2/drivers/usb.hpp"

namespace k2::storage {
namespace {
store::RealHw g_hw;
std::uint32_t g_gen = 0;

Result finish(const char* what, store::Err e, unsigned slot = 0) {
    K2LOG("[STORE] %s -> %s slot=%u bank=%u\n", what, store::err_name(e), slot, static_cast<unsigned>(flash::store_bank()));
    if (e == store::Err::Ok) g_gen++;
    return {e, slot};
}

// The log snapshot: the newest kLogKeep bytes of the log ring in /log.txt, mirrored in RAM for the drive.
// Small on purpose: .bss growth comes straight out of the stack (tools/gate.py checks the budget). The buffer
// itself lives in the .mode_drive overlay: only the MSC drive serves SAVEDLOG.TXT from RAM, and in Drive mode
// that tenant owns the region for the whole session. Every other mode reads just the length from the volume.
constexpr std::size_t kLogKeep = 512;
constexpr const char* kLogPath = "/log.txt";
K2_MODE_RAM_DRIVE char g_log[kLogKeep];
std::size_t g_log_len = 0;
bool g_log_loaded = false;

void load_log() {
    if (g_log_loaded) return;
    g_log_loaded = true;
    g_log_len = 0;
    std::size_t n = 0;
    if (mode_ram::owner() == mode_ram::Tenant::Drive) {
        if (fs::instance().read_file(kLogPath, {reinterpret_cast<std::uint8_t*>(g_log), kLogKeep}, n) == fs::Err::Ok) g_log_len = n;
    } else if (fs::instance().size_of(kLogPath, n) == fs::Err::Ok && n <= kLogKeep) {
        // read_file would answer TooBig for a file larger than the mirror; the length then stays 0, as before.
        g_log_len = n;
    }
}
}  // namespace

store::Info info() {
    store::Info i = store::Store<store::RealHw>(g_hw).load();
    i.log_len = static_cast<std::uint32_t>(saved_log_len());
    return i;
}

store::Brief brief() {
    store::Brief b = store::Store<store::RealHw>(g_hw).brief();
    b.log_len = static_cast<std::uint32_t>(saved_log_len());
    return b;
}

std::span<const char> saved_log() {
    load_log();
    // The mirror buffer is in the .mode_drive overlay; outside Drive mode the region belongs to another
    // tenant (or is free), so only the length (saved_log_len) is meaningful there.
    if (mode_ram::owner() != mode_ram::Tenant::Drive) return {};
    return {g_log, g_log_len};
}

std::size_t saved_log_len() {
    load_log();
    return g_log_len;
}

Result save_emarker() {
    const emlib::Result r = lib_save_current();
    store::Err e = store::Err::Ok;
    switch (r.err) {
    case emlib::Err::Ok: break;
    case emlib::Err::Full: e = store::Err::Full; break;
    case emlib::Err::Invalid: e = kNoEmarker; break;
    default: e = store::Err::FlashProgram; break;
    }
    return {e, r.seq & 0xFFu};
}

Result save_log() {
    const auto text = usb::log_view();
    std::size_t total = text[0].size() + text[1].size();
    std::size_t skip = total > kLogKeep ? total - kLogKeep : 0;       // keep the newest text
    // Staged in the scratch arena: the RAM mirror itself lives in the .mode_drive overlay (see load_log),
    // which another tenant may own while this runs.
    const scratch::Lease lease("storage.save_log");
    auto& stage = scratch::as<std::array<char, kLogKeep>>();
    std::size_t n = 0;
    for (const auto part : text) {
        const std::size_t cut = std::min(skip, part.size());
        skip -= cut;
        std::memcpy(stage.data() + n, part.data() + cut, part.size() - cut);
        n += part.size() - cut;
    }
    const fs::Err e = n ? fs::instance().write_file(kLogPath, {reinterpret_cast<const std::uint8_t*>(stage.data()), n})
                        : fs::instance().remove(kLogPath);
    const bool ok = e == fs::Err::Ok || e == fs::Err::NotFound;
    g_log_loaded = ok;
    g_log_len = ok ? n : 0;
    return finish("save-log", ok ? store::Err::Ok : e == fs::Err::NoSpace ? store::Err::Full : store::Err::FlashProgram);
}

emlib::Result lib_save(const emrec::Record& rec, const emk_quirk::Quirks& quirks) {
    const emlib::Result r = emlib::Library(fs::instance()).save(rec, quirks);
    K2LOG("[STORE] lib-save -> %s #%u%s\n", emlib::err_name(r.err), static_cast<unsigned>(r.seq), r.existed ? " (already saved)" : "");
    if (r.err == emlib::Err::Ok && !r.existed) g_gen++;
    return r;
}

emlib::Result lib_save_current() {
    const pd::Emarker& em = fusb302::emarker();
    if (!em.valid || em.num_vdos < 5 || (em.spec_rev != 1 && em.spec_rev != 2)) {
        emlib::Result r;
        r.err = emlib::Err::Invalid;
        return r;
    }
    return lib_save(emrec::make({em.raw_vdos.data(), 5}, em.spec_rev));
}

unsigned lib_list(std::span<std::uint16_t> out) { return emlib::Library(fs::instance()).list(out); }

bool lib_get(std::uint16_t seq, emlib::Entry& out) { return emlib::Library(fs::instance()).get(seq, out); }

bool lib_get(std::uint16_t seq, emlib::Entry& out, emk_quirk::Quirks& quirks) {
    return emlib::Library(fs::instance()).get(seq, out, quirks);
}

emlib::Err lib_remove(std::uint16_t seq) {
    const emlib::Err e = emlib::Library(fs::instance()).remove(seq);
    K2LOG("[STORE] lib-remove #%u -> %s\n", static_cast<unsigned>(seq), emlib::err_name(e));
    if (e == emlib::Err::Ok) g_gen++;
    return e;
}

emlib::Err lib_rename(std::uint16_t seq, std::string_view name) {
    const emlib::Err e = emlib::Library(fs::instance()).rename(seq, name);
    K2LOG("[STORE] lib-rename #%u -> %s\n", static_cast<unsigned>(seq), emlib::err_name(e));
    if (e == emlib::Err::Ok) g_gen++;
    return e;
}

std::uint8_t bank() { return flash::store_bank(); }
std::uint32_t generation() { return g_gen; }

}  // namespace k2::storage
