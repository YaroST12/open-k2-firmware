// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Transition capture driver: a high-resolution flight recorder for PD voltage transitions. While armed it puts the
// power monitor into its fast mode and samples VBUS (power monitor) and CC1/CC2 (ADC ring) about every millisecond
// into a ring of the last ~1.9 s, with PD messages (Request, Accept, PS_RDY, resets, attach / detach) as markers on
// the same microsecond timeline. A trigger (a failed charger-test step, a Hard Reset, a detach, or by hand) keeps
// recording for 400 ms more and freezes the capture. The charger test can ask for the frozen capture to be kept
// on the littlefs volume (k2/pd/transcap_store.hpp), where the step's detail screen finds it again.
//
// The ring lives in the mode_ram overlay (tenant Trans): arming is refused while Net, Drive or Sniff own it. The
// pure parts (ring, codec, thumbnail, store) are in transcap_calc.hpp / transcap_store.hpp. Everything here runs in
// the main loop; sampling is cooperative (poll() runs once per loop pass, every millisecond or sooner), the
// timestamps record the real spacing, including the gaps of a long screen redraw.
#pragma once
#include <cstddef>
#include <cstdint>
#include <span>

#include "k2/pd/transcap_calc.hpp"
#include "k2/pd/transcap_store.hpp"

namespace k2::transcap {

enum class ArmResult : std::uint8_t { Ok, Busy, NoMonitor };
const char* name(ArmResult r);

struct Status {
    State state = State::Off;
    Reason reason = Reason::None;
    bool fast = false;                 // power monitor in fast mode (INA226)
    bool keep = false;                 // a persist is pending for the capture
    std::uint16_t samples = 0;
    std::uint8_t markers = 0;
    std::uint32_t lost = 0;            // samples that fell out of the ring
    std::uint32_t span_us = 0;         // first to last sample
    std::uint32_t max_gap_us = 0;      // largest sample spacing since arm
    std::uint32_t trigger_age_ms = 0;  // since the trigger, 0 when not triggered
    std::uint32_t saved_seq = 0;       // stored copy of this capture (0 none)
};

// Starts a fresh recording (an armed or frozen capture is dropped). Claims the overlay and the fast VBUS mode,
// registers the PD event sink. Refused (Busy) while Net / Drive / Sniff own the overlay.
ArmResult arm(std::uint8_t step = 0, std::uint16_t arg = 0);   // step / arg label the Arm marker (PDO, mV)
// Stops: restores the power monitor, drops the capture, releases the overlay.
void disarm();
// disarm() only while still just recording: a capture in its post window, or frozen and waiting to be kept, is left
// to finish (the main loop writes it and releases the overlay by itself). For callers that end a run.
void disarm_if_armed();
// Starts the post window of an armed capture; the reason sticks. False when not armed.
bool trigger(Reason r);
// True once the capture has a trigger (post window or frozen), whoever pulled it.
bool triggered();
// Marker from the caller (charger test: the step and the requested voltage). Ignored unless recording.
void mark(Mark code, std::uint8_t step = 0, std::uint16_t arg = 0);
// Asks for the capture to be written to the volume once it has frozen (run id + step index identify it).
// The overlay is released again after the write succeeded. Only honoured for a triggered capture (post window or
// frozen): a capture still just recording has nothing to keep.
void keep(std::uint32_t run_id, std::uint16_t step);
// Main loop: one sample when due, the arm timeout, persistence of a kept capture.
void poll();

Status status();
bool active();   // armed, in the post window or frozen (the overlay is held)

// The live capture's persisted stream (transcap_calc.hpp encode()) from byte `off`; 0 once past the end or when
// there is no frozen capture. Frozen only: an armed ring is still changing.
std::size_t read_live(std::uint32_t off, std::span<std::uint8_t> out);

// Writes the frozen capture now (the remote's `save`), regardless of quiet time. Returns its seq or 0.
std::uint32_t save_now(std::uint32_t run_id, std::uint16_t step);

// The volume: newest first / by run and step / by seq.
std::size_t list_stored(std::span<Meta> out);
bool find_stored(std::uint32_t run_id, std::uint16_t step, Meta& m);
bool get_stored(std::uint32_t seq, Meta& m);
std::size_t read_stored(std::uint32_t seq, std::uint32_t off, std::span<std::uint8_t> out);
void clear_stored();

}  // namespace k2::transcap
