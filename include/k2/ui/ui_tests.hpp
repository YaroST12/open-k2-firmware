// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Tests 5-page carousel (Bench > Tests):
//   0 Overview   4-row summary of Charger, Cable, Battery, Device (OK jumps to that tab)
//   1 Charger    active charger tests (Test charger, Request voltage, Fast charge, Report cards, Stress test)
//   2 Cable      active cable tests (Spoof as..., Resistance, Loop probe)
//   3 Battery    handled seamlessly by BatteryPage (as_tests_tab()) via Ui::switch_tests_tab()
//   4 Device     active device tests (Interrogate, Twins)
// Long -/+ flips between all 5 pages; Short Back on tabs 1/2/4 returns to Overview; Short Back on Overview pops to Bench.
#pragma once
#include <cstdint>
#include "k2/battery/battery.hpp"
#include "k2/fastcharge/fastcharge.hpp"
#include "k2/pd/pd.hpp"
#include "k2/pd/pd_ext.hpp"
#include "k2/pd/pd_vdm.hpp"
#include "k2/ui/battery_view.hpp"
#include "k2/ui/kit.hpp"
#include "k2/ui/ui_pages.hpp"
#include "k2/ui/ui_tools.hpp"
#include "k2/ui/ui_types.hpp"

namespace k2 {

namespace tests_view {

inline constexpr int kMetricChars = 7;   // right-aligned font 16 metric (e.g. "240W", "82.4%", "100%")
inline constexpr int kSubChars = 32;     // line 2 font 12 summary (<= 32 chars = 192 px)

// Pure formatting helpers for the 4 Overview rows (host-tested).
void format_charger(const pd::SourceCaps &caps, bool pd_link, fastcharge::Protocol fc, bool vbus,
                    StrBuf<kMetricChars + 1> &metric, StrBuf<kSubChars + 1> &line);
void format_cable(const pd::Emarker &em, StrBuf<kMetricChars + 1> &metric, StrBuf<kSubChars + 1> &line);
void format_battery(const battery::view::Shown &shown, std::string_view status,
                    StrBuf<kMetricChars + 1> &metric, StrBuf<kSubChars + 1> &line);
void format_device(bool id_cur, const pd::Identity &id, const pd::ExtInfo &ext, uint32_t rev,
                   const pd::Contract &ctr, bool pd_link,
                   StrBuf<kMetricChars + 1> &metric, StrBuf<kSubChars + 1> &line);

}  // namespace tests_view

class TestsPage final : public PageBase {
public:
    static constexpr int kOverviewRows = 4;
    static constexpr int16_t kRowY0 = 28;
    static constexpr int16_t kRowPitch = 44;
    static constexpr int16_t kOverviewEnd = kRowY0 + kOverviewRows * kRowPitch;   // 204 <= kFooterY (214)
    static constexpr int16_t kIconX = 8, kIconDy = 5;
    static constexpr int16_t kNameX = 28, kNameDy = 5;
    static constexpr int16_t kMetricX = 224 - 8 - tests_view::kMetricChars * 8;   // 160
    static constexpr int16_t kSubX = 28, kSubDy = 25;

    explicit TestsPage(UiContext &c, TestTab initial = TestTab::Overview);
    void draw_static() override;
    void update(uint32_t now_ms) override;
    bool on_event(Event e) override;
    Strip labels() const override;
    void invalidate() override;
    void enter() override;
    void leave() override;

    TestTab tab() const { return tab_; }
    void set_tab(TestTab t, uint32_t now_ms);
    int selected() const { return sel_; }
    ListState list_state() const;

private:
    enum class ItemKind : uint8_t { Act, DevicePlace };
    struct Item {
        bench::Act act;
        ItemKind kind;
    };
    struct Items { const Item *a; int n; };

    static Items tab_items(TestTab t);
    static void row_fn(void *self, int index, bool focused, kit::RowSpec &out);
    void apply_header();
    int count() const;
    void paint_overview();
    void paint_overview_row(int i, bool focus, std::string_view metric, std::string_view sub);
    void update_list(uint32_t now_ms);

    TestTab tab_ = TestTab::Overview;
    int sel_ = 0;
    SmallField<tests_view::kMetricChars> metric_[kOverviewRows];
    SmallField<tests_view::kSubChars> sub_[kOverviewRows];
    uint32_t row_sig_[kOverviewRows] = {};
    Elem foot_;
    StrBuf<12> tail_;
};

}  // namespace k2
