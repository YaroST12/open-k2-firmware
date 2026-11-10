// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Fast-charge trigger driver, see k2/fastcharge/fctrig.hpp.
#include "k2/core/log.hpp"
#include "k2/core/scratch.hpp"
#include "k2/fastcharge/fctrig.hpp"

#include <algorithm>
#include <array>

#include "k2/drivers/adc.hpp"
#include "k2/fastcharge/dpsniff.hpp"
#include "k2/drivers/dpdm.hpp"
#include "k2/pd/link.hpp"
#include "k2/pd/pdtrace.hpp"
#include "k2/drivers/power_monitor.hpp"
#include "k2/drivers/systick.hpp"
#include "k2/drivers/uart2.hpp"
#include "k2/fastcharge/ufcs.hpp"
#include "k2/fastcharge/ufcs_diag.hpp"
#include "k2/drivers/usb.hpp"
#include "k2/fastcharge/wire.hpp"

namespace k2::fctrig {

namespace {

// One ACCP exchange: master ping, slave ping, 200 us, the whole TX frame (header once, EOF master ping inside),
// 50 us, slave ping, then the RX loop starts at once on the falling edge that ends the slave ping; afterwards
// 100 us, slave ping (result ignored), 250 us, master ping, 50 us, slave ping (ignored). Blocking for ~10 to
// 30 ms; D+ is held at 0.6 V by every step. Parity of the received bytes is not checked.
bool master_ping() {
    fc::StepList<4> l;
    wire::master_ping(l);
    dpdm::play(l.span());
    return true;
}

bool slave_ping() {
    dpdm::set_dm(fc::Drive::RxPulldown);
    if (dpdm::wait_dm(true, wire::kSlavePingWaitUs) < 0) return false;
    const int w = dpdm::wait_dm(false, wire::kSlavePingMaxUs);
    return w >= 0 && wire::slave_ping_ok(static_cast<std::uint32_t>(w));
}

constexpr std::size_t kMaxRxBytes = 3;
constexpr std::size_t kRxSamplesPerByte = 175;

bool exchange_once(std::span<const std::uint8_t> tx, std::span<std::uint8_t> rx) {
    if (rx.size() > kMaxRxBytes) return false;
    dpdm::set_dp(fc::Drive::V0p6);
    master_ping();
    if (!slave_ping()) return false;
    dpdm::delay_us(200);
    fc::StepList<64> l;
    wire::frame_steps(l, tx);
    wire::master_ping(l);
    dpdm::play(l.span());
    dpdm::delay_us(50);
    if (!slave_ping()) return false;
    if (!rx.empty()) {
        // The D- capture window is transient (capture_dm overwrites every byte before decode_frame reads
        // them), so it comes from the scratch arena instead of a 585 B static in .bss. Main loop only, and
        // nothing on this path leases the arena (the leases are bootscreen, share pack/paint, bbpersist,
        // emlib, transcap.save, the journal replay - none of them run the wire exchange).
        const scratch::Lease lease("fctrig.exchange");
        auto& s = scratch::as<std::array<std::uint8_t, kMaxRxBytes * kRxSamplesPerByte + 60>>();
        const std::span<std::uint8_t> cap{s.data(), rx.size() * kRxSamplesPerByte + 60};
        dpdm::capture_dm(cap, wire::kRxPeriodUs);
        if (wire::decode_frame(cap, rx) != rx.size()) return false;
    }
    dpdm::delay_us(100);
    slave_ping();
    dpdm::delay_us(250);
    master_ping();
    dpdm::delay_us(50);
    slave_ping();
    dpdm::set_dm(fc::Drive::V0);
    return true;
}

// Wire log helpers. Every line carries t = millis; hex has no separators (the log encoder escapes
// spaces). The `[UART2]` lines are the raw bytes on PA2/PA3, the `[UFCS]` lines are what the engine
// made of them.
unsigned u(std::uint32_t v) { return static_cast<unsigned>(v); }   // uint32_t is long on ARM, the log format wants int-sized

void log_tx(std::span<const std::uint8_t> b) {
    const unsigned t = u(systick::millis());
    for (std::size_t o = 0; o < b.size(); o += ufcs::kHexChunk) {
        char h[2 * ufcs::kHexChunk + 1];
        ufcs::hex(b.subspan(o, std::min(ufcs::kHexChunk, b.size() - o)), h);
        K2LOG("[UART2] tx t=%u n=%d o=%d %s\n", t, static_cast<int>(b.size()), static_cast<int>(o), h);
    }
}

void log_rx(std::span<const std::uint8_t> b) {
    const unsigned t = u(systick::millis());
    for (std::size_t o = 0; o < b.size(); o += ufcs::kHexChunk) {
        char h[2 * ufcs::kHexChunk + 1];
        ufcs::hex(b.subspan(o, std::min(ufcs::kHexChunk, b.size() - o)), h);
        K2LOG("[UART2] rx t=%u n=%d o=%d %s\n", t, static_cast<int>(b.size()), static_cast<int>(o), h);
    }
}

// Receiver error flags, logged when the totals change (at most every 100 ms so a noisy line cannot flood the log).
uart2::Errors g_err_logged;
std::uint32_t g_err_log_ms;

void log_errors(bool force) {
    const uart2::Errors e = uart2::errors();
    if (e == g_err_logged) return;
    const std::uint32_t t = systick::millis();
    if (!force && t - g_err_log_ms < 100) return;
    g_err_logged = e;
    g_err_log_ms = t;
    K2LOG("[UART2] err t=%u framing=%u noise=%u overrun=%u parity=%u\n", u(t), u(e.framing), u(e.noise), u(e.overrun),
          u(e.parity));
}

// UFCS physical handshake: both lines to 0 V for 50 ms, then D-
// is toggled 3.3 V 2 ms / 0 V 8 ms twice while D+ (PA7, digital) must follow it, then D- is released
// and D+ must go high within 15 ms. Not yet confirmed on a UFCS source.
//
// Pass/fail is the level test only. Each D+ edge is also timed from the D- edge that should
// cause it and logged with its class: a BC1.2 DCP shorts D+ to D-, so it "answers" every edge within
// microseconds and passes this test without speaking UFCS (ufcs::classify, "mirror"). The reference
// gives no latency for a real source, so the class is a label, not a verdict.
bool ufcs_handshake(ufcs::Handshake& t) {
    t = {{-1, -1}, {-1, -1}, -1};   // the timings go back to the caller (the probe records them)
    dpdm::set_dp(fc::Drive::V0);
    dpdm::set_dm(fc::Drive::V0);
    dpdm::delay_us(50'000);
    dpdm::set_dp(fc::Drive::Listen);
    const bool pre = dpdm::read().pa7;   // D+ before we touch D-: high here means something else drives it
    bool ok = true;
    for (int i = 0; i < 2; i++) {
        dpdm::set_dm(fc::Drive::V3p3);
        t.rise_us[i] = dpdm::wait_dp(true, 2'000);
        dpdm::delay_us(ufcs::remaining_us(2'000, t.rise_us[i]));   // keep the 2 ms high time
        if (!dpdm::read().pa7) ok = false;
        dpdm::set_dm(fc::Drive::V0);
        t.fall_us[i] = dpdm::wait_dp(false, 8'000);
        if (i == 1) break;   // the 4th signal: D+ high detection starts here (below)
        dpdm::delay_us(ufcs::remaining_us(8'000, t.fall_us[i]));   // and its 8 ms low time
        if (dpdm::read().pa7) ok = false;
    }
    // T/TAF 083-2024 7.3 (fig. 5): the sink starts watching D+ for high at the START of the 4th
    // signal (D- low, tDet4 6..10 ms) and keeps watching for tDpDet (11..15 ms). A source may end the
    // D+/D- short and pull D+ high while D- is still low: the CUKTECH 10 charger (C1) answered inside
    // the 8 ms and the old "D+ must still be low at the end" check failed it (fin=0, ok=0). D- held
    // low keeps a plain DCP short low, so this cannot pass a mirror.
    t.final_us = -1;
    if (t.fall_us[1] >= 0) {
        const int up = dpdm::wait_dp(true, ufcs::remaining_us(8'000, t.fall_us[1]));
        if (up >= 0) t.final_us = t.fall_us[1] + up;   // from the 4th signal's D- edge
    }
    // No answer inside tDet4: release D- (D+ must go high within
    // 15 ms). Driving D- high here instead made a DCP's D+/D- short pass at once (fin=0 us, class
    // mirror on the Baseus FH22 charger): only a source that drives D+ itself can answer a released
    // D-. The FH22 answers here, about 1 ms after the release.
    if (t.final_us < 0) {
        dpdm::set_dm(fc::Drive::HiZ);
        const int w = dpdm::wait_dp(true, 15'000);
        if (w >= 0) t.final_us = 8'000 + w;
    }
    dpdm::set_dm(fc::Drive::HiZ);
    ok = ok && t.final_us >= 0;
    K2LOG("[UFCS] hs t=%u pre=%d rise=%d,%d fall=%d,%d fin=%d us class=%s ok=%d\n", u(systick::millis()), pre ? 1 : 0,
          t.rise_us[0], t.rise_us[1], t.fall_us[0], t.fall_us[1], t.final_us, ufcs::name(ufcs::classify(t)), ok ? 1 : 0);
    return ok;
}

std::uint32_t g_vooc_miss;   // consecutive vooc_rx calls without a frame

// The runtime UFCS tuner (RAM only, defaults = the built-in behaviour, reset at boot). The engine and
// the probe read it; ufcs_set() refuses changes while a probe runs, so a probe sees one fixed tuner.
ufcs::Tuner g_tuner;

// Handshake, then hand D+/D- to USART2 with the tuner's line settings. Shared by the engine's UFCS
// open and the probe. `hs` receives the handshake timings.
bool ufcs_session_open(ufcs::Handshake& hs) {
    // Up to 3 tries (retry within 0..10 ms, at most 3 attempts, then D+/D- HiZ).
    bool up = false;
    for (int attempt = 0; attempt < 3 && !up; attempt++) {
        if (attempt) dpdm::delay_us(5'000);
        up = ufcs_handshake(hs);
    }
    if (!up) {
        dpdm::set_dp(fc::Drive::HiZ);
        dpdm::set_dm(fc::Drive::HiZ);
        K2LOG("[FC] ufcs handshake: no answer\n");
        return false;
    }
    dpdm::set_dp(fc::Drive::HiZ);   // hand D+/D- to PA3/PA2 (USART2)
    dpdm::set_dm(fc::Drive::HiZ);
    if (!uart2::claim({g_tuner.baud, g_tuner.stop_bits})) return false;
    g_err_logged = {};
    // Tuner experiments on the lines in the UART phase (the dividers are separate pins from PA2/PA3).
    if (g_tuner.dm == 1) dpdm::set_dm(fc::Drive::V0);
    else if (g_tuner.dm == 2) dpdm::set_dm(fc::Drive::V3p3);
    if (g_tuner.dp == 1) dpdm::set_dp(fc::Drive::V0p6);
    dpdm::delay_us(static_cast<std::uint32_t>(g_tuner.delay_ms) * 1000);   // default 10 ms before the first frame
    K2LOG("[UFCS] open t=%u uart up, %u 8N%u, TX=D- RX=D+, dm=%u dp=%u, first frame next\n", u(systick::millis()),
          u(g_tuner.baud), u(g_tuner.stop_bits), u(g_tuner.dm), u(g_tuner.dp));
    return true;
}

struct HwEnv final : fc::Env {
    const ufcs::Tuner& ufcs_tuner() override { return g_tuner; }
    std::uint32_t now_ms() override { return systick::millis(); }
    int vbus_mv() override {
        const auto v = power::vbus_fast_mv();   // the engine judges landed / band / source lost / overvoltage: no 2.5 s average
        return v ? *v : -1;
    }
    int dp_mv() override {
        const auto r = adc::read();
        return r ? r->dp_mv : -1;
    }
    int dm_mv() override {
        const auto r = adc::read();
        return r ? r->dm_mv : -1;
    }
    int ceiling_mv() override { return power::max_vbus_mv(); }
    link::Reason inline_reason() override { return link::inline_now(); }
    void set_dp(fc::Drive d) override { dpdm::set_dp(d); }
    void set_dm(fc::Drive d) override { dpdm::set_dm(d); }
    void play(std::span<const fc::Step> s) override { dpdm::play(s); }
    bool xfer(std::span<const std::uint8_t> tx, std::span<std::uint8_t> rx) override {
        for (int attempt = 0; attempt < 3; attempt++) {
            if (exchange_once(tx, rx)) return true;
            dpdm::set_dm(fc::Drive::V0);
        }
        return false;
    }
    void log_reg(std::uint8_t reg, std::uint8_t value) override {
        K2LOG("[FC] reg 0x%02X = 0x%02X\n", reg, value);
    }
    void log_qc3(int vbus_mv, int pending_mv, int step_mv, int n) override {
        K2LOG("[QC3] vbus=%d pending=%d step=%d n=%d\n", vbus_mv, pending_mv, step_mv, n);
        // The Black Box shows each batch beside the VBUS samples: VBUS before it and the signed pulse count; the next
        // batch's mark carries the result, so a gap between pulses and voltage reads straight off the timeline.
        pdtrace::mark(pdtrace::kMarkQc3Batch, std::uint16_t(vbus_mv < 0 ? 0 : vbus_mv > 0xFFFF ? 0xFFFF : vbus_mv), std::uint16_t(std::int16_t(n)));
    }
    void log_qc3_coarse(int step_mv) override {
        K2LOG("[QC3] coarse step ~%d mV/pulse on QC3+: charger steps like QC3\n", step_mv);
    }
    void log_write(std::uint8_t reg, std::uint8_t value, bool acked) override {
        K2LOG("[FC] write 0x%02X = 0x%02X %s\n", reg, value, acked ? "ack" : "REFUSED");
    }
    int vooc_rx(std::uint32_t timeout_us) override {
        const int f = dpdm::rx_command(timeout_us);
        if (f >= 0) {
            K2LOG("[VOOC] cmd 0x%02X frame 0x%03X\n", static_cast<unsigned>(f >> 1), static_cast<unsigned>(f));
            g_vooc_miss = 0;
        } else if (++g_vooc_miss % 150 == 1) {   // no frame: say which D+ pin moves, about every second
            const dpdm::EdgeCount c = dpdm::count_dp_edges(3000);
            K2LOG("[VOOC] no frame x%u, D+ falls in 3 ms: PA7=%u PB0=%u\n", static_cast<unsigned>(g_vooc_miss),
                         static_cast<unsigned>(c.pa7), static_cast<unsigned>(c.pb0));
        }
        return f;
    }
    bool vooc_tx(std::uint16_t word) override {
        const bool ok = dpdm::tx_reply(word);
        K2LOG("[VOOC] reply 0x%03X %s\n", static_cast<unsigned>(word), ok ? "sent" : "no clock");
        return ok;
    }
    bool ufcs_open() override {
        ufcs::Handshake hs;
        return ufcs_session_open(hs);
    }
    // VIVO: release PB1/PB2, set PA7/PB0 to analog input and
    // init USART2; no D+/D- handshake. Same hand-over as ufcs_open() without the handshake.
    bool vivo_open() override {
        dpdm::set_dp(fc::Drive::HiZ);
        dpdm::set_dm(fc::Drive::HiZ);
        return uart2::claim();
    }
    bool ufcs_send(std::span<const std::uint8_t> tx) override {
        const unsigned t = u(systick::millis());
        const bool ok = uart2::tx(tx);
        log_tx(tx);
        if (!ok) K2LOG("[UART2] tx failed t=%u n=%d\n", t, static_cast<int>(tx.size()));
        return ok;
    }
    std::size_t ufcs_recv(std::span<std::uint8_t> out) override {
        const std::size_t n = uart2::rx(out);
        if (n > 0) log_rx(out.first(n));
        log_errors(false);
        return n;
    }
    void ufcs_close() override {
        log_errors(true);
        uart2::release();
    }
    void log_ufcs_drop(std::span<const std::uint8_t> held, std::size_t used, ufcs::Why why) override {
        char h[2 * ufcs::kHexChunk + 1];
        ufcs::hex(held.first(std::min(ufcs::kHexChunk, held.size())), h);
        K2LOG("[UFCS] drop t=%u why=%s used=%d have=%d %s\n", u(systick::millis()), ufcs::why_name(why), static_cast<int>(used),
              static_cast<int>(held.size()), h);
    }
    void log_ufcs_frame(const ufcs::Frame& f) override {
        K2LOG("[UFCS] frame t=%u %s cmd=%d id=%d len=%d to=%d\n", u(systick::millis()), f.data ? "data" : "ctl", f.cmd, f.id,
              static_cast<int>(f.payload.size()), static_cast<int>(f.addr));
        if (f.data && f.cmd == ufcs::kDataOutputCap) {   // the [UART2] hex line is cut short; one decoded line per mode
            std::array<ufcs::Cap, ufcs::kMaxCaps> caps{};
            const std::size_t n = ufcs::decode_caps(f.payload, caps);
            for (std::size_t i = 0; i < n; i++)
                K2LOG("[UFCS] cap %d: %d-%dmV %d-%dmA\n", static_cast<int>(caps[i].pos), caps[i].min_mv, caps[i].max_mv,
                      caps[i].min_ma, caps[i].max_ma);
        }
    }
    void log_ufcs_echo() override { K2LOG("[UFCS] echo: own frames only, D+/D- shorted (no UFCS)\n"); }
    void log_ufcs_retry(const char* what, int try_no, std::span<const std::uint8_t> pending) override {
        char h[2 * ufcs::kHexChunk + 1];
        ufcs::hex(pending.first(std::min(ufcs::kHexChunk, pending.size())), h);
        K2LOG("[UFCS] retry t=%u %s try=%d pending=%d %s\n", u(systick::millis()), what, try_no, static_cast<int>(pending.size()), h);
    }
    void reset_bus() override { dpdm::start_reset(); }
    bool reset_busy() override { return dpdm::reset_busy(); }
};

HwEnv g_env;
fc::Engine g_engine{g_env};
fc::Scanner g_scanner{g_engine, g_env};

// Hardware glue of the UFCS probe: the same open / send / receive / close as the engine's UFCS session
// (so the wire log lines are the same), but driven by the Prober, which never sends a Request.
struct ProbeHw final : ufcs::ProbeEnv {
    std::uint32_t now_ms() override { return systick::millis(); }
    int vbus_mv() override { return g_env.vbus_mv(); }
    bool inline_now() override { return link::inline_now() != link::Reason::None; }
    bool open(ufcs::Handshake& hs) override { return ufcs_session_open(hs); }
    bool send(std::span<const std::uint8_t> b) override { return g_env.ufcs_send(b); }
    std::size_t recv(std::span<std::uint8_t> out) override { return g_env.ufcs_recv(out); }
    ufcs::UartErrors errors() override { return uart2::errors(); }
    void close() override { g_env.ufcs_close(); }
    void reset_bus() override { dpdm::start_reset(); }
};
ProbeHw g_probe_env;
ufcs::Prober g_prober{g_probe_env};
bool g_probe_was_running;

std::size_t g_logged_results = 0;
State g_last_state = State::Idle;
Fail g_refused = Fail::None;   // why the last start / scan was refused (shown by status() while Idle)

}  // namespace

namespace {
std::uint32_t cycles_to_us(std::uint32_t c) { return c / 240; }   // DWT cycle counter runs at the 240 MHz core clock
}

LabXfer lab_ping(int retries) {
    const std::uint32_t t0 = dpdm::cycles();
    LabXfer r{false, 0, 0};
    while (r.tries < retries && !r.ok) {
        r.tries++;
        dpdm::set_dp(fc::Drive::V0p6);
        master_ping();
        r.ok = slave_ping();
        if (!r.ok) dpdm::set_dm(fc::Drive::V0);
    }
    r.us = cycles_to_us(dpdm::cycles() - t0);
    return r;
}

LabXfer lab_xfer(std::span<const std::uint8_t> tx, std::span<std::uint8_t> rx, int retries) {
    const std::uint32_t t0 = dpdm::cycles();
    LabXfer r{false, 0, 0};
    while (r.tries < retries && !r.ok) {
        r.tries++;
        r.ok = exchange_once(tx, rx);
        if (!r.ok) dpdm::set_dm(fc::Drive::V0);
    }
    r.us = cycles_to_us(dpdm::cycles() - t0);
    return r;
}

Fail start(Proto p, int target_mv, bool confirmed, bool allow_inline, int scp_ma, int ufcs_ma) {
    if (g_scanner.running() || dpsniff::armed() || g_prober.running()) return Fail::Busy;   // the edge sniffer is passive and keeps the lines
    const bool resetting = dpdm::reset_busy();
    const Fail f = g_engine.start(p, target_mv, confirmed, allow_inline, scp_ma, ufcs_ma);
    if (resetting && f == Fail::None) dpdm::cancel_reset();   // the session drives the lines now
    K2LOG("[FC] start %s %dmV confirmed=%d inline_ok=%d -> %s (link: %s)\n", fc::name(p), target_mv,
                 confirmed ? 1 : 0, allow_inline ? 1 : 0, fc::name(f), link::name(link::inline_now()));
    g_refused = f;
    return f;
}

Fail set_target(int target_mv, bool confirmed, int ufcs_ma) {
    const Fail f = g_engine.set_target(target_mv, confirmed, ufcs_ma);
    K2LOG("[FC] target %dmV confirmed=%d -> %s\n", target_mv, confirmed ? 1 : 0, fc::name(f));
    return f;
}

Fail scan_start(bool confirmed, bool allow_inline) {
    if (dpsniff::armed() || g_prober.running()) return Fail::Busy;
    const Fail f = g_scanner.start(confirmed, allow_inline);
    g_logged_results = 0;
    K2LOG("[FC] scan start confirmed=%d inline_ok=%d -> %s (link: %s)\n", confirmed ? 1 : 0,
                 allow_inline ? 1 : 0, fc::name(f), link::name(link::inline_now()));
    g_refused = f;
    return f;
}
bool scan_running() { return g_scanner.running(); }
bool scan_done() { return g_scanner.done(); }
std::size_t scan_step() { return g_scanner.step(); }
Fail scan_fail() { return g_scanner.fail(); }
std::span<const fc::ScanResult> scan_results() { return g_scanner.results(); }

const ufcs::Tuner& ufcs_tuner() { return g_tuner; }

TuneSet ufcs_set(ufcs::Field f, std::uint32_t v) {
    if (g_prober.running()) return TuneSet::Busy;   // a probe runs with one fixed tuner
    if (!ufcs::set(g_tuner, f, v)) return TuneSet::Range;
    K2LOG("[UFCS] tune %s=%u\n", ufcs::kFields[static_cast<std::size_t>(f)].name, u(v));
    return TuneSet::Ok;
}

bool ufcs_tuner_reset() {
    if (g_prober.running()) return false;
    g_tuner = ufcs::kDefaultTuner;
    K2LOG("[UFCS] tune reset to defaults\n");
    return true;
}

ufcs::ProbeStart ufcs_probe_start() {
    if (g_scanner.running() || dpsniff::armed() || g_engine.state() != State::Idle) return ufcs::ProbeStart::Busy;
    if (dpdm::reset_busy()) return ufcs::ProbeStart::Resetting;   // let the source see the lines released first
    g_prober.set_tuner(g_tuner);
    const ufcs::ProbeStart s = g_prober.start();
    g_probe_was_running = s == ufcs::ProbeStart::Ok;
    K2LOG("[UFCS] probe start -> %s vbus=%d (link: %s)\n", ufcs::name(s), g_prober.result().vbus_start,
          link::name(link::inline_now()));
    return s;
}
bool ufcs_probe_running() { return g_prober.running(); }
bool ufcs_probe_done() { return g_prober.done(); }
void ufcs_probe_stop() { g_prober.stop(); }
std::span<const std::uint8_t> ufcs_probe_blob() { return g_prober.blob(); }
const ufcs::ProbeResult& ufcs_probe_result() { return g_prober.result(); }

void stop() {
    g_refused = Fail::None;
    g_prober.stop();
    g_scanner.abort();
    g_engine.stop();
    K2LOG("[FC] stop: reset to 5V\n");
}

void poll() {
    dpdm::poll();
    g_prober.poll();
    if (g_probe_was_running && !g_prober.running()) {   // the probe just ended: one summary line
        g_probe_was_running = false;
        const ufcs::ProbeResult& r = g_prober.result();
        K2LOG("[UFCS] probe done t=%u %s hs=%s tx=%d rx=%d bytes=%d err=%u,%u,%u,%u vbus=%d..%d ms=%d\n", u(systick::millis()),
              ufcs::name(r.verdict), ufcs::name(r.hs_class), r.tx_frames, r.rx_frames, r.rx_bytes, u(r.err.framing),
              u(r.err.noise), u(r.err.overrun), u(r.err.parity), r.vbus_min, r.vbus_max, r.total_ms);
    }
    g_engine.poll();
    g_scanner.poll();
    const auto res = g_scanner.results();
    for (; g_logged_results < res.size(); g_logged_results++) {
        K2LOG("[FC] scan %s %s %dmV\n", fc::name(res[g_logged_results].proto),
                     fc::name(res[g_logged_results].outcome), res[g_logged_results].mv);
    }
    if (g_engine.state() != g_last_state) {
        g_last_state = g_engine.state();
        K2LOG("[FC] %s %s vbus=%dmV target=%dmV tries=%d%s%s\n", fc::name(g_engine.proto()), fc::name(g_last_state),
                     g_engine.reached_mv(), g_engine.target_mv(), g_engine.vooc_tries(), g_last_state == State::Failed ? " reason=" : "",
                     g_last_state == State::Failed ? fc::name(g_engine.fail()) : "");
    }
}

void service_ufcs() { g_engine.service_ufcs(); }
bool paint_critical() { return g_engine.paint_critical(); }

Status status() {
    const Fail f = g_engine.fail() != Fail::None || g_engine.state() != State::Idle ? g_engine.fail() : g_refused;
    // Always the live INA reading: the engine only tracks VBUS while it runs, so an idle status would
    // otherwise show 0 mV next to a charging phone.
    const auto m = power::read();
    return {g_engine.state(), g_engine.proto(), f, g_engine.target_mv(), g_engine.vooc_tries(), m ? m->vbus_mv : g_engine.reached_mv(), g_engine.adapter_mv()};
}

const fc::Caps& caps() { return g_engine.caps(); }

}  // namespace k2::fctrig
