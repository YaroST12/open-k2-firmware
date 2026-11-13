// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Debug shell line parser, see k2/core/shell.hpp.
#include "k2/core/shell.hpp"
#include "k2/core/build_config.h"

namespace k2::shell {
namespace {

constexpr std::size_t kMaxTokens = 12;

constexpr std::string_view kHelp =
    "state                       V/A, D+/D-, CC, fast-charge, PD\r\n"
    "diag                        Black Box diagnosis of the PD session (read-only)\r\n"
    "log on|off                  stream the chip log here\r\n"
    "dp|dm hiz|0|0.6|3.3         raw D+/D- drive (0.6 and 3.3 need confirm)\r\n"
    "cc open <ms>                open the CC lines (1..1000 ms)\r\n"
    "pd silent <s>               legacy only, no PD answers (1..600 s)\r\n"
    "pd reset | pd rescan        PD hard reset / CC rescan\r\n"
    "fc trigger <proto> <mV> [confirm]   qc2 qc3 qc3+ afc fcp scp vooc svooc svooc65 ufcs vivo\r\n"
    "fc reset                    back to 5 V, passive idle\r\n"
    "fc vooc [on|off]            inline VOOC monitor summary (passive, raw words; on/off RAM only)\r\n"
    "stop                        cancel test, stop FC, release PD to 5 V, disarm spoof (no confirm)\r\n"
    "sniff stream | sniff stop  passive D+/D- edge stream (binary frames, tools/sniff_stream.py)\r\n"
    "trace stream [all] | trace stop   PD black box records as binary frames (tools/pd_trace.py stream)\r\n"
    "stream on [log|trace|power[=Hz]|run|all] [old] | stream off   everything, one binary stream (tools/k2_stream.py)\r\n"
    "lab: accp|qc2|qc3|qc3+|afc|fcp|scp|vooc|ufcs|vivo|dpdm|vbus|cc rp|rd|pd caps|request|... see docs/PROTOCOL_LAB.md\r\n"
    "ufcs show|set <field> <v>|reset|probe|result   UFCS frame/timing tuner (RAM) and the 5 V probe, no confirm\r\n"
    "lcd show | lcd spi <4|8>    LCD SPI clock divider (RAM only, /8 at boot) and paint totals, no confirm\r\n"
    "twin list|predict [n]|status|cancel|learn identify|sniff [dry] [name]|rename n name   device twins (docs/DEVICE_TWIN.md)\r\n"
    "twin run [n|all] confirm | twin delete n confirm   the live proof negotiates with the charger as the twin\r\n"
#if K2_PDSTRESS
    "pdstress start [--mask m --cycles pct --pace 0..2 --max-s s --max-trans n] confirm | status | result [off] | stop   PD Stress, docs/PD_STRESS.md\r\n"
#endif
    "above 5 V needs a trailing confirm; closing the port returns to 5 V\r\n";

constexpr std::array<std::string_view, static_cast<std::size_t>(fc::Proto::Count)> kProtos = {
    "qc2", "qc3", "qc3+", "afc", "fcp", "scp", "vooc", "svooc", "svooc65", "ufcs", "vivo"};

bool is_space(char c) { return c == ' ' || c == '\t'; }

char lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c; }

bool same(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); i++) if (lower(a[i]) != b[i]) return false;
    return true;
}

// Unsigned decimal, at most 6 digits; false on anything else.
bool number(std::string_view s, std::uint32_t& out) {
    if (s.empty() || s.size() > 6) return false;
    std::uint32_t v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        v = v * 10 + static_cast<std::uint32_t>(c - '0');
    }
    out = v;
    return true;
}

struct Tokens {
    std::array<std::string_view, kMaxTokens> t{};
    std::size_t n = 0;
    bool overflow = false;
};

Tokens split(std::string_view line) {
    Tokens r;
    std::size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && is_space(line[i])) i++;
        if (i >= line.size()) break;
        const std::size_t start = i;
        while (i < line.size() && !is_space(line[i])) i++;
        if (r.n == kMaxTokens) { r.overflow = true; break; }
        r.t[r.n++] = line.substr(start, i - start);
    }
    return r;
}

void err(Backend& b, const char* why) {
    b.write("err: ");
    b.write(why);
    b.write("\r\n");
}

void done(Backend& b, const char* result) {
    if (result) err(b, result);
    else b.write("ok\r\n");
}

// D+/D- locked by the backend: the refusal is written and true returned; the command must not reach the backend.
bool locked(Backend& b) {
    const char* why = b.lines_locked();
    if (why) err(b, why);
    return why != nullptr;
}

// Consumes an optional trailing `confirm` from [first, n); false when something else follows.
bool trailing_confirm(const Tokens& k, std::size_t first, bool& confirm) {
    confirm = false;
    if (k.n == first) return true;
    if (k.n == first + 1 && same(k.t[first], "confirm")) { confirm = true; return true; }
    return false;
}

void cmd_drive(Backend& b, const Tokens& k, bool dp) {
    if (k.n < 2) return err(b, "usage: dp|dm hiz|0|0.6|3.3");
    dpdm::Drive d;
    bool needs_confirm = false;
    if (same(k.t[1], "hiz")) d = dpdm::Drive::HiZ;
    else if (k.t[1] == "0") d = dpdm::Drive::V0;
    else if (k.t[1] == "0.6") { d = dpdm::Drive::V0p6; needs_confirm = true; }
    else if (k.t[1] == "3.3") { d = dpdm::Drive::V3p3; needs_confirm = true; }
    else return err(b, "level is hiz, 0, 0.6 or 3.3");
    bool confirm;
    if (!trailing_confirm(k, 2, confirm)) return err(b, "unexpected argument");
    if (needs_confirm && !confirm) return err(b, "this level needs a trailing confirm");
    if (locked(b)) return;
    done(b, b.drive(dp, d));
}

void cmd_cc(Backend& b, const Tokens& k) {
    std::uint32_t ms;
    if (k.n != 3 || !same(k.t[1], "open") || !number(k.t[2], ms) || ms < 1 || ms > 1000)
        return err(b, "usage: cc open <1..1000 ms>");
    done(b, b.cc_open(ms));
}

void cmd_pd(Backend& b, const Tokens& k) {
    std::uint32_t s;
    if (k.n == 3 && same(k.t[1], "silent")) {
        if (!number(k.t[2], s) || s < 1 || s > 600) return err(b, "usage: pd silent <1..600 s>");
        return done(b, b.pd_silent(s));
    }
    if (k.n == 2 && same(k.t[1], "reset")) return done(b, b.pd_reset());
    if (k.n == 2 && same(k.t[1], "rescan")) return done(b, b.pd_rescan());
    err(b, "usage: pd silent <s> | reset | rescan");
}

void cmd_fc(Backend& b, const Tokens& k) {
    if (k.n == 2 && same(k.t[1], "reset")) return done(b, b.fc_reset());
    if (k.n >= 2 && same(k.t[1], "vooc")) {
        if (k.n == 2) return done(b, b.vooc_monitor(2));
        if (k.n == 3 && same(k.t[2], "on")) return locked(b) ? void() : done(b, b.vooc_monitor(1));
        if (k.n == 3 && same(k.t[2], "off")) return locked(b) ? void() : done(b, b.vooc_monitor(0));
        return err(b, "usage: fc vooc [on|off]");
    }
    if (k.n < 4 || !same(k.t[1], "trigger")) return err(b, "usage: fc trigger <proto> <mV> [confirm] | fc reset");
    std::size_t proto = kProtos.size();
    for (std::size_t i = 0; i < kProtos.size(); i++) if (same(k.t[2], kProtos[i])) proto = i;
    if (proto == kProtos.size()) return err(b, "unknown protocol");
    std::uint32_t mv;
    if (!number(k.t[3], mv) || mv < 1 || mv > static_cast<std::uint32_t>(kMaxMv))
        return err(b, "mV must be 1..36000");
    bool confirm;
    if (!trailing_confirm(k, 4, confirm)) return err(b, "unexpected argument");
    if (static_cast<int>(mv) > fc::kConfirmAboveMv && !confirm) return err(b, "above 5000 mV needs a trailing confirm");
    if (locked(b)) return;
    done(b, b.fc_trigger(static_cast<fc::Proto>(proto), static_cast<int>(mv), confirm));
}

void cmd_sniff(Backend& b, const Tokens& k) {
    std::uint32_t rate = 0;
    int mode = -1;
    if (k.n == 2 && same(k.t[1], "stream")) mode = 1;
    else if (k.n == 2 && same(k.t[1], "stop")) mode = 0;
    else if (k.n == 3 && same(k.t[1], "synth") && number(k.t[2], rate) && rate >= 1 && rate <= 100000) mode = 2;
    if (mode >= 0) return locked(b) ? void() : done(b, b.sniff(mode, mode == 2 ? rate : 0));
    err(b, "usage: sniff stream | sniff stop | sniff synth <1..100000 edges/s>");
}

void cmd_trace(Backend& b, const Tokens& k) {
    if (k.n == 2 && same(k.t[1], "stream")) return done(b, b.trace(1));
    if (k.n == 3 && same(k.t[1], "stream") && same(k.t[2], "all")) return done(b, b.trace(2));
    if (k.n == 2 && same(k.t[1], "stop")) return done(b, b.trace(0));
    err(b, "usage: trace stream [all] | trace stop");
}

// stream on [log|trace|power[=Hz]|run|all ...] [old] | stream off. Kind bits match k2::dbg::Mask. `all power=50` is fine
// (every kind, power at 50 Hz); naming one kind twice is a typo.
void cmd_stream(Backend& b, const Tokens& k) {
    constexpr const char* kUsage = "usage: stream on [log|trace|power[=1..100 Hz]|run|all] [old] | stream off";
    if (k.n == 2 && same(k.t[1], "off")) return done(b, b.stream(0, 0, false));
    if (k.n < 2 || !same(k.t[1], "on")) return err(b, kUsage);
    unsigned mask = 0, named = 0, hz = 0;
    bool history = false;
    for (std::size_t i = 2; i < k.n; i++) {
        unsigned bit = 0;
        std::uint32_t v = 0;
        const std::string_view t = k.t[i];
        if (same(t, "old")) {
            if (history) return err(b, kUsage);
            history = true;
            continue;
        }
        if (same(t, "log")) bit = 1;
        else if (same(t, "trace")) bit = 2;
        else if (same(t, "run")) bit = 8;
        else if (same(t, "all")) bit = 15;
        else if (same(t, "power")) bit = 4;
        else if (t.size() > 6 && same(t.substr(0, 6), "power=") && number(t.substr(6), v) && v >= 1 && v <= 100) {
            if (hz) return err(b, kUsage);
            hz = v;
            mask |= 4;
            continue;
        } else return err(b, kUsage);
        if (named & bit) return err(b, kUsage);
        named |= bit;
        mask |= bit;
    }
    done(b, b.stream(mask ? mask : 15, hz, history));
}

// A library number 1..12.
bool twin_seq(std::string_view s, std::uint8_t& out) {
    std::uint32_t v;
    if (!number(s, v) || v < 1 || v > kTwinMax) return false;
    out = static_cast<std::uint8_t>(v);
    return true;
}

// The rest of the line from token `first` on, as one name (single spaces inside are kept as typed); false when it is empty or too long.
bool twin_name(const Tokens& k, std::size_t first, std::string_view& name) {
    if (k.n <= first) return false;
    const char* b = k.t[first].data();
    const char* e = k.t[k.n - 1].data() + k.t[k.n - 1].size();
    name = std::string_view(b, static_cast<std::size_t>(e - b));
    return name.size() <= kTwinNameMax;
}

void cmd_twin(Backend& b, const Tokens& k) {
    constexpr const char* kUsage =
        "usage: twin list | predict [n|all] | run [n|all] confirm | status | cancel | learn identify|sniff [dry] [name] | "
        "learn inline confirm [spoof lib|builtin n] [dry] [name] | rename n name | delete n confirm";
    constexpr const char* kInlineUsage = "usage: twin learn inline confirm [spoof lib|builtin <n>] [dry] [name]";
    if (k.n < 2) return err(b, kUsage);
    const std::string_view s = k.t[1];
    TwinCmd c;
    if (same(s, "list") && k.n == 2) { c.op = TwinOp::List; return done(b, b.twin(c)); }
    if (same(s, "status") && k.n == 2) { c.op = TwinOp::Status; return done(b, b.twin(c)); }
    if (same(s, "cancel") && k.n == 2) { c.op = TwinOp::Cancel; return done(b, b.twin(c)); }
    if (same(s, "predict")) {
        c.op = TwinOp::Predict;
        if (k.n == 2 || (k.n == 3 && same(k.t[2], "all"))) c.all = true;
        else if (k.n != 3 || !twin_seq(k.t[2], c.seq)) return err(b, "usage: twin predict [1..12|all]");
        return done(b, b.twin(c));
    }
    if (same(s, "run")) {
        c.op = TwinOp::Run;
        std::size_t i = 2;
        if (k.n > i && same(k.t[i], "all")) { c.all = true; i++; }
        else if (k.n > i && !same(k.t[i], "confirm")) {
            if (!twin_seq(k.t[i], c.seq)) return err(b, "usage: twin run [1..12|all] confirm");
            i++;
        } else c.all = true;
        if (k.n == i) return err(b, "a live run negotiates with the charger and needs a trailing confirm");
        if (k.n != i + 1 || !same(k.t[i], "confirm")) return err(b, "usage: twin run [1..12|all] confirm");
        c.confirm = true;
        return done(b, b.twin(c));
    }
    if (same(s, "learn")) {
        if (k.n < 3) return err(b, kUsage);
        std::size_t i = 3;
        if (same(k.t[2], "identify")) c.op = TwinOp::Learn;
        else if (same(k.t[2], "sniff")) c.op = TwinOp::LearnSniff;
        else if (same(k.t[2], "inline")) {
            c.op = TwinOp::LearnInline;
            if (k.n == i || !same(k.t[i], "confirm"))
                return err(b, "an inline interrogation hard-resets the link twice: twin learn inline confirm [dry] [name]");
            c.confirm = true;
            i++;
            if (k.n > i && same(k.t[i], "spoof")) {
                std::uint32_t id;
                if (k.n < i + 3 || !number(k.t[i + 2], id) || id > 0xFFFFu || (same(k.t[i + 1], "lib") && id == 0))
                    return err(b, kInlineUsage);
                if (same(k.t[i + 1], "lib")) c.spoof_kind = 1;
                else if (!same(k.t[i + 1], "builtin")) return err(b, kInlineUsage);
                c.spoof = true;
                c.spoof_id = static_cast<std::uint16_t>(id);
                i += 3;
            }
        } else return err(b, "usage: twin learn identify|sniff|inline [confirm] [dry] [name]");
        if (k.n > i && same(k.t[i], "dry")) { c.dry = true; i++; }
        if (k.n > i && !twin_name(k, i, c.name)) return err(b, "name is at most 24 characters");
        return done(b, b.twin(c));
    }
    if (same(s, "rename")) {
        c.op = TwinOp::Rename;
        if (k.n < 4 || !twin_seq(k.t[2], c.seq)) return err(b, "usage: twin rename 1..12 <name>");
        if (!twin_name(k, 3, c.name)) return err(b, "name is at most 24 characters");
        return done(b, b.twin(c));
    }
    if (same(s, "delete")) {
        c.op = TwinOp::Delete;
        if (k.n < 3 || !twin_seq(k.t[2], c.seq) || k.n > 4) return err(b, "usage: twin delete 1..12 confirm");
        if (k.n == 3) return err(b, "a delete needs a trailing confirm");
        if (!same(k.t[3], "confirm")) return err(b, "usage: twin delete 1..12 confirm");
        c.confirm = true;
        return done(b, b.twin(c));
    }
    err(b, kUsage);
}

}  // namespace

void Shell::execute(std::string_view line) {
    const Tokens k = split(line);
    if (k.n == 0) return;
    if (k.overflow) return err(b_, "too many arguments");
    const std::string_view c = k.t[0];
    if (same(c, "help")) return b_.write(kHelp);
    if (same(c, "state")) return b_.state();
    if (same(c, "stop")) {
        if (k.n != 1) return err(b_, "usage: stop");
        return done(b_, b_.stop());
    }
    if (same(c, "log")) {
        if (k.n == 2 && same(k.t[1], "on")) { b_.log_stream(true); return b_.write("ok\r\n"); }
        if (k.n == 2 && same(k.t[1], "off")) { b_.log_stream(false); return b_.write("ok\r\n"); }
        return err(b_, "usage: log on|off");
    }
    if (same(c, "dp")) return cmd_drive(b_, k, true);
    if (same(c, "dm")) return cmd_drive(b_, k, false);
    const bool legacy_cc = same(c, "cc") && k.n > 1 && same(k.t[1], "open");
    const bool legacy_pd = same(c, "pd") && k.n > 1 && (same(k.t[1], "silent") || same(k.t[1], "reset") || same(k.t[1], "rescan"));
    if (legacy_cc) return cmd_cc(b_, k);
    if (legacy_pd) return cmd_pd(b_, k);
    if (lab::is_family(c)) {
        lab::Cmd cmd;
        if (const char* why = lab::parse(k.t.data(), k.n, cmd)) return err(b_, why);
        if (lab::drives_lines(cmd.id) && locked(b_)) return;
        return done(b_, b_.lab(cmd));
    }
    if (same(c, "cc")) return cmd_cc(b_, k);
    if (same(c, "pd")) return cmd_pd(b_, k);
    if (same(c, "fc")) return cmd_fc(b_, k);
    if (same(c, "sniff")) return cmd_sniff(b_, k);
    if (same(c, "trace")) return cmd_trace(b_, k);
    if (same(c, "stream")) return cmd_stream(b_, k);
    if (same(c, "twin")) return cmd_twin(b_, k);
    if (same(c, "diag")) {
        if (k.n != 1) return err(b_, "usage: diag");
        if (const char* why = b_.diag()) err(b_, why);
        return;
    }
    err(b_, "unknown command (help)");
}

void Shell::feed(char c) {
    const bool after_cr = last_cr_;
    last_cr_ = c == '\r';
    if (c == '\n' && after_cr) return;   // second half of CR LF
    if (c == '\r' || c == '\n') {
        if (overflow_) err(b_, "line too long");
        else execute({buf_.data(), len_});
        len_ = 0;
        overflow_ = false;
        return;
    }
    if (c == '\b' || c == 0x7F) {
        if (len_ > 0 && !overflow_) len_--;
        return;
    }
    if (c < ' ') return;
    if (len_ == buf_.size()) overflow_ = true;
    else if (!overflow_) buf_[len_++] = c;
}

}  // namespace k2::shell
