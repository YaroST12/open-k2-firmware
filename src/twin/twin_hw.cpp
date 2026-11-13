// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#include "k2/twin/twin_hw.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <new>

#include "k2/core/log.hpp"
#include "k2/core/loop_guard.hpp"
#include "k2/core/storage.hpp"
#include "k2/drivers/fusb302.hpp"
#include "k2/pd/emk_spoof.hpp"
#include "k2/pd/pd_source.hpp"
#include "k2/pd/power_source.hpp"
#include "k2/twin/usbdata.hpp"

namespace k2::twin_hw {
namespace {

static_assert(twin::kMaxSinkPdos <= 7, "pd::Sink answers Get_Sink_Cap with at most seven objects");

// The words the sink engine answers Get_Sink_Cap with while a twin is impersonated: the engine keeps the pointer, not a copy.
std::array<std::uint32_t, twin::kMaxSinkPdos> g_words{};

class HwEnv final : public twin::live::Env {
public:
    bool load(std::uint8_t seq, twin::Twin& out) override { return storage::twin_get(seq, out); }

    void impersonate(const twin::Twin* t) override {
        if (t == nullptr || t->num_pdos == 0) {   // a twin made from a sniffed Request has no sink PDOs: the K2's own go out
            fusb302::sink_cap_override(nullptr, 0);
            return;
        }
        g_words = t->pdos;
        fusb302::sink_cap_override(g_words.data(), t->num_pdos);
    }

    void detach() override { fusb302::unit_detach(); }
    void attach() override { fusb302::unit_attach(false); }
    void hold() override { fusb302::unit_hold(false); }
    pd::Err request(pd::Choice c) override { return fusb302::sink_request(c, true); }
    void release() override { fusb302::sink_release(); }

    void record(std::uint8_t seq, const twin::live::Slot& s) override {
        K2LOG("[TWIN] live #%u %s %u mV skip=%u\n", unsigned(seq), twin::live::name(s.verdict), unsigned(s.measured_mv),
              unsigned(s.skip));
    }

    // A silent charger is woken with the K2's own Rd (rd_for_run): the START path then takes its 5 V contract on the first caps,
    // the run owns the line like a charger-test unit. False when a sink is inline.
    bool wake() override {
        if (!fusb302::rd_for_run(true)) return false;
        woke_ = true;
        K2LOG("[TWIN] wake: Rd presented to a silent charger\n");
        return true;
    }
    void unwake() override {
        if (!woke_) return;
        woke_ = false;
        fusb302::rd_for_run(false);
        K2LOG("[TWIN] wake: Rd released\n");
    }

    void finished(twin::live::Reason why) override {
        fusb302::set_run_owner(false);   // the sleeping-port wake may act again
        K2LOG("[TWIN] live end %s\n", twin::live::name(why));
        if (!woke_) return;   // no Rd of ours (never woken, or unwake() gave it back already)
        woke_ = false;
        if (keep_wake_ && (why == twin::live::Reason::Done || why == twin::live::Reason::Cancelled)) {
            held_ = true;   // the page keeps the list on screen until wake_release()
            return;
        }
        fusb302::rd_for_run(false);
        K2LOG("[TWIN] wake: Rd released\n");
    }

    void keep_wake(bool keep) { keep_wake_ = keep; }
    bool held() const { return held_; }
    void drop_held() { held_ = false; }

private:
    bool woke_ = false;        // this run presented the Rd and has not given it back
    bool keep_wake_ = false;   // the starter (the page) takes the Rd over when the run ends
    bool held_ = false;        // the run ended and left the Rd to the page
};

HwEnv g_env;
alignas(twin::live::Runner) std::uint8_t g_run_raw[sizeof(twin::live::Runner)];
bool g_run_inited;

twin::live::Runner& run_obj() {
    if (!g_run_inited) {
        new (g_run_raw) twin::live::Runner();
        g_run_inited = true;
    }
    return *std::launder(reinterpret_cast<twin::live::Runner*>(g_run_raw));
}

twin::live::Input make_input(const LiveCtx& c) {
    twin::live::Input in;
    in.now = c.now;
    in.vbus_mv = c.vbus_fast_mv;
    in.caps = &fusb302::caps();   // the real list, never the debug fake one
    in.caps_seq = fusb302::caps_count();
    in.sink = fusb302::sink_status();
    in.facts = facts_now();
    in.cap_mv = c.cap_mv;
    in.self_powered = fusb302::self_powered() || power_source::bus_powered();   // as the charger test judges it
    in.busy = c.busy || loopguard::locked();   // the HID-cable loop 5 V lock: refused to start (Busy), a run is cancelled to 5 V
    in.inline_link = c.inline_link;
    in.wakeable = !pd::is_fake() && twin::live::may_wake(fusb302::wait_kind(), c.vbus_fast_mv, c.inline_link);
    return in;
}

// A name for the library: empty = auto. Too long or with bytes the library rejects = left for it to refuse (Err::Invalid).
void put_name(twin::Twin& t, std::string_view name) {
    if (name.empty()) return;
    t.name = {};
    std::memcpy(t.name.data(), name.data(), std::min<std::size_t>(name.size(), twin::kNameMax));
}

Learned save_learned(Learned out, std::string_view name, bool save) {
    put_name(out.twin, name);
    if (!save) {
        out.how = Learn::Ok;
        return out;
    }
    out.store = storage::twin_save(out.twin);
    out.how = out.store.err == twin::Err::Ok ? Learn::Ok : Learn::Store;
    return out;
}

}  // namespace

twin::Facts facts_now() {
    twin::Facts f;
    const pd::ExtInfo& ex = fusb302::ext();
    if (ex.caps_ext.valid) f.claim_w = ex.caps_ext.pdp_w;
    const pd::CableLimits& cab = fusb302::physical_cable();
    if (cab.observed) {   // an unread cable is "unknown", not the 3 A / 20 V default
        f.cable_ma = cab.max_ma;
        f.cable_mv = cab.max_mv;
    }
    return f;
}

bool predict(const twin::Twin& t, twin::Outcome& out) {
    const pd::SourceCaps& caps = pd::active_caps();   // the debug fake list too: `twin predict` is a calculation
    if (!caps.valid) return false;
    out = twin::predict(t, caps, facts_now());
    return true;
}

std::uint32_t predict_stamp() {
    const pd::SourceCaps& caps = pd::active_caps();
    std::uint32_t h = caps.valid ? 2166136261u : 0u;
    const auto mix = [&h](std::uint32_t v) { h = (h ^ v) * 16777619u; };
    if (caps.valid) {
        mix(caps.num_pdos);
        for (std::size_t i = 0; i < caps.num_pdos && i < caps.pdos.size(); ++i) mix(caps.pdos[i].raw);
        const twin::Facts f = facts_now();
        mix(f.claim_w);
        mix(f.cable_ma);
        mix(f.cable_mv);
    }
    return h;
}

void log_predict(std::uint8_t seq, const twin::Outcome& o) {
    K2LOG("[TWIN] predict #%u kind=%u %u mV %u mA want=%u mW lim=%u obs=%u\n", unsigned(seq), unsigned(o.kind), unsigned(o.mv),
          unsigned(o.ma), unsigned(o.want_mw), unsigned(o.limiter), unsigned(o.observed));
}

bool can_wake(const LiveCtx& ctx) {
    return !fusb302::caps().valid && make_input(ctx).wakeable;
}

void wake_release() {
    if (!g_env.held()) return;
    g_env.drop_held();
    fusb302::rd_for_run(false);
    K2LOG("[TWIN] wake: Rd released\n");
}

twin::live::Refusal live_start(std::span<const std::uint8_t> seqs, bool confirmed, const LiveCtx& ctx, bool keep_wake) {
    if (pd::is_fake()) return twin::live::Refusal::NoSource;   // a fake list has no charger behind it
    fusb302::set_run_owner(true);
    g_env.keep_wake(keep_wake);
    const twin::live::Refusal r = run_obj().start(g_env, seqs, make_input(ctx), confirmed);
    K2LOG("[TWIN] live start n=%u -> %s\n", unsigned(seqs.size()), twin::live::name(r));
    if (r != twin::live::Refusal::None) fusb302::set_run_owner(false);   // a started run that already ended cleared it itself
    else fusb302::reread_take_over();   // the run owns the link: a pd-reread attach is the run's, its release timer must not fire
    return r;
}

void live_poll(const LiveCtx& ctx) {
    if (!g_run_inited || !run_obj().running()) return;
    run_obj().poll(make_input(ctx));
}

void live_cancel() {
    if (g_run_inited && run_obj().running()) run_obj().cancel();
}

bool live_running() { return g_run_inited && run_obj().running(); }

const twin::live::Runner& live() { return run_obj(); }

// The three learners hold a whole Twin (and by-value copies of it); out of line so they never inline into Ui::tick's frame (FRAME_MAX).
[[gnu::noinline]] Learned learn_from_identify(std::string_view name, bool save) {
    Learned out;
    const pd::IdStatus& st = fusb302::identify_status();
    if (st.state != pd::IdState::Done || !fusb302::identify_current()) return out;   // how = NotReady
    const pd::VdmInfo& vdm = fusb302::sink_vdm();
    const pd::SourceCaps& caps = fusb302::caps();
    const std::uint8_t rev = caps.valid ? caps.spec_rev : 2;   // the revision Identify itself spoke (fusb302.cpp identify start)
    if (!twin::from_identify(fusb302::identify_identity(), st, rev, std::span<const std::uint16_t>(vdm.svids.data(), vdm.num_svids),
                             out.twin))
        return out;   // finished without a single sink PDO: nothing to predict from
    out = save_learned(out, name, save);
    K2LOG("[TWIN] learn identify -> %u vid=%04x pdos=%u modes=%u\n", unsigned(out.how), unsigned(out.twin.vid),
          unsigned(out.twin.num_pdos), unsigned(out.twin.decl.n_modes));
    return out;
}

[[gnu::noinline]] Learned learn_from_sniff(std::string_view name, bool save) {
    Learned out;
    if (!(fusb302::inline_mode() || fusb302::far_sink_seen())) {
        out.how = Learn::NotInline;
        return out;
    }
    out.how = Learn::NoContract;
    const pd::VdmInfo& vdm = fusb302::sink_vdm();
    if (!twin::from_sniffed(fusb302::contract(), fusb302::caps(), vdm, fusb302::sink_caps(), out.twin)) return out;
    // What the wire configured (Enter_USB, Thunderbolt / DisplayPort Enter_Mode): kept only when something was negotiated.
    twin::make_observed(fusb302::usb_link(-1, -1), fusb302::usb_raw(), out.twin.vid, out.twin.obs);
    out = save_learned(out, name, save);
    K2LOG("[TWIN] learn sniff -> %u vid=%04x pdos=%u obs=%u\n", unsigned(out.how), unsigned(out.twin.vid), unsigned(out.twin.num_pdos),
          unsigned(out.twin.obs.valid));
    return out;
}

namespace {
alignas(InlineLearn) std::uint8_t g_inline_raw[sizeof(InlineLearn)];
bool g_inline_inited;
std::array<char, twin::kNameMax + 1> g_inline_name{};
std::size_t g_inline_name_n = 0;
bool g_inline_spoof = false;   // the run's own spoof is armed (disarmed by inline_poll once the run is over)

InlineLearn& inline_obj() {
    if (!g_inline_inited) {
        new (g_inline_raw) InlineLearn();
        g_inline_inited = true;
    }
    return *std::launder(reinterpret_cast<InlineLearn*>(g_inline_raw));
}

pd::IqRefusal inline_start_with(std::string_view name, bool save, bool confirmed, const LiveCtx& ctx, const emrec::Record* rec,
                                const emk_quirk::Quirks* quirks) {
    pd::IqFacts f;
    f.confirmed = confirmed;
    f.busy = ctx.busy || ctx.owners_busy || live_running() || loopguard::locked();
    f.self_powered = power_source::bus_powered();   // the driver adds its own pinned-Rd self power
    f.spoof = rec != nullptr;
    if (rec != nullptr && (!emk_spoof::usable(*rec, *quirks) || emk_spoof::active())) {   // a spoof somebody else armed stays theirs
        K2LOG("[TWIN] learn inline start refused: spoof %s\n", emk_spoof::active() ? "busy" : "not usable");
        return pd::IqRefusal::BadSpoof;
    }
    const pd::IqRefusal r = fusb302::interrogate_start(f);
    K2LOG("[TWIN] learn inline start save=%u spoof=%u -> %s\n", unsigned(save), unsigned(f.spoof), pd::iq_refusal_name(r));
    if (r != pd::IqRefusal::None) return r;
    InlineLearn& inl = inline_obj();
    inl = InlineLearn{};
    inl.pending = true;
    inl.save = save;
    g_inline_name_n = std::min<std::size_t>(name.size(), twin::kNameMax);
    std::memcpy(g_inline_name.data(), name.data(), g_inline_name_n);
    // Armed after the start succeeded (the driver refuses a start while any spoof is armed). The record was checked above.
    if (rec != nullptr) g_inline_spoof = emk_spoof::arm_run(*rec, *quirks) == emk_spoof::Err::Ok;
    return r;
}
}  // namespace

pd::IqRefusal inline_start(std::string_view name, bool save, bool confirmed, const LiveCtx& ctx) {
    return inline_start_with(name, save, confirmed, ctx, nullptr, nullptr);
}

pd::IqRefusal inline_start_cable(std::string_view name, bool save, bool confirmed, const LiveCtx& ctx, bool saved, std::uint16_t id) {
    emlib::Entry ent;
    emk_quirk::Quirks quirks;
    emrec::Record rec;
    if (saved) {
        if (!storage::lib_get(id, ent, quirks)) return pd::IqRefusal::BadSpoof;
        rec = ent.rec;
    } else {
        if (id >= emrec::kBuiltinCount) return pd::IqRefusal::BadSpoof;
        rec = emrec::builtin_cable(id);
        quirks = emk_quirk::builtin_quirks(id);
    }
    return inline_start_with(name, save, confirmed, ctx, &rec, &quirks);
}

[[gnu::noinline]] void inline_poll() {
    if (g_inline_spoof && !fusb302::interrogate_running()) {   // the run is over (done, cancelled, aborted or detached)
        emk_spoof::disarm();
        g_inline_spoof = false;
    }
    if (!g_inline_inited) return;
    InlineLearn& inl = inline_obj();
    if (!inl.pending || fusb302::interrogate_running()) return;
    inl.pending = false;
    inl.done = true;
    Learned out;
    out.how = Learn::NoContract;
    const pd::IqStatus& st = fusb302::interrogate_status();
    if (twin::from_interrogated(st, out.twin))
        out = save_learned(out, std::string_view(g_inline_name.data(), g_inline_name_n), inl.save);
    inl.result = out;
    K2LOG("[TWIN] learn inline -> %u vid=%04x pdos=%u req=%u rev=%08x ext=%u\n", unsigned(out.how), unsigned(out.twin.vid),
          unsigned(out.twin.num_pdos), unsigned(out.twin.has_request()), unsigned(out.twin.revision), unsigned(out.twin.sink_ext_len));
}

void inline_cancel() {
    const bool pending = g_inline_inited && inline_obj().pending;
    if (!pending && !fusb302::interrogate_running()) return;
    if (g_inline_inited) inline_obj().pending = false;   // a stopped run teaches nothing
    fusb302::interrogate_abort();
}

bool inline_running() { return (g_inline_inited && inline_obj().pending) || fusb302::interrogate_running(); }

const InlineLearn& inline_learn() { return inline_obj(); }

const pd::IqStatus& inline_status() { return fusb302::interrogate_status(); }

}  // namespace k2::twin_hw
