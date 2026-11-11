// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#include "k2/twin/twin_hw.hpp"

#include <algorithm>
#include <array>
#include <cstring>

#include "k2/core/log.hpp"
#include "k2/core/storage.hpp"
#include "k2/drivers/fusb302.hpp"
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

    void finished(twin::live::Reason why) override {
        fusb302::set_run_owner(false);   // the sleeping-port wake may act again
        K2LOG("[TWIN] live end %s\n", twin::live::name(why));
    }
};

HwEnv g_env;
twin::live::Runner g_run;

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
    in.busy = c.busy;
    in.inline_link = c.inline_link;
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

twin::live::Refusal live_start(std::span<const std::uint8_t> seqs, bool confirmed, const LiveCtx& ctx) {
    if (pd::is_fake()) return twin::live::Refusal::NoSource;   // a fake list has no charger behind it
    fusb302::set_run_owner(true);
    const twin::live::Refusal r = g_run.start(g_env, seqs, make_input(ctx), confirmed);
    K2LOG("[TWIN] live start n=%u -> %s\n", unsigned(seqs.size()), twin::live::name(r));
    if (r != twin::live::Refusal::None) fusb302::set_run_owner(false);   // a started run that already ended cleared it itself
    else fusb302::reread_take_over();   // the run owns the link: a pd-reread attach is the run's, its release timer must not fire
    return r;
}

void live_poll(const LiveCtx& ctx) {
    if (!g_run.running()) return;
    g_run.poll(make_input(ctx));
}

void live_cancel() {
    if (g_run.running()) g_run.cancel();
}

bool live_running() { return g_run.running(); }

const twin::live::Runner& live() { return g_run; }

Learned learn_from_identify(std::string_view name, bool save) {
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

Learned learn_from_sniff(std::string_view name, bool save) {
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

}  // namespace k2::twin_hw
