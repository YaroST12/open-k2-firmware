// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Loop probe "Host report", USB half: see k2/loopprobe/usb_fp.hpp. The classifier rules are UNVERIFIED guesses.
#include "k2/loopprobe/usb_fp.hpp"

namespace k2::loopprobe::usbfp {

void Recorder::add(Ev kind, std::uint32_t now_ms, const std::uint8_t* pkt) {
    if (!started_) {
        started_ = true;
        t0_ = now_ms;
    }
    last_ = now_ms;
    if (trace_.total < 0xFFFF) trace_.total++;
    if (trace_.n >= kEvents) {
        trace_.flags = std::uint8_t(trace_.flags | kFlagOverflow);
        return;
    }
    Event& e = trace_.ev[trace_.n++];
    const std::uint32_t dt = now_ms - t0_;
    e.t_ms = std::uint16_t(dt > 0xFFFF ? 0xFFFF : dt);
    e.kind = std::uint8_t(kind);
    if (pkt) {
        e.req_type = pkt[0];
        e.req = pkt[1];
        e.value = std::uint16_t(pkt[2] | pkt[3] << 8);
        e.index = std::uint16_t(pkt[4] | pkt[5] << 8);
        e.length = std::uint16_t(pkt[6] | pkt[7] << 8);
        if (e.req_type == 0x00 && e.req == kReqSetConfiguration && e.value != 0) flag(kFlagConfigured);
    }
}

void Recorder::setup(const std::uint8_t (&pkt)[8], std::uint32_t now_ms) { add(Ev::Setup, now_ms, pkt); }

namespace {
bool is_get_desc(const Event& e) { return Ev(e.kind) == Ev::Setup && e.req_type == 0x80 && e.req == kReqGetDescriptor; }
std::uint8_t dtype(const Event& e) { return std::uint8_t(e.value >> 8); }
std::uint8_t didx(const Event& e) { return std::uint8_t(e.value); }
}  // namespace

Features features(const Trace& t) {
    Features f;
    f.addr_ms = 0xFFFF;
    f.sof = t.flags & kFlagSof;
    bool addressed = false, cfg_seen = false;   // in trace order (the flag says "at some point")
    for (std::size_t i = 0; i < t.n; i++) {
        const Event& e = t.ev[i];
        switch (Ev(e.kind)) {
        case Ev::Reset:
            if (!addressed && f.resets_before_addr < 0xFF) f.resets_before_addr++;
            continue;
        case Ev::Suspend: f.suspended = true; continue;
        case Ev::Resume: continue;
        case Ev::Setup: break;
        }
        if (f.setups < 0xFF) f.setups++;
        if (e.req_type == 0x00 && e.req == kReqSetAddress && !addressed) {
            addressed = true;
            f.addr_ms = e.t_ms;
        }
        if (e.req_type == 0x00 && e.req == kReqSetConfiguration) cfg_seen = true;
        if (!is_get_desc(e)) continue;
        switch (dtype(e)) {
        case kDescDevice:
            if (!f.first_dev_len) f.first_dev_len = e.length;
            break;
        case kDescQualifier: f.qualifier = true; break;
        case kDescBos: f.bos = true; break;
        case kDescString:
            if (didx(e) == 0xEE) { f.ms_os = true; break; }
            if (didx(e) != 0 && !f.str_len) f.str_len = e.length;
            if (didx(e) != 0 && cfg_seen && f.strings_after_config < 0xFF) f.strings_after_config++;
            if (e.length <= 4)
                for (std::size_t j = i + 1; j < t.n; j++)
                    if (is_get_desc(t.ev[j]) && t.ev[j].value == e.value && t.ev[j].length > e.length) {
                        f.str_short_first = true;
                        break;
                    }
            break;
        default: break;
        }
    }
    f.configured = cfg_seen || (t.flags & kFlagConfigured);
    return f;
}

// The rules, in order. UNVERIFIED: each one is a guess from commonly described habits, to be re-derived from real traces.
Os classify(const Trace& t, const char** why) {
    const Features f = features(t);
    const auto say = [&](Os o, const char* w) {
        if (why) *why = w;
        return o;
    };
    if (f.setups == 0) return f.sof ? say(Os::Unknown, "SOFs, no requests") : say(Os::ChargeOnly, "no SOF, no requests");
    if (f.ms_os) return say(Os::Windows, "MS OS 0xEE string read");
    if (f.first_dev_len == 64 && f.resets_before_addr >= 2 && f.qualifier)
        return say(Os::Windows, "64-byte first read, second reset, qualifier");
    if (f.first_dev_len == 8 || f.str_short_first)
        return f.configured ? say(Os::MacOs, "short first reads (Apple), configured")
                            : say(Os::Ios, "short first reads (Apple), never configured");
    if (f.first_dev_len == 64 && !f.qualifier && (f.str_len == 255 || f.str_len == 0)) {
        if (f.strings_after_config >= 2) return say(Os::Android, "Linux pattern, strings re-read after configuration");
        if (f.suspended) return say(Os::ChromeOs, "Linux pattern, suspended (autosuspend)");
        return say(Os::Linux, "64-byte first read, 255-byte strings");
    }
    return say(Os::Unknown, "no rule matched");
}

const char* os_name(Os o) {
    static constexpr const char* k[] = {"unknown", "charge-only", "Windows", "macOS", "Linux", "Android", "iOS", "ChromeOS"};
    return std::size_t(o) < std::size(k) ? k[std::size_t(o)] : "?";
}

}  // namespace k2::loopprobe::usbfp
