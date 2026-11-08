// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#include "k2/drivers/drive.hpp"
#include "k2/pd/pdtrace_view.hpp"

#include <algorithm>
#include <cstring>

#include "k2/pd/emjson.hpp"
#include "k2/pd/emname.hpp"
#include "k2/core/textbuf.hpp"

namespace k2::drive {

namespace {
enum : std::uint32_t { kReadme = 1, kStatus, kLog, kRipple, kSavedLog, kNativeBin, kNativeTxt, kCards, kSummary, kCap0 = 16, kCableBase = 0x10000 };   // kCap0..kCap0+7 are black box slots (below kCableBase)
constexpr std::string_view kCardsLongName = "CARDS.JSON", kSummaryLongName = "SUMMARY.md";
constexpr std::size_t kSummaryKey = std::size_t{1} << 20;   // card_slot_ value offset: a SUMMARY.md slot is cached apart from a CARDS.JSON one
}  // namespace

void Drive::init(const Env& env) {
    env_ = env;
    rebuild();
}

void Drive::set_native_page(std::span<const std::uint8_t> page) {
    native_page_ = page;
    rebuild();
}

void Drive::set_cables(std::span<const std::uint16_t> seqs) {
    n_cables_ = std::min(seqs.size(), cables_.size());
    std::copy_n(seqs.begin(), n_cables_, cables_.begin());
    rebuild();
}

void Drive::set_cards(unsigned count) {
    n_cards_ = std::min(count, kMaxCards);
    rebuild();
}

void Drive::set_saved_log(std::span<const char> text) {
    env_.saved_log = text;
    rebuild();
}

std::size_t Drive::render(std::uint32_t id, std::span<char> out) {
    if (out.data() == scratch_.data()) card_slot_ = ~std::size_t{0};
    TextBuf t(out);
    switch (id) {
    case kReadme:
        t.put("K2 drive\n========\n");
        t.put_raw("Firmware ");
        t.put_raw(env_.version);
        t.put_raw(" (");
        t.put_raw(env_.git);
        t.put_raw(")\n\n");
        t.put("This drive is read-only. It is computed by the firmware; nothing here is a real\n");
        t.put("file system in flash. Re-mount it to see changes.\n\n");
        t.put("STATUS.TXT    live readings at the time you read the file\n");
        t.put("LOG.TXT       recent USB log lines\n");
        t.put("SAVEDLOG.TXT  log saved to flash (store save-log)\n");
        t.put("RIPPLE.CSV    last ripple capture (not recorded yet)\n");
        t.put("BLACKBOX/     CAPn.JSN trigger captures of the PD black box, a meta line (identity), then one JSON line per record\n");
        t.put("CARDS.JSON    charger report cards, newest first (name them with k2_remote.py cards rename)\n");
        t.put("SUMMARY.md    the same cards as short text, one block per charger\n");
        t.put("CABLES/       saved cables, one JSON file each (name.json)\n");
        t.put("NATIVE/       the e-marker library already on the unit, when it has one\n\n");
        t.put("Save the cable on the port from the E-marker tool (OK), or with\n");
        t.put("k2_remote.py emarker save. List: emarker list. Rename: emarker rename.\n");
        return t.size();
    case kStatus:
        return env_.status ? env_.status(env_.ctx, out) : 0;
    case kLog:
        return env_.log ? env_.log(env_.ctx, out) : 0;
    case kRipple:
        t.put("# no capture recorded yet\nt_us,mV,mA\n");
        return t.size();
    case kSavedLog: {
        const std::size_t n = std::min(out.size(), env_.saved_log.size());
        std::memcpy(out.data(), env_.saved_log.data(), n);
        return n;
    }
    case kNativeBin: {
        const std::size_t n = std::min(out.size(), native_page_.size());
        std::memcpy(out.data(), native_page_.data(), n);
        return n;
    }
    case kNativeTxt:
        return render_native(out);
    default: break;
    }
    if (id >= kCableBase) return render_cable(id, out);
    return 0;
}

std::size_t Drive::render_native(std::span<char> out) {
    if (const std::size_t n = emrec::describe_native_page(native_page_, out)) return n;
    TextBuf t(out);
    t.put("The library page does not parse as an e-marker table (see LIBRARY.BIN).\n");
    return t.size();
}

std::size_t Drive::render_cable(std::uint32_t id, std::span<char> out) {
    emlib::Entry e;
    if (!env_.cable || !env_.cable(env_.ctx, static_cast<std::uint16_t>(id - kCableBase), e)) return 0;
    return emjson::render(e, out);
}

std::size_t Drive::rendered_size(std::uint32_t id) { return render(id, scratch_); }

void Drive::rebuild() {
    n_ = 0;
    auto add = [&](std::string_view name, int parent, bool dir, std::uint32_t size, std::uint32_t content) {
        specs_[n_++] = {vfat::name83(name), static_cast<std::int16_t>(parent), dir, 0, size, content};
        return static_cast<int>(n_ - 1);
    };
    add("README.TXT", -1, false, static_cast<std::uint32_t>(rendered_size(kReadme)), kReadme);
    add("STATUS.TXT", -1, false, kStatusSize, kStatus);
    add("LOG.TXT", -1, false, kLogSize, kLog);
    if (!env_.saved_log.empty()) add("SAVEDLOG.TXT", -1, false, static_cast<std::uint32_t>(env_.saved_log.size()), kSavedLog);
    add("RIPPLE.CSV", -1, false, static_cast<std::uint32_t>(rendered_size(kRipple)), kRipple);
    const int bb = add("BLACKBOX", -1, true, 0, 0);
    for (unsigned c = 0; c < env_.capture_slots && c < 8 && env_.capture; c++) {
        if (env_.capture(env_.ctx, c).empty()) continue;
        const char nm[] = {'C', 'A', 'P', static_cast<char>('0' + c), '.', 'J', 'S', 'N'};
        add(std::string_view(nm, sizeof nm), bb, false, static_cast<std::uint32_t>(pdtrace::view::kJsonSize), kCap0 + c);
    }
    if (n_cards_) {   // 8.3 holds three extension letters: CARDS.JSN, with the long name CARDS.JSON
        const int ci = add("CARDS.JSN", -1, false, 2 + n_cards_ * kCardSlot + 2, kCards);
        specs_[ci].lfn_len = static_cast<std::uint8_t>(kCardsLongName.size());
        const int si = add("SUMMARY.MD", -1, false, n_cards_ * kSummarySlot, kSummary);
        specs_[si].lfn_len = static_cast<std::uint8_t>(kSummaryLongName.size());
    }
    if (n_cables_) {
        const int cd = add("CABLES", -1, true, 0, 0);
        // The short names already handed out are the specs of this directory: no array of them on the stack.
        struct Taken { const vfat::FileSpec* specs; const std::size_t* n; int dir; } taken{specs_.data(), &n_, cd};
        for (std::size_t i = 0; i < n_cables_ && n_ < vfat::kMaxEntries; i++) {
            emlib::Entry e;
            if (!env_.cable || !env_.cable(env_.ctx, cables_[i], e)) continue;
            const std::string_view nm(e.name.data());
            const emname::Short83 sh = emname::short_name(nm, [](const void* c, const emname::Short83& name) {
                const auto& t = *static_cast<const Taken*>(c);
                for (std::size_t j = 0; j < *t.n; j++)
                    if (t.specs[j].parent == t.dir && t.specs[j].name == name) return true;
                return false;
            }, &taken);
            specs_[n_++] = {sh, static_cast<std::int16_t>(cd), false, static_cast<std::uint8_t>(nm.size() + 5),
                            static_cast<std::uint32_t>(rendered_size(kCableBase + cables_[i])), kCableBase + cables_[i]};
        }
    }
    if (!native_page_.empty()) {
        const int fd = add("NATIVE", -1, true, 0, 0);
        add("LIBRARY.BIN", fd, false, static_cast<std::uint32_t>(native_page_.size()), kNativeBin);
        add("LIBRARY.TXT", fd, false, static_cast<std::uint32_t>(rendered_size(kNativeTxt)), kNativeTxt);
    }
    ok_ = img_.build(std::span<const vfat::FileSpec>(specs_.data(), n_));
}

std::size_t Drive::lfn(void* self, std::uint32_t content, std::span<char> out) {
    auto* d = static_cast<Drive*>(self);
    if (content == kCards) {
        TextBuf t(out);
        t.put_raw(kCardsLongName);
        return t.size();
    }
    if (content == kSummary) {
        TextBuf t(out);
        t.put_raw(kSummaryLongName);
        return t.size();
    }
    emlib::Entry e;
    if (content < kCableBase || !d->env_.cable ||
        !d->env_.cable(d->env_.ctx, static_cast<std::uint16_t>(content - kCableBase), e))
        return 0;
    TextBuf t(out);
    t.put_raw(std::string_view(e.name.data()));
    t.put_raw(".json");
    return t.size();
}

std::size_t Drive::long_name(std::size_t index, std::span<char> out) {
    if (index >= n_ || !specs_[index].lfn_len) return 0;
    return lfn(this, specs_[index].content, out);
}

void Drive::fill_capture(std::uint32_t content, std::uint32_t offset, std::span<std::uint8_t> out) {
    const auto rec = env_.capture ? env_.capture(env_.ctx, content - kCap0) : std::span<const std::uint8_t>{};
    std::array<char, pdtrace::view::kJsonLine> line;
    std::size_t row = ~std::size_t{0};
    for (std::size_t i = 0; i < out.size(); i++) {
        const std::size_t pos = offset + i, r = pos / pdtrace::view::kJsonLine;
        if (r != row) { pdtrace::view::json_line(rec, r, line); row = r; }
        out[i] = static_cast<std::uint8_t>(line[pos % pdtrace::view::kJsonLine]);
    }
}

void Drive::fill_cards(std::uint32_t offset, std::span<std::uint8_t> out) {
    for (std::size_t i = 0; i < out.size(); i++) {
        const std::size_t pos = offset + i;
        if (pos < 2) { out[i] = static_cast<std::uint8_t>("[\n"[pos]); continue; }
        const std::size_t at = pos - 2, slot = at / kCardSlot, col = at % kCardSlot;
        if (slot >= n_cards_) {
            const std::size_t tail = at - n_cards_ * kCardSlot;
            out[i] = static_cast<std::uint8_t>("]\n"[tail < 2 ? tail : 1]);
            continue;
        }
        if (slot != card_slot_) {   // render this card once for the bytes of the same slot
            report_card::Card c;
            std::size_t n = 0;
            if (env_.card && env_.card(env_.ctx, static_cast<unsigned>(slot), c))
                n = report_card::render_json(c, std::span<char>(scratch_.data(), kCardSlot - 2));
            std::memset(&scratch_[n], ' ', kCardSlot - 2 - n);
            card_slot_ = slot;
        }
        out[i] = col < kCardSlot - 2 ? static_cast<std::uint8_t>(scratch_[col])
                                     : static_cast<std::uint8_t>(col == kCardSlot - 2 ? (slot + 1 < n_cards_ ? ',' : ' ') : '\n');
    }
}

void Drive::fill_summary(std::uint32_t offset, std::span<std::uint8_t> out) {   // kSummarySlot bytes per card: the text, space padding, a final newline
    for (std::size_t i = 0; i < out.size(); i++) {
        const std::size_t pos = offset + i, slot = pos / kSummarySlot, col = pos % kSummarySlot;
        if (slot >= n_cards_) { out[i] = 0; continue; }
        if (card_slot_ != kSummaryKey + slot) {
            report_card::Card c;
            std::size_t n = 0;
            if (env_.card && env_.card(env_.ctx, static_cast<unsigned>(slot), c))
                n = report_card::render_summary(c, std::span<char>(scratch_.data(), kSummarySlot - 1));
            std::memset(&scratch_[n], ' ', kSummarySlot - n);
            card_slot_ = kSummaryKey + slot;
        }
        out[i] = col < kSummarySlot - 1 ? static_cast<std::uint8_t>(scratch_[col]) : static_cast<std::uint8_t>('\n');
    }
}

void Drive::fill(void* self, std::uint32_t content, std::uint32_t offset, std::span<std::uint8_t> out) {
    auto* d = static_cast<Drive*>(self);
    if (content >= kCap0 && content < kCap0 + 8) { d->fill_capture(content, offset, out); return; }
    if (content == kCards) { d->fill_cards(offset, out); return; }
    if (content == kSummary) { d->fill_summary(offset, out); return; }
    d->card_slot_ = ~std::size_t{0};
    std::size_t cap = kLogSize;
    if (content == kStatus) cap = kStatusSize;
    else if (content != kLog) cap = kLogSize;     // other files are shorter than the scratch
    std::size_t len = d->render(content, std::span<char>(d->scratch_.data(), cap));
    len = std::min(len, cap);
    if (content == kStatus || content == kLog) {   // fixed size: pad with spaces, end with a newline
        if (len < cap) std::memset(&d->scratch_[len], ' ', cap - len);
        d->scratch_[cap - 1] = '\n';
        len = cap;
    }
    for (std::size_t i = 0; i < out.size(); i++)
        out[i] = offset + i < len ? static_cast<std::uint8_t>(d->scratch_[offset + i]) : 0;
}

void Drive::read(std::uint32_t lba, std::span<std::uint8_t, vfat::kSectorSize> out) {
    img_.read(lba, out, vfat::Provider{&Drive::fill, this, &Drive::lfn});
}

void Drive::read_file(std::size_t index, std::uint32_t offset, std::span<std::uint8_t> out) {
    std::memset(out.data(), 0, out.size());
    if (index >= n_ || specs_[index].dir || offset >= specs_[index].size) return;
    const std::size_t n = std::min<std::size_t>(out.size(), specs_[index].size - offset);
    fill(this, specs_[index].content, offset, out.first(n));
}

}  // namespace k2::drive
