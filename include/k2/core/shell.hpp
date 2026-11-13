// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// USB serial debug shell: the line parser (pure, host-tested). A host terminal on the CDC-ACM port
// types one command per line; the parser checks the grammar, the argument limits and the confirm
// rule, and only then calls the Backend, which is the firmware glue over the existing engines
// (src/core/shell_hw.cpp). Nothing here touches hardware.
//
// Safety rules enforced here, same as the remote:
//  - anything that can move VBUS above 5 V (a trigger above 5000 mV, a raw D+/D- level of 0.6 V or
//    3.3 V, which together select a charger voltage) needs a trailing `confirm`;
//  - no target above kMaxMv (the INA226 36 V ceiling); the engine clamps to the real chip again.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "k2/drivers/dpdm.hpp"
#include "k2/fastcharge/fc_engine.hpp"
#include "k2/core/lab.hpp"

namespace k2::shell {

inline constexpr int kMaxMv = 36000;
inline constexpr std::size_t kLineMax = 128;

// A parsed `twin ...` line (k2/twin/wire.hpp has the same operations on the HID remote). `seq` is the library number 1..12;
// `all` = every twin; `name` points into the typed line (valid during the call only), at most kTwinNameMax characters.
// LearnInline: interrogate the device inline (twin_hw::inline_start; two Hard Resets, so `confirm` is set only when typed).
enum class TwinOp : std::uint8_t { List, Predict, Run, Status, Learn, LearnSniff, Delete, Rename, Cancel, LearnInline };
inline constexpr std::size_t kTwinNameMax = 24;
inline constexpr unsigned kTwinMax = 12;
struct TwinCmd {
    TwinOp op = TwinOp::List;
    std::uint8_t seq = 0;
    bool all = false;
    bool confirm = false;   // a run and a delete reach the backend only with it
    bool dry = false;       // learn: show what would be saved, save nothing
    bool spoof = false;     // learn inline: show the charger this cable identity for the run only
    std::uint8_t spoof_kind = 0;   // 0 = built-in index, 1 = saved library number (twin::wire::kInlineSpoofBuiltin / Saved)
    std::uint16_t spoof_id = 0;
    std::string_view name;
};

// Every action returns nullptr when done, else a short reason ("busy", "no monitor", ...).
struct Backend {
    virtual void write(std::string_view s) = 0;                // text to the terminal
    virtual void state() = 0;                                  // writes the one-line state itself
    virtual void log_stream(bool on) = 0;
    virtual const char* drive(bool dp, dpdm::Drive d) = 0;     // raw D+ (dp) or D- level
    virtual const char* cc_open(std::uint32_t ms) = 0;
    virtual const char* pd_silent(std::uint32_t seconds) = 0;
    virtual const char* pd_reset() = 0;
    virtual const char* pd_rescan() = 0;
    virtual const char* fc_trigger(fc::Proto p, int mv, bool confirm) = 0;
    virtual const char* fc_reset() = 0;
    // The one-call stop (k2/core/stop.hpp), the same path as the HID `stop` command and the lease expiry; prints its own report line.
    virtual const char* stop() = 0;
    // Passive D+/D- edge stream (k2/fastcharge/dpsniff.hpp): 0 stop, 1 stream, 2 synthetic source at `rate` edges/s (no pins).
    virtual const char* sniff(int mode, std::uint32_t rate) = 0;
    // PD black box stream (k2/core/trace_stream.hpp): 0 stop, 1 stream from now, 2 stream from the oldest ring record.
    virtual const char* trace(int mode) { (void)mode; return "trace not available"; }
    // Inline VOOC monitor (k2/fastcharge/dpsniff.hpp, passive): 0 off, 1 on (RAM switch), 2 keep; prints the summary itself.
    virtual const char* vooc_monitor(int mode) { (void)mode; return "vooc monitor not available"; }
    // Multiplexed live debug stream (k2/core/dbg_stream.hpp): `mask` 0 stops it, else the kinds to send as bits (1 log, 2 trace,
    // 4 power, 8 run); `power_hz` 0 = default; `history` starts from what the rings still hold instead of from now.
    virtual const char* stream(unsigned mask, unsigned power_hz, bool history) {
        (void)mask; (void)power_hz; (void)history;
        return "stream not available";
    }
    // Protocol lab command (k2/core/lab.hpp), already parsed and confirm-checked; prints its own result lines.
    virtual const char* lab(const lab::Cmd&) { return "lab not available"; }
    // Device twin command (k2/twin), already parsed and confirm-checked; prints its own result lines.
    virtual const char* twin(const TwinCmd&) { return "twin not available"; }
    // Black Box diagnosis (k2/pd/diag_live.hpp, read-only): prints the verdict, the findings and the healthy lines itself.
    virtual const char* diag() { return "diag not available"; }
    // D+/D- are locked (the guarded SuperVOOC trigger, k2/fastcharge/svooc_trig.hpp, drives D-): the reason, else nullptr.
    // While locked the parser refuses every command that drives or claims D+/D- or the sniffer (dp, dm, fc trigger,
    // fc vooc on|off, sniff, the lab commands of lab::drives_lines) before the backend sees it; stop and fc reset still pass.
    virtual const char* lines_locked() { return nullptr; }

protected:
    ~Backend() = default;
};

class Shell {
public:
    explicit Shell(Backend& b) : b_(b) {}

    // One received byte: edits the line (backspace), runs it on CR or LF. A CR LF pair is one end.
    void feed(char c);
    // Run one complete line.
    void execute(std::string_view line);

private:
    Backend& b_;
    std::array<char, kLineMax> buf_{};
    std::size_t len_ = 0;
    bool overflow_ = false;
    bool last_cr_ = false;
};

}  // namespace k2::shell
