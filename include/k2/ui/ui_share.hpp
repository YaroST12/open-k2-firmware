// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The share view: one record (a saved cable, the source's PDOs, a report card) as the QR code(s) of the generic share frame
// (k2/ui/qrshare.hpp), a page of its own opened from a Share action (long OK) elsewhere. See docs/QR_SHARE.md.
//
// The opener builds its record straight into Ui::share_buffer() (k2/ui/qrshare_records.hpp), then Ui::open_share() names it
// (ShareRecord::begin) and pushes the page. begin() re-packs a card or cable record to schema v2 in place in the record buffer
// (k2/ui/qrshare_records.hpp pack_v2_in_place: the bit-packed link form, smaller than any LZ4 block, so those two kinds never
// LZ4-pack), and LZ4-packs the other records in place when that saves a quarter (qrshare::pack, frame flag kPackedFlag), so a
// record that needed two parts may need one. The page shows part i of n under the code as "title i/n"; with more than one part it steps
// through them by itself every kCycleMs (a phone that holds still catches each part in turn) until [+] / [-] pick one by hand. It
// repaints only when the part changes (or after a capture / resync). A record that fits one link code (a report card, a cable,
// the source's PDOs, the identity, boot info) opens as its link form (k2/ui/qrshare_link.hpp): one code holding a web address,
// so a phone camera opens the page that shows it; OK switches to the K2 data frame (what tools/k2_qr.py imports) and back. The
// card and the cable come out as the smaller schema v2 link (version '2'); every cable now fits one code, including the 8-row
// quirk one (510 B raw -> 189 B v2). The log is far too big for one link code and shows only the K2 data frames, as before. The
// link text is built at paint time in the scratch arena. The encoder workspace is not kept: each paint takes the scratch arena,
// encodes the part, draws it and gives the arena back, so the view costs only the record buffer in RAM.
//
// The code is shown FULL SCREEN and centred, the smallest QR version that holds the text, at the module size and quiet zone
// the look policy gives that version (qrview.hpp look_for: 3 px up to version 13, 2 px from 14 to 21, versions 22 to 24 only
// for the wide camera). The page owns the whole panel, button strip included (hides_strip). The caption goes in the margin
// under the code when that holds a line; a long OK shows the text view (what the code is, the wide-camera hint, the keys)
// and back. A code of version 18 or more says "Preparing code..." first and polls USB through the encode.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "k2/ui/qr.hpp"
#include "k2/ui/qrshare.hpp"
#include "k2/ui/qrshare_link.hpp"
#include "k2/ui/qrshare_records.hpp"
#include "k2/ui/qrview.hpp"
#include "k2/ui/ui_pages.hpp"

namespace k2 {

// The record a Share action hands to the view. The opener fills it BEFORE the page exists (the page is built on the open),
// so it lives in Ui, not in the page: the one resident piece of the share feature.
class ShareRecord {
public:
    // The record buffer: the largest record any opener builds (a saved cable with its quirk rows, two parts).
    static constexpr std::size_t kBuffer = qrshare::records::kMaxEmarker;
    static constexpr std::size_t kTitleMax = 24;

    std::span<std::uint8_t> buffer() { return rec_; }
    // The record in buffer() (`len` bytes) becomes the one shown; false when it has no part (empty or over kMaxRecord).
    // A card or cable record is re-packed to schema v2 in place (the bit-packed link form, always at most the v1 record);
    // any other record is LZ4-packed in place when that saves a quarter, as before.
    bool begin(qrshare::Type type, std::uint8_t schema, std::string_view title, std::size_t len);

    qrshare::Type type() const { return type_; }
    std::uint8_t schema() const { return schema_; }
    bool packed() const { return packed_; }            // record() is the LZ4 block of what the opener built
    bool v2() const { return v2_; }                    // record() is the schema v2 (bit-packed) form, not the v1 record
    std::span<const std::uint8_t> record() const { return {rec_.data(), len_}; }
    std::string_view title() const { return {title_buf_.data(), title_len_}; }
    unsigned parts() const { return parts_; }

private:
    std::size_t len_ = 0;
    std::size_t title_len_ = 0;
    unsigned parts_ = 0;
    std::array<std::uint8_t, kBuffer> rec_{};
    std::array<char, kTitleMax + 1> title_buf_{};
    qrshare::Type type_ = qrshare::Type::Emarker;
    std::uint8_t schema_ = 1;
    bool packed_ = false;
    bool v2_ = false;
};

class SharePage final : public PageBase {
public:
    SharePage(UiContext &c, ShareRecord &r) : PageBase(c, Page::Count, "Share"), r_(r) {}

    static constexpr std::size_t kBuffer = ShareRecord::kBuffer;
    static constexpr std::size_t kTitleMax = ShareRecord::kTitleMax;
    static constexpr uint32_t kCycleMs = 1500;
    static constexpr int kFrameMaxVersion = 12;   // the K2 data frames: 65 modules, three px each; two parts of a 510-byte record need 11
    static constexpr int kMinModulePx = 2;        // the least the look policy ever picks (versions 14 and up)
    // A link of more characters than this needs QR version 18 or more: encoding it takes long enough on the device that the page
    // says so first (the notice below), and keeps USB and the PD engine serviced through it (one poll per mask candidate).
    static constexpr int kBusyVersion = 18;
    static constexpr int kBusyChars = 644;                                   // version 17's byte capacity at level L
    static constexpr std::string_view kBusyText = "Preparing code...";
    static constexpr std::string_view kWideHint = "Use the wide camera";   // a code the main camera does not read (version 22 and up)
    static constexpr std::string_view hint_for(const qrview::Look& l) { return l.wide ? kWideHint : std::string_view{}; }
    // Under the link form's code: what the phone will show. At most 37 characters (a 224 px line of 6 px cells).
    static constexpr std::string_view link_caption(qrshare::Type t) {
        switch (t) {
            case qrshare::Type::Emarker: return "Scan: cable details";
            case qrshare::Type::PdCaps: return "Scan: charger offers";
            case qrshare::Type::Identity:
            case qrshare::Type::BootInfo: return "Scan: firmware info";
            default: return "Scan: full results";   // the report card
        }
    }

    void draw_static() override;
    void update(uint32_t now_ms) override;
    bool on_event(Event e) override;
    PanelLabels labels() const override;
    void invalidate() override { painted_ = false; }
    void enter() override;
    bool hides_strip() const override { return true; }   // the code covers the whole panel, button strip included

    qrshare::Type type() const { return r_.type(); }
    std::uint8_t schema() const { return r_.schema(); }
    bool packed() const { return r_.packed(); }
    bool v2() const { return r_.v2(); }
    std::span<const std::uint8_t> record() const { return r_.record(); }
    std::string_view title() const { return r_.title(); }
    unsigned parts() const { return r_.parts(); }
    unsigned index() const { return index_; }
    bool cycling() const { return cycling_; }
    bool link() const { return link_; }                // the code on show is the web link (a card's default), not the K2 data frame
    int modules() const { return n_; }                 // modules per side of the code last drawn, 0 when it did not encode
    int version() const { return n_ > 0 ? qrview::version_of(n_) : 0; }
    unsigned encodes() const { return encodes_; }      // codes built since the page was opened (one per paint)
    unsigned paints() const { return paints_; }        // codes drawn on the panel since the page was opened
    qrview::Area area() const { return {}; }           // the whole panel: the code is centred on it
    const qrview::Plan &plan() const { return plan_; } // where the code last drawn sits (px == 0: none)
    const qrview::Look &look() const { return look_; } // the policy row of its version
    bool info() const { return info_; }                // the text view (Long OK) is on
    bool caption_shown() const { return caption_shown_; }   // the line under the code is on the panel (the margin holds it)
    bool busy_shown() const { return busy_shown_; }    // the last paint said "Preparing code..." before the encode
    static constexpr int kTextH = qrview::kLineH + 2;

private:
public:
    // The record has a link form (a supported type whose record, v2-packed or packed or as built, fits one code): OK switches
    // link and data frame. The card and the cable always have one now (schema v2 shrinks them under the limit, the 8-row
    // quirk cable included); only the log never does.
    bool has_link_form() const { return parts() != 0 && qrshare::link::fits(type(), record().size()); }
private:
    void show(unsigned index, uint32_t now_ms);        // pick a part; the next update encodes and draws it
    void paint(bool clear);
    void paint_info();                                 // the text view: what the code is, the camera hint, the keys
    void wipe();                                       // the whole panel white (in an erase pass: the background)

    ShareRecord &r_;
    unsigned index_ = 0;
    bool cycling_ = false;
    bool link_ = false;          // showing the link form (qrshare_link.hpp); OK switches it with the K2 data frame
    bool info_ = false;          // the text view is on (Long OK), not the code
    uint32_t shown_ms_ = 0;
    int n_ = 0;
    bool painted_ = false;       // the code on the panel is the part and title now selected
    int drawn_n_ = 0;            // modules per side of the box the panel holds (a smaller one leaves the old edge behind)
    bool caption_shown_ = false, busy_shown_ = false;
    qrview::Plan plan_{};
    qrview::Look look_{};
    unsigned encodes_ = 0, paints_ = 0;
};

}  // namespace k2
