// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The Battery place: see k2/ui/ui_battery.hpp.
#include "k2/ui/ui_battery.hpp"

#include <algorithm>
#include <string_view>

#include "k2/battery/battery_hw.hpp"
#include "k2/core/strpack.hpp"
#include "k2/ui/qrshare.hpp"
#include "k2/ui/trunc.hpp"
#include "k2/ui/ui.hpp"
#include "k2/ui/ui_style.hpp"
#include "k2/ui/ui_widgets.hpp"

namespace k2 {

namespace {
namespace bt = battery;
namespace view = battery::view;
using view::Act;

constexpr uint32_t kNoteMs = 2500;   // a refusal holds the footer this long

// The focused rows' notes (font 12, <= kNoteMaxPx).
constexpr const char *kNotes[] = {
    K2_P("charger in, bank on output"),        // StartCharge
    K2_P("bank on output, load on input"),     // StartDischarge, same port
    K2_P("bank on input, load on output"),     // StartDischarge, moved
    K2_P("charge, then discharge"),            // StartRoundTrip
    K2_P("ends this phase now"),               // EndPhase
    K2_P("the discharge starts now"),          // Next
    K2_P("ends the run here"),                 // Finish
    K2_P("the bank stays on the output"),      // Wiring, same port
    K2_P("the bank moves to the input"),       // Wiring, moved
    K2_P("phase ends under this current"),     // EndMa
    K2_P("for this long"),                     // EndS
    K2_P("the newest runs kept on flash"),     // Runs
    K2_P("the run as a QR code"),              // Share
    K2_P("Wh out / Wh in, so far"),            // Eff, running
    K2_P("Wh out / Wh in"),                    // Eff, final
    K2_P("measures its last phase again"),     // Continue
};
static_assert(all_fit_p(kNotes, kLabelAdv, kit::kNoteMaxPx), "a Battery note is wider than the note box");

// Rows without a tail run to the tail's edge; a row with one leaves the gap and the widest tail ("123.4Wh"; Wiring's
// "Same port" sits beside a six-letter name).
constexpr const char *kBareRows[] = {K2_P("Start round trip"), K2_P("Start discharge"), K2_P("End discharge"), K2_P("Finish run"), K2_P("Share as QR"),
                                     K2_P("No saved runs"), K2_P("Continue run")};
static_assert(all_fit_p(kBareRows, kSmallAdvPx, kit::kTailX - kit::kTextX), "a Battery row runs into the tail");
constexpr const char *kTailRows[] = {K2_P("Efficiency"), K2_P("End below"), K2_P("End after"), K2_P("#999 Dischg"), K2_P("#999 Round")};
static_assert(all_fit_p(kTailRows, kSmallAdvPx, kit::kTailX - kit::kTextX - kit::kTailGap - 7 * static_cast<int>(kSmallAdvPx)),
              "a Battery row and its tail do not fit one row");

const char *mode_name(bt::Mode m) {
    switch (m) {
    case bt::Mode::Charge: return K2_P("Charge");
    case bt::Mode::Discharge: return K2_P("Dischg");
    case bt::Mode::RoundTrip: break;
    }
    return K2_P("Round");
}

bool live_any() { return bt::hw::service().rec().state() != bt::State::Idle; }

const char *armed_text(Act a, const view::Shown &s) {
    if (a == Act::Finish) return K2_P("Press again to end run");
    if (a == Act::Continue) return K2_P("Press again to continue");
    return s.kind == bt::Kind::Charge ? K2_P("Press again: end charge") : K2_P("Press again: end dischg");
}
}  // namespace

BatteryPage::BatteryPage(UiContext &c) : PageBase(c, Page::Count, rail::kBattery, Icon::Battery) {
    bar_icon_ = true;
}

void BatteryPage::enter() {
    view_ = View::Main;
    build();
    sel_ = std::clamp(static_cast<int>(ctx_.keep.place_sel[static_cast<int>(bench::Place::Battery)]), 0, count() - 1);
}

void BatteryPage::leave() {
    if (view_ == View::Main) ctx_.keep.place_sel[static_cast<int>(bench::Place::Battery)] = static_cast<uint8_t>(sel_);
}

void BatteryPage::build() {
    if (view_ == View::Stored) {
        shown_ = view::shown(rec_, true);
        rows_ = view::rows(shown_, true, runs_sel_ == 0 && !bt::hw::service().rec().active());   // only the newest continues
        return;
    }
    bt::hw::service().rec().to_record(rec_);
    shown_ = view::shown(rec_, live_any());
    rows_ = view::rows(shown_, false);
}

int BatteryPage::count() const {
    if (view_ == View::Runs) return n_runs_ ? n_runs_ : 1;
    return rows_.n;
}

Act BatteryPage::act() const {
    if (view_ == View::Runs) return Act::Runs;
    return sel_ >= 0 && sel_ < rows_.n ? rows_.a[sel_] : Act::In;
}

bool BatteryPage::armed() const {
    return armed_until_ && static_cast<int32_t>(armed_until_ - ctx_.now) > 0 && act() == armed_act_ && view_ != View::Runs;
}

void BatteryPage::load_runs() {
    n_runs_ = 0;
    fs::Fs *f = bt::hw::service().volume();
    if (!f) return;
    uint16_t ids[bt::store::kKeep];
    const int n = bt::store::list(*f, ids, bt::store::kKeep);
    for (int i = 0; i < n; i++) {
        bt::Record r;
        if (!bt::store::load(*f, ids[i], r)) continue;
        runs_[n_runs_++] = view::shown(r, true);
    }
    pair_ = view::pair(runs_, n_runs_);
}

void BatteryPage::set_view(View v, int sel) {
    view_ = v;
    armed_until_ = 0;
    bar_valid_ = false;
    build();
    sel_ = std::clamp(sel, 0, count() - 1);
    ctx_.rows.restart();
    ctx_.rows.invalidate();
}

void BatteryPage::draw_static() {
    title_bar();
    ctx_.rows.init(ctx_.content, kit::kPlaceListY, &BatteryPage::row_fn, this);
    build();
    ctx_.rows.set_count(count());
    ctx_.rows.select(sel_);
    kit::fill_list_gap(ctx_.content, kit::kPlaceListY);
    invalidate();
}

void BatteryPage::invalidate() {
    foot_.invalidate();
    full_ = true;
    ctx_.rows.invalidate();
}

// ---- rows -----------------------------------------------------------------------------------------------------------------

void BatteryPage::row_fn(void *self, int index, bool focused, kit::RowSpec &out) {
    auto *p = static_cast<BatteryPage *>(self);
    out = kit::RowSpec{};
    p->tail_.clear();
    p->note_row_.clear();
    p->text_.clear();
    if (p->view_ == View::Runs) p->run_row(index, focused, out);
    else if (index >= 0 && index < p->rows_.n) p->action_row(p->rows_.a[index], focused, out);
}

void BatteryPage::action_row(Act a, bool focused, kit::RowSpec &out) {
    const view::Shown &s = shown_;
    out.text = view::act_name(a, s);
    out.tail_color = kInk2;
    const bool same = bt::hw::service().wiring() == bt::Wiring::SamePort;
    const bt::Config &cfg = s.any && s.state != bt::RunState::Done ? s.cfg : bt::hw::service().config();
    const char *note = nullptr;
    switch (a) {
    case Act::In:
    case Act::Out: {
        const bool in = a == Act::In;
        tail_.put(view::wh(in ? s.sum.uwh_in : s.sum.uwh_out).view());
        out.tail_color = kEnergy;
        note_row_.put(view::mah(in ? s.sum.uah_in : s.sum.uah_out).view()).put(in ? K2_P(" into the bank") : K2_P(" out of the bank"));
        break;
    }
    case Act::Time:
        tail_.put(view::hms(s.secs).view());
        if (s.gaps) note_row_.put_u32(s.gaps).put(s.gaps == 1 ? K2_P(" gap not counted") : K2_P(" gaps not counted"));
        else if (s.phases == 2) note_row_.put(view::hms(s.secs - s.phase_secs).view()).put(K2_P(" in, ")).put(view::hms(s.phase_secs).view()).put(K2_P(" out"));
        break;
    case Act::Eff:
        if (s.eff != bt::kNoRatio) tail_.put(view::pct(s.eff).view());
        else tail_.put("none");
        out.tail_color = s.eff_final ? kInk : kInk2;
        note = s.eff_final ? kNotes[14] : kNotes[13];
        break;
    case Act::StartCharge: note = kNotes[0]; break;
    case Act::StartDischarge: note = same ? kNotes[1] : kNotes[2]; break;
    case Act::StartRoundTrip: note = kNotes[3]; break;
    case Act::EndPhase: note = kNotes[4]; break;
    case Act::Next: note = kNotes[5]; break;
    case Act::Finish: note = kNotes[6]; break;
    case Act::Wiring:
        tail_.put(same ? K2_P("Same port") : K2_P("Moved"));
        note = same ? kNotes[7] : kNotes[8];
        break;
    case Act::EndMa:
        tail_.put_u32(cfg.end_ma).put("mA");
        note = kNotes[9];
        break;
    case Act::EndS:
        if (cfg.end_s >= 120 && cfg.end_s % 60 == 0) tail_.put_u32(cfg.end_s / 60u).put_char('m');
        else tail_.put_u32(cfg.end_s).put_char('s');
        note = kNotes[10];
        break;
    case Act::Runs:
        out.chevron = true;
        note = kNotes[11];
        break;
    case Act::Share:
        note = kNotes[12];
        break;
    case Act::Continue:
        note = kNotes[15];
        break;
    }
    if (armed_until_ && a == armed_act_ && armed()) out.text_color = kCaution;
    out.tail = tail_.view();
    if (focused) out.note = note ? std::string_view(note) : note_row_.view();
}

void BatteryPage::run_row(int i, bool focused, kit::RowSpec &out) {
    if (n_runs_ == 0) {
        out.text = K2_P("No saved runs");
        out.text_color = kDim;
        return;
    }
    if (i < 0 || i >= n_runs_) return;
    const view::Shown &r = runs_[i];
    text_.put_char('#').put_u32(r.id).put_char(' ').put(mode_name(r.mode));
    out.text = text_.view();
    out.chevron = false;
    if (r.eff_final) tail_.put(view::pct(r.eff).view());
    else if (r.lost) tail_.put("cut");
    else tail_.put(view::wh(r.mode == bt::Mode::Discharge ? r.sum.uwh_out : r.sum.uwh_in).view());
    out.tail = tail_.view();
    out.tail_color = r.eff_final ? kInk : kInk2;
    if (focused) {   // both energies of a round trip; else the other unit of the tail's (the Wh of a cut one), the time
        const bool dis = r.mode == bt::Mode::Discharge;
        if (r.mode == bt::Mode::RoundTrip) {
            note_row_.put(view::wh(r.sum.uwh_in).view()).put(K2_P(" in, ")).put(view::wh(r.sum.uwh_out).view()).put(K2_P(" out"));
        } else {
            note_row_.put(r.lost ? view::wh(dis ? r.sum.uwh_out : r.sum.uwh_in).view() : view::mah(dis ? r.sum.uah_out : r.sum.uah_in).view());
            note_row_.put(dis ? K2_P(" out, ") : K2_P(" in, ")).put(view::hm(r.secs).view());
        }
        out.note = note_row_.view();
    }
}

// ---- paint ----------------------------------------------------------------------------------------------------------------

void BatteryPage::update(uint32_t now_ms) {
    title_badge(false);
    build();
    if (armed_until_ && static_cast<int32_t>(armed_until_ - now_ms) <= 0) armed_until_ = 0;
    if (note_until_ && static_cast<int32_t>(note_until_ - now_ms) <= 0) note_until_ = 0;
    // the identity block
    kit::Ident id;
    if (view_ == View::Runs && pair_.eff != bt::kNoRatio) {   // a charge and a discharge recorded apart: their efficiency
        id.head.put(view::pct(pair_.eff).view());
        StrBuf<12> ids;
        ids.put_char('#').put_u32(pair_.lo).put_char('-').put_u32(pair_.hi);
        id.add(ids.view(), pair_.eff <= 1000 ? kit::Ident::Ok : kit::Ident::Outline);   // over 100 %: a part measured
        id.line.put(view::wh(pair_.uwh_out).view()).put(K2_P(" out / ")).put(view::wh(pair_.uwh_in).view()).put(K2_P(" in"));
    } else if (view_ == View::Runs) {
        id.head.put(K2_P("Saved runs"));
        id.dim = n_runs_ == 0;
        id.line.put(n_runs_ ? K2_P("Newest first, OK opens one") : K2_P("A run is saved when it ends"));
    } else {
        view::ident(shown_, id);
    }
    kit::paint_identity(ctx_.content, id, full_, id_sig_);
    full_ = false;
    // the footer: a refusal, an armed hand end, else the state
    StrBuf<28> f;
    FooterTone tone = FooterTone::Info;
    const bt::Service &sv = bt::hw::service();
    if (note_until_) {
        f.put(note_.view());
        tone = note_tone_;
    } else if (armed()) {
        f.put(armed_text(armed_act_, shown_));
        tone = FooterTone::Caution;
    } else if (view_ == View::Runs) {
        f.put_u32(static_cast<uint32_t>(n_runs_)).put(" of ").put_u32(bt::store::kKeep).put(K2_P(" kept on flash"));
    } else if (view_ == View::Stored) {
        tone = view::footer(shown_, true, false, false, false, f);
    } else {
        tone = view::footer(shown_, bt::hw::monitor_online(), sv.write_failed(), sv.rec().dirty() && !sv.discarded(), sv.discarded(), f);
    }
    const bool pill = tone != FooterTone::Info && !f.view().empty();
    // A pill floats over the rows and a new one never covers the old one's ends: repaint the rows under it first.
    if (!pill ? !pill_.view().empty() : pill_.view() != f.view()) {
        if (!pill_.view().empty()) {
            const int16_t gap = kit::list_end(kit::kPlaceListY);   // under the list nothing else paints
            gfx_fill(ctx_.content, 0, gap, ctx_.content.w, static_cast<int16_t>(kFooterY - gap), kBg);
            ctx_.rows.invalidate();
        }
        pill_.clear();
        if (pill) pill_.put(f.view());
    }
    sel_ = std::clamp(sel_, 0, count() - 1);
    ctx_.rows.set_count(count());
    ctx_.rows.select(sel_);
    ctx_.rows.draw(now_ms, ctx_.budget);
    if (pill) draw_toast(ctx_.content, tone, f.view());
}

// ---- events ---------------------------------------------------------------------------------------------------------------

void BatteryPage::refuse(const char *text, FooterTone tone) {
    note_.clear();
    note_.put(text);
    note_tone_ = tone;
    note_until_ = ctx_.now + kNoteMs;
    if (!note_until_) note_until_ = 1;
}

void BatteryPage::ok() {
    bt::Service &sv = bt::hw::service();
    if (view_ == View::Runs) {
        if (!n_runs_) return;
        fs::Fs *f = sv.volume();
        runs_sel_ = sel_;
        if (!f || !bt::store::load(*f, runs_[sel_].id, rec_)) { refuse(K2_P("Run not readable"), FooterTone::Error); return; }
        set_view(View::Stored, 0);
        return;
    }
    const Act a = act();
    switch (a) {
    case Act::StartCharge:
    case Act::StartDischarge:
    case Act::StartRoundTrip: {
        const bt::Mode m = a == Act::StartCharge ? bt::Mode::Charge : a == Act::StartDischarge ? bt::Mode::Discharge : bt::Mode::RoundTrip;
        switch (sv.start(m, sv.wiring(), bt::hw::monitor_online())) {
        case bt::Recorder::Start::Ok:
            build();
            sel_ = rows_.facts;   // the first thing to do now: End charge
            ctx_.rows.invalidate();
            break;
        case bt::Recorder::Start::NoMonitor: refuse(K2_P("No power monitor"), FooterTone::Error); break;
        case bt::Recorder::Start::Busy: refuse(K2_P("Last run not saved yet"), FooterTone::Caution); break;
        case bt::Recorder::Start::BadArg: refuse(K2_P("Settings out of range"), FooterTone::Error); break;
        }
        return;
    }
    case Act::EndPhase:
    case Act::Finish:
    case Act::Continue:
        if (!armed() || armed_act_ != a) {   // the first OK arms, the second within the window acts
            armed_act_ = a;
            armed_until_ = ctx_.now + Ui::kConfirmWindowMs;
            if (!armed_until_) armed_until_ = 1;
            return;
        }
        armed_until_ = 0;
        if (a == Act::EndPhase) sv.rec().end_phase();
        else if (a == Act::Finish) sv.rec().finish();
        else {
            static constexpr const char *kCont[] = {K2_P("Run continues"), K2_P("A run is measuring"), K2_P("No power monitor"),
                                                    K2_P("No run to continue"), K2_P("Only the newest run")};   // Service::Cont
            const auto c = sv.continue_run(shown_.id, bt::hw::monitor_online());
            refuse(kCont[static_cast<int>(c)], c == bt::Service::Cont::Ok ? FooterTone::Ok : FooterTone::Caution);
            if (c != bt::Service::Cont::Ok) return;
            set_view(View::Main, 0);
        }
        build();
        sel_ = rows_.facts;
        ctx_.rows.invalidate();
        return;
    case Act::Next:
        sv.rec().next();
        build();
        sel_ = rows_.facts;
        ctx_.rows.invalidate();
        return;
    case Act::Wiring:
        sv.wiring() = sv.wiring() == bt::Wiring::SamePort ? bt::Wiring::Moved : bt::Wiring::SamePort;
        return;
    case Act::EndMa: {
        bt::Config c = sv.config();
        c.end_ma = view::next_step(view::kEndMaSteps, static_cast<int>(std::size(view::kEndMaSteps)), c.end_ma);
        sv.set_config(c);
        return;
    }
    case Act::EndS: {
        bt::Config c = sv.config();
        c.end_s = view::next_step(view::kEndSSteps, static_cast<int>(std::size(view::kEndSSteps)), c.end_s);
        sv.set_config(c);
        return;
    }
    case Act::Runs:
        load_runs();
        set_view(View::Runs, 0);
        return;
    case Act::Share: {
        if (view_ != View::Stored) bt::hw::service().rec().to_record(rec_);
        const std::size_t len = bt::encode(rec_, ctx_.ui->share_buffer());
        ctx_.ui->open_share(qrshare::Type::Battery, qrshare::kSchemaBattery, "Battery run", len, ctx_.now);   // k2-plain: share title is memcpy'd
        return;   // the share view is on top now
    }
    case Act::In:
    case Act::Out:
    case Act::Time:
    case Act::Eff: return;
    }
}

bool BatteryPage::on_event(Event e) {
    if (e.press != Press::Short) return false;
    if (e.button == Button::Plus || e.button == Button::Minus) {
        sel_ = std::clamp(sel_ + (e.button == Button::Plus ? 1 : -1), 0, count() - 1);
        armed_until_ = 0;   // moving off disarms a hand end
        update(ctx_.now);   // the Ui re-reads labels() after the event: the OK verb follows the row
        return true;
    }
    if (e.button == Button::Ok) {
        ok();
        if (ctx_.ui->menu() == Menu::Battery) update(ctx_.now);   // a share view on top paints itself
        return true;
    }
    if (e.button == Button::Back) {
        if (view_ == View::Stored) { set_view(View::Runs, runs_sel_); update(ctx_.now); return true; }
        if (view_ == View::Runs) {   // back on the Saved runs row
            set_view(View::Main, 0);
            for (int i = 0; i < rows_.n; i++)
                if (rows_.a[i] == Act::Runs) sel_ = i;
            update(ctx_.now);
            return true;
        }
        armed_until_ = 0;
    }
    return false;   // BACK in the Main view: the Ui pops the place
}

Strip BatteryPage::labels() const {
    Strip l = list_strip(base_labels());
    if (view_ == View::Runs) l[Button::Ok] = n_runs_ ? sl::View : sl::None;
    else if (armed()) l[Button::Ok] = sl::Confirm;
    else l[Button::Ok] = view::act_verb(act());
    return l;
}

}  // namespace k2
