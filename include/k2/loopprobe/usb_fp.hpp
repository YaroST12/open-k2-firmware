// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Loop probe "Host report", USB half: a compact trace of how a host enumerates the K2's HID device (the control requests in
// order, bus resets, suspend / resume, SOF seen) and a pure classifier that guesses the host's operating system from it.
// Host-tested in tests/test_usb_fp.cpp over synthetic traces. The recorder is fed from the USB interrupt
// (src/loopprobe/host_report_hw.cpp, K2_HOST_REPORT=1 only).
//
// CLASSIFIER RULES ARE UNVERIFIED GUESSES: no real capture of any host was available when they were written. They encode
// commonly described enumeration habits (first GET_DESCRIPTOR(Device) length, the second bus reset before SET_ADDRESS, the
// MS OS 1.0 string 0xEE, the device qualifier, string request lengths, strings read again after SET_CONFIGURATION) and must be
// re-derived from real traces (docs/LOOP_PROBE.md, "Host report", hardware plan) before anyone trusts a label.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace k2::loopprobe::usbfp {

inline constexpr std::size_t kEvents = 24;

enum class Ev : std::uint8_t { Setup, Reset, Suspend, Resume };

// One trace entry: a SETUP packet (the 8 raw bytes, minus nothing) or a bus event, stamped in ms after the first bus reset.
struct Event {
    std::uint16_t t_ms = 0;
    std::uint8_t kind = 0;       // Ev
    std::uint8_t req_type = 0;   // bmRequestType (Setup)
    std::uint8_t req = 0;        // bRequest
    std::uint16_t value = 0, index = 0, length = 0;
};

inline constexpr std::uint8_t kFlagSof = 1u << 0;          // the frame counter moved (a host sends SOFs)
inline constexpr std::uint8_t kFlagConfigured = 1u << 1;   // SET_CONFIGURATION was accepted (the device is mounted)
inline constexpr std::uint8_t kFlagOverflow = 1u << 2;     // more events than kEvents: the rest are counted, not stored

struct Trace {
    std::uint8_t n = 0;
    std::uint16_t total = 0;     // events seen (saturated)
    std::uint8_t flags = 0;
    std::array<Event, kEvents> ev{};
};

// Fills a Trace from the USB interrupt. Times are relative to the first bus reset (or the first event).
class Recorder {
public:
    void clear() { *this = Recorder{}; }
    void setup(const std::uint8_t (&pkt)[8], std::uint32_t now_ms);
    void bus_reset(std::uint32_t now_ms) { add(Ev::Reset, now_ms); }
    void suspend(std::uint32_t now_ms) { add(Ev::Suspend, now_ms); }
    void resume(std::uint32_t now_ms) { add(Ev::Resume, now_ms); }
    void flag(std::uint8_t f) { trace_.flags = std::uint8_t(trace_.flags | f); }
    const Trace& trace() const { return trace_; }
    std::uint32_t last_ms() const { return last_; }   // time of the newest event (absolute ms), 0 = none

private:
    void add(Ev kind, std::uint32_t now_ms, const std::uint8_t* pkt = nullptr);
    Trace trace_{};
    std::uint32_t t0_ = 0, last_ = 0;
    bool started_ = false;
};

// Standard request codes and descriptor types the features read.
inline constexpr std::uint8_t kReqGetDescriptor = 6, kReqSetAddress = 5, kReqSetConfiguration = 9;
inline constexpr std::uint8_t kDescDevice = 1, kDescConfig = 2, kDescString = 3, kDescQualifier = 6, kDescBos = 15;

// What the classifier looks at, also reported (the JSON shows the evidence beside the label).
struct Features {
    std::uint8_t setups = 0;
    std::uint16_t first_dev_len = 0;       // wLength of the first GET_DESCRIPTOR(Device), 0 = none
    std::uint8_t resets_before_addr = 0;   // bus resets before SET_ADDRESS
    std::uint16_t addr_ms = 0;             // SET_ADDRESS time (ms after the first reset), 0xFFFF = none
    bool ms_os = false;                    // GET_DESCRIPTOR(String 0xEE): the MS OS 1.0 descriptor
    bool qualifier = false;                // GET_DESCRIPTOR(Device_Qualifier)
    bool bos = false;                      // GET_DESCRIPTOR(BOS)
    bool configured = false;               // SET_CONFIGURATION seen
    std::uint16_t str_len = 0;             // wLength of the first non-zero-index string request, 0 = none
    bool str_short_first = false;          // a string index read with a short wLength (<= 4) before its full read
    std::uint8_t strings_after_config = 0; // string requests after SET_CONFIGURATION (a userspace re-read)
    bool suspended = false;
    bool sof = false;
};
Features features(const Trace& t);

enum class Os : std::uint8_t { Unknown, ChargeOnly, Windows, MacOs, Linux, Android, Ios, ChromeOs };
// The guess (see the warning at the top): `why` names the rule that fired (a short static string).
Os classify(const Trace& t, const char** why = nullptr);
const char* os_name(Os o);

}  // namespace k2::loopprobe::usbfp
