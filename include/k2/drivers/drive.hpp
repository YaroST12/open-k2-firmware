// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Content of the USB drive: which files exist, what the text files say, and the FAT16 volume
// built from them. Pure logic; the hardware seam is the Env of function pointers (live status,
// RAM log), the saved cables (set_cables) and the native library page (set_native_page).
//
//   README.TXT        what this is, firmware version and git hash, how to use it
//   STATUS.TXT        live readings at the time of the read (fixed 1024 bytes, space padded)
//   LOG.TXT           recent USB log lines (fixed 3072 bytes, space padded)
//   SAVEDLOG.TXT      the log snapshot saved to flash (only when one exists)
//   RIPPLE.CSV        placeholder for the last ripple capture
//   BLACKBOX/         CAP0.JSN..CAP2.JSN: the frozen PD black box captures as fixed-width JSON lines
//                     (only slots that hold a capture)
//   CABLES/           <short>.JSN per saved cable (JSON, docs/EMARKER_JSON.md), long name <name>.json
//                     (the long names are produced on read from Env::cable, not stored)
//   SUMMARY.md        (short name SUMMARY.MD) the same cards as short Markdown text (report_card::render_summary), kSummarySlot bytes each
//   CARDS.JSON        (short name CARDS.JSN) the charger report cards (k2/chargertest/report_card.hpp) as one JSON array, newest first, only when
//                     a card exists. Each card sits in a fixed kCardSlot byte slot (space padded) so the file is
//                     generated on read, one card at a time, without a buffer for all of them.
//   NATIVE/           only when the library page holds a native e-marker table:
//                     LIBRARY.BIN (the raw 2 KB page) and LIBRARY.TXT (header, checksum, records)
// The volume is read-only. Its layout is rebuilt by the set_* calls; a host that cached the old
// directory keeps showing it until it re-mounts.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "k2/pd/emlib.hpp"
#include "k2/pd/emrec.hpp"
#include "k2/chargertest/report_card_json.hpp"
#include "k2/chargertest/report_card_store.hpp"
#include "k2/drivers/vfat.hpp"

namespace k2::drive {

inline constexpr std::uint32_t kStatusSize = 1024;
inline constexpr std::uint32_t kLogSize = 3072;
inline constexpr std::uint32_t kCardSlot = report_card::kJsonMax;   // JSON, padding, then "," or " " and a newline
static_assert(kCardSlot <= kLogSize, "one card renders into the scratch buffer");
inline constexpr std::uint32_t kSummarySlot = report_card::kSummaryMax;   // SUMMARY.md: text, padding, newline
static_assert(kSummarySlot <= kLogSize);
inline constexpr unsigned kMaxCards = report_card::kMaxCards;

struct Env {
    std::string_view version;
    std::string_view git;
    // Render live text into `out`; return the length. Called on every read of the file.
    std::size_t (*status)(void* ctx, std::span<char> out) = nullptr;
    std::size_t (*log)(void* ctx, std::span<char> out) = nullptr;
    std::span<const char> saved_log;     // flash snapshot, empty when none
    // Raw black box records of capture `slot` (empty = none). Read on every access.
    std::span<const std::uint8_t> (*capture)(void* ctx, unsigned slot) = nullptr;
    unsigned capture_slots = 0;
    // Saved cable `seq` from the library; false when gone. Read on every rebuild and access.
    bool (*cable)(void* ctx, std::uint16_t seq, emlib::Entry& out) = nullptr;
    // Report card number `index` (0 = newest); false when gone. Read on every access.
    bool (*card)(void* ctx, unsigned index, report_card::Card& out) = nullptr;
    void* ctx = nullptr;
};

class Drive {
public:
    void init(const Env& env);
    // The saved cables' numbers (ascending); adds or removes CABLES/. Names come from Env::cable.
    void set_cables(std::span<const std::uint16_t> seqs);

    // The number of report cards (at most kMaxCards); adds or removes CARDS.JSON. Cards come from Env::card.
    void set_cards(unsigned count);

    // The flash log snapshot (empty = none); adds or removes SAVEDLOG.TXT. The span must stay valid.
    void set_saved_log(std::span<const char> text);

    // The raw library page when it is not ours (empty = none); adds or removes NATIVE/. The span
    // must stay valid (it points into flash).
    void set_native_page(std::span<const std::uint8_t> page);

    bool ok() const { return ok_; }
    std::span<const vfat::FileSpec> files() const { return {specs_.data(), n_}; }
    std::uint32_t sectors() const { return img_.sectors(); }
    std::uint32_t cluster_count() const { return img_.cluster_count(); }

    void read(std::uint32_t lba, std::span<std::uint8_t, vfat::kSectorSize> out);
    // Bytes of table entry `index` from `offset` (clipped to the file size; the rest of `out` is zero).
    void read_file(std::size_t index, std::uint32_t offset, std::span<std::uint8_t> out);
    // The long name of table entry `index` ("" when it has none), copied into `out`.
    std::size_t long_name(std::size_t index, std::span<char> out);

private:
    Env env_{};
    std::span<const std::uint8_t> native_page_{};
    bool ok_ = false;
    std::array<vfat::FileSpec, vfat::kMaxEntries> specs_{};
    std::size_t n_ = 0;
    std::array<std::uint16_t, emlib::kCapacity> cables_{};   // saved cable numbers; names are read on demand
    std::size_t n_cables_ = 0;
    unsigned n_cards_ = 0;
    std::size_t card_slot_ = ~std::size_t{0};   // card whose JSON currently sits in scratch_ (any other use of scratch_ clears it)
    vfat::Image img_;
    std::array<char, kLogSize> scratch_{};

    void rebuild();
    // Render content `id` into scratch_; returns the text length. `cap` is the buffer limit.
    std::size_t render(std::uint32_t id, std::span<char> out);
    std::size_t rendered_size(std::uint32_t id);
    static std::size_t lfn(void* self, std::uint32_t content, std::span<char> out);
    static void fill(void* self, std::uint32_t content, std::uint32_t offset, std::span<std::uint8_t> out);
    // The big-frame branches of render() and fill() live in their own (never inlined) functions: this code
    // runs inside the USB service call that the display driver makes while it waits, on top of whatever
    // page is drawing, so the dispatcher frames stay small and only one branch's locals are ever live.
    [[gnu::noinline]] std::size_t render_cable(std::uint32_t id, std::span<char> out);
    [[gnu::noinline]] std::size_t render_native(std::span<char> out);
    [[gnu::noinline]] void fill_capture(std::uint32_t content, std::uint32_t offset, std::span<std::uint8_t> out);
    [[gnu::noinline]] void fill_cards(std::uint32_t offset, std::span<std::uint8_t> out);
    [[gnu::noinline]] void fill_summary(std::uint32_t offset, std::span<std::uint8_t> out);
};

}  // namespace k2::drive
