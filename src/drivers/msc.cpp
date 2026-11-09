// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// MSC callbacks for TinyUSB (Bulk-Only Transport, SCSI transparent command set). Read-only.
#include "k2/core/log.hpp"
#include "k2/drivers/msc.hpp"

#include <array>
#include <cstddef>
#include <cstring>
#include <new>

#include "k2/drivers/drive.hpp"
#include "k2/pd/emlib.hpp"
#include "k2/drivers/fusb302.hpp"
#include "k2/drivers/mode_ram.hpp"
#include "k2/pd/pdtrace.hpp"
#include "k2/drivers/power_monitor.hpp"
#include "k2/chargertest/report_card_hw.hpp"
#include "k2/core/storage.hpp"
#include "k2/core/textbuf.hpp"
#include "k2/drivers/usb.hpp"
#include "tusb.h"
#include "k2/core/version.hpp"

namespace k2::msc {
namespace {

// The drive (~7.9 KB) lives in the RAM it shares with the network stack (k2/drivers/mode_ram.hpp); it is
// constructed by init() once the lease is ours and stays null in every other mode.
alignas(drive::Drive) K2_MODE_RAM_DRIVE std::byte g_drive_mem[sizeof(drive::Drive)];
drive::Drive* g_drive = nullptr;
std::uint32_t g_gen = 0, g_card_gen = 0;
// Card numbers, newest first. Drive-only state like the Drive object: the .mode_drive overlay is ours for the
// whole session in Drive mode and never touched in the others (the region is zeroed at boot, like .bss).
K2_MODE_RAM_DRIVE std::array<std::uint16_t, drive::kMaxCards> g_cards;
unsigned g_caps = 0;   // bit per black box slot that holds a capture

unsigned cap_mask() {
    unsigned m = 0;
    for (unsigned i = 0; i < pdtrace::kCapSlots; i++)
        if (!pdtrace::capture_data(i).empty()) m |= 1u << i;
    return m;
}

const char* pdo_name(pd::PdoType t) {
    switch (t) {
    case pd::PdoType::Fixed: return "fixed";
    case pd::PdoType::Battery: return "battery";
    case pd::PdoType::Variable: return "variable";
    case pd::PdoType::Pps: return "pps";
    case pd::PdoType::Avs: return "avs";
    default: return "unknown";
    }
}

std::size_t render_status(void*, std::span<char> out) {
    TextBuf t(out);
    t.put("K2 status at read time\r\n");
    if (const auto m = power::read()) {
        t.put("vbus_mv: %d\r\ncurrent_ma: %d\r\npower_mw: %d\r\n", static_cast<int>(m->vbus_mv),
              static_cast<int>(m->current_ma), static_cast<int>(m->power_mw));
    } else {
        t.put("power monitor: offline\r\n");
    }
    const pd::SourceCaps& c = fusb302::caps();
    if (c.valid) {
        t.put("source caps: %u PDOs, PD spec rev %u, CC%u\r\n", static_cast<unsigned>(c.num_pdos),
              static_cast<unsigned>(c.spec_rev), static_cast<unsigned>(c.active_cc));
        for (unsigned i = 0; i < c.num_pdos && i < c.pdos.size(); i++) {
            const pd::Pdo& p = c.pdos[i];
            t.put("pdo%u: %s %u-%u mV %u mA\r\n", i + 1, pdo_name(p.type), static_cast<unsigned>(p.min_mv),
                  static_cast<unsigned>(p.max_mv), static_cast<unsigned>(p.max_ma));
        }
    } else {
        t.put("source caps: none\r\n");
    }
    const pd::Emarker& e = fusb302::emarker();
    if (e.valid) {
        t.put("cable: vid %04X pid %04X, %u A, %u V, %u W, %s\r\n", static_cast<unsigned>(e.vid),
              static_cast<unsigned>(e.pid), static_cast<unsigned>(e.max_current_a),
              static_cast<unsigned>(e.max_voltage_v), static_cast<unsigned>(e.max_power_w), e.speed_str);
    } else {
        t.put("cable: no e-marker read\r\n");
    }
    return t.size();
}

std::size_t render_log(void*, std::span<char> out) { return usb::log_text(out); }

void load_store() {
    const store::Brief i = storage::brief();
    g_drive->set_saved_log(storage::saved_log());
    g_drive->set_native_page(i.library_page);
    std::array<std::uint16_t, emlib::kCapacity> seqs;
    const unsigned n = storage::lib_list(seqs);
    g_drive->set_cables(std::span<const std::uint16_t>(seqs.data(), n));
    g_card_gen = report_card::hw::generation();
    g_drive->set_cards(static_cast<unsigned>(report_card::hw::numbers(g_cards)));
}

}  // namespace

void init() {
    if (!mode_ram::claim(mode_ram::Tenant::Drive)) {
        K2LOG("[MSC] shared RAM held by another USB function, drive off\n");
        return;
    }
    g_drive = ::new (g_drive_mem) drive::Drive{};
    drive::Env env;
    env.version = kFwVersion;
    env.git = kGitHash;
    env.status = render_status;
    env.log = render_log;
    env.capture = [](void*, unsigned slot) { return pdtrace::capture_data(slot); };
    env.capture_slots = pdtrace::kCapSlots;
    env.cable = [](void*, std::uint16_t seq, emlib::Entry& e) { return storage::lib_get(seq, e); };
    env.card = [](void*, unsigned index, report_card::Card& c) {
        return index < g_cards.size() && report_card::hw::get(g_cards[index], c);
    };
    g_drive->init(env);
    load_store();
    g_gen = storage::generation();
    g_caps = cap_mask();
}

void poll() {
    if (!g_drive) return;
    if (const unsigned m = cap_mask(); m != g_caps) {   // a capture appeared or was cleared
        g_caps = m;
        load_store();
    }
    if (g_card_gen != report_card::hw::generation()) {   // a charger test saved, renamed or deleted a card
        load_store();
        K2LOG("[MSC] volume rebuilt after card change\n");
    }
    if (g_gen == storage::generation()) return;
    g_gen = storage::generation();
    load_store();
    K2LOG("[MSC] volume rebuilt after store change\n");
}

}  // namespace k2::msc

// ---- TinyUSB callbacks (C linkage) -----------------------------------------------------------
extern "C" {

void tud_msc_inquiry_cb(uint8_t, uint8_t vendor_id[8], uint8_t product_id[16], uint8_t product_rev[4]) {
    std::memcpy(vendor_id, "OpenK2  ", 8);
    std::memcpy(product_id, "K2 Drive        ", 16);
    std::memcpy(product_rev, "1.0 ", 4);
}

bool tud_msc_test_unit_ready_cb(uint8_t) { return k2::msc::g_drive && k2::msc::g_drive->ok(); }

void tud_msc_capacity_cb(uint8_t, uint32_t* block_count, uint16_t* block_size) {
    *block_count = k2::msc::g_drive ? k2::msc::g_drive->sectors() : 0;
    *block_size = k2::vfat::kSectorSize;
}

bool tud_msc_start_stop_cb(uint8_t, uint8_t, bool, bool) { return true; }   // nothing to eject

bool tud_msc_is_writable_cb(uint8_t) { return false; }

int32_t tud_msc_read10_cb(uint8_t, uint32_t lba, uint32_t offset, void* buffer, uint32_t bufsize) {
    using k2::vfat::kSectorSize;
    auto* out = static_cast<uint8_t*>(buffer);
    if (!k2::msc::g_drive) return -1;
    // The endpoint buffer is a whole number of sectors, so the stack hands over whole sectors at offset 0: each
    // is read straight into the buffer (a 512-byte bounce buffer here sat on the stack of every USB service).
    static_assert(CFG_TUD_MSC_EP_BUFSIZE % kSectorSize == 0, "partial-sector reads would need a bounce buffer");
    if (offset % kSectorSize != 0 || bufsize % kSectorSize != 0) return -1;
    for (uint32_t done = 0; done < bufsize; done += kSectorSize)
        k2::msc::g_drive->read(lba + (offset + done) / kSectorSize, std::span<uint8_t, kSectorSize>(out + done, kSectorSize));
    return static_cast<int32_t>(bufsize);
}

int32_t tud_msc_write10_cb(uint8_t lun, uint32_t, uint32_t, uint8_t*, uint32_t) {
    tud_msc_set_sense(lun, SCSI_SENSE_DATA_PROTECT, 0x27, 0x00);   // write protected
    return -1;
}

int32_t tud_msc_scsi_cb(uint8_t lun, uint8_t const[16], void*, uint16_t) {
    tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x20, 0x00);   // invalid command operation code
    return -1;
}

}  // extern "C"
