// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Debug shell glue, see k2/core/shell_hw.hpp.
#include "k2/core/shell_hw.hpp"

#include <algorithm>
#include <array>
#include <optional>
#include <string_view>

#include "k2/fastcharge/accp.hpp"
#include "k2/drivers/adc.hpp"
#include "k2/drivers/dpdm.hpp"
#include "k2/fastcharge/dpsniff.hpp"
#include "k2/fastcharge/fastcharge.hpp"
#include "k2/fastcharge/fctrig.hpp"
#include "k2/core/fmt.hpp"
#include "k2/drivers/fusb302.hpp"
#include "k2/drivers/lcd.hpp"
#include "k2/drivers/lcd_timing.hpp"
#include "k2/core/gap_meter.hpp"
#include "k2/core/paint_meter.hpp"
#include "k2/drivers/power_monitor.hpp"
#include "k2/fastcharge/qc.hpp"
#include "k2/fastcharge/ufcs.hpp"
#include "k2/drivers/uart2.hpp"
#include "k2/pd/pd_sink.hpp"
#include "k2/fastcharge/vooc.hpp"
#include "k2/core/lab.hpp"
#include "k2/core/shell.hpp"
#include "k2/drivers/systick.hpp"
#include "k2/core/dbg_stream_hw.hpp"
#include "k2/pd/pdtrace.hpp"
#include "k2/drivers/usb.hpp"
#include "k2/drivers/usbcfg.hpp"
#include "k2/core/remote.hpp"
#include "k2/core/remote_proto.hpp"
#include "tusb.h"
#include "k2/ui/ui.hpp"

namespace k2::shell_hw {
namespace {

constexpr std::uint32_t kWriteTimeoutMs = 50;   // a stalled host must not stall the main loop

Remote* g_remote = nullptr;
Ui* g_ui = nullptr;

bool g_session = false;        // the host has the port open (DTR) and the link is up
bool g_log_on = false;         // `log on`
bool g_fc_started = false;     // a trigger this shell started and has not reset
bool g_raw_dp = false, g_raw_dm = false;   // a raw level is driven
std::uint32_t g_silent_until = 0;          // `pd silent` deadline (ms), 0 = not silent

bool session_up() { return tud_mounted() && !tud_suspended() && tud_cdc_connected(); }

// Whole text to the host, waiting briefly for FIFO room (the help text is longer than the FIFO).
void put(std::string_view s) {
    if (!g_session) return;
    const std::uint32_t t0 = systick::millis();
    while (!s.empty()) {
        const std::uint32_t n = tud_cdc_write(s.data(), static_cast<std::uint32_t>(s.size()));
        s.remove_prefix(n);
        if (s.empty()) break;
        tud_cdc_write_flush();
        tud_task();
        if (systick::millis() - t0 > kWriteTimeoutMs) return;
    }
}

// usb::log tap: one chip log line to the terminal; dropped when the FIFO has no room.
void tap(std::string_view line) {
    if (!g_log_on || !g_session) return;
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.remove_suffix(1);
    if (tud_cdc_write_available() < line.size() + 2) return;
    tud_cdc_write(line.data(), static_cast<std::uint32_t>(line.size()));
    tud_cdc_write("\r\n", 2);
}

const char* fc_reason(remote::Status s) {
    if (s == remote::Status::Ok) return nullptr;
    if (s == remote::Status::Busy) return "busy";
    const fc::Fail f = fctrig::status().fail;
    return f == fc::Fail::None ? "refused" : fc::name(f);
}

void raw_to_hiz() {
    if (!g_raw_dp && !g_raw_dm) return;
    dpdm::set_dp(dpdm::Drive::HiZ);
    dpdm::set_dm(dpdm::Drive::HiZ);
    g_raw_dp = g_raw_dm = false;
    dpdm::start_reset();   // 0 V for 500 ms, then passive idle: the charger is back at 5 V
}

void fc_stop() {
    g_remote->fc_cmd(remote::kFcReset, 0, 0, 0);
    g_fc_started = false;
}

void silent_end() {
    if (!g_silent_until) return;
    g_silent_until = 0;
    fusb302::legacy_only(false);
}

void sniff_stop() {
    dpsniff::cancel();
    const dpsniff::StreamStats st = dpsniff::stream_stats();
    char b[96];
    const std::size_t n = fmt::format(std::span<char>(b, sizeof b), "sniff stats edges=%u overruns=%u bytes=%u\r\n",
                                      static_cast<unsigned>(st.edges), static_cast<unsigned>(st.overruns),
                                      static_cast<unsigned>(st.bytes));
    put({b, n});
}

// End the debug stream and print its counters: the `trace stats` line is what tools/pd_trace.py checks, the `stream stats`
// line (sent/lost per kind) is what tools/k2_stream.py checks.
void stream_stop() {
    const std::uint8_t mask = dbg_hw::stream_mask();
    const dbg::Stats s = dbg_hw::stop();
    char b[192];
    std::size_t n;
    if (mask & dbg::kTrace) {
        n = fmt::format(std::span<char>(b, sizeof b), "trace stats records=%u lost=%u\r\n", static_cast<unsigned>(s.trace_records),
                        static_cast<unsigned>(s.trace_lost));
        put({b, n});
    }
    n = fmt::format(std::span<char>(b, sizeof b),
                    "stream stats frames=%u trace=%u/%u log=%u/%u run=%u/%u power=%u folded=%u same=%u stalls=%u\r\n",
                    static_cast<unsigned>(s.frames), static_cast<unsigned>(s.trace_records), static_cast<unsigned>(s.trace_lost),
                    static_cast<unsigned>(s.log_entries), static_cast<unsigned>(s.log_lost), static_cast<unsigned>(s.run_events),
                    static_cast<unsigned>(s.run_lost), static_cast<unsigned>(s.power_frames), static_cast<unsigned>(s.power_folded),
                    static_cast<unsigned>(s.power_same), static_cast<unsigned>(s.stalls));
    put({b, n});
}

// ---- protocol lab (k2/core/lab.hpp): the table is parsed and confirm-checked by the shell; this runs it ----

constexpr int kLabAbortMv = 12000;           // lab commands (no `fc trigger` running) drop the lines above this
constexpr int kLabDefaultRetries = 3;
constexpr unsigned kMaxPulses = 200;

void service() {   // let the CDC FIFO drain during a long command
    tud_cdc_write_flush();
    tud_task();
}

// One result line: `cmd k=v ...` (human) or `{"cmd":"..","k":v,..}` (--json, for tools/proto_lab.py).
struct Line {
    char b[200];
    std::size_t n = 0;
    bool json;
    explicit Line(const lab::Cmd& c, std::string_view name) : json(c.o.json) {
        if (json) add("{\"cmd\":\"%s\"", name.data());
        else add("%s", name.data());
    }
    template <class... A>
    void add(const char* f, A... a) {
        if (n < sizeof b) n += fmt::format(std::span<char>(b + n, sizeof b - n), f, a...);
    }
    void u(const char* k, std::uint32_t v) { add(json ? ",\"%s\":%u" : " %s=%u", k, static_cast<unsigned>(v)); }
    void i(const char* k, int v) { add(json ? ",\"%s\":%d" : " %s=%d", k, v); }
    void x(const char* k, unsigned v) { add(json ? ",\"%s\":%u" : " %s=0x%02X", k, v); }   // JSON: plain number
    void t(const char* k, bool v) { add(json ? (v ? ",\"%s\":true" : ",\"%s\":false") : " %s=%d", k, v ? 1 : 0); }
    void s(const char* k, const char* v) { add(json ? ",\"%s\":\"%s\"" : " %s=%s", k, v); }
    void hex(const char* k, std::span<const std::uint8_t> d) {
        char h[2 * lab::kMaxBytes + 1];
        std::size_t m = 0;
        for (std::uint8_t v : d) {
            h[m++] = "0123456789ABCDEF"[v >> 4];
            h[m++] = "0123456789ABCDEF"[v & 15];
        }
        h[m] = 0;
        s(k, h);
    }
    void send() {
        if (json) add("}");
        add("\r\n");
        put({b, n});
        service();
    }
};

// Which numeric options / flags a command honours. The grammar accepts all of them; an option a command
// does not use is refused instead of being silently ignored.
enum : unsigned { kRetries = 1, kTimeout = 2, kPulse = 4, kHold = 8, kWatch = 16, kSop = 32, kKeep = 64 };

unsigned allowed_opts(lab::Id id) {
    using I = lab::Id;
    switch (id) {
    case I::AccpPing: case I::AccpRead: case I::AccpWrite: case I::AccpDump: return kRetries | kKeep;
    case I::Qc3Up: case I::Qc3Down: case I::Qc3pUp: case I::Qc3pDown: return kPulse | kKeep;
    case I::Dpdm: return kHold | kKeep;
    case I::Vbus: return kWatch;
    case I::UfcsPing: case I::UfcsCaps: case I::UfcsRaw: return kTimeout;
    case I::VoocListen: case I::VoocAnswer: return kTimeout | kKeep;
    default: return 0;   // engine-driven requests and PD verbs: defaults only (hs / frame-gap options are reserved)
    }
}

const char* check_opts(const lab::Cmd& c) {
    const unsigned ok = allowed_opts(c.id);
    const lab::Opts& o = c.o;
    if (o.retries && !(ok & kRetries)) return "--retries is not used by this command";
    if (o.timeout_ms && !(ok & kTimeout)) return "--timeout-ms is not used by this command";
    if ((o.pulse_us || o.gap_us) && !(ok & kPulse)) return "--pulse-us/--gap-us are not used by this command";
    if (o.hold_ms && !(ok & kHold)) return "--hold-ms is not used by this command";
    if (o.watch_ms && !(ok & kWatch)) return "--watch is not used by this command";
    if (o.sop) return "--sop: PD message injection is not implemented in this firmware";
    if (o.hs_ms || o.frame_gap_us) return "--hs-ms/--frame-gap-us are reserved (engine timing hooks not added)";
    if ((o.hold || o.release) && !(ok & kKeep)) return "--hold/--release are not used by this command";
    return nullptr;
}

// Nothing else may own the pins; `drives` adds the rules of the raw `dp`/`dm` commands.
const char* lab_guard(bool drives) {
    if (g_ui->ports_busy()) return "busy (charger test or release running)";
    if (dpsniff::armed() || dpsniff::streaming()) return "busy (sniff active)";
    if (fctrig::scan_running()) return "busy (scan running)";
    if (fctrig::ufcs_probe_running()) return "busy (ufcs probe running)";
    if (drives) {
        if (g_ui->link().inline_mode()) return "a sink is attached";
        if (power::max_vbus_mv() == 0) return "no power monitor";
    }
    return nullptr;
}

bool over_abort_mv() {
    const auto pm = power::read();
    return pm && pm->vbus_mv > kLabAbortMv;
}

const char* abort_lines() {
    raw_to_hiz();
    return "abort: VBUS above 12 V, lines released";
}

void lab_vbus_line(const lab::Cmd& c) {
    Line l(c, "vbus");
    l.u("ms", systick::millis());
    const auto pm = power::read();
    const auto r = adc::read();
    l.i("mv", pm ? int(pm->vbus_mv) : -1);
    l.i("ma", pm ? int(pm->current_ma) : -1);
    l.i("dp", r ? int(r->dp_mv) : -1);
    l.i("dm", r ? int(r->dm_mv) : -1);
    l.send();
}

void accp_line(const lab::Cmd& c, lab::Id id, int reg, bool is_write, std::span<const std::uint8_t> rx,
               const fctrig::LabXfer& r) {
    Line l(c, lab::name(id));
    l.x("reg", static_cast<unsigned>(reg));
    l.t("link", r.ok);
    bool ack;
    if (is_write) {
        ack = r.ok && accp::parse_write_ack(rx);
    } else {
        const auto v = r.ok ? accp::parse_read(rx) : std::nullopt;
        ack = v.has_value();
        if (ack) l.x("val", *v);
    }
    l.t("ack", ack);
    l.hex("rx", rx);
    l.u("tries", r.tries);
    l.u("us", r.us);
    l.send();
}

const char* lab_accp(const lab::Cmd& c) {
    using I = lab::Id;
    if (const char* why = lab_guard(true)) return why;
    const int retries = c.o.retries ? static_cast<int>(c.o.retries) : kLabDefaultRetries;
    g_raw_dp = g_raw_dm = true;   // the exchange drives both lines: the session end must release them
    if (c.id == I::AccpPing) {
        const fctrig::LabXfer r = fctrig::lab_ping(retries);
        Line l(c, lab::name(c.id));
        l.t("ack", r.ok);
        l.u("tries", r.tries);
        l.u("us", r.us);
        l.send();
    } else {
        unsigned from = c.a[0], to = from;
        if (c.id == I::AccpRead && c.na > 1) {
            if (c.a[1] == 0 || c.a[1] > 64) return "n is 1..64";
            to = from + c.a[1] - 1;
        } else if (c.id == I::AccpDump) {
            to = c.a[1];
        }
        if (to > 255) return "register range passes 0xFF";
        for (unsigned reg = from; reg <= to; reg++) {
            std::array<std::uint8_t, 3> rx{};
            fctrig::LabXfer r;
            if (c.id == I::AccpWrite) {
                const auto tx = accp::write_frame(static_cast<std::uint8_t>(reg), static_cast<std::uint8_t>(c.a[1]));
                r = fctrig::lab_xfer(tx, std::span<std::uint8_t>(rx.data(), 2), retries);
                accp_line(c, c.id, static_cast<int>(reg), true, std::span<const std::uint8_t>(rx.data(), 2), r);
            } else {
                const auto tx = accp::read_frame(static_cast<std::uint8_t>(reg));
                r = fctrig::lab_xfer(tx, rx, retries);
                accp_line(c, c.id, static_cast<int>(reg), false, rx, r);
            }
            if (over_abort_mv() && !g_fc_started) return abort_lines();
        }
    }
    if (c.o.release) raw_to_hiz();
    return nullptr;
}

// qc3 / qc3+ up|down: pulse trains on the continuous-mode baseline (D+ 0.6 V, D- 3.3 V, set with dpdm).
const char* lab_qc_pulse(const lab::Cmd& c) {
    using I = lab::Id;
    if (const char* why = lab_guard(true)) return why;
    if (!g_raw_dp || !g_raw_dm) return "set the baseline first: dpdm 0.6 3.3 confirm";
    const bool plus = c.id == I::Qc3pUp || c.id == I::Qc3pDown;
    const bool up = c.id == I::Qc3Up || c.id == I::Qc3pUp;
    const unsigned n = c.na ? c.a[0] : 1;
    if (n == 0 || n > kMaxPulses) return "pulse count is 1..200";
    const std::uint32_t plus_hold = up ? qc::kQc3PlusPulseUs : qc::kQc3PlusDownPulseUs;
    const std::uint32_t hold = c.o.pulse_us ? c.o.pulse_us : (plus ? plus_hold : qc::kQc3PulseUs);
    const std::uint32_t gap = c.o.gap_us ? c.o.gap_us : (plus ? qc::kQc3PlusGapUs : qc::kQc3GapUs);
    const unsigned per_chunk = std::clamp<unsigned>(18000u / (hold + gap), 1u, 15u);   // one play() masks IRQs <= ~20 ms
    const auto before = power::read();
    const std::uint32_t t0 = dpdm::cycles();
    for (unsigned done = 0; done < n;) {
        fc::StepList<32> l;
        const unsigned m = std::min(per_chunk, n - done);
        for (unsigned k = 0; k < m; k++) {
            if (up) l.push(dpdm::Drive::V3p3, dpdm::Drive::V3p3, hold);
            else l.push(dpdm::Drive::V0p6, dpdm::Drive::V0p6, hold);
            l.push(dpdm::Drive::V0p6, dpdm::Drive::V3p3, gap);
        }
        dpdm::play(l.span());
        done += m;
        if (over_abort_mv()) return abort_lines();
    }
    const std::uint32_t us = (dpdm::cycles() - t0) / 240;
    systick::delay_ms(50);   // let VBUS settle before the read-back
    const auto after = power::read();
    Line l(c, lab::name(c.id));
    l.u("n", n);
    l.u("pulse_us", hold);
    l.u("gap_us", gap);
    l.u("us", us);
    l.i("mv_before", before ? int(before->vbus_mv) : -1);
    l.i("mv_after", after ? int(after->vbus_mv) : -1);
    l.send();
    if (c.o.release) raw_to_hiz();
    return nullptr;
}

// Engine-driven requests (qc2 / qc3 set / afc / fcp / scp / vooc wake / ufcs / vivo): the engine owns the
// handshake, timing and safety rules, so only the target (and the SCP current) is a parameter.
const char* lab_engine(const lab::Cmd& c) {
    using I = lab::Id;
    fc::Proto p;
    int mv = static_cast<int>(c.a[0]);
    switch (c.id) {
    case I::Qc2: p = fc::Proto::Qc2; break;
    case I::Qc3Set: p = fc::Proto::Qc3; break;
    case I::Qc3pSet: p = fc::Proto::Qc3Plus; break;
    case I::Afc: p = fc::Proto::Afc; break;
    case I::Fcp: p = fc::Proto::Fcp; break;
    case I::Scp: p = fc::Proto::Scp; break;
    case I::VoocWake: p = fc::Proto::Vooc; mv = 5000; break;
    case I::UfcsRequest: p = fc::Proto::Ufcs; break;
    default: p = fc::Proto::Vivo; break;   // VivoRequest
    }
    if (const char* why = lab_guard(false)) return why;
    remote::Status s;
    if (c.id == I::Scp) {
        // fc_cmd carries no current: same checks as Remote::fc_cmd, then the engine call with the SCP current.
        const fctrig::Fail f = fctrig::start(p, mv, c.o.confirm, false, static_cast<int>(c.a[1]));
        s = f == fctrig::Fail::None ? remote::Status::Ok : f == fctrig::Fail::Busy ? remote::Status::Busy : remote::Status::BadArg;
    } else {
        s = g_remote->fc_cmd(remote::kFcTrigger, static_cast<std::uint8_t>(p), static_cast<std::uint16_t>(mv),
                             c.o.confirm ? remote::kFcConfirm : 0);
    }
    if (s == remote::Status::Ok) g_fc_started = true;
    if (const char* why = fc_reason(s)) return why;
    Line l(c, lab::name(c.id));
    l.u("target_mv", static_cast<unsigned>(mv));
    if (c.id == I::Scp) l.u("ma", c.a[1]);
    else if (c.na > 1) l.u("ma_applied", 0);   // the engine has a fixed current for this protocol
    l.send();
    return nullptr;
}

const char* lab_vooc_frames(const lab::Cmd& c) {
    if (const char* why = lab_guard(true)) return why;
    if (fctrig::status().state != fc::State::Idle) return "busy";
    g_raw_dp = g_raw_dm = true;
    dpdm::set_dp(dpdm::Drive::Listen);
    dpdm::set_dm(dpdm::Drive::RxPulldown);
    const std::uint32_t to_ms = c.o.timeout_ms ? c.o.timeout_ms : 1000;
    const std::uint32_t t0 = dpdm::cycles();
    const int f = dpdm::rx_command(to_ms * 1000u);
    Line l(c, lab::name(c.id));
    l.t("frame", f >= 0);
    if (f >= 0) {
        l.x("frame_raw", static_cast<unsigned>(f));
        l.x("cmd", vooc::frame_cmd(static_cast<std::uint16_t>(f)));
        if (c.id == lab::Id::VoocAnswer) {
            const bool match = vooc::frame_cmd(static_cast<std::uint16_t>(f)) == c.a[0];
            l.t("match", match);
            if (match) l.t("sent", dpdm::tx_reply(static_cast<std::uint16_t>(c.a[1])));
        }
    }
    l.u("us", (dpdm::cycles() - t0) / 240);
    l.send();
    if (c.o.release) raw_to_hiz();
    return nullptr;
}

const char* lab_dpdm(const lab::Cmd& c) {
    const auto d0 = static_cast<dpdm::Drive>(c.a[0]), d1 = static_cast<dpdm::Drive>(c.a[1]);
    const bool raise = d0 != dpdm::Drive::HiZ || d1 != dpdm::Drive::HiZ;
    if (const char* why = lab_guard(raise)) return why;
    if (raise && (fctrig::status().state != fc::State::Idle)) return "busy";
    dpdm::set_dp(d0);
    dpdm::set_dm(d1);
    g_raw_dp = d0 != dpdm::Drive::HiZ;
    g_raw_dm = d1 != dpdm::Drive::HiZ;
    if (c.o.hold_ms) {
        const std::uint32_t t0 = systick::millis();
        while (systick::millis() - t0 < c.o.hold_ms) {
            if (over_abort_mv()) return abort_lines();
            service();
        }
        raw_to_hiz();   // held for the asked time, then released: HiZ and a line reset
    } else if (c.o.release) {
        raw_to_hiz();
    }
    return nullptr;
}

const char* lab_vbus(const lab::Cmd& c) {
    lab_vbus_line(c);
    if (!c.o.watch_ms) return nullptr;
    const std::uint32_t t0 = systick::millis();
    std::uint32_t next = t0 + 100;
    while (systick::millis() - t0 < c.o.watch_ms) {
        service();
        if (static_cast<std::int32_t>(systick::millis() - next) >= 0) {
            next += 100;
            lab_vbus_line(c);
        }
    }
    return nullptr;
}

const char* lab_pd_caps(const lab::Cmd& c) {
    const pd::SourceCaps& caps = fusb302::caps();
    Line l(c, lab::name(c.id));
    l.t("valid", caps.valid);
    l.u("pdos", caps.valid ? caps.num_pdos : 0);
    l.send();
    for (unsigned i = 0; caps.valid && i < caps.num_pdos && i < pd::kMaxPdos; i++) {
        Line p(c, "pd.pdo");
        p.u("pos", i + 1);
        p.u("raw", caps.pdos[i].raw);
        p.u("min_mv", caps.pdos[i].min_mv);
        p.u("max_mv", caps.pdos[i].max_mv);
        p.u("max_ma", caps.pdos[i].max_ma);
        p.send();
    }
    return nullptr;
}

// UFCS bus access (USART2, 115200 8N1): claims the port for the command and always releases it.
// No D+/D- handshake here; a source that needs it is driven by `ufcs request`.
const char* lab_ufcs(const lab::Cmd& c) {
    using I = lab::Id;
    if (const char* why = lab_guard(true)) return why;
    if (fctrig::status().state != fc::State::Idle) return "busy";
    if (!uart2::claim()) return "USART2 busy";
    std::array<std::uint8_t, 40> tx{};
    std::size_t n = 0;
    if (c.id == I::UfcsRaw) {
        for (std::size_t i = 0; i < c.nbytes; i++) tx[i] = c.bytes[i];
        n = c.nbytes;
    } else if (c.id == I::UfcsCaps) {
        n = ufcs::encode_control(tx, ufcs::kCtlGetOutputCap, 0);
    } else {
        n = ufcs::encode_control(tx, ufcs::kCtlPing, 0);
    }
    std::array<std::uint8_t, 96> rx{};
    std::size_t got = 0;
    const std::uint32_t to = c.o.timeout_ms ? c.o.timeout_ms : 300;
    const std::uint32_t t0 = systick::millis();
    const bool sent = uart2::tx(std::span<const std::uint8_t>(tx.data(), n));
    while (sent && systick::millis() - t0 < to && got < rx.size()) {
        got += uart2::rx(std::span<std::uint8_t>(rx.data() + got, rx.size() - got));
        service();
    }
    uart2::release();
    Line l(c, lab::name(c.id));
    l.t("sent", sent);
    l.hex("tx", std::span<const std::uint8_t>(tx.data(), n));
    l.u("rx_len", static_cast<std::uint32_t>(got));
    l.hex("rx", std::span<const std::uint8_t>(rx.data(), std::min<std::size_t>(got, lab::kMaxBytes)));
    ufcs::Frame f;
    std::size_t used = 0;
    const bool frame = ufcs::decode(std::span<const std::uint8_t>(rx.data(), got), f, used) == ufcs::Parse::Ok;
    l.t("frame", frame);
    if (frame) {
        l.u("cmd", f.cmd);
        l.t("data", f.data);
    }
    l.send();
    if (frame && f.data && c.id == I::UfcsCaps) {
        std::array<ufcs::Cap, ufcs::kMaxCaps> caps{};
        const std::size_t k = ufcs::decode_caps(f.payload, caps);
        for (std::size_t i = 0; i < k; i++) {
            Line p(c, "ufcs.cap");
            p.u("pos", static_cast<std::uint32_t>(caps[i].pos));
            p.u("min_mv", static_cast<std::uint32_t>(caps[i].min_mv));
            p.u("max_mv", static_cast<std::uint32_t>(caps[i].max_mv));
            p.u("max_ma", static_cast<std::uint32_t>(caps[i].max_ma));
            p.send();
        }
    }
    return nullptr;
}

// UFCS tuner (RAM only) and the 5 V probe. `set` / `reset` / `probe` need an idle bus (no test, scan, sniff or
// trigger); `show` and `result` only read. The probe is asynchronous: `ufcs probe` starts it, `ufcs result`
// (or the [UFCS] probe done log line) tells when it ended. It never leaves 5 V and never sends a Request.
void ufcs_tune_line(const lab::Cmd& c, ufcs::Field f) {
    const ufcs::FieldDef& d = ufcs::kFields[static_cast<std::size_t>(f)];
    Line l(c, "ufcs.tune");
    l.s("field", d.name);
    l.u("val", ufcs::get(fctrig::ufcs_tuner(), f));
    l.u("def", d.def);
    l.u("lo", d.lo);
    l.u("hi", d.hi);
    l.send();
}

void ufcs_result_lines(const lab::Cmd& c) {
    const ufcs::ProbeResult& r = fctrig::ufcs_probe_result();
    Line l(c, "ufcs.result");
    l.s("state", fctrig::ufcs_probe_running() ? "running" : fctrig::ufcs_probe_done() ? "done" : "idle");
    l.s("verdict", ufcs::name(r.verdict));
    l.s("class", ufcs::name(r.hs_class));
    l.u("tx", static_cast<std::uint32_t>(r.tx_frames));
    l.u("rx", static_cast<std::uint32_t>(r.rx_frames));
    l.u("bytes", static_cast<std::uint32_t>(r.rx_bytes));
    l.u("framing", r.err.framing);
    l.u("noise", r.err.noise);
    l.u("overrun", r.err.overrun);
    l.u("parity", r.err.parity);
    l.u("ms", static_cast<std::uint32_t>(r.total_ms));
    l.t("truncated", r.truncated);
    l.send();
    Line h(c, "ufcs.hs");
    h.i("rise1_us", r.hs.rise_us[0]);
    h.i("fall1_us", r.hs.fall_us[0]);
    h.i("rise2_us", r.hs.rise_us[1]);
    h.i("fall2_us", r.hs.fall_us[1]);
    h.i("final_us", r.hs.final_us);
    h.i("vbus_start", r.vbus_start);
    h.i("vbus_min", r.vbus_min);
    h.i("vbus_max", r.vbus_max);
    h.send();
    // The timeline: events { u16 ms, u8 kind, u8 n, n bytes }; a long event goes out in 32-byte lines.
    const std::span<const std::uint8_t> b = fctrig::ufcs_probe_blob();
    std::size_t off = ufcs::kProbeHeader;
    while (off + 4 <= b.size()) {
        const unsigned ms = b[off] | (b[off + 1] << 8);
        const bool tx = b[off + 2] == ufcs::kEventTx;
        const std::size_t len = b[off + 3];
        if (off + 4 + len > b.size()) break;
        for (std::size_t k = 0; k < len || (len == 0 && k == 0); k += lab::kMaxBytes) {
            Line e(c, "ufcs.ev");
            e.u("ms", ms);
            e.s("dir", tx ? "tx" : "rx");
            e.hex("hex", b.subspan(off + 4 + k, std::min<std::size_t>(len - k, lab::kMaxBytes)));
            e.send();
            if (len == 0) break;
        }
        off += 4 + len;
    }
}

const char* lab_ufcs_tune(const lab::Cmd& c) {
    using I = lab::Id;
    if (c.id == I::UfcsShow) {
        for (std::size_t i = 0; i < ufcs::kFieldCount; i++) ufcs_tune_line(c, static_cast<ufcs::Field>(i));
        return nullptr;
    }
    if (c.id == I::UfcsResult) {
        ufcs_result_lines(c);
        return nullptr;
    }
    if (const char* why = lab_guard(false)) return why;
    if (fctrig::status().state != fc::State::Idle) return "busy (trigger running)";
    if (c.id == I::UfcsSet) {
        const auto f = static_cast<ufcs::Field>(c.a[1]);
        const fctrig::TuneSet s = fctrig::ufcs_set(f, c.a[0]);
        if (s == fctrig::TuneSet::Busy) return "busy (probe running)";
        if (s == fctrig::TuneSet::Range) return "value out of range for this field";
        ufcs_tune_line(c, f);
    } else if (c.id == I::UfcsReset) {
        if (!fctrig::ufcs_tuner_reset()) return "busy (probe running)";
        Line l(c, "ufcs.reset");
        l.t("ok", true);
        l.send();
    } else {   // UfcsProbe
        const ufcs::ProbeStart s = fctrig::ufcs_probe_start();
        Line l(c, "ufcs.probe");
        l.t("started", s == ufcs::ProbeStart::Ok);
        l.s("err", ufcs::name(s));
        l.send();
        return s == ufcs::ProbeStart::Ok ? nullptr : "probe refused (busy, inline, not 5 V, no monitor or bus resetting)";
    }
    return nullptr;
}

// PD verbs on the existing sink engine: it owns messages, GoodCRC, retries and timeouts. The result is
// the engine's refusal (if any); the outcome arrives asynchronously (`state`, `pd caps`).
const char* lab_pd(const lab::Cmd& c) {
    using I = lab::Id;
    if (const char* why = lab_guard(false)) return why;
    pd::Choice ch;
    if (c.id == I::PdRequest) {
        ch.pos = static_cast<std::uint8_t>(c.a[0]);
        if (c.na > 1) ch.mv = static_cast<std::uint16_t>(c.a[1]);
        if (c.na > 2) ch.ma = static_cast<std::uint16_t>(c.a[2]);
    } else if (c.id == I::PdPps || c.id == I::PdAvs) {
        const pd::PdoType want = c.id == I::PdPps ? pd::PdoType::Pps : pd::PdoType::Avs;
        const pd::SourceCaps& caps = fusb302::caps();
        for (unsigned i = 0; caps.valid && i < caps.num_pdos && !ch.pos; i++)
            if (caps.pdos[i].type == want) ch.pos = static_cast<std::uint8_t>(i + 1);
        if (!ch.pos) return "the source offers no such PDO";
        ch.mv = static_cast<std::uint16_t>(c.a[0]);
        ch.ma = static_cast<std::uint16_t>(c.a[1]);
    }
    pd::Err e = pd::Err::Ok;
    bool ok = true;
    if (c.id == I::PdGet) {
        if (c.word != "source-cap") return "pd get: only source-cap";
        ok = fusb302::get_source_caps(c.o.confirm);
    } else if (c.id == I::PdReread) {
        ok = fusb302::reread_start(c.na ? static_cast<std::uint8_t>(c.a[0]) : pd::kRereadDefault);   // PD mode only; the result is a [REREAD] line and a Black Box mark
    } else if (c.id == I::PdVdm) {
        ok = fusb302::identify_start(c.o.confirm);   // runs Discover Identity, SVIDs and Modes in turn
    } else {
        e = fusb302::sink_request(ch, c.o.confirm);   // pd epr: pos 0 = enter EPR and read the list
    }
    Line l(c, lab::name(c.id));
    l.t("started", ok && e == pd::Err::Ok);
    l.s("err", pd::err_name(e));
    l.send();
    return ok ? nullptr : "refused (no CC, or inline: an inline K2 never transmits)";
}

// LCD SPI clock lab switch: `lcd show` reads the divider and the paint totals, `lcd spi <4|8>` sets it (RAM only, /8 at
// every boot) and repaints the whole screen. It never touches VBUS or the lines, so it needs no lab_guard: it also works
// during a charger test, where a lighter wire time is what is being measured.
const char* lab_lcd(const lab::Cmd& c) {
    if (c.id == lab::Id::LcdSpi) {
        if (!lcd::set_spi_div(c.a[0])) return "divider not changed (4 or 8 only, and not while a screenshot or stream is open)";
        g_ui->repaint(systick::millis());
    }
    const unsigned div = lcd::spi_div();
    const lcd::SpiDiv sd = div == 4 ? lcd::SpiDiv::Div4 : lcd::SpiDiv::Div8;
    const lcd::DmaInfo d = lcd::dma_info();
    const paint::Reading t = paint::meter.total();
    Line l(c, lab::name(c.id));
    l.u("div", div);
    l.u("mhz", lcd::kApb1Hz / 1'000'000u / div);
    l.u("service_px", lcd::service_pixels(sd));
    l.u("dma_service_px", lcd::dma_service_px(sd));
    l.s("dma", !d.enabled ? "disabled" : d.ok ? "ok" : "stalled");
    l.u("transfers", d.transfers);
    l.u("stalls", d.stalls);
    l.u("paint_px", t.px);
    l.u("paint_ms", t.paint_ms);
    l.u("fs_ms", t.fs_ms);
    l.u("blocked_ms", gap::blocked.run_peak(systick::millis()));   // the run's longest blocked-paint window
    l.send();
    return nullptr;
}

const char* lab_run(const lab::Cmd& c) {
    using I = lab::Id;
    if (const char* why = check_opts(c)) return why;
    switch (c.id) {
    case I::LcdShow: case I::LcdSpi: return lab_lcd(c);
    case I::AccpPing: case I::AccpRead: case I::AccpWrite: case I::AccpDump: return lab_accp(c);
    case I::Qc3Up: case I::Qc3Down: case I::Qc3pUp: case I::Qc3pDown: return lab_qc_pulse(c);
    case I::Qc2: case I::Qc3Set: case I::Qc3pSet: case I::Afc: case I::Fcp: case I::Scp: case I::VoocWake:
    case I::UfcsRequest: case I::VivoRequest: return lab_engine(c);
    case I::VoocListen: case I::VoocAnswer: return lab_vooc_frames(c);
    case I::Dpdm: return lab_dpdm(c);
    case I::Vbus: return lab_vbus(c);
    case I::PdCaps: return lab_pd_caps(c);
    case I::UfcsPing: case I::UfcsCaps: case I::UfcsRaw: return lab_ufcs(c);
    case I::UfcsSet: case I::UfcsShow: case I::UfcsReset: case I::UfcsProbe: case I::UfcsResult: return lab_ufcs_tune(c);
    case I::PdRequest: case I::PdPps: case I::PdAvs: case I::PdEpr: case I::PdGet: case I::PdReread: case I::PdVdm: return lab_pd(c);
    case I::PdHardReset:
        return g_remote->pd_hard_reset().status == remote::Status::Ok ? nullptr : "refused (sink attached or busy)";
    default: return "not implemented in this firmware (see docs/PROTOCOL_LAB.md)";
    }
}

struct Hw final : shell::Backend {
    void write(std::string_view s) override { put(s); }

    void state() override {
        char b[200];
        std::size_t n = 0;
        auto add = [&](const char* f, auto... a) {
            n += fmt::format(std::span<char>(b + n, sizeof b - n), f, a...);
        };
        if (const auto pm = power::read()) add("V=%dmV I=%dmA", int(pm->vbus_mv), int(pm->current_ma));
        else add("V=n/a I=n/a");
        if (const auto r = adc::read()) {
            add(" dp=%d dm=%d cc1=%d cc2=%d", int(r->dp_mv), int(r->dm_mv), int(r->cc1_mv), int(r->cc2_mv));
        } else {
            add(" dp=n/a dm=n/a cc1=n/a cc2=n/a");
        }
        add(" fc=%s", fastcharge::name(fastcharge::read()));
        const fctrig::Status t = fctrig::status();
        add(" trig=%s", fc::name(t.state));
        const pd::SourceCaps& c = fusb302::caps();
        const pd::Contract& k = fusb302::contract();
        add(" pd=%dpdo", c.valid ? int(c.num_pdos) : 0);
        if (k.valid) add(" %dmV/%dmA", int(k.mv), int(k.op_ma));
        add("\r\n");
        put({b, n});
    }

    void log_stream(bool on) override { g_log_on = on; }

    const char* drive(bool dp, dpdm::Drive d) override {
        if (d != dpdm::Drive::HiZ) {
            if (fctrig::status().state != fc::State::Idle || g_ui->ports_busy()) return "busy";
            if (g_ui->link().inline_mode()) return "a sink is attached";
            if (power::max_vbus_mv() == 0) return "no power monitor";
        }
        if (dp) {
            dpdm::set_dp(d);
            g_raw_dp = d != dpdm::Drive::HiZ;
        } else {
            dpdm::set_dm(d);
            g_raw_dm = d != dpdm::Drive::HiZ;
        }
        return nullptr;
    }

    const char* cc_open(std::uint32_t ms) override {
        return fusb302::cc_open(ms) ? nullptr : "a sink is attached";
    }

    const char* pd_silent(std::uint32_t seconds) override {
        if (g_ui->ports_busy()) return "busy";
        fusb302::legacy_only(true);   // blocks 300 ms
        g_silent_until = systick::millis() + seconds * 1000u;
        if (!g_silent_until) g_silent_until = 1;
        return nullptr;
    }

    const char* pd_reset() override {
        return g_remote->pd_hard_reset().status == remote::Status::Ok ? nullptr : "refused (sink attached or busy)";
    }

    const char* pd_rescan() override {
        fusb302::rescan();
        return nullptr;
    }

    const char* fc_trigger(fc::Proto p, int mv, bool confirm) override {
        const remote::Status s = g_remote->fc_cmd(remote::kFcTrigger, static_cast<std::uint8_t>(p),
                                                  static_cast<std::uint16_t>(mv), confirm ? remote::kFcConfirm : 0);
        if (s == remote::Status::Ok) g_fc_started = true;
        return fc_reason(s);
    }

    const char* lab(const lab::Cmd& c) override { return lab_run(c); }

    const char* fc_reset() override {
        fc_stop();
        return nullptr;
    }

    // The same one-call path as the HID `stop` command (Remote::stop); on top of it the raw D+/D- levels this shell drove go
    // back to Hi-Z, since they are shell state the remote does not know about.
    const char* stop() override {
        const remote::StopInfo s = g_remote->stop();
        g_fc_started = false;
        const bool raw = g_raw_dp || g_raw_dm;
        if (raw) raw_to_hiz();
        char b[96];
        const std::size_t n = fmt::format(std::span<char>(b, sizeof b), "stop active=%u vbus=%d fc_release_busy=%u raw=%u\r\n",
                                          static_cast<unsigned>(s.active), static_cast<int>(s.vbus_mv),
                                          s.fc_release_busy ? 1u : 0u, raw ? 1u : 0u);
        put({b, n});
        return nullptr;
    }

    // Read-only on the rings and the CDC port: no arming rules, nothing it could disturb.
    const char* stream(unsigned mask, unsigned hz, bool history) override {
        if (mask == 0) {
            if (!dbg_hw::running()) return "no stream running";
            stream_stop();
            return nullptr;
        }
        if (dbg_hw::running()) return "already streaming (stream off first)";
        dbg_hw::start(static_cast<std::uint8_t>(mask), static_cast<std::uint16_t>(hz), history);
        return nullptr;
    }

    // `trace stream` / `trace stop` stay as the black-box-only alias of `stream on trace` / `stream off`.
    const char* trace(int mode) override {
        if (mode == 0) return stream(0, 0, false);
        return stream(dbg::kTrace, 0, mode == 2);
    }

    // Same arming rules as `lab sniff arm`: nothing may own the ports, and no raw level is driven.
    const char* sniff(int mode, std::uint32_t rate) override {
        if (mode == 0) {
            if (!dpsniff::streaming()) return "no stream running";
            sniff_stop();
            return nullptr;
        }
        if (dpsniff::armed()) return "a capture is already armed";
        if (mode == 1 && (g_ui->ports_busy() || fctrig::status().state != fc::State::Idle || fctrig::scan_running() ||
                          g_raw_dp || g_raw_dm || g_fc_started)) return "busy";
        const dpsniff::ArmResult r = mode == 1 ? dpsniff::arm_stream() : dpsniff::arm_synth(rate);
        return r == dpsniff::ArmResult::Ok ? nullptr : r == dpsniff::ArmResult::NoRam ? "no RAM (drive or net mode)" : "busy";
    }

    // Inline VOOC monitor: the same RAM switch and summary as remote FC op 4. Raw reply words in hex, no units.
    const char* vooc_monitor(int mode) override {
        if (mode == 0 || mode == 1) dpsniff::monitor_enable(mode == 1);
        const dpsniff::MonitorInfo m = dpsniff::monitor_info();
        const vooc::Summary& s = m.s;
        char b[120];
        std::size_t n = fmt::format(std::span<char>(b, sizeof b),
                                    "vooc %s %s sessions=%u active=%u wakes=%u partials=%u edges=%u frames=%u\r\n",
                                    m.enabled ? "on" : "off", m.running ? "running" : "idle", unsigned(s.sessions),
                                    unsigned(s.active), unsigned(s.wakes), unsigned(s.partials), unsigned(s.edges),
                                    unsigned(s.frames));
        put({b, n});
        n = fmt::format(std::span<char>(b, sizeof b),
                        "vooc hs=%u/%u ident=0x%02X/0x%03X pairs=%u r82=0x%03X min=0x%03X max=0x%03X n=%u p82_us=%u/%u n=%u\r\n",
                        unsigned(s.handshake), unsigned(s.hs_pairs), unsigned(s.ident), unsigned(s.ident_reply),
                        unsigned(s.pairs), unsigned(s.r82_last), unsigned(s.r82_min), unsigned(s.r82_max),
                        unsigned(s.r82_n), unsigned(s.p82_last_us), unsigned(s.p82_mean_us()), unsigned(s.p82_n));
        put({b, n});
        for (std::size_t i = 0; i < s.ncmds && i < s.cmds.size(); i++) {
            n = fmt::format(std::span<char>(b, sizeof b), "vooc cmd 0x%02X n=%u last=0x%03X\r\n", unsigned(s.cmds[i].cmd),
                            unsigned(s.cmds[i].count), unsigned(s.cmds[i].last));
            put({b, n});
        }
        if (s.other) {
            n = fmt::format(std::span<char>(b, sizeof b), "vooc other=%u (table full)\r\n", unsigned(s.other));
            put({b, n});
        }
        return nullptr;
    }
};

Hw g_hw;
shell::Shell g_shell{g_hw};

// The session ended: back to 5 V and passive idle.
void session_end() {
    g_session = false;
    g_log_on = false;
    if (dpsniff::streaming()) dpsniff::cancel();
    dbg_hw::stop();
    if (g_fc_started) fc_stop();
    silent_end();
    raw_to_hiz();
}

}  // namespace

void init(Remote& remote, Ui& ui) {
    if (!usbcfg::current.shell) return;
    g_remote = &remote;
    g_ui = &ui;
    dbg_hw::init(remote, ui);
    usb::set_log_tap(tap);
}

void poll() {
    if (!g_remote) return;
    const bool up = session_up();
    if (!up) {
        if (g_session) session_end();
        return;
    }
    if (!g_session) {
        g_session = true;
        put("open-k2 shell, 'help' lists the commands\r\n");
    }
    // A raw level above 5 V-class signalling is only safe under the monitor's ceiling: drop it if VBUS got there.
    if (g_raw_dp || g_raw_dm) {
        const auto pm = power::read();
        if (!pm || pm->vbus_mv > power::max_vbus_mv() || (!g_fc_started && pm->vbus_mv > kLabAbortMv)) raw_to_hiz();
    }
    if (g_silent_until && static_cast<std::int32_t>(systick::millis() - g_silent_until) >= 0) silent_end();
    std::array<char, 32> buf;
    for (std::uint32_t n; (n = tud_cdc_read(buf.data(), buf.size())) > 0;)
        for (std::uint32_t i = 0; i < n; i++) g_shell.feed(buf[i]);
    // Edge stream: whole frames while the FIFO has room, a few per call, never waiting.
    if (dpsniff::streaming()) {
        dpsniff::stream_service();
        std::array<std::uint8_t, edges::kFrameMax> f;
        for (int i = 0; i < 8 && tud_cdc_write_available() >= f.size(); i++) {
            const std::size_t n = dpsniff::stream_frame(f);
            if (n == 0) break;
            tud_cdc_write(f.data(), static_cast<std::uint32_t>(n));
        }
    }
    // Debug stream (k2/core/dbg_stream.hpp): whole frames while the FIFO has room, a bounded number per pass, never waiting.
    if (dbg_hw::running())
        dbg_hw::service(tud_cdc_write_available(), [](std::span<const std::uint8_t> f) {
            tud_cdc_write(f.data(), static_cast<std::uint32_t>(f.size()));
        });
    tud_cdc_write_flush();
}

}  // namespace k2::shell_hw
