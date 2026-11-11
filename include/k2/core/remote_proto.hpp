// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Remote-control protocol over the 64-byte HID reports (EP1 OUT requests, EP1 IN responses).
// Pure: bytes in, bytes out, no hardware. The firmware adapter is src/core/remote.cpp, the host
// side is tools/k2_remote.py; both must match this file.
//
// All multi-byte fields are little endian. Every report is exactly 64 bytes, zero padded.
//
//   request  (host -> K2):  A5 4B | cmd | seq | len | payload[len]            len <= 59
//   response (K2 -> host):  A6 4B | cmd | seq | status | len | payload[len]   len <= 58
//
// The response magic starts with 0xA6: log lines on the same IN endpoint are ASCII text, so a
// report whose first byte has the high bit set is never a log. The bootloader frames
// start with FF 55, so neither magic collides with it.
//
// Command id ranges, one per subsystem: pick the next free id inside your range. tools/check_cmd_ids.py
// (make gate) parses these lines and fails on an id outside every range. Ids from 0x80 up are
// response-only stream frames (0x8A screenshot, 0x8B ripple), not commands.
//   range core      0x01-0x1F
//   range debug     0x20-0x2F
//   range system    0x30-0x3F
//   range pd        0x40-0x4F
//   range tools     0x50-0x5F
//   range storage   0x60-0x6F
//   range blackbox  0x70-0x7F
//
// Commands (payload layouts; sizes in bytes):
//   0x01 PING        req -            rsp proto(1) uptime_ms(4) git(24, NUL padded) version(rest)
//   0x02 BUTTON      req btn(1) ms(2) rsp btn(1) ms(2)      btn: 0 Back 1 Minus 2 Ok 3 Plus
//   0x03 GET_STATE   req -            rsp 40: page cc spec_rev num_pdos pd_valid chip pm_online
//                                    log_on (8 x u8), vbus_mv(i32) current_ma(i32) power_mw(i32)
//                                    energy_mwh(i32), then the analog front end: dp_mv(2) dm_mv(2)
//                                    cc1_mv(2) cc2_mv(2) aux_mv(2) temp_deci_c(i16) fc(1) flags(1)
//                                    fc = fastcharge::Protocol verdict; flags bit0 ADC reading
//                                    valid, bit1 board NTC temperature valid, bit2 NTC calibrated,
//                                    bit3 MCU temperature valid; then mcu_temp_deci_c(i16), then the link
//                                    block and the reset flags (53 bytes in all, see encode_state)
//                         req [part(1)] part 0 = the reply above; any other payload: BadLength (more than
//                                    one byte) or BadArg (unknown part), so a host can tell an old build
//                                    part 1, 31 bytes: mah(i32) elapsed_s(u32) proto_kind(1) limit_ma(2)
//                                    max_mv(2) typec_ma(2) busy(1) fc_release(1) topology(1) wait_kind(1)
//                                    pd_none_why(1) open_cc_mv(2, 0xFFFF = unknown) depth(1) stack(3, Menu
//                                    indices, 0xFF = none) sub(1: bits 1:0 Connection tab, bit2 popup, bit3
//                                    details) sel(1, 0xFF = none) count(1) home_window(1) flags(1: bit0 hold
//                                    card, 1 rotation notice, 2 e-marker scanning, 3 listening, 4 quiet,
//                                    5 identify: no device); enums: ui_types.hpp, home_model.hpp
//                                    part 2, 26 bytes: fusb_id(1) pd_rx(4) usb_resets(4) usb_isrs(4)
//                                    log_sent(4) log_dropped(4) poll_gap_max_ms(4) usb_configured(1)
//                                    part 3, 57 bytes: flags(1: bit i button i dim, bit4 strip covered), then
//                                    Back Minus Ok Plus x (short(7) long(7)), NUL padded ASCII
//   0x04 GET_PDOS    req [index(1)]   rsp valid spec_rev num_pdos index (4 x u8) and, when
//                                    index < num_pdos: type(1) raw(4) min_mv(2) max_mv(2)
//                                    max_ma(2) max_mw(4)
//   0x05 GET_EMARKER req [part(1)]    part 0: valid active vid(2) pid(2) xid(4) hw_rev fw_rev
//                                    max_a max_v max_w(2) num_vdos, then "vendor\0speed";
//                                    part 1: num_vdos raw VDOs (4 bytes each)
//   0x06 ACTION      req id(1)        rsp id(1)     id: 1 PD rescan 2 e-marker scan 3 energy reset
//   0x07 SET_PAGE    req page(1)      rsp page(1)
//   0x08 REBOOT      req -            rsp -         (the reset follows the response)
//   0x0A SCREENSHOT  req -            rsp width(2) height(2) strip_rows(1) num_strips(1) format(1)
//                                    format 1 = RLE565. After the response the K2 streams
//                                    num_strips strips of strip_rows full-width rows, top to bottom,
//                                    as stream frames (cmd 0x8A, same seq, status 0):
//                                      A6 4B 8A seq 00 len | strip(1) flags(1) | rle bytes (<= 56)
//                                    flags bit0 = last chunk of this strip, bit7 = capture aborted
//                                    (no more frames). Chunks of a strip concatenate to one RLE565
//                                    stream (rle565.hpp) of width * strip_rows RGB565 pixels.
//   0x09 LOG         req [on(1)]      rsp on(1)     without a payload it only reports the state
//   0x0B CHARGER_TEST req - (metadata) or start(1) run_id(4) revision(4) (rows).
//                                    rsp schema(1)=1 run_id(4) revision(4) state(1) reason(1)
//                                    total(1) current(1) start(1) n(1), then n<=2 rows:
//                                    group(1) pos(1) flags(1) target_mv(2) request_ma(2)
//                                    measured_mv(2) verdict(1) reason(1) detail(1) raw_pdo(4).
//                                    Enums match k2/chargertest/charger_test.hpp; flags bit0 continuous, bit1 AVS,
//                                    bit5 a deep-sweep row (its detail byte is then the recorded point count, not
//                                    the in-RAM kDetailSweep marker; the row's target_mv/measured_mv are the peak's
//                                    setpoint and the peak).
//                                    Metadata has start=n=0. current==total means no current step.
//                                    start<=total; start==total is an empty end page. BadArg otherwise;
//                                    mismatched run/revision is Busy (retry metadata, no partial export).
//                                    Read-only borrowed RAM; no start/cancel/poll/voltage or flash writes.
//                                    Schema 1 projects legacy groups/reasons; new unavailable/revision
//                                    rows are omitted. Selection and source evidence are unavailable.
//                                    Opt-in schema 2: req 2 (metadata), or 2 start run_id revision.
//                                    rsp the same first 15 bytes, schema=2, then run_selected(4),
//                                    draft_selected(4; bit 31 = the PPS deep sweep on, bit 30 = the AVS deep sweep
//                                    on - Options flags, not algorithm bits - they were always 0 before),
//                                    wire_rev[1:0]/draft_stage[5:2](1),
//                                    RMDO spec nibbles(1), EvidenceFlag(1), monitor_mv(2),
//                                    physical_cable_mv(2) ma(2), RMDO version nibbles(1),
//                                    source_session(4), raw_source_header(2), observed_pdos(1),
//                                    ps_rdy_timing(1: TimingFlag bits seen on any PD row - bit0 PS_RDY with VBUS out of
//                                    band, bit1 PS_RDY inferred, bit2 later than the spec maximum), ps_rdy_max(1: the worst
//                                    Accept -> PS_RDY time in 20 ms units, 255 = 5.1 s or more, 0 = none measured).
//                                    Header is 42 bytes; n<=1, row byte 0 is Algorithm, not Group.
//                                    Evidence is retained at run start; draft selection/stage are live.
//                                    Header 2 means PD3.x, not a minor revision. Source RMDO is a
//                                    self-report; EPR/SPR AVS are feature evidence, not conformance.
//                                    The view, req 3 (one byte; BadArg on firmware without it, BadLength when longer):
//                                    rsp stage(1) cursor(1) rows(1) flags(2, kCtView*) confirm_left_ms(2) wait_elapsed_ms(2)
//                                    wait_total_ms(2) run_elapsed_s(2) card_no(2) delta_n(1) delta_runs(2) runnable(1)
//                                    skipped(1) refusal(1) refusal_mv(2) pps_points(2) avs_points(2) topology(1)
//                                    run_guard(1) rmdo(1: major << 4 | minor), then the footer's words (0..27 chars, the
//                                    text the footer paints). Read-only, from RAM; rows and message are 0 / empty while the
//                                    page is not on screen. Timers saturate at 65535.
//   0x0D CHARGER_CABLE req - (read) or kind(1) seq(2) (set).  rsp kind(1) seq(2) run_kind(1) run_seq(2) name(24)
//                                    The cable identity of the "Cable identity" dialog (k2/chargertest/ct_cable.hpp): kind 0 = Off,
//                                    1 = built-in cable (seq = its id 1, 2 or 4: EPR 50V 5A, CUKTECH 6A,
//                                    OnePlus 100W; seq 0 = id 1, answered back as 1; seq 3 is the retired OnePlus 100W
//                                    (mag), accepted and answered back as 4), 2 = saved clone (seq =
//                                    library id). The set form
//                                    picks the row the START dialog opens on (the device remembers the last choice, so
//                                    tools/ct_start.py sets it before every START: its default is Off). rsp: the choice,
//                                    then the identity of the current / last run and its name (NUL padded, empty for
//                                    Off). Busy while a run is active, BadArg for a malformed choice or a clone that is
//                                    not in the library, BadLength otherwise.
//   0x30 DFU_ENTER   req magic(4) = 44 46 55 21 ("DFU!", u32 0x21554644) [reset(1)]   rsp -
//                                    reset: 0 = SYSRESETREQ (default, also the 4-byte form),
//                                    1 = watchdog reset, other values BadArg.
//                                    Clears the app's bootloader marker and resets into the
//                                    DFU bootloader (k2/drivers/dfu.hpp). The magic is the confirmation: a
//                                    wrong value answers BadArg, a wrong length BadLength. The
//                                    response goes out first, the reset follows.
//   0x33 FACTORY_WIPE req magic(4) = 57 49 50 45 ("WIPE", u32 0x45504957) [reset(1)]
//                                    rsp err(1) sectors(2) erased(2) verified(2) failed(2) first_fail(4) status(1)
//                                    Wipes this firmware's flash data (k2/core/factory_wipe.hpp): unmounts the
//                                    file system, erases every 2 KB sector of the littlefs volume (90 sectors) and
//                                    only those pages of the legacy range that carry the signature of one of our old
//                                    stores (sectors = volume + those); the native storage pages are never
//                                    touched. Reads them all back, clears
//                                    this firmware's backup registers, then enters DFU exactly like DFU_ENTER (reset:
//                                    0 sys / 1 iwdg). The reply is sent first (it takes a few seconds: the host
//                                    must wait). err 0 = wiped and verified, the DFU entry follows; 1 = an erase
//                                    failed (first_fail = its address, status = k2::flash::Status); 2 = a sector
//                                    does not read blank (failed = how many, first_fail = the first). On err != 0
//                                    the K2 stays in the app (its volume is gone until the next boot formats it)
//                                    and does NOT enter DFU. The reply status is Ok whenever the request parsed.
//                                    Wrong length BadLength, wrong magic or reset > 1 BadArg. Not offered over HTTP.
//   0x34 FLASH_DUMP  req addr(4) len(1)   rsp addr(4) len(1) data(len)
//                                    Read-only window into the pre-existing device storage,
//                                    0x080E0000..0x08100000 (kFlashDumpBase/End), len 1..48 (kFlashDumpMax). The
//                                    data is read straight from the memory-mapped flash; nothing is erased,
//                                    programmed or confirmed. For dumping a unit that has never run this
//                                    firmware when the bootloader does not serve that area (tools/k2_remote.py flash-dump). BadLength unless the
//                                    payload is exactly 5 bytes; BadArg for len 0 or > 48 or a window outside the area.
//   0x35 BOOT_INFO   req part(1) off(2)   rsp part(1) off(2) total(2) n(1) data(n)
//                                    How THIS boot decided its mode (k2/core/bootinfo.hpp), RAM only, read-only, valid in every mode.
//                                    part 0 = the K2B1 text line (ASCII, at most 240 bytes, the same text the LCD shows as a QR
//                                    code in a key-mode boot). off = byte offset, total = its length, n <= kBootInfoChunk (48).
//                                    The text is rendered when off = 0 and then held, so a client that reads from 0 upward gets
//                                    one consistent line (its live fields t/vb/c1/c2/pl are those of the off = 0 call).
//                                    part 1 = the first-seconds power samples, one per 50 ms (index * 50 ms): off = first sample
//                                    index, total = samples kept, n = samples in this reply (<= kBootInfoSamples = 5), data = n x 9
//                                    bytes: vbus_mv u16, cc1_mv u16, cc2_mv u16, aux_mv u16, flags u8 (bit0 = PLUS held).
//                                    BadLength unless the payload is exactly 3 bytes; BadArg for another part or off > total.
//   0x31 DEBUG_FAULT req magic(4) = "BOOM" kind(1)   rsp -
//                                    Provokes a CPU fault to test the [FAULT] handler (k2/core/fault.hpp):
//                                    kind 0 = undefined instruction, 1 = read above the default
//                                    96 KB of SRAM, 2 = recursion into the stack guard (stack
//                                    overflow). The K2 logs the fault and resets. Exactly 5
//                                    bytes (BadLength), wrong magic or kind > 2 BadArg.
//   0x46 USB_LINK    req -            rsp family(1) tbt(1) conf(1) source(1) flags(1) mbps(4) text(rest)
//                                    USB / Thunderbolt link classified from sniffed PD (k2/pd/usb_link.hpp); see UsbLinkSnapshot.
//   0x47 PD_REREAD   req op(1) [mask]  rsp state(1) step(1) mask(1) ok(1) skipped(1) pdos_before(1) pdos_last(1) caps_rx(1) flags(1)
//                                    Source-cap re-read (k2/pd/pd_reread.hpp), PD mode only: an after-contract
//                                    conversation to see whether the charger lists more PDOs afterwards. op 0 status
//                                    (no mask), 1 start with mask (1 caps-ext, 2 identity, 4 svids, 8 get-cap,
//                                    16 second get-cap; 1..31, else BadArg), 2 abort (no mask; also ends the attach
//                                    the start made). Without a contract the start makes one (Rd, Request PDO 1,
//                                    up to 8 s), settles 1 s, then runs the steps; 30 s after the run (at once on
//                                    abort or failure) the contract is dropped (PDO 1, no Auto-CRC) and the Rd it
//                                    presented goes. Busy when refused (inline, charger test, sink / Identify / probe busy).
//                                    op 3 [3, mode] (no run, replies with the current status) selects how the Pd-mode
//                                    sink answers Get_Sink_Cap_Extended: 0 silent, 1 Sink_Capabilities_Extended (SKEDB:
//                                    AVS + PPS capable; the default, it makes the UGREEN X865 list its SPR AVS APDO),
//                                    2 Not_Supported; mode > 2 BadArg. Not stored: reboot = 1.
//                                    op 4 [4, line] (no run, replies with the current status) forces the CC line the next
//                                    run's attach scan may take: 1 or 2, 0 = normal (the pure line choice otherwise); line
//                                    > 2 BadArg. RAM only: stays for every later attach scan (a charger test START too)
//                                    until op 4 0, op 2 (abort) or a reboot.
//                                    op 5 [5, on] (no run, replies with the current status) the Ra mode (k2/pd/emk_ra.hpp):
//                                    on = 1 drives PC13 high (an Ra on CC1) at once, presents Rd on CC2 alone from then
//                                    on and forces the attach scan to CC2; on = 0 ends it; on > 1 BadArg. Busy when
//                                    refused (sink inline, a contract or tool running, boot-pinned Rd that reaches CC1, VCONN experiment).
//                                    RAM only: also ends on a detach (not inside a charger-test run), op 2 and a reboot.
//                                    state = pd::RereadState, step = pd::RereadStep, flags bit 0 = an SPR AVS APDO
//                                    came back, bit 1 = still attaching (waiting for the contract / settling), bit 2 =
//                                    no contract came (nothing sent), bit 3 = the Ra mode is on. pdos_before = PDOs held
//                                    when the steps began.
//                                    Wrong length BadLength, unknown op BadArg.
//   0x70 BLACKBOX    req op(1) [arg]  rsp flags(1) next(4) head(4) [records]
//                                    The PD black box (k2/pd/pdtrace.hpp): a RAM ring of binary FUSB302 / PD
//                                    event records. op 0 read: arg = cursor u32 (default 0), the response
//                                    carries whole records from the cursor (<= 49 bytes) and the cursor
//                                    for the next read in `next`; head = total bytes ever written.
//                                    op 1 clear, op 2 freeze (arg u8: 1 freeze, 0 resume), op 3 status;
//                                    those answer flags/next/head with no records (next = oldest cursor).
//                                    flags bit0 the cursor was overwritten (moved up to the oldest
//                                    record), bit1 frozen, bit2 the cursor was beyond head (reboot).
//                                    Trigger-frozen captures (3 slots): op 4 list answers now_ms(4) then
//                                    3 x trigger(1) len(2) age_ms(4), trigger 0 = empty slot, 1 contract
//                                    change, 2 hard reset, 3 VBUS dip, 4 detach; op 5 read, req slot(1)
//                                    offset(2), answers like op 0 with offsets inside the slot (next, head
//                                    = slot length) and flags bit3 = still recording; op 6 clear captures.
//   0x71 TRANSCAP    req op(1) [args] rsp per op
//                                    The transition capture (k2/pd/transcap.hpp): VBUS (fast INA226) and CC1/CC2
//                                    at ~1 kHz around a PD voltage step, PD messages as markers. Ops:
//                                    0 status: rsp state(1) reason(1) flags(1) samples(2) markers(1) lost(4)
//                                      span_us(4) max_gap_us(4) trigger_age_ms(4) saved_seq(4) (26 bytes);
//                                      state 0 off 1 armed 2 post 3 frozen; reason 0 none 1 manual 2 shutoff
//                                      3 no-psrdy 4 hardreset 5 detach 6 timeout 7 voltage 8 other; flags
//                                      bit0 fast VBUS mode, bit1 a persist is pending.
//                                    1 arm: rsp result(1) 0 ok 1 busy (Net / Drive / Sniff own the overlay)
//                                      2 no power monitor. Restarts an armed capture.
//                                    2 trigger: rsp ok(1); 1 = it was armed and now records the post window.
//                                    3 disarm: rsp 0; drops the capture and restores the power monitor.
//                                    4 save: rsp seq(4); writes the frozen capture to the volume (0 = none).
//                                    5 live: req off(4); rsp the frozen capture's encoded stream from byte
//                                      `off` (<= 58 bytes; 0 bytes = end or no frozen capture).
//                                    6 list: rsp count(1) then per stored capture, newest first, seq(4)
//                                      run_id(4) step(2) reason(1).
//                                    7 stored: req seq(4) off(4); rsp stream bytes from `off` (0 = end).
//                                    8 clear: rsp 0; deletes every stored capture.
//                                    The stream format is k2/pd/transcap_calc.hpp encode(); decode it with
//                                    tools/pd_trace.py transition.
//   0x50 CABLE_R     req [op(1)]       rsp flags(1) grade(1) r_mohm(2) idle_mv(2) idle_ma(2) load_mv(2) load_ma(2)
//                                        pos_mohm(2) gnd_mohm(2)
//                                    op 0 (or no payload) status; op 1 reset the cable-resistance
//                                    reference; op 2 select the passive two-point meter; op 3 select
//                                    the Kelvin 8-wire mode (sense wires on CC1/CC2/D-, needs >= 450 mA,
//                                    the K2's own path is not in the figure); ops 1..3 then answer
//                                    status, and a mode change clears the reading. flags bit0 idle
//                                    reference taken, bit1 reading valid (the other fields are 0
//                                    until then), bit2 Kelvin mode; in Kelvin mode r_mohm is the total,
//                                    pos/gnd its legs, idle and load fields stay 0. grade is
//                                    cable_r::Grade (0 waiting, 1 excellent .. 4 poor). Never drives
//                                    VBUS or the load. Other ops BadArg, longer payloads BadLength.
//   0x51 LAB         req op(1) [args]  rsp per op
//                                    op 1 sniff, args sub(1) [confirm(1)]: sub 0 cancel, 1 arm, 2 status, 3 selftest (D+ toggled,
//                                    needs confirm = 1, only while armed and nothing attached). Answers state(1) reason(1)
//                                    flags(1: bit0 overflow, bit1 stored) edges(2). The capture is read with BLACKBOX capread.
//                                    op 0 load probe, args duty_pct(1) ms(2): pulses the PA8 test load (TIM1 CH1) for
//                                    ms (<= 200) at duty (<= 95) and answers reason(1) duty(1) ms(2) idle_ma(2s)
//                                    peak_ma(2s) min_vbus_mv(2) vbus_before_mv(2) temp_before_dc(2s) temp_after_dc(2s) pin_high_pct(1,
//                                    PA8 read back during the pulse).
//                                    reason = k2::load::Abort (6 = ran to the end). Needs the confirm flag byte after the
//                                    args (1 = yes); Busy while a trigger or test runs; never drives D+/D-/CC.
//   0x52 UFCS        req op(1) [args]  rsp per op
//                                    The UFCS frame/timing tuner (RAM only, reset at boot, k2/fastcharge/ufcs_tune.hpp) and the 5 V probe
//                                    (k2/fastcharge/ufcs_probe.hpp). Never raises VBUS, never sends a Request, needs no confirm. The field
//                                    indices are k2::ufcs::Field (0..kFieldCount-1, in the kFields table order).
//                                    op 0 status: rsp state(1: 0 idle, 1 running, 2 done) verdict(1) class(1) start(1) blob_len(2)
//                                      nfields(1). start = the ProbeStart code of the last probe op (0 = started).
//                                    op 1 get, args field(1): rsp field(1) value(4) lo(4) hi(4) default(4). BadArg for an unknown field.
//                                    op 2 set, args field(1) value(4): rsp as get after the change. BadArg for an unknown field or a
//                                      value outside [lo, hi]; Busy while a probe or a trigger runs.
//                                    op 3 reset: every field back to its default; rsp as status. Busy while a probe runs.
//                                    op 4 probe: start the probe; rsp as status. Busy while a test, scan, sniff, trigger or probe
//                                      runs; any other refusal (inline, not 5 V, no monitor, bus still resetting) is Ok with the
//                                      ProbeStart code in `start`.
//                                    op 5 stop: abort a running probe (the bus is reset); rsp as status.
//                                    op 6 read, args off(2): rsp off(2) total(2) n(1) data(n), n <= kUfcsChunk (48). The result blob
//                                      (layout in k2/fastcharge/ufcs_probe.hpp: header with verdict, handshake timings, counts, errors, then the
//                                      TX/RX event log), valid at any time and frozen once the probe is done. BadArg for off > total.
//   0x53 QRTEST      req op(1) [args]  rsp per op
//                                    The QR scan-test rig (k2/ui/ui_qrtest.hpp): a full-screen test QR on the panel for
//                                    phone-camera scan checks. It only draws (read-only for the ports); it needs no confirm.
//                                    op 0 list, args off(2) (default 0): rsp off(2) total(2) n(1) data(n), n <= kQrTestChunk (48):
//                                      the fit table as text lines, computed on the device from the QR capacity table and the
//                                      panel geometry box = (modules + 2*quiet) * px <= 240, one line per row:
//                                      "v13 m69 p3 q4 box231 fit c425 r281" = version, modules, px, quiet, box px, fits,
//                                      byte-L chars, max record B; per (px, quiet) combo (px 3 then 2, quiet 4..1) the largest
//                                      version that fits and the first that does not. BadArg for off > total.
//                                    op 1 show, args version(1) px(1) quiet(1): rsp version(1) px(1) quiet(1). Validates
//                                      version 1..40, px 2..3, quiet 1..4 and refuses BadArg with a reason text in the payload
//                                      when the box does not fit the 240 px panel at that px/quiet. Opens the QrTest page
//                                      (full-screen code, chrome hidden; BACK or op 2 leaves it); Busy when the arena or the
//                                      ports refuse it now (a charger test running).
//                                    op 2 stop: rsp showing(1) = 1 when a test screen was on the stack and is popped now.
//                                    Wrong length BadLength, unknown op BadArg.
//   0x32 FC          req op(1) [args] [args]  rsp state proto fail scan_flags scan_step target_mv(2) vbus_mv(2)
//                                    num_results(1) { proto outcome mv(2) } x num_results
//                                    op 0 status; op 1 reset (to 5 V, then passive idle);
//                                    op 2 scan, arg flags(1); op 3 trigger, args proto(1) mv(2) flags(1).
//                                    flags bit0 = confirm (needed by the scan and above 5000 mV, else
//                                    BadArg), bit1 = override the PD-contract / load guard (never a
//                                    confirmation); other bits BadArg. A guard refusal answers BadArg
//                                    --- vooc-monitor (k2/vooc-detect) ---
//                                    op 4 vooc, args page(1) [on(1)]: the inline VOOC monitor (passive, never drives;
//                                      k2/fastcharge/vooc_decode.hpp), not the FC block. on = 0 / 1 switches it first
//                                      (RAM only). page 0: flags(1: bit0 enabled, bit1 running, bit2 session active,
//                                      bit3 handshake, bit4 paused: data traffic) sessions(2) wakes(2) partials(2)
//                                      edges(4) frames(4) pairs(4) hs_pairs(1) ident(1) ident_reply(2) r82_last(2)
//                                      r82_min(2) r82_max(2) r82_n(2) p82_last_us(4) p82_mean_us(4) p82_n(2) ncmds(1)
//                                      other(2) storms(2) storm_edges(2) storm_ms(2) retry_ms(4) = 54 bytes; reply words
//                                      0xFFFF = none; storms = edge-storm guard trips since boot (monitor and sniff),
//                                      storm_edges / storm_ms = the last trip's window, retry_ms = the pause left.
//                                      page 1 / 2: n(1) { cmd(1) count(2) last(2) } x n, rows 0-7 / 8-15.
//                                      Other pages or on > 1 BadArg, wrong length BadLength. Never Busy.
//                                    --- end vooc-monitor ---
//   0x21 ADC_RAW     req -            rsp ok(1) all(10 x u16) scan(7 x u16)
//                                    all = one-shot raw counts of ADC channels 0..7, 16, 17;
//                                    scan = the 100-sweep averages in slot order aux cc1 cc2 dp dm
//                                    ntc vref. Both are raw 12-bit counts, no scaling.
//   0x22 RIPPLE      req [code(1)]    rsp code(1) period_ns(4) count(2) min(2) max(2) mean(2)
//                                    code = ADC sample-time code 0..7 (default 0, fastest); the K2
//                                    captures `count` single-channel samples of PA1 and then
//                                    streams them as frames (cmd 0x8B, same seq, status 0):
//                                      A6 4B 8B seq 00 len | offset(2) | samples (<= 28 x u16)
//                                    offset = index of the first sample in the frame.
//                                    A failed capture answers Busy and sends no frames.
#pragma once
#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include "k2/core/bootinfo.hpp"
#include "k2/core/lease.hpp"
#include "k2/core/stop.hpp"
#include "k2/pd/cc_probe.hpp"
#include "k2/chargertest/charger_test.hpp"
#include "k2/chargertest/ct_cable.hpp"
#include "k2/pd/emk_learn.hpp"
#include "k2/pd/emk_quirk.hpp"
#include "k2/fastcharge/edge_guard.hpp"
#include "k2/fastcharge/ufcs_tune.hpp"
#include "k2/fastcharge/vooc_decode.hpp"
#include "k2/ui/ui_qrtest.hpp"

namespace k2::remote {

constexpr size_t kFrameSize = 64;
constexpr uint8_t kReqMagic0 = 0xA5, kReqMagic1 = 0x4B;
constexpr uint8_t kRspMagic0 = 0xA6, kRspMagic1 = 0x4B;
constexpr size_t kReqHeader = 5;
constexpr size_t kRspHeader = 6;
constexpr size_t kMaxReqPayload = kFrameSize - kReqHeader;   // 59
constexpr size_t kMaxRspPayload = kFrameSize - kRspHeader;   // 58
constexpr uint8_t kProtoVersion = 1;
constexpr size_t kPingGitLen = 24;   // NUL padded; fits "<7-char hash>-dirty" and a tag-describe string

constexpr uint16_t kMinPressMs = 30;      // must outlast the 20 ms debounce
constexpr uint16_t kMaxPressMs = 10000;
constexpr size_t kMaxPdos = 11;   // 7 SPR + 4 EPR positions, matches pd::kMaxPdos
constexpr size_t kMaxVdos = 6;

// The single table of request ids. kCmdIds lists every id once; the static_assert below fails the
// build when two commands share a value, so an id collision between branches cannot slip through.
enum class Cmd : uint8_t {
    Ping = 1, Button = 2, GetState = 3, GetPdos = 4, GetEmarker = 5,
    Action = 6, SetPage = 7, Reboot = 8, Log = 9, Screenshot = 10,
    DebugLink = 0x25,   // debug: 1 = hold the link attached, 0 = detached (until the next Rescan)
    DebugPdos = 0x20,   // debug: load N synthetic PDOs, 0 clears
    AdcRaw = 0x21, Ripple = 0x22,
    DebugPdMsg = 0x23,  // debug: feed one PD message to the receive path: sop, header, data objects
    DebugCcMatrix = 0x26, // debug: set the CC switch matrix PC13/14/15 (bit0 PC13, bit1 PC14, bit2 PC15)
    DebugVconn = 0x27,    // debug: [op] 0 status, 1 send VCONN_Swap (K2 takes the VCONN role), 2 VCONN off -> VconnSnapshot
    DebugEprInfo = 0x28,  // debug: [op] 0 status, 1 ask EPR_Get_Source_Cap (gated on the EPR bit), 2 ask regardless -> EprInfoSnapshot
    DebugLcd = 0x29,      // debug: LCD DMA state, no payload -> LcdSnapshot
    DebugCcProbe = 0x2A,  // debug: [sw0, pc, ms u16 (, modes u16)] hold a SWITCHES0 value on a waiting charger, sample CC -> see encode_cc_probe
    GetIdentity = 0x24, // source identity: VID/PID/bcd, SVIDs, vendor and Apple model
    DfuEnter = 0x30,    // needs kDfuMagic as payload
    FactoryWipe = 0x33, // erase the file system and our own legacy pages, then DFU; needs kFactoryWipeMagic, see above
    FlashDump = 0x34,   // read-only window into 0x080E0000..0x08100000, see above
    BootInfo = 0x35,    // how this boot decided its mode: K2B1 text and first-seconds samples, see above
    // Active PD (wave 2C). Every one of them is refused inline; a request above 5 V needs the
    // confirm flag (Status::NeedConfirm without it). Ids 0x31/0x32 belong to DebugFault/Fc.
    IdentifyDevice = 0x40,  // [op] 0 status, 1 start, 2 start (same as 1; the inline refusal has no confirm) -> IdentifySnapshot
    PdRequest = 0x41,       // [pos, flags (bit0 confirm, bit1 over-load lab-only), mv u16, ma u16] -> [pd::Err]
    PdReset = 0x42,         // optional [flags]: bit0 confirm, bit1 inline lab-only (kPdFlagResetInline, needs bit0; refused while a charger test runs) -> [pd::Err]
    PdTriggerStatus = 0x43, // -> PdTriggerSnapshot
    PdRelease = 0x44,       // back to 5 V (Request PDO 1)
    PdMatch = 0x45,         // passive wants-vs-offers verdict -> MatchSnapshot (see encode_match)
    UsbLink = 0x46,         // USB / Thunderbolt link classified from sniffed PD -> UsbLinkSnapshot (see encode_usb_link)
    PdReread = 0x47,        // [op, mask] source-cap re-read: 0 status, 1 start, 2 abort, 3 sink-cap-ext mode, ..., 8 Rd glitch of mask x 10 ms -> RereadInfo (see above)
    PdAttach = 0x48,        // [target_mv u16] [target_ma u16] [flags] -> PdTriggerSnapshot; mv 0 = 5000 mV, clamped to 36000 mV; flags bit0 = confirm (needed above 5000 mV); refused inline / busy = status + err byte
    PdHandback = 0x49,      // optional [flags]: bit0 confirm (kPdFlagConfirm) -> [HandbackVerdict]; phase-1 lab hand-back (see k2/core/handback.hpp)
    AppleRead = 0x4A,       // lab-only Apple charger readout (SVID 0x05AC, k2/pd/apple_readout.hpp): [op] 0 status, 1 arm + re-request the current contract -> AppleReadInfo; a refusal carries its pd::AppleRefusal as the one-byte payload
    Stop = 0x4B,            // no payload, no confirm (the safe direction): cancel a charger test, stop FC with the measured 5 V release, release PD, disarm a spoof -> [active mask (k2/core/stop.hpp), vbus_mv i32 LE, fc_release_busy]
    Lease = 0x4C,           // [0] status, or [1, seconds u16 LE] set the length (0 = default 30 s, max 600) -> [armed, left_s u16 LE, length_s u16 LE, ended] (k2/core/lease.hpp)
    DebugFault = 0x31,  // debug: provoke a CPU fault, needs kFaultMagic + kind
    Fc = 0x32,          // active fast-charge trigger, see the layout above
    Store = 0x60,       // flash store: list / save e-marker / save log, see kStore* below (the saves: Busy while a run or an FC release owns the link)
    EmLib = 0x61,       // saved-cable library: list / get / save / delete / rename / spoof, see kEm* below
    Cards = 0x62,       // charger report cards: list / get (chunked) / rename / delete, see kCards* below (rename, delete: Ok + [kCardsErrBusy] while a run or an FC release owns the link)
    Lab = 0x51,         // lab probes: PA8 load pulse, see the layout above
    CableR = 0x50,      // passive cable resistance (k2/pd/cable_r.hpp), see the layout above
    Ufcs = 0x52,        // UFCS tuner and 5 V probe (k2/fastcharge/ufcs_tune.hpp, k2/fastcharge/ufcs_probe.hpp), see the layout above
    QRTest = 0x53,      // QR scan-test rig (k2/ui/ui_qrtest.hpp): fit table, full-screen test QR, stop; see the layout above
    BlackBox = 0x70,    // PD black box ring: read / clear / freeze / status, see the layout above
    TransCap = 0x71,    // transition capture: arm / trigger / status / read, see the layout above
    ChargerTest = 0x0B, // read-only RAM charger-suite metadata / paged results
    ChargerUnits = 0x0C, // [n, unit...] the units of the next charger run, in order (charger_test::Unit); n = 0 = default order.
                         // Busy while a run is active, BadArg for an unknown unit or a duplicate. Reset to default by every START.
    ChargerCable = 0x0D, // [kind, seq(2)] the cable identity of the next START (k2/chargertest/ct_cable.hpp); no payload reads it back.
    ChargerHunt = 0x0E,  // [slot] read-only: the evidence of one refused 6 A hunt row (charger_test::HuntEvidence), see kHuntSlotBytes.
    ChargerSweep = 0x0F, // [on(1)] the PPS deep sweep of the next charger run (opt-in, never in a preset; an Options flag, not an
                         // algorithm bit - bit 31 of the draft selection in CHARGER_TEST schema 2). No payload reads it back.
                         // Busy while a run is active.
    ChargerAvsSweep = 0x54, // [on(1)] the AVS deep sweep of the next charger run (opt-in, never in a preset; an Options flag
                         // next to the PPS sweep's, journaled in the meta record). rsp [on(1)]. Busy while a run is active.
};

inline constexpr Cmd kCmdIds[] = {
    Cmd::Ping, Cmd::Button, Cmd::GetState, Cmd::GetPdos, Cmd::GetEmarker, Cmd::Action,
    Cmd::SetPage, Cmd::Reboot, Cmd::Log, Cmd::Screenshot, Cmd::DebugPdos, Cmd::AdcRaw,
    Cmd::Ripple, Cmd::DfuEnter, Cmd::FactoryWipe, Cmd::FlashDump, Cmd::DebugFault, Cmd::Fc, Cmd::DebugLink, Cmd::DebugPdMsg, Cmd::DebugCcMatrix, Cmd::DebugVconn,
    Cmd::DebugEprInfo, Cmd::DebugLcd, Cmd::DebugCcProbe,
    Cmd::GetIdentity, Cmd::IdentifyDevice, Cmd::PdRequest, Cmd::PdReset, Cmd::PdTriggerStatus, Cmd::PdRelease, Cmd::PdMatch, Cmd::UsbLink, Cmd::PdReread,
    Cmd::PdAttach, Cmd::PdHandback, Cmd::AppleRead, Cmd::Stop, Cmd::Lease,
    Cmd::CableR, Cmd::Lab, Cmd::Ufcs, Cmd::QRTest, Cmd::Store, Cmd::EmLib, Cmd::Cards, Cmd::BlackBox, Cmd::TransCap, Cmd::ChargerTest,
    Cmd::ChargerUnits, Cmd::ChargerCable, Cmd::ChargerHunt, Cmd::ChargerSweep, Cmd::ChargerAvsSweep, Cmd::BootInfo,
};

constexpr bool cmd_ids_unique() {
    for (size_t i = 0; i < sizeof kCmdIds / sizeof kCmdIds[0]; ++i)
        for (size_t j = i + 1; j < sizeof kCmdIds / sizeof kCmdIds[0]; ++j)
            if (kCmdIds[i] == kCmdIds[j]) return false;
    return true;
}
static_assert(cmd_ids_unique(), "two remote commands share one request id");

// ACTION ids (k2::Action in ui_types.hpp: 1 PdRescan, 2 EmarkerScan, 3 EnergyReset, 4 FcScan, 5 FcReset, 6 Identify).
// An action that can raise the output above 5 V or drive D+/D- needs the confirm byte on the remote path, the way the
// screen needs Review + two released OK presses: today that is FcScan (the QC handshake walks the output up). The others
// never raise VBUS: PdRescan is passive inline / a Get_Source_Cap / a CC cycle, EmarkerScan injects VCONN on CC (refused
// inline), Identify is a PD message (refused inline), EnergyReset and FcReset only clear state or lower the output.
// The dispatcher and Remote::action both ask this one function.
constexpr uint8_t kActionFcScan = 4;
constexpr bool action_needs_confirm(uint8_t id) { return id == kActionFcScan; }

constexpr uint8_t kPdFlagConfirm = 1 << 0;
// Lab-only (k2_remote `pd request --over-load`, never the UI / WebHID / charger test): lift the inline
// refusal while the far sink is a passive load - no PD message from it this attach (pd::over_load_allowed).
constexpr uint8_t kPdFlagOverLoad = 1 << 1;
constexpr uint8_t kPdFlagResetInline = 1 << 1;   // Cmd::PdReset only: lab-only Hard Reset while a live sink is attached inline (needs kPdFlagConfirm too; see pd::lab_reset_verdict)

constexpr uint32_t kFaultMagic = 0x4D4F4F42;  // "BOOM" little endian
constexpr uint32_t kDfuMagic = 0x21554644;   // "DFU!" little endian: the DFU_ENTER confirmation
constexpr uint32_t kFactoryWipeMagic = 0x45504957;   // "WIPE" little endian: the FACTORY_WIPE confirmation

// FLASH_DUMP window: the pre-existing device storage, read only. The response is addr(4) len(1) data.
constexpr uint32_t kFlashDumpBase = 0x080E0000, kFlashDumpEnd = 0x08100000;
constexpr size_t kFlashDumpMax = 48;
static_assert(5 + kFlashDumpMax <= kMaxRspPayload);

// BOOT_INFO: the reply header is part(1) off(2) total(2) n(1); part 0 carries text bytes, part 1 carries 9-byte samples.
constexpr uint8_t kBootInfoText = 0, kBootInfoSamplePart = 1;
constexpr size_t kBootInfoChunk = 48, kBootInfoSamples = 5, kBootInfoSampleBytes = 9;
static_assert(6 + kBootInfoChunk <= kMaxRspPayload);
static_assert(6 + kBootInfoSamples * kBootInfoSampleBytes <= kMaxRspPayload);


// FC op codes (first payload byte of Cmd::Fc).
constexpr uint8_t kFcStatus = 0, kFcReset = 1, kFcScan = 2, kFcTrigger = 3;
constexpr uint8_t kFcProtoCount = 11;         // fc::Proto: Qc2 .. Vivo
constexpr uint16_t kFcSafeMv = 5000;          // above this a request needs the confirm flag
constexpr uint8_t kFcConfirm = 1;             // flags bit0: the voltage (above 5 V, and the scan) is confirmed
constexpr uint8_t kFcAllowInline = 2;         // flags bit1: override the PD-contract / load guard
constexpr uint8_t kFcFlagMask = kFcConfirm | kFcAllowInline;
constexpr size_t kFcMaxResults = 7;
static_assert(10 + 4 * kFcMaxResults <= kMaxRspPayload);
// --- vooc-monitor (k2/vooc-detect): FC op 4, the inline VOOC monitor summary (layout above) ---
constexpr uint8_t kFcVooc = 4;
constexpr uint8_t kVoocPages = 3;             // 0 summary, 1 / 2 command rows
constexpr size_t kVoocRowsPerPage = 8;
constexpr size_t kVoocSummaryBytes = 54;
static_assert(kVoocSummaryBytes <= kMaxRspPayload && 1 + 5 * kVoocRowsPerPage <= kMaxRspPayload);
static_assert((kVoocPages - 1) * kVoocRowsPerPage >= vooc::kMaxCmds);
struct VoocSnapshot {
    bool enabled = false, running = false;
    vooc::Summary s{};
    edges::GuardInfo guard{};
};
// --- end vooc-monitor ---

// Store op codes (first payload byte of Cmd::Store). The request is [op] for list / save-emarker /
// save-log. Op 2 (erase of the native pages) is retired: it answers BadArg. save-emarker saves the
// inline cable into the saved-cable library (same as EmLib save). The response (always Status::Ok
// once the request parsed) is StoreInfo, 8 bytes: [err, count, flags, slot, log_len u16, bank,
// native]; err is k2::store::Err (0 = Ok), count the saved cables, slot the new cable number (low byte).
constexpr uint8_t kStoreList = 0, kStoreSaveEmarker = 1, kStoreSaveLog = 3;
constexpr uint8_t kStoreFlagSingle = 1, kStoreFlagForeign = 2, kStoreFlagLog = 4;
// CARDS op codes (first payload byte of Cmd::Cards); the layouts are in k2/chargertest/report_card_remote.hpp, which
// src/core/remote.cpp checks against these. Delete needs the trailing 1; rename takes a 1..20 byte name.
inline constexpr uint8_t kCardsList = 0, kCardsGet = 1, kCardsRename = 2, kCardsDelete = 3;
inline constexpr uint8_t kCardsErrBusy = 5;   // Ok + [kCardsErrBusy]: a rename / delete while a run or an FC release owns the link (report_card::remote::kErrBusy)
inline constexpr size_t kCardsNameMax = 20, kCardsChunks = 6;   // 250 bytes / 48 = 5.2 -> 6 chunks (v10 payload; report_card::remote::kChunks, static_asserted in remote.cpp)

// EMLIB op codes (first payload byte of Cmd::EmLib), the saved-cable library of k2/pd/emlib.hpp. Every
// response starts with err (k2::emlib::Err: 0 Ok, 1 Full, 2 Invalid, 3 NotFound, 4 Flash, 5 Exists;
// 6 = kEmErrInline, a live sink refuses a spoof). Cables are numbered 1..9999 ("seq", u16 LE).
//   LIST    req [0, start]          rsp [err, total, n, seq u16 * n]   (n <= kEmListMax, from index start)
//   GET     req [1, seq u16]        rsp [err, seq u16, name(24, NUL padded), vid u16, pid u16]
//   SAVE    req [2]                 rsp [err, seq u16, existed]   saves the cable last read (e-marker scan)
//   DELETE  req [3, seq u16, 1]     rsp [err]                     the trailing 1 is the confirmation
//   (SAVE, DELETE, RENAME and LEARN save are refused with status Busy + [kEmErrBusy] while a charger test or an FC
//   release owns the link: the flash is shared with the test's own card save)
//   RENAME  req [4, seq u16, name]  rsp [err, seq u16]            name 1..24 chars of [A-Za-z0-9 -_.]
//   SPOOF   req [5, 0]              rsp [err, active, requests u16, replies u16, seq u16] (8 bytes)  status
//           req [5, 1, seq u16]     arm that cable;  req [5, 2]   disarm (same response)
//           req [5, 3, id]          arm a built-in cable (emrec::builtin_cable ids: 0 generic 5 A/20 V, 1 EPR 50V 5A,
//                                   2 CUKTECH 6A, 4 OnePlus 100W; 3 is the retired OnePlus 100W (mag) and arms 4);
//                                   an id past the table is BadArg
//   LEARN   (blocks: a run takes seconds; refused with kEmErrBusy while a PD tool is active)
//           req [6, 0]              rsp summary (k2::emk_learn::kSummaryBytes = 13): the last run, outcome 0 = none yet
//           req [6, 1, options]     run: powers the cable in the bottom port (VCONN) and replays Discover Identity /
//                                   SVIDs / Modes and the seeds; options = [flags (bit0 sweep, bit1 confirm inline),
//                                   nseeds <= 4, seed * nseeds], a seed = svid u16, cmd 0..31, nvdo 0..2, vdo u32 * nvdo;
//                                   rsp summary (outcome 1 learned, 2 no identity, 3 aborted)
//           req [6, 2, i]           transcript entry i of the last run, rsp k2::emk_learn::encode_entry ([3] past the end)
//           req [6, 3]              save the last learned clone with its extra replies, rsp [err, seq u16, existed]
//   UNMATCHED req [7, i]            the messages an armed fake cable received and nothing answered, newest first:
//           i 255 -> [err, total u16, held]; i 0..3 -> that plus sop (1 SOP', 2 SOP''), n, header u16, words (u32 LE);
//           an i past the held entries -> [3]
constexpr uint8_t kEmList = 0, kEmGet = 1, kEmSave = 2, kEmDelete = 3, kEmRename = 4, kEmSpoof = 5, kEmLearn = 6, kEmUnmatched = 7;
constexpr uint8_t kEmLearnStatus = 0, kEmLearnRun = 1, kEmLearnEntry = 2, kEmLearnSave = 3;
constexpr uint8_t kEmSpoofStatus = 0, kEmSpoofArm = 1, kEmSpoofDisarm = 2, kEmSpoofBuiltin = 3;
constexpr uint8_t kEmErrInline = 6;
constexpr uint8_t kEmErrBusy = 7;  // active negotiation/scan/test refuses an arm, disarm remains explicit
constexpr size_t kEmListMax = (kMaxRspPayload - 3) / 2;   // 27 numbers per LIST response
constexpr size_t kEmNameMax = 24;
// BLACKBOX op codes (first payload byte of Cmd::BlackBox); match k2::pdtrace::kOp*.
constexpr uint8_t kBbRead = 0, kBbClear = 1, kBbFreeze = 2, kBbStatus = 3, kBbCapList = 4, kBbCapRead = 5, kBbCapClear = 6;
// TRANSCAP op codes (first payload byte of Cmd::TransCap); the status response is kTcStatusBytes long.
constexpr uint8_t kTcStatus = 0, kTcArm = 1, kTcTrigger = 2, kTcDisarm = 3, kTcSave = 4, kTcLive = 5, kTcList = 6,
                  kTcStored = 7, kTcClear = 8;
constexpr size_t kTcStatusBytes = 26;
constexpr size_t kTcListEntry = 11;   // seq(4) run_id(4) step(2) reason(1)
// CABLE_R op codes (optional first payload byte of Cmd::CableR).
// LAB op codes (first payload byte of Cmd::Lab).
constexpr uint8_t kLabLoad = 0;
constexpr uint8_t kLabSniff = 1;   // D+/D- edge capture: sub-op cancel 0, arm 1, status 2, selftest 3 (needs confirm)

constexpr uint8_t kCableStatus = 0, kCableReset = 1, kCablePassive = 2, kCableKelvin = 3;
// UFCS op codes (first payload byte of Cmd::Ufcs); the read reply header is off(2) total(2) n(1).
constexpr uint8_t kUfcsStatus = 0, kUfcsGet = 1, kUfcsSet = 2, kUfcsReset = 3, kUfcsProbe = 4, kUfcsStop = 5, kUfcsRead = 6;
constexpr size_t kUfcsChunk = 48, kUfcsFieldBytes = 17;
static_assert(5 + kUfcsChunk <= kMaxRspPayload);
static_assert(kUfcsFieldBytes <= kMaxRspPayload);
// QRTEST op codes (first payload byte of Cmd::QrTest); the list reply header is off(2) total(2) n(1).
constexpr uint8_t kQrTestList = 0, kQrTestShow = 1, kQrTestStop = 2;
constexpr size_t kQrTestChunk = 48;
static_assert(5 + kQrTestChunk <= kMaxRspPayload);

constexpr uint8_t kCmdScreenData = 0x8A;     // stream frames of a screenshot (not a request id)
constexpr uint8_t kShotFlagLast = 0x01;      // last chunk of a strip
constexpr uint8_t kShotFlagAbort = 0x80;     // capture aborted, no more frames
constexpr uint8_t kShotFormatRle565 = 1;
constexpr size_t kShotChunk = kMaxRspPayload - 2;   // RLE bytes per stream frame (56)

constexpr uint8_t kCmdRippleData = 0x8B;     // stream frames of a ripple capture
constexpr size_t kRippleChunk = (kMaxRspPayload - 2) / 2;   // samples per stream frame (28)
constexpr size_t kAdcRawAll = 10, kAdcScanSlots = 7;

enum class Status : uint8_t { Ok = 0, UnknownCmd = 1, BadLength = 2, BadArg = 3, Busy = 4, NeedConfirm = 5 };

// The reply for an action the UI declined to run (a charger test or an FC release owns the ports, a negotiation is
// in flight): Busy with no payload, never NeedConfirm - NeedConfirm means "add the confirm byte" and only the
// confirm check (action_needs_confirm) answers it. A host that retried with confirm after a busy refusal would get nothing new.
constexpr Status action_start_status(bool started) { return started ? Status::Ok : Status::Busy; }

enum class ParseError : uint8_t {
    None,
    TooShort,    // fewer bytes than a header
    BadMagic,    // not ours (log echo, bootloader frame, noise): never answered
    Truncated,   // header promises more payload than was received
    Oversize,    // payload length above kMaxReqPayload
};

struct ParseResult {
    ParseError error = ParseError::None;
    uint8_t cmd = 0;
    uint8_t seq = 0;
    std::span<const uint8_t> payload{};
    bool header_valid = false;   // cmd/seq are meaningful (magic matched, header complete)
};

ParseResult parse_request(std::span<const uint8_t> raw);

// Writes a 64-byte response into `out` (zero padded). A payload above kMaxRspPayload is cut.
// Returns the number of meaningful bytes (header + payload).
size_t build_response(std::span<uint8_t, kFrameSize> out, uint8_t cmd, uint8_t seq, Status status,
                      std::span<const uint8_t> payload = {});

// Plain data handed over by the firmware (or a test fake) so the encoder needs no hardware.
struct PingInfo {
    uint32_t uptime_ms = 0;
    std::string_view git{};
    std::string_view version{};
};

struct ChargerTestSnapshot {
    uint32_t run_id = 0, revision = 0;
    uint8_t state = 0, reason = 0, current = 0;
    std::span<const charger_test::Step> steps{};
    charger_test::Evidence evidence{};
    uint32_t configured = charger_test::kAllAlgorithms;
    uint8_t stage = 0;
    uint8_t cursor = 0;    // setup list cursor, saturates at 15 (wire: high nibble of the PDO-count byte)
    uint8_t resumed = 0;   // times this run was resumed after a reboot (the wire saturates at 3)
    ctsweep::View sweep{}; // the finished sweep rows; a sweep row's wire detail byte is its recorded point count
};
inline ChargerTestSnapshot charger_test_snapshot(const charger_test::Runner& r,
                                                 uint32_t configured = charger_test::kAllAlgorithms, uint8_t stage = 0, uint8_t cursor = 0) {
    return {r.run_id(), r.revision(), static_cast<uint8_t>(r.state()),
            static_cast<uint8_t>(r.reason()), static_cast<uint8_t>(r.index()), r.steps(), r.evidence(), configured, stage, cursor,
            r.resumed(), r.sweep_view()};
}
constexpr uint8_t kChargerTestSchema = 1;
constexpr size_t kChargerTestHeader = 15, kChargerTestRow = 16, kChargerTestPage = 2;
constexpr uint8_t kChargerTestSchema2 = 2;
constexpr size_t kChargerTestHeader2 = 42, kChargerTestPage2 = 1;
static_assert(kChargerTestHeader + kChargerTestRow * kChargerTestPage <= kMaxRspPayload);
static_assert(kChargerTestHeader2 + kChargerTestRow * kChargerTestPage2 <= kMaxRspPayload);
size_t charger_test_count(const ChargerTestSnapshot&, uint8_t schema);

// The charger-test view (`0x0B req [3]`): the chrome of the wizard and the run screens that the schema-2 reply has no room for.
// Plain data the firmware fills from the resident session and, while the page is on screen, from the page itself; the
// words in `message` are the footer's, character for character. `rows` and `message` are page things: 0 and empty with no page
// (kCtViewOnScreen clear). Timers saturate at 65535. The reply is kChargerTestViewFixed bytes plus message_len (<= 27).
constexpr uint8_t kChargerTestViewReq = 3;
constexpr size_t kChargerTestViewFixed = 30, kChargerTestViewMessage = 27;
static_assert(kChargerTestViewFixed + kChargerTestViewMessage <= kMaxRspPayload);
constexpr uint16_t kCtViewOnScreen = 1u << 0,       // the page is on screen (rows and message are live)
                   kCtViewConfirming = 1u << 1,     // the START confirm window is open
                   kCtViewConfirmReattach = 1u << 2,   // ... and it announces the planned re-attach
                   kCtViewSelfSupplied = 1u << 3,   // the K2 runs on the measured port
                   kCtViewResumePending = 1u << 4,  // a journaled run waits to resume
                   kCtViewAutoResume = 1u << 5,     // ... and resumes by itself once VBUS is steady
                   kCtViewGateReady = 1u << 6,      // the resume gate is open (VBUS held steady)
                   kCtViewResumeHeld = 1u << 7,     // BACK or an unarmable identity: no Rd is presented until OK asks again
                   kCtViewBehindCable = 1u << 8,    // the Ra guard says the charger sits behind a cable
                   kCtViewResumeRd = 1u << 9,       // the resume presented the Rd and waits for the charger
                   kCtViewWaitPaused = 1u << 10,    // Waiting: the Rd is off between the two attempts
                   kCtViewWaitRetry = 1u << 11,     // Waiting: the second attempt is running
                   kCtViewArmedIsPick = 1u << 12,   // the identity the Spoof tool armed is the one the Cable step has picked
                   kCtViewRmdoValid = 1u << 13,     // `rmdo` holds the live source's Revision message
                   kCtViewCableArmed = 1u << 14;    // some identity is armed (the ARMED / CURR pills exist)
struct ChargerTestView {
    uint8_t stage = 0, cursor = 0, rows = 0;
    uint16_t flags = 0;
    uint16_t confirm_left_ms = 0, wait_elapsed_ms = 0, wait_total_ms = 0, run_elapsed_s = 0;
    uint16_t card_no = 0;
    uint8_t delta_n = 0;
    uint16_t delta_runs = 0;
    uint8_t runnable = 0, skipped = 0, refusal = 0;   // the Review preview (Summary): units that run / skip, why START is refused
    uint16_t refusal_mv = 0;
    uint16_t pps_points = 0, avs_points = 0;          // the deep sweeps' point counts for the source in hand (0 = line hidden)
    uint8_t topology = 0, run_guard = 0;              // raguard::Topology as the guard reads it now / the run's journal byte
    uint8_t rmdo = 0;                                  // the live source's RMDO revision nibbles (major << 4 | minor), valid per flag
    std::array<char, kChargerTestViewMessage> message{};
    uint8_t message_len = 0;
};
size_t encode_charger_test_view(std::span<uint8_t> out, const ChargerTestView&);
struct StateInfo {
    uint8_t page = 0, active_cc = 0, spec_rev = 0, num_pdos = 0, pd_valid = 0;
    uint8_t chip = 0, pm_online = 0, log_on = 0;
    int32_t vbus_mv = 0, current_ma = 0, power_mw = 0, energy_mwh = 0;
    uint16_t dp_mv = 0, dm_mv = 0, cc1_mv = 0, cc2_mv = 0, aux_mv = 0;   // analog front end
    int16_t temp_deci_c = 0;  // board NTC
    int16_t mcu_temp_deci_c = 0;   // internal die sensor, valid when adc_flags bit3
    uint8_t fc = 0;           // fastcharge::Protocol
    uint8_t adc_flags = 0;    // bit0 reading valid, bit1 NTC valid, bit2 NTC calibrated, bit3 MCU temp valid
    uint8_t link_flags = 0;   // kLink* bits: charger data stale / waiting (kLinkWaiting = re-attached, no caps yet, OR a charger waiting for a sink per the Rp-trace classifier), inline, cable stale / scanned, crossed CC / flip hint, no PD this attach (kLinkNoPd, the settled verdict; the why rides the existing pd_valid/caps fields)
    uint32_t age_s = 0;       // seconds since the charger was unplugged (0 while live)
    uint32_t cable_age_s = 0; // seconds since the e-marker scan
    uint32_t reset_flags = 0; // CRM CTRLSTS reset flags of this boot (bits 26..31: PIN POR SW IWDG WWDG LPWR)
};
constexpr uint8_t kLinkStale = 1 << 0, kLinkWaiting = 1 << 1, kLinkInline = 1 << 2,
                  kLinkCableStale = 1 << 3, kLinkCableScanned = 1 << 4,
                  kLinkCrossed = 1 << 5,    // crossed CC through the K2 (pd::Link::cc_cross == Crossed): flip one cable
                  kLinkFlipHint = 1 << 6,   // Rp-vs-Ra on one line, nothing on the other (cc_cross == Hint): "Flip a cable?"
                  kLinkNoPd = 1 << 7;       // the settled "PD present" verdict is None this attach (ROADMAP 2026-11-10, Row C)
// GET_STATE part 1 (session): the chrome and the derived session facts the screens show (Ui::Session, field for field).
// sel = 0xFF and count = 0 when the page has no list; open_cc_mv = 0xFFFF when there is no standby reading.
constexpr size_t kSessionLen = 31;
struct SessionInfo {
    int32_t mah = 0;
    uint32_t elapsed_s = 0;
    uint8_t proto_kind = 0;
    uint16_t limit_ma = 0, max_mv = 0, typec_ma = 0;
    uint8_t busy = 0, fc_release = 0, topology = 0, wait_kind = 0, pd_none_why = 0;
    uint16_t open_cc_mv = 0xFFFF;
    uint8_t depth = 0;
    std::array<uint8_t, 3> stack{0xFF, 0xFF, 0xFF};
    uint8_t sub = 0, sel = 0xFF, count = 0, home_window = 0, flags = 0;
};
size_t encode_session(std::span<uint8_t> out, const SessionInfo&);
// GET_STATE part 2 (diag): the counters the Diag page prints. poll_gap_max_ms is the longest main-loop gap since the last
// [USB] heartbeat line (the driver zeroes it there), exactly what the page shows.
constexpr size_t kDiagLen = 26;
struct DiagInfo {
    uint8_t fusb_id = 0;
    uint32_t pd_rx = 0, usb_resets = 0, usb_isrs = 0, log_sent = 0, log_dropped = 0, poll_gap_max_ms = 0;
    uint8_t usb_configured = 0;
};
size_t encode_diag(std::span<uint8_t> out, const DiagInfo&);
// GET_STATE part 3 (keys): what the four buttons do on the page on screen (Ui::labels(), Button order Back Minus Ok Plus).
// Per button a short-press and a long-press word, NUL padded, at most kKeyChars (7) characters: the strip's own widest
// words are 7 ("Charger"), a longer one is cut. The words are the semantic labels, not the drawn text (the slot
// width drops the long word on a narrow slot; the host can tell from the strip geometry). flags: bit i = button i is dim,
// bit 4 = the page covers the strip (the QR code), so none of the four is drawn.
constexpr size_t kKeyChars = 7;
constexpr size_t kKeysLen = 1 + 4 * 2 * kKeyChars;   // 57, inside the 58 byte reply
struct KeyInfo { char shrt[kKeyChars + 1]{}; char lng[kKeyChars + 1]{}; };
struct KeysInfo { uint8_t flags = 0; KeyInfo k[4]; };
size_t encode_keys(std::span<uint8_t> out, const KeysInfo&);
struct AdcRawSnapshot {
    uint8_t ok = 0;
    std::array<uint16_t, kAdcRawAll> all{};      // channels 0..7, 16, 17
    std::array<uint16_t, kAdcScanSlots> scan{};  // aux cc1 cc2 dp dm ntc vref
};
// VCONN_Swap experiment (DebugVconn): pd::VconnState / pd::VconnWhy as bytes, `ok` = the op was accepted.
struct VconnSnapshot {
    uint8_t ok = 0, state = 0, why = 0, applied = 0, swaps_rx = 0, active_cc = 0;
    uint16_t cc1_mv = 0, cc2_mv = 0;
};
// Informational EPR_Get_Source_Cap (DebugEprInfo): `ok` = the op was accepted (a charger test is not running),
// `err` = pd::Err of the ask, `state` = pd::EprInfo (None, Pending, Got, Refused, Timeout), then the PDOs of the
// list that came back (raw words by position; EPR PDOs from position 8) when state is Got.
constexpr size_t kEprInfoPdos = 11;
struct EprInfoSnapshot {
    uint8_t ok = 0, err = 0, state = 0, num_pdos = 0;
    std::array<uint32_t, kEprInfoPdos> pdos{};
};
// LCD DMA state (DebugLcd): flags bit0 build option on, bit1 DMA still trusted (no stalled transfer). Behind it the paint
// totals since boot (k2/core/paint_meter.hpp): pixels sent to the panel, ms inside the lcd stream calls (wire wait and the
// service rounds served inside them included), ms inside the file-system flash writes; then the SPI3 clock divider
// (8 at every boot, 4 after the lab switch `lcd spi 4`); last the longest time the charger-test page sat blocked from
// painting in the current or last run (k2/core/gap_meter.hpp BlockMeter), u16 ms, 0 = never.
struct LcdSnapshot {
    uint8_t flags = 0;
    uint32_t transfers = 0, stalls = 0;
    uint32_t paint_px = 0, paint_ms = 0, fs_ms = 0;
    uint8_t spi_div = 8;
    uint16_t blocked_ms = 0;
};
struct RippleInfo {
    uint32_t period_ns = 0;
    uint16_t count = 0, min = 0, max = 0, mean = 0;
};
struct FcResult { uint8_t proto = 0, outcome = 0; uint16_t mv = 0; };
struct FcInfo {
    uint8_t state = 0, proto = 0, fail = 0;      // fc::State, fc::Proto, fc::Fail
    uint8_t scan_flags = 0;                      // bit0 scan running, bit1 scan done
    uint8_t scan_step = 0;
    uint16_t target_mv = 0, vbus_mv = 0;
    uint8_t num_results = 0;
    std::array<FcResult, kFcMaxResults> results{};
};
struct FactoryWipeInfo {   // k2::factory_wipe::Result on the wire (14 bytes)
    uint8_t err = 0;           // 0 wiped and verified (DFU follows), 1 erase failed, 2 verify failed
    uint16_t sectors = 0;      // sectors in the wipe list
    uint16_t erased = 0;       // erases that succeeded
    uint16_t verified = 0;     // sectors that read back all 0xFF
    uint16_t failed = 0;       // sectors that did not
    uint32_t first_fail = 0;   // address of the first failing sector, 0 when none
    uint8_t status = 0;        // k2::flash::Status of the failing erase
};
struct StoreInfo {
    uint8_t err = 0;       // k2::store::Err of the operation just run (0 for list)
    uint8_t count = 0;     // e-marker records in the library
    uint8_t flags = 0;     // kStoreFlag*
    uint8_t slot = 0;      // slot written by save-emarker
    uint16_t log_len = 0;  // bytes of saved log text
    uint8_t bank = 0;      // flash register set in use (2, 1, 0 = unknown)
    uint8_t native = 0;    // records in the native-format library page (read only), 0 = none
};
struct CableRInfo {
    uint8_t flags = 0;                           // bit0 idle reference taken, bit1 estimate valid, bit2 Kelvin mode
    uint8_t grade = 0;                           // cable_r::Grade
    uint16_t r_mohm = 0;
    uint16_t idle_mv = 0, idle_ma = 0, load_mv = 0, load_ma = 0;
    uint16_t pos_mohm = 0, gnd_mohm = 0;         // Kelvin mode only: the two legs of r_mohm
};
struct UfcsInfo {
    uint8_t state = 0;                  // 0 idle, 1 probe running, 2 probe done
    uint8_t verdict = 0;                // ufcs::ProbeVerdict
    uint8_t hs_class = 0;               // ufcs::HsClass
    uint8_t start = 0;                  // ufcs::ProbeStart of the last probe op (0 = started)
    uint16_t blob_len = 0;              // bytes the read op serves
};
struct LabResult {
    uint8_t status = 0;                 // remote::Status the backend reports (0 = Ok)
    uint8_t reason = 0, duty = 0;
    uint16_t ms = 0;
    int16_t idle_ma = 0, peak_ma = 0;
    uint16_t min_vbus_mv = 0, vbus_before_mv = 0;
    int16_t temp_before_dc = 0, temp_after_dc = 0;
    uint8_t pin_high_pct = 0;
};
struct SniffInfo {
    uint8_t status = 0;                 // remote::Status the backend reports (0 = Ok)
    uint8_t state = 0;                  // 0 idle, 1 armed, 2 recording
    uint8_t reason = 0;                 // edges::End of the last session
    uint8_t flags = 0;                  // bit0 buffer overflowed, bit1 a Black Box capture was stored
    uint16_t edges = 0;
};
struct PdoRec {
    uint8_t type = 0;
    uint32_t raw = 0;
    uint16_t min_mv = 0, max_mv = 0, max_ma = 0;
    uint32_t max_mw = 0;
};
struct PdoSnapshot {
    uint8_t valid = 0, spec_rev = 0, num_pdos = 0;
    std::array<PdoRec, kMaxPdos> pdo{};
};
struct EmarkerSnapshot {
    uint8_t valid = 0, is_active = 0;
    uint16_t vid = 0, pid = 0;
    uint32_t xid = 0;
    uint8_t hw_rev = 0, fw_rev = 0, max_a = 0, max_v = 0;
    uint16_t max_w = 0;
    uint8_t num_vdos = 0;
    std::string_view vendor{}, speed{};
    std::array<uint32_t, kMaxVdos> vdo{};
    // Part 2: cable detail.
    uint8_t sop = 0, spec_rev = 0, vdo_version = 0, latency_code = 0, termination = 0;
    uint8_t flags = 0;                       // kEmFlag* bits
    uint8_t max_temp_c = 0, shutdown_temp_c = 0, u3_power = 0, usb_lanes = 0;
};

constexpr uint8_t kEmFlagTypeC = 1 << 0, kEmFlagVconn = 1 << 1, kEmFlagEpr = 1 << 2,
                  kEmFlagVdo2 = 1 << 3, kEmFlagUsb4 = 1 << 4, kEmFlagGen2 = 1 << 5,
                  kEmFlagOptical = 1 << 6, kEmFlagThunderbolt = 1 << 7;

constexpr size_t kMaxIdSvids = 8;
constexpr uint8_t kIdFlagEpr = 1 << 0, kIdFlagEprPartial = 1 << 1, kIdFlagCuktech = 1 << 2,
                  kIdFlagXiaomi = 1 << 3;

struct IdentitySnapshot {
    uint8_t flags = 0, valid = 0;
    uint16_t vid = 0, pid = 0, bcd = 0;
    uint8_t num_svids = 0;
    std::array<uint16_t, kMaxIdSvids> svids{};
    std::string_view vendor{}, model{};
};

// Result of a trigger operation: the status the dispatcher answers with and the pd::Err value.
struct PdOpResult { Status status = Status::Ok; uint8_t err = 0; };
// Result of the phase-1 lab hand-back: the verdict (pd::HandbackVerdict) rides the reply payload, allowed or refused.
struct HandbackResult { Status status = Status::Ok; uint8_t verdict = 0; };
struct PdTriggerSnapshot {
    uint8_t state = 0, result = 0, held = 0, epr = 0, pos = 0;
    uint16_t mv = 0, ma = 0;
    uint32_t hard_resets = 0;          // Hard Resets the K2 sent
    uint32_t source_hard_resets = 0;   // Hard Resets the source sent (appended: clients that read 13 bytes ignore it)
    uint8_t pin = 0;                   // fusb302::pin_rd(): 1 / 2 = an Rd is pinned on CC1 / CC2, 0 = none (appended: clients that read 17 bytes ignore it)
    uint8_t trial_line = 0;            // fusb302::line_trial(): the line (1 / 2) our single Rd is trying while the direct-plug schedule is live, else 0 (appended: clients that read 18 bytes ignore it)
    uint8_t trial_moves = 0;           // how often the direct-plug fallback moved the Rd this attach (appended after trial_line)
};
// Result of PD_ATTACH: Ok carries the trigger snapshot; any other status is a refusal (err = pd::Err) and the
// snapshot is not sent. `raised` (not on the wire): the attach actually started or holds a contract above 5 V (a request
// above 5 V was accepted by the sink engine). An attach that only presented Rd, found no PDO, or whose request failed
// raised nothing, so the dead-man lease must not arm for it.
struct PdAttachResult { Status status = Status::Ok; uint8_t err = 0; PdTriggerSnapshot snapshot{}; bool raised = false; };
// PD_MATCH payload (ui_match.hpp verdict): [caps_seen, request_seen, sink_ok, flags, verdict, rows,
// want_pos, want_mv u16, want_ma u16, need_ma u16, have_ma u16, src_max_mv u16, src_max_w u16 (watts),
// row state per PDO (rows bytes)]. Row states: 0 unknown, 1 selected, 2 selected low, 3 ok, 4 low, 5 no,
// 6 gone (seen earlier from this source, not in its latest caps).
constexpr size_t kMatchMaxRows = 11;
// USB_LINK payload (k2/pd/usb_link.hpp): [family, tbt, confidence, source, flags (b0 cable limited, b1 DP 4-lane,
// b2 billboard), mbps u32, text...]. family: 0 unknown 1 low speed 2 full speed 3 USB2 4 USB3.2 5 USB4 6 USB4v2;
// tbt 0..3 = none / TBT3 / TBT4 / TBT5; confidence 0..3 = none / hint / advertised / negotiated; source 0 none
// 1 D+/D- 2 identity 3 cable 4 Enter_USB 5 Enter_Mode TBT 6 DP alt 7 Data_Reset; text is the short UI line.
struct UsbLinkSnapshot {
    uint8_t family = 0, tbt = 0, conf = 0, source = 0, flags = 0;
    uint32_t mbps = 0;
    std::array<char, 28> text{};
    uint8_t text_len = 0;
};
struct MatchSnapshot {
    uint8_t caps_seen = 0, request_seen = 0, sink_ok = 0, flags = 0, verdict = 0, rows = 0, want_pos = 0;
    uint16_t want_mv = 0, want_ma = 0, need_ma = 0, have_ma = 0, src_max_mv = 0, src_max_w = 0;
    std::array<uint8_t, kMatchMaxRows> row{};
};
constexpr size_t kMaxSinkPdos = 7;
struct IdentifyProgress {
    uint8_t state = 0, step = 0, ok_mask = 0, skipped_mask = 0, retries = 0, num_sink_pdos = 0;
    std::array<uint32_t, kMaxSinkPdos> sink_pdos{};
    uint32_t revision = 0;
};
struct IdentifyResult { Status status = Status::Ok; uint8_t err = 0; IdentifyProgress progress{}; };   // err: pd::Err of a refusal, sent as the one-byte payload
constexpr uint8_t kRereadMaxMask = 31;   // five steps (pd::kRereadMaskAll)
constexpr uint8_t kSinkCapExtMaxMode = 2;   // pd::SinkCapExtMode: 0 Silent, 1 Skedb, 2 NotSupported
struct RereadInfo {
    uint8_t state = 0, step = 0, mask = 0, ok_mask = 0, skipped_mask = 0, pdos_before = 0, pdos_last = 0, caps_rx = 0, flags = 0;
};
struct RereadResult { Status status = Status::Ok; RereadInfo info{}; };

// Lab-only Apple charger readout (SVID 0x05AC, k2/pd/apple_readout.hpp). why/reads/ok mirror
// pd::AppleInfo; the six strings are NUL-terminated on the wire (the serial is a result field only -
// it never goes into a binlog %s).
struct AppleReadInfo {
    uint8_t armed = 0, why = 0, reads = 0, ok = 0;
    std::string_view maker{}, product{}, fw{}, hw{}, field90{}, serial{};
};
struct AppleReadResult { Status status = Status::Ok; uint8_t refusal = 0; AppleReadInfo info{}; };   // refusal: pd::AppleRefusal as the one-byte payload

// STOP reply: what was active before (stop::kTest | kFc | kPd | kSpoof), the latest VBUS sample after the actions, and
// whether the measured FC release window is still open (the VBUS then may not have settled yet).
using StopInfo = k2::stop::Report;
// LEASE reply: the host dead-man lease (k2/core/lease.hpp): armed, seconds left (rounded up), the length, why the last one ended.
using LeaseInfo = k2::lease::Info;

struct ScreenInfo { uint16_t width = 240, height = 240; uint8_t strip_rows = 4; uint8_t num_strips = 60; };

// Writes one stream frame of a screenshot into `out` (zero padded).
size_t build_stream_frame(std::span<uint8_t, kFrameSize> out, uint8_t seq, uint8_t strip, uint8_t flags,
                          std::span<const uint8_t> rle = {});

// Writes one stream frame of a ripple capture into `out` (zero padded): the index of the first
// sample, then up to kRippleChunk samples. Returns the number of meaningful bytes.
size_t build_ripple_frame(std::span<uint8_t, kFrameSize> out, uint8_t seq, uint16_t offset,
                          std::span<const uint16_t> samples);

// Payload encoders: return the number of bytes written (never more than out.size()).
size_t encode_ping(std::span<uint8_t> out, const PingInfo &p);
size_t encode_charger_test(std::span<uint8_t> out, const ChargerTestSnapshot&, uint8_t start, bool rows,
                           uint8_t schema = kChargerTestSchema);
size_t encode_state(std::span<uint8_t> out, const StateInfo &s);
size_t encode_pdo(std::span<uint8_t> out, const PdoSnapshot &s, uint8_t index);
size_t encode_screen_info(std::span<uint8_t> out, const ScreenInfo &i);
size_t encode_emarker(std::span<uint8_t> out, const EmarkerSnapshot &e, uint8_t part);
size_t encode_identity(std::span<uint8_t> out, const IdentitySnapshot &s);
size_t encode_pd_trigger(std::span<uint8_t> out, const PdTriggerSnapshot &s);
size_t encode_match(std::span<uint8_t> out, const MatchSnapshot &s);
size_t encode_usb_link(std::span<uint8_t> out, const UsbLinkSnapshot &s);
size_t encode_identify_progress(std::span<uint8_t> out, const IdentifyProgress &p);
size_t encode_reread(std::span<uint8_t> out, const RereadInfo &i);
size_t encode_apple_read(std::span<uint8_t> out, const AppleReadInfo &i, uint8_t part);   // part 0 header, 1..6 one field each
size_t encode_stop(std::span<uint8_t> out, const StopInfo &s);   // 6 bytes: active, vbus_mv i32 LE, fc_release_busy
size_t encode_lease(std::span<uint8_t> out, const LeaseInfo &l);   // 6 bytes: armed, left_s u16 LE, length_s u16 LE, ended
size_t encode_adc_raw(std::span<uint8_t> out, const AdcRawSnapshot &a);
size_t encode_vconn(std::span<uint8_t> out, const VconnSnapshot &v);
size_t encode_epr_info(std::span<uint8_t> out, const EprInfoSnapshot &s);
size_t encode_lcd(std::span<uint8_t> out, const LcdSnapshot &s);
// DebugCcProbe reply: flags(1) n(1) sw0(1) sw1(1) pc(1) modes(2) of the snapshot that was restored, then n samples of
// t_us(4) cc1_raw(2) cc2_raw(2) vbus_mv(2) st0_cc1(1) st0_cc2(1). At most 7 + 4 * 12 = 55 bytes.
size_t encode_cc_probe(std::span<uint8_t> out, const ccprobe::Result &r);
size_t encode_fc(std::span<uint8_t> out, const FcInfo &i);
size_t encode_store(std::span<uint8_t> out, const StoreInfo &i);
size_t encode_factory_wipe(std::span<uint8_t> out, const FactoryWipeInfo &i);
size_t encode_cable_r(std::span<uint8_t> out, const CableRInfo &i);
size_t encode_lab(std::span<uint8_t> out, const LabResult &r);
size_t encode_sniff(std::span<uint8_t> out, const SniffInfo &r);
size_t encode_vooc(std::span<uint8_t> out, const VoocSnapshot &v, uint8_t page);   // vooc-monitor: FC op 4, one page
size_t encode_ufcs(std::span<uint8_t> out, const UfcsInfo &i);   // status block: state verdict class start blob_len(2) nfields
// One tuner field: field(1) value(4) lo(4) hi(4) default(4); lo / hi / default come from ufcs::kFields.
size_t encode_ufcs_field(std::span<uint8_t> out, uint8_t field, uint32_t value);
size_t encode_ripple(std::span<uint8_t> out, uint8_t code, const RippleInfo &r);

// Cmd::ChargerCable: the backend's status and the identity state after the (optional) set.
struct ChargerCableReply {
    Status status = Status::Ok;
    ctcable::Info info;
};
size_t encode_charger_cable(std::span<uint8_t> out, const ctcable::Info &i);

// Cmd::ChargerHunt: request [slot]; reply run_id(4), count(1) = refused hunt rows held, then the slot's evidence when slot < count:
// step, cause, answer, last_step, caps, pos, type (u8 each), flags, apdo_ma, req_mv, req_ma, cable_ma, authed_ms, answer_ms (u16
// each), apdo, rdo (u32 each). charger_test::HuntEvidence field for field; a slot at or past the count returns the header only.
inline constexpr size_t kHuntSlotBytes = 7 + 7 * 2 + 2 * 4;   // 29
static_assert(5 + kHuntSlotBytes <= kMaxRspPayload);
struct ChargerHuntReply {
    Status status = Status::Ok;
    uint32_t run_id = 0;
    uint8_t count = 0;
    charger_test::HuntEvidence slot;   // valid when the asked slot < count
};
size_t encode_charger_hunt(std::span<uint8_t> out, const ChargerHuntReply &r, bool with_slot);

// What the firmware (or a test fake) provides to the dispatcher. A duck-typed seam rather than
// a virtual class: no vtables, no __cxa_pure_virtual in the image.
template <class B>
concept Backend = requires(B &b, uint8_t u8, uint16_t u16, bool on) {
    { b.ping() } -> std::same_as<PingInfo>;
    { b.charger_test_results() } -> std::same_as<ChargerTestSnapshot>;
    { b.charger_test_view() } -> std::same_as<ChargerTestView>;   // 0x0B req [3]: the wizard / run chrome (a read, never moves anything)
    { b.charger_units(std::span<const uint8_t>{}) } -> std::same_as<Status>;   // unit order of the next run (empty = default)
    { b.charger_sweep(true) } -> std::same_as<Status>;   // the PPS deep sweep of the next run (opt-in)
    { b.charger_avs_sweep(true) } -> std::same_as<Status>;   // the AVS deep sweep of the next run (opt-in)
    { b.charger_cable(static_cast<const ctcable::Choice *>(nullptr)) } -> std::same_as<ChargerCableReply>;   // null = read only
    { b.charger_hunt(u8) } -> std::same_as<ChargerHuntReply>;   // evidence of refused hunt row `u8`
    { b.press(u8, u16) } -> std::same_as<Status>;
    { b.state() } -> std::same_as<StateInfo>;
    { b.session() } -> std::same_as<SessionInfo>;   // 0x03 req [1]: the session chrome (a read, never moves anything)
    { b.diag() } -> std::same_as<DiagInfo>;         // 0x03 req [2]: the Diag page counters (a read)
    { b.keys() } -> std::same_as<KeysInfo>;         // 0x03 req [3]: the strip labels (a read)
    { b.pdos() } -> std::same_as<PdoSnapshot>;
    { b.emarker() } -> std::same_as<EmarkerSnapshot>;
    { b.identity() } -> std::same_as<IdentitySnapshot>;
    { b.action(u8, true) } -> std::same_as<Status>;
    { b.set_page(u8) } -> std::same_as<Status>;
    { b.reboot() } -> std::same_as<void>;
    { b.debug_fault(u8) } -> std::same_as<void>;  // the fault follows the response
    { b.dfu_enter(u8) } -> std::same_as<void>;    // reset mode; the reset follows the response
    { b.factory_wipe(u8) } -> std::same_as<FactoryWipeInfo>;   // runs the wipe now; on success DFU follows the response
    { b.flash_read(uint32_t{}, std::span<uint8_t>{}) } -> std::same_as<void>;   // copies memory-mapped flash; the dispatcher checks the window
    { b.boot_text(on) } -> std::same_as<uint16_t>;   // renders the K2B1 text when true (offset 0) and returns its length
    { b.boot_text_read(u16, std::span<uint8_t>{}) } -> std::same_as<void>;   // copies bytes of the held text from the offset
    { b.boot_sample_count() } -> std::same_as<uint16_t>;
    { b.boot_sample(u16) } -> std::same_as<bootinfo::PowerSample>;
    { b.screenshot() } -> std::same_as<Status>;
    { b.screen_info() } -> std::same_as<ScreenInfo>;
    { b.debug_fake_pdos(u8) } -> std::same_as<Status>;
    { b.debug_pd_msg(u8, u16, std::span<const uint8_t>{}) } -> std::same_as<Status>;
    { b.debug_cc_matrix(u8, u16) } -> std::same_as<Status>;
    { b.debug_vconn(u8) } -> std::same_as<VconnSnapshot>;
    { b.debug_epr_info(u8) } -> std::same_as<EprInfoSnapshot>;
    { b.debug_lcd() } -> std::same_as<LcdSnapshot>;
    { b.debug_cc_probe(std::declval<const ccprobe::Args &>(), std::declval<ccprobe::Result &>()) } -> std::same_as<Status>;
    { b.debug_link(on) } -> std::same_as<Status>;
    { b.log_enabled() } -> std::same_as<bool>;
    { b.set_log(on) } -> std::same_as<void>;
    { b.adc_raw() } -> std::same_as<AdcRawSnapshot>;
    { b.ripple(u8) } -> std::same_as<Status>;     // runs the capture, keeps the samples
    { b.ripple_info() } -> std::same_as<RippleInfo>;
    { b.identify_device(u8) } -> std::same_as<IdentifyResult>;
    { b.reread(u8, u8) } -> std::same_as<RereadResult>;   // op (0 status, 1 start, 2 abort), step mask
    { b.apple_read(u8) } -> std::same_as<AppleReadResult>;   // lab-only: op 0 status, 1 arm + re-request the current contract
    { b.pd_request(u8, u16, u16, on, on) } -> std::same_as<PdOpResult>;   // pos, mv, ma, confirm, over_load (lab only)
    { b.pd_release() } -> std::same_as<PdOpResult>;
    { b.stop() } -> std::same_as<StopInfo>;   // the one-call stop (k2/core/stop.hpp)
    { b.lease_touch() } -> std::same_as<void>;   // any valid request from the host refreshes a running lease
    { b.lease_arm() } -> std::same_as<void>;   // an accepted command that can hold the output above 5 V arms it
    { b.lease(u8, u16) } -> std::same_as<LeaseInfo>;   // op 0 status, op 1 set the length (seconds); returns the state
    { b.pd_hard_reset(u8) } -> std::same_as<PdOpResult>;   // flags byte: kPdFlagConfirm | kPdFlagResetInline (0 for the shell/UI paths)
    { b.pd_handback(u8) } -> std::same_as<HandbackResult>;  // flags byte: kPdFlagConfirm only; the verdict is the reply payload
    { b.pd_attach(u16, u16, on) } -> std::same_as<PdAttachResult>;   // mv, ma, confirm: refusal = status + err, no snapshot
    { b.pd_trigger() } -> std::same_as<PdTriggerSnapshot>;
    { b.match_info() } -> std::same_as<MatchSnapshot>;
    { b.usb_link() } -> std::same_as<UsbLinkSnapshot>;
    { b.fc_cmd(u8, u8, u16, u8) } -> std::same_as<Status>;   // op, proto, mV, flags (kFcConfirm | kFcAllowInline)
    { b.fc_info() } -> std::same_as<FcInfo>;
    { b.vooc_monitor(u8) } -> std::same_as<VoocSnapshot>;   // vooc-monitor: 0 off, 1 on, 2 keep; then the summary
    { b.store_cmd(u8, u8) } -> std::same_as<StoreInfo>;   // op (list / save-emarker / save-log), slot unused
    { b.cable_r_reset() } -> std::same_as<void>;      // clears the passive reference
    { b.cable_r_mode(true) } -> std::same_as<void>;   // false = passive meter, true = Kelvin 8-wire
    { b.cable_r_info() } -> std::same_as<CableRInfo>;
    { b.lab_load(uint8_t{}, uint16_t{}) } -> std::same_as<LabResult>;   // PA8 load probe
    { b.lab_sniff(uint8_t{}) } -> std::same_as<SniffInfo>;               // D+/D- edge capture
    { b.ufcs_info() } -> std::same_as<UfcsInfo>;
    { b.ufcs_get(u8) } -> std::same_as<std::uint32_t>;                   // field index (checked by the dispatcher)
    { b.ufcs_set(u8, std::uint32_t{}) } -> std::same_as<Status>;         // field, value (range checked); Busy while a probe runs
    { b.ufcs_op(u8) } -> std::same_as<Status>;                           // kUfcsReset / kUfcsProbe / kUfcsStop
    { b.ufcs_read(uint16_t{}, std::span<uint8_t>{}) } -> std::same_as<size_t>;   // blob bytes from off, returns how many
    { b.qrtest_show(u8, u8, u8) } -> std::same_as<Status>;   // version, px, quiet: open the full-screen test QR page
    { b.qrtest_stop() } -> std::same_as<bool>;               // pop the test page when it is showing
    { b.blackbox(u8, std::uint32_t{}, std::span<uint8_t>{}) } -> std::same_as<size_t>;   // op, arg, out; returns the payload size
    { b.ports_busy() } -> std::same_as<bool>;   // a charger test or an FC release owns the link (Ui::ports_busy)
    { b.emlib(std::span<const uint8_t>{}, std::span<uint8_t>{}) } -> std::same_as<size_t>;   // whole request payload (checked), out; returns the payload size
    { b.cards(std::span<const uint8_t>{}, std::span<uint8_t>{}) } -> std::same_as<size_t>;   // same shape as emlib
    { b.transcap(std::span<const uint8_t>{}, std::span<uint8_t>{}) } -> std::same_as<size_t>;   // same shape as emlib
};

namespace detail {
inline uint16_t rd16(const uint8_t *p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
inline uint32_t rd32(const uint8_t *p) { return rd16(p) | (static_cast<uint32_t>(rd16(p + 2)) << 16); }
}

// Parses `raw`, runs the command on `backend` and writes the response into `out`.
// Returns false when the report is not a request (no response is due); a valid header with a
// bad body (truncated / oversize / wrong length / unknown command) still gets an error response.
template <Backend B>
bool handle_request(std::span<const uint8_t> raw, B &backend, std::span<uint8_t, kFrameSize> out) {
    const ParseResult r = parse_request(raw);
    if (!r.header_valid) return false;
    backend.lease_touch();   // any request from a live host (malformed ones too) keeps its lease; it arms nothing

    auto reply =[&](Status st, std::span<const uint8_t> payload = {}) {
        build_response(out, r.cmd, r.seq, st, payload);
        return true;
    };
    if (r.error != ParseError::None) return reply(Status::BadLength);

    std::array<uint8_t, kMaxRspPayload> buf{};
    const auto p = r.payload;
    auto reply_n = [&](size_t n) { return reply(Status::Ok, std::span<const uint8_t>(buf.data(), n)); };

    switch (static_cast<Cmd>(r.cmd)) {
    case Cmd::Ping:
        if (!p.empty()) return reply(Status::BadLength);
        return reply_n(encode_ping(buf, backend.ping()));
    case Cmd::ChargerTest: {
        if (!p.empty() && p.size() != 1 && p.size() != 9 && p.size() != 10) return reply(Status::BadLength);
        if (p.size() == 1 && p[0] == kChargerTestViewReq) return reply_n(encode_charger_test_view(buf, backend.charger_test_view()));
        const bool modern = p.size() == 1 || p.size() == 10;
        if (modern && p[0] != kChargerTestSchema2) return reply(Status::BadArg);
        const uint8_t schema = modern ? kChargerTestSchema2 : kChargerTestSchema;
        const bool rows = p.size() > 1;
        const auto q = modern ? p.subspan(1) : p;
        const auto s = backend.charger_test_results();
        const uint8_t start = rows ? q[0] : 0;
        if (rows && (detail::rd32(&q[1]) != s.run_id || detail::rd32(&q[5]) != s.revision))
            return reply(Status::Busy);
        if (start > charger_test_count(s, schema)) return reply(Status::BadArg);
        return reply_n(encode_charger_test(buf, s, start, rows, schema));
    }
    case Cmd::ChargerUnits: {
        if (p.empty() || p.size() != size_t(p[0]) + 1) return reply(Status::BadLength);
        if (p[0] > charger_test::kMaxUnits) return reply(Status::BadArg);
        uint32_t seen = 0;
        for (const uint8_t u : p.subspan(1)) {
            if (u >= static_cast<uint8_t>(charger_test::Unit::Count) || (seen & (1u << u))) return reply(Status::BadArg);
            seen |= 1u << u;
        }
        const Status st = backend.charger_units(p.subspan(1));
        if (st != Status::Ok) return reply(st);
        buf[0] = p[0];
        return reply_n(1);
    }
    case Cmd::ChargerCable: {
        if (p.size() != 0 && p.size() != 3) return reply(Status::BadLength);
        ctcable::Choice choice;
        if (!p.empty()) {
            if (p[0] > static_cast<uint8_t>(ctcable::Kind::Saved)) return reply(Status::BadArg);
            choice = {static_cast<ctcable::Kind>(p[0]), detail::rd16(&p[1])};
            if (!ctcable::valid(choice)) return reply(Status::BadArg);
        }
        const auto r = backend.charger_cable(p.empty() ? nullptr : &choice);
        if (r.status != Status::Ok) return reply(r.status);
        return reply_n(encode_charger_cable(buf, r.info));
    }
    case Cmd::ChargerHunt: {
        if (p.size() != 1) return reply(Status::BadLength);
        if (p[0] >= charger_test::kHuntSlots) return reply(Status::BadArg);
        const auto r = backend.charger_hunt(p[0]);
        if (r.status != Status::Ok) return reply(r.status);
        return reply_n(encode_charger_hunt(buf, r, p[0] < r.count));
    }
    case Cmd::ChargerSweep: {
        if (p.size() != 1 || p[0] > 1) return reply(Status::BadLength);
        const Status st = backend.charger_sweep(p[0] != 0);
        if (st != Status::Ok) return reply(st);
        buf[0] = p[0];
        return reply_n(1);
    }
    case Cmd::ChargerAvsSweep: {
        if (p.size() != 1 || p[0] > 1) return reply(Status::BadLength);
        const Status st = backend.charger_avs_sweep(p[0] != 0);
        if (st != Status::Ok) return reply(st);
        buf[0] = p[0];
        return reply_n(1);
    }
    case Cmd::Button: {
        if (p.size() != 3) return reply(Status::BadLength);
        const uint16_t ms = detail::rd16(&p[1]);
        if (p[0] > 3 || ms < kMinPressMs || ms > kMaxPressMs) return reply(Status::BadArg);
        const Status st = backend.press(p[0], ms);
        if (st != Status::Ok) return reply(st);
        buf[0] = p[0]; buf[1] = p[1]; buf[2] = p[2];
        return reply_n(3);
    }
    case Cmd::GetState:
        if (p.size() > 1) return reply(Status::BadLength);
        if (p.empty() || p[0] == 0) return reply_n(encode_state(buf, backend.state()));
        if (p[0] == 1) return reply_n(encode_session(buf, backend.session()));
        if (p[0] == 2) return reply_n(encode_diag(buf, backend.diag()));
        if (p[0] == 3) return reply_n(encode_keys(buf, backend.keys()));
        return reply(Status::BadArg);
    case Cmd::GetPdos: {
        if (p.size() > 1) return reply(Status::BadLength);
        const uint8_t index = p.empty() ? 0 : p[0];
        const PdoSnapshot snap = backend.pdos();
        if (index >= kMaxPdos || (index > 0 && index >= snap.num_pdos)) return reply(Status::BadArg);
        return reply_n(encode_pdo(buf, snap, index));
    }
    case Cmd::GetEmarker: {
        if (p.size() > 1) return reply(Status::BadLength);
        const uint8_t part = p.empty() ? 0 : p[0];
        if (part > 2) return reply(Status::BadArg);
        return reply_n(encode_emarker(buf, backend.emarker(), part));
    }
    case Cmd::GetIdentity: {
        if (!p.empty()) return reply(Status::BadLength);
        return reply_n(encode_identity(buf, backend.identity()));
    }
    case Cmd::Action: {
        // payload: action id, optional second byte 1 = confirm (inline: may interrupt charging; FcScan: raises VBUS)
        if (p.size() != 1 && p.size() != 2) return reply(Status::BadLength);
        const bool confirm = p.size() == 2 && p[1] != 0;
        if (action_needs_confirm(p[0]) && !confirm) return reply(Status::NeedConfirm);
        const Status st = backend.action(p[0], confirm);
        if (st != Status::Ok) return reply(st);
        if (p[0] == kActionFcScan) backend.lease_arm();   // the scan raises VBUS under host control (accepted, so confirmed)
        buf[0] = p[0];
        return reply_n(1);
    }
    case Cmd::SetPage: {
        if (p.size() != 1) return reply(Status::BadLength);
        const Status st = backend.set_page(p[0]);
        if (st != Status::Ok) return reply(st);
        buf[0] = p[0];
        return reply_n(1);
    }
    case Cmd::Reboot:
        if (!p.empty()) return reply(Status::BadLength);
        backend.reboot();
        return reply_n(0);
    case Cmd::DfuEnter:
        if (p.size() != 4 && p.size() != 5) return reply(Status::BadLength);
        if (detail::rd32(p.data()) != kDfuMagic) return reply(Status::BadArg);
        if (p.size() == 5 && p[4] > 1) return reply(Status::BadArg);
        backend.dfu_enter(p.size() == 5 ? p[4] : 0);
        return reply_n(0);
    case Cmd::FactoryWipe:
        if (p.size() != 4 && p.size() != 5) return reply(Status::BadLength);
        if (detail::rd32(p.data()) != kFactoryWipeMagic) return reply(Status::BadArg);
        if (p.size() == 5 && p[4] > 1) return reply(Status::BadArg);
        return reply_n(encode_factory_wipe(buf, backend.factory_wipe(p.size() == 5 ? p[4] : 0)));
    case Cmd::FlashDump: {
        if (p.size() != 5) return reply(Status::BadLength);
        const uint32_t addr = detail::rd32(p.data());
        const uint8_t n = p[4];
        // 64-bit arithmetic: a window at the top of the address space cannot wrap around
        if (n == 0 || n > kFlashDumpMax || addr < kFlashDumpBase || uint64_t{addr} + n > kFlashDumpEnd) return reply(Status::BadArg);
        for (size_t i = 0; i < 4; i++) buf[i] = p[i];
        buf[4] = n;
        backend.flash_read(addr, std::span<uint8_t>(buf.data() + 5, n));
        return reply_n(5 + n);
    }
    case Cmd::BootInfo: {
        if (p.size() != 3) return reply(Status::BadLength);
        const uint8_t part = p[0];
        const uint16_t off = detail::rd16(&p[1]);
        if (part == kBootInfoText) {
            const uint16_t total = backend.boot_text(off == 0);
            if (off > total) return reply(Status::BadArg);
            const size_t n = std::min<size_t>(kBootInfoChunk, total - off);
            buf[0] = part; buf[1] = uint8_t(off); buf[2] = uint8_t(off >> 8);
            buf[3] = uint8_t(total); buf[4] = uint8_t(total >> 8); buf[5] = uint8_t(n);
            backend.boot_text_read(off, std::span<uint8_t>(buf.data() + 6, n));
            return reply_n(6 + n);
        }
        if (part == kBootInfoSamplePart) {
            const uint16_t total = backend.boot_sample_count();
            if (off > total) return reply(Status::BadArg);
            const size_t n = std::min<size_t>(kBootInfoSamples, total - off);
            buf[0] = part; buf[1] = uint8_t(off); buf[2] = uint8_t(off >> 8);
            buf[3] = uint8_t(total); buf[4] = uint8_t(total >> 8); buf[5] = uint8_t(n);
            for (size_t i = 0; i < n; i++) {
                const bootinfo::PowerSample s = backend.boot_sample(uint16_t(off + i));
                uint8_t *d = &buf[6 + i * kBootInfoSampleBytes];
                const uint16_t v[4] = {s.vbus_mv, s.cc1_mv, s.cc2_mv, s.aux_mv};
                for (size_t k = 0; k < 4; k++) { d[2 * k] = uint8_t(v[k]); d[2 * k + 1] = uint8_t(v[k] >> 8); }
                d[8] = s.plus ? 1 : 0;
            }
            return reply_n(6 + n * kBootInfoSampleBytes);
        }
        return reply(Status::BadArg);
    }
    case Cmd::DebugFault:
        if (p.size() != 5) return reply(Status::BadLength);
        if (detail::rd32(p.data()) != kFaultMagic || p[4] > 2) return reply(Status::BadArg);
        backend.debug_fault(p[4]);
        return reply_n(0);
    case Cmd::Screenshot: {
        if (!p.empty()) return reply(Status::BadLength);
        const Status st = backend.screenshot();
        if (st != Status::Ok) return reply(st);
        return reply_n(encode_screen_info(buf, backend.screen_info()));
    }
    case Cmd::DebugPdos: {
        if (p.size() != 1) return reply(Status::BadLength);
        if (p[0] > kMaxPdos) return reply(Status::BadArg);
        const Status st = backend.debug_fake_pdos(p[0]);
        if (st != Status::Ok) return reply(st);
        buf[0] = p[0];
        return reply_n(1);
    }
    case Cmd::DebugLink: {
        if (p.size() != 1) return reply(Status::BadLength);
        if (p[0] > 1) return reply(Status::BadArg);
        const Status st = backend.debug_link(p[0] != 0);
        if (st != Status::Ok) return reply(st);
        buf[0] = p[0];
        return reply_n(1);
    }
    case Cmd::DebugPdMsg: {
        // sop (0 SOP, 1 SOP', 2 SOP''), PD header (LE), then exactly header.num_do data objects.
        if (p.size() < 3) return reply(Status::BadLength);
        const uint16_t header = detail::rd16(&p[1]);
        const size_t body = ((header >> 12) & 7) * 4u;
        if (p.size() != 3 + body) return reply(Status::BadLength);
        if (p[0] > 2) return reply(Status::BadArg);
        const Status st = backend.debug_pd_msg(p[0], header, p.subspan(3));
        if (st != Status::Ok) return reply(st);
        return reply_n(0);
    }
    case Cmd::DebugCcMatrix: {
        if (p.size() < 1 || p.size() > 3 || p[0] > 7) return reply(Status::BadArg);
        const uint16_t modes = p.size() == 3 ? detail::rd16(&p[1]) : 0x333u;   // default: all OutPp50
        const Status st = backend.debug_cc_matrix(p[0], modes);
        if (st != Status::Ok) return reply(st);
        return reply_n(0);
    }
    case Cmd::DebugVconn:   // [op]: 0 status, 1 send VCONN_Swap, 2 VCONN off, 3 lab round trip (take the role undriven, hand it back)
        if (p.size() != 1) return reply(Status::BadLength);
        if (p[0] > 4) return reply(Status::BadArg);
        return reply_n(encode_vconn(buf, backend.debug_vconn(p[0])));
    case Cmd::DebugEprInfo:   // [op]: 0 status, 1 ask (gated), 2 ask regardless of the EPR bit
        if (p.size() != 1) return reply(Status::BadLength);
        if (p[0] > 2) return reply(Status::BadArg);
        return reply_n(encode_epr_info(buf, backend.debug_epr_info(p[0])));
    case Cmd::DebugLcd:
        if (!p.empty()) return reply(Status::BadLength);
        return reply_n(encode_lcd(buf, backend.debug_lcd()));
    case Cmd::DebugCcProbe: {
        // [sw0, pc, ms u16] or [sw0, pc, ms u16, modes u16]; the rules (ranges, VCONN only with PC13 low) are k2/pd/cc_probe.hpp.
        if (p.size() != 4 && p.size() != 6) return reply(Status::BadLength);
        ccprobe::Args a;
        a.sw0 = p[0]; a.pc = p[1]; a.ms = detail::rd16(&p[2]);
        if (p.size() == 6) a.modes = detail::rd16(&p[4]);
        if (ccprobe::check_args(a) != ccprobe::ArgError::None) return reply(Status::BadArg);
        ccprobe::Result res;
        const Status st = backend.debug_cc_probe(a, res);
        if (st != Status::Ok) return reply(st);
        return reply_n(encode_cc_probe(buf, res));
    }
    case Cmd::AdcRaw:
        if (!p.empty()) return reply(Status::BadLength);
        return reply_n(encode_adc_raw(buf, backend.adc_raw()));
    case Cmd::Ripple: {
        if (p.size() > 1) return reply(Status::BadLength);
        const uint8_t code = p.empty() ? 0 : p[0];
        if (code > 7) return reply(Status::BadArg);
        const Status st = backend.ripple(code);
        if (st != Status::Ok) return reply(st);
        return reply_n(encode_ripple(buf, code, backend.ripple_info()));
    }
    case Cmd::IdentifyDevice: {
        if (p.size() != 1) return reply(Status::BadLength);
        if (p[0] > 2) return reply(Status::BadArg);
        const IdentifyResult res = backend.identify_device(p[0]);
        if (res.status != Status::Ok) {
            buf[0] = res.err;   // a refusal carries its pd::Err (inline) as the one-byte payload
            return reply(res.status, std::span<const uint8_t>(buf.data(), 1));
        }
        return reply_n(encode_identify_progress(buf, res.progress));
    }
    case Cmd::PdReread: {
        if (p.empty()) return reply(Status::BadLength);
        if (p[0] > 8) return reply(Status::BadArg);
        const bool has_arg = p[0] == 1 || p[0] == 3 || p[0] == 4 || p[0] == 5 || p[0] == 6 || p[0] == 8;
        if (p.size() != (has_arg ? 2u : 1u)) return reply(Status::BadLength);
        if (p[0] == 1 && (p[1] == 0 || p[1] > kRereadMaxMask)) return reply(Status::BadArg);
        if (p[0] == 3 && p[1] > kSinkCapExtMaxMode) return reply(Status::BadArg);
        if (p[0] == 4 && p[1] > 2) return reply(Status::BadArg);
        if (p[0] == 5 && p[1] > 1) return reply(Status::BadArg);
        if (p[0] == 6 && p[1] > 2) return reply(Status::BadArg);
        if (p[0] == 8 && p[1] == 0) return reply(Status::BadArg);   // the Rd gap in 10 ms steps: 0 would be no gap
        const RereadResult res = backend.reread(p[0], has_arg ? p[1] : uint8_t{0});
        if (res.status != Status::Ok) return reply(res.status);
        return reply_n(encode_reread(buf, res.info));
    }
    case Cmd::AppleRead: {
        // Lab-only Apple charger readout: [op, part] - op 1 arms (replies the header); op 0 reads back
        // part 0 (the header) or part 1..6 (one field each). The backend gates the refusals.
        if (p.empty() || p.size() > 2) return reply(Status::BadLength);
        if (p[0] > 1) return reply(Status::BadArg);
        if (p[0] == 1 && p.size() != 1) return reply(Status::BadLength);
        if (p[0] == 0 && p.size() != 2) return reply(Status::BadLength);
        const uint8_t part = p[0] == 0 ? p[1] : uint8_t{0};
        if (part > 6) return reply(Status::BadArg);
        const AppleReadResult res = backend.apple_read(p[0]);
        if (res.status != Status::Ok) {
            buf[0] = res.refusal;   // the refusal reason (pd::AppleRefusal) as the one-byte payload
            return reply(res.status, std::span<const uint8_t>(buf.data(), 1));
        }
        return reply_n(encode_apple_read(buf, res.info, part));
    }
    case Cmd::PdRequest: {
        if (p.size() != 6) return reply(Status::BadLength);
        const PdOpResult res = backend.pd_request(p[0], detail::rd16(&p[2]), detail::rd16(&p[4]),
                                                   (p[1] & kPdFlagConfirm) != 0, (p[1] & kPdFlagOverLoad) != 0);
        if (res.status == Status::Ok && (p[0] != 1 || detail::rd16(&p[2]) > kFcSafeMv)) backend.lease_arm();   // above 5 V
        buf[0] = res.err;
        return reply(res.status, std::span<const uint8_t>(buf.data(), 1));
    }
    case Cmd::PdReset: {
        if (p.size() > 1) return reply(Status::BadLength);
        const uint8_t flags = p.empty() ? uint8_t{0} : p[0];
        if (flags & ~uint8_t(kPdFlagConfirm | kPdFlagResetInline)) return reply(Status::BadArg);
        const PdOpResult res = backend.pd_hard_reset(flags);
        buf[0] = res.err;
        return reply(res.status, std::span<const uint8_t>(buf.data(), 1));
    }
    case Cmd::PdRelease: {
        if (!p.empty()) return reply(Status::BadLength);
        const PdOpResult res = backend.pd_release();
        buf[0] = res.err;
        return reply(res.status, std::span<const uint8_t>(buf.data(), 1));
    }
    case Cmd::Stop: {
        // No confirm: every action here only moves toward 5 V / idle. The reply says what was active and where VBUS is.
        if (!p.empty()) return reply(Status::BadLength);
        return reply_n(encode_stop(buf, backend.stop()));
    }
    case Cmd::Lease: {
        // [0] status, [1, seconds u16 LE] set the length. Reads and sets never arm; the arming commands do (above).
        if (p.empty()) return reply(Status::BadLength);
        if (p[0] > 1) return reply(Status::BadArg);
        if (p.size() != (p[0] == 0 ? 1u : 3u)) return reply(Status::BadLength);
        const uint16_t secs = p[0] == 1 ? detail::rd16(&p[1]) : uint16_t{0};
        if (secs > k2::lease::kMaxSeconds) return reply(Status::BadArg);
        return reply_n(encode_lease(buf, backend.lease(p[0], secs)));
    }
    case Cmd::PdAttach: {
        // pd attach [target_mv [target_ma [flags]]]: attach as PD sink, request the target voltage, hold the contract.
        // Payload: target_mv (u16), optionally target_ma (u16), optionally flags (bit0 = confirm, the PD_REQUEST bit).
        // The backend decides (pd::attach_verdict): a refusal answers status + one err byte like PD_REQUEST, with no
        // snapshot; only an accepted attach replies the PdTriggerSnapshot.
        if (p.size() != 2 && p.size() != 4 && p.size() != 5) return reply(Status::BadLength);
        const uint8_t flags = p.size() == 5 ? p[4] : uint8_t{0};
        if (flags & ~uint8_t(kPdFlagConfirm)) return reply(Status::BadArg);
        const uint16_t target_mv = detail::rd16(&p[0]);
        const uint16_t target_ma = p.size() >= 4 ? detail::rd16(&p[2]) : 0;
        const PdAttachResult res = backend.pd_attach(target_mv, target_ma, (flags & kPdFlagConfirm) != 0);
        if (res.status != Status::Ok) {
            buf[0] = res.err;
            return reply(res.status, std::span<const uint8_t>(buf.data(), 1));
        }
        if (res.raised) backend.lease_arm();   // only an attach that started / holds a request above 5 V (not Rd-only, no PDO, a failed request)
        return reply_n(encode_pd_trigger(buf, res.snapshot));
    }
    case Cmd::PdHandback: {
        // pd handback [--confirm]: phase-1 lab op, the whole Fix sequence (record gone -> Rd dropped -> one Hard
        // Reset). Optional flags byte, bit0 confirm only; the reply payload is the HandbackVerdict (allowed or the
        // refusal reason, also logged on the K2).
        if (p.size() > 1) return reply(Status::BadLength);
        const uint8_t flags = p.empty() ? uint8_t{0} : p[0];
        if (flags & ~uint8_t(kPdFlagConfirm)) return reply(Status::BadArg);
        const HandbackResult res = backend.pd_handback(flags);
        buf[0] = res.verdict;
        return reply(res.status, std::span<const uint8_t>(buf.data(), 1));
    }
    case Cmd::PdTriggerStatus:
        if (!p.empty()) return reply(Status::BadLength);
        return reply_n(encode_pd_trigger(buf, backend.pd_trigger()));
    case Cmd::UsbLink:
        if (!p.empty()) return reply(Status::BadLength);
        return reply_n(encode_usb_link(buf, backend.usb_link()));
    case Cmd::PdMatch:
        if (!p.empty()) return reply(Status::BadLength);
        return reply_n(encode_match(buf, backend.match_info()));
    case Cmd::Fc: {
        // The confirmation rules are enforced here, before the backend: the scan and any request
        // above 5 V are refused without the confirm byte.
        if (p.empty()) return reply(Status::BadLength);
        // --- vooc-monitor (k2/vooc-detect): passive readout, no confirm, no ports check ---
        if (p[0] == kFcVooc) {
            if (p.size() != 2 && p.size() != 3) return reply(Status::BadLength);
            if (p[1] >= kVoocPages || (p.size() == 3 && p[2] > 1)) return reply(Status::BadArg);
            return reply_n(encode_vooc(buf, backend.vooc_monitor(p.size() == 3 ? p[2] : 2), p[1]));
        }
        // --- end vooc-monitor ---
        switch (p[0]) {
        case kFcStatus:
        case kFcReset:
            if (p.size() != 1) return reply(Status::BadLength);
            break;
        case kFcScan:
            if (p.size() > 2) return reply(Status::BadLength);
            if (p.size() < 2 || !(p[1] & kFcConfirm) || (p[1] & ~kFcFlagMask)) return reply(Status::BadArg);
            break;
        case kFcTrigger: {
            if (p.size() != 5) return reply(Status::BadLength);
            const uint16_t mv = detail::rd16(&p[2]);
            // Above 5 V needs the confirm bit; so does --allow-inline at any voltage (it drives D+/D- with a
            // sink possibly attached: both flags, like `pd reset --inline --confirm`).
            if (p[1] >= kFcProtoCount || (p[4] & ~kFcFlagMask) ||
                ((mv > kFcSafeMv || (p[4] & kFcAllowInline)) && !(p[4] & kFcConfirm)))
                return reply(Status::BadArg);
            break;
        }
        default:
            return reply(Status::BadArg);
        }
        if (p[0] != kFcStatus) {
            const bool trig = p[0] == kFcTrigger;
            const Status st = backend.fc_cmd(p[0], trig ? p[1] : 0, trig ? detail::rd16(&p[2]) : 0,
                                             p[0] == kFcScan ? p[1] : trig ? p[4] : 0);
            if (st != Status::Ok) return reply(st);
            // a scan, or a trigger above 5 V, leaves the output under host control
            if (p[0] == kFcScan || (trig && detail::rd16(&p[2]) > kFcSafeMv)) backend.lease_arm();
        }
        return reply_n(encode_fc(buf, backend.fc_info()));
    }
    case Cmd::Store: {
        if (p.empty()) return reply(Status::BadLength);
        switch (p[0]) {
        case kStoreList:
        case kStoreSaveEmarker:
        case kStoreSaveLog:
            if (p.size() != 1) return reply(Status::BadLength);
            break;
        default:
            return reply(Status::BadArg);
        }
        // The saves share the flash with a charger test's own card save: Busy while a run or an FC release
        // owns the link, before the backend sees the call (list stays open).
        if (p[0] != kStoreList && backend.ports_busy()) return reply(Status::Busy);
        return reply_n(encode_store(buf, backend.store_cmd(p[0], p.size() > 1 ? p[1] : 0)));
    }
    case Cmd::CableR:
        if (p.size() > 1) return reply(Status::BadLength);
        if (p.size() == 1) {
            if (p[0] > kCableKelvin) return reply(Status::BadArg);
            if (p[0] == kCableReset) backend.cable_r_reset();
            if (p[0] >= kCablePassive) backend.cable_r_mode(p[0] == kCableKelvin);
        }
        return reply_n(encode_cable_r(buf, backend.cable_r_info()));
    case Cmd::Lab: {
        if (p.empty()) return reply(Status::BadLength);
        if (p[0] == kLabSniff) {
            if (p.size() != 2 && p.size() != 3) return reply(Status::BadLength);
            if (p[1] > 3) return reply(Status::BadArg);
            if (p[1] == 3 && (p.size() != 3 || p[2] != 1)) return reply(Status::NeedConfirm);
            const SniffInfo r = backend.lab_sniff(p[1]);
            if (r.status != static_cast<uint8_t>(Status::Ok)) return reply(static_cast<Status>(r.status));
            return reply_n(encode_sniff(buf, r));
        }
        if (p[0] != kLabLoad) return reply(Status::BadArg);
        if (p.size() != 5) return reply(Status::BadLength);
        if (p[4] != 1) return reply(Status::NeedConfirm);
        const LabResult r = backend.lab_load(p[1], static_cast<uint16_t>(p[2] | p[3] << 8));
        if (r.status != static_cast<uint8_t>(Status::Ok)) return reply(static_cast<Status>(r.status));
        return reply_n(encode_lab(buf, r));
    }
    case Cmd::Ufcs: {
        if (p.empty()) return reply(Status::BadLength);
        switch (p[0]) {
        case kUfcsStatus: case kUfcsReset: case kUfcsProbe: case kUfcsStop:
            if (p.size() != 1) return reply(Status::BadLength);
            break;
        case kUfcsGet:
            if (p.size() != 2) return reply(Status::BadLength);
            if (p[1] >= ufcs::kFieldCount) return reply(Status::BadArg);
            return reply_n(encode_ufcs_field(buf, p[1], backend.ufcs_get(p[1])));
        case kUfcsSet: {
            if (p.size() != 6) return reply(Status::BadLength);
            if (p[1] >= ufcs::kFieldCount) return reply(Status::BadArg);
            const uint32_t v = detail::rd32(&p[2]);
            const ufcs::FieldDef &d = ufcs::kFields[p[1]];
            if (v < d.lo || v > d.hi) return reply(Status::BadArg);
            const Status st = backend.ufcs_set(p[1], v);
            if (st != Status::Ok) return reply(st);
            return reply_n(encode_ufcs_field(buf, p[1], backend.ufcs_get(p[1])));
        }
        case kUfcsRead: {
            if (p.size() != 3) return reply(Status::BadLength);
            const uint16_t off = detail::rd16(&p[1]);
            const uint16_t total = backend.ufcs_info().blob_len;
            if (off > total) return reply(Status::BadArg);
            const size_t want = std::min<size_t>(kUfcsChunk, total - off);
            const size_t n = std::min(want, backend.ufcs_read(off, std::span<uint8_t>(buf.data() + 5, want)));
            buf[0] = uint8_t(off); buf[1] = uint8_t(off >> 8);
            buf[2] = uint8_t(total); buf[3] = uint8_t(total >> 8); buf[4] = uint8_t(n);
            return reply_n(5 + n);
        }
        default:
            return reply(Status::BadArg);
        }
        if (p[0] != kUfcsStatus) {
            const Status st = backend.ufcs_op(p[0]);
            if (st != Status::Ok) return reply(st);
        }
        return reply_n(encode_ufcs(buf, backend.ufcs_info()));
    }
    case Cmd::QRTest: {
        if (p.empty()) return reply(Status::BadLength);
        switch (p[0]) {
        case kQrTestList: {
            if (p.size() != 1 && p.size() != 3) return reply(Status::BadLength);
            const uint16_t off = p.size() == 3 ? detail::rd16(&p[1]) : 0;
            if (off > qrtest::list_total()) return reply(Status::BadArg);
            return reply_n(qrtest::list_chunk(off, buf));
        }
        case kQrTestShow: {
            if (p.size() != 4) return reply(Status::BadLength);
            // The argument validation is pure (k2/ui/ui_qrtest.hpp) and runs here, before the backend: a bad
            // range or a box that does not fit the panel answers BadArg with the reason as the payload text.
            char reason[qrtest::kReasonMax];
            const int rn = qrtest::check_show(p[1], p[2], p[3], reason);
            if (rn != 0)
                return reply(Status::BadArg,
                             std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(reason), static_cast<size_t>(rn)));
            const Status st = backend.qrtest_show(p[1], p[2], p[3]);
            if (st != Status::Ok) return reply(st);
            buf[0] = p[1]; buf[1] = p[2]; buf[2] = p[3];
            return reply_n(3);
        }
        case kQrTestStop:
            if (p.size() != 1) return reply(Status::BadLength);
            buf[0] = backend.qrtest_stop() ? 1 : 0;
            return reply_n(1);
        default:
            return reply(Status::BadArg);
        }
    }
    case Cmd::EmLib: {
        if (p.empty()) return reply(Status::BadLength);
        switch (p[0]) {
        case kEmList:
            if (p.size() > 2) return reply(Status::BadLength);
            break;
        case kEmSave:
            if (p.size() != 1) return reply(Status::BadLength);
            break;
        case kEmGet:
            if (p.size() != 3) return reply(Status::BadLength);
            break;
        case kEmDelete:
            if (p.size() != 4) return reply(Status::BadLength);
            if (p[3] != 1) return reply(Status::BadArg);
            break;
        case kEmRename:
            if (p.size() < 4 || p.size() > 3 + kEmNameMax) return reply(Status::BadLength);
            break;
        case kEmSpoof:
            if (p.size() < 2) return reply(Status::BadLength);
            if (p[1] > kEmSpoofBuiltin) return reply(Status::BadArg);
            if (p[1] == kEmSpoofBuiltin && p.size() == 3 && p[2] >= emrec::kBuiltinCount) return reply(Status::BadArg);
            if (p.size() != (p[1] == kEmSpoofArm ? 4u : p[1] == kEmSpoofBuiltin ? 3u : 2u)) return reply(Status::BadLength);
            break;
        case kEmLearn: {
            if (p.size() < 2) return reply(Status::BadLength);
            if (p[1] > kEmLearnSave) return reply(Status::BadArg);
            if (p[1] == kEmLearnRun) {
                emk_learn::Options o;
                bool confirm = false;
                if (!emk_learn::parse_options(p.subspan(2), o, confirm)) return reply(Status::BadArg);
            } else if (p.size() != (p[1] == kEmLearnEntry ? 3u : 2u)) {
                return reply(Status::BadLength);
            }
            break;
        }
        case kEmUnmatched:
            if (p.size() != 2) return reply(Status::BadLength);
            if (p[1] != emk_quirk::kSeenTotals && p[1] >= emk_quirk::SeenLog::kDepth) return reply(Status::BadArg);
            break;
        default:
            return reply(Status::BadArg);
        }
        // Library writes share the flash with the charger test's own card save: refused while a run or an FC
        // release owns the link, before the backend sees them (reads, spoof and learn status stay open).
        if ((p[0] == kEmSave || p[0] == kEmDelete || p[0] == kEmRename || (p[0] == kEmLearn && p[1] == kEmLearnSave)) &&
            backend.ports_busy()) {
            buf[0] = kEmErrBusy;
            return reply(Status::Busy, std::span<const uint8_t>(buf.data(), 1));
        }
        return reply_n(backend.emlib(p, buf));
    }
    case Cmd::Cards: {
        if (p.empty()) return reply(Status::BadLength);
        switch (p[0]) {
        case kCardsList:
            if (p.size() != 1 && p.size() != 2) return reply(Status::BadLength);   // [0] or [0, offset]
            break;
        case kCardsGet:
            if (p.size() != 4) return reply(Status::BadLength);
            if (p[3] >= kCardsChunks) return reply(Status::BadArg);
            break;
        case kCardsDelete:
            if (p.size() != 4) return reply(Status::BadLength);
            if (p[3] != 1) return reply(Status::BadArg);
            break;
        case kCardsRename:
            if (p.size() < 4 || p.size() > 3 + kCardsNameMax) return reply(Status::BadLength);
            break;
        default:
            return reply(Status::BadArg);
        }
        // Rename and delete write the card flash a running charger test saves into at its end: refused while a
        // run or an FC release owns the link, in the cards' own Ok + [kCardsErrBusy] shape.
        if ((p[0] == kCardsRename || p[0] == kCardsDelete) && backend.ports_busy()) {
            buf[0] = kCardsErrBusy;
            return reply(Status::Ok, std::span<const uint8_t>(buf.data(), 1));
        }
        return reply_n(backend.cards(p, buf));
    }
    case Cmd::BlackBox: {
        if (p.empty()) return reply(Status::BadLength);
        uint32_t arg = 0;
        switch (p[0]) {
        case kBbRead:
            if (p.size() != 1 && p.size() != 5) return reply(Status::BadLength);
            if (p.size() == 5) arg = detail::rd32(&p[1]);
            break;
        case kBbFreeze:
            if (p.size() != 2) return reply(Status::BadLength);
            arg = p[1];
            break;
        case kBbCapRead:
            if (p.size() != 4) return reply(Status::BadLength);
            arg = static_cast<uint32_t>(p[1]) << 16 | detail::rd16(&p[2]);
            break;
        case kBbClear:
        case kBbStatus:
        case kBbCapList:
        case kBbCapClear:
            if (p.size() != 1) return reply(Status::BadLength);
            break;
        default:
            return reply(Status::BadArg);
        }
        return reply_n(backend.blackbox(p[0], arg, buf));
    }
    case Cmd::TransCap: {
        if (p.empty()) return reply(Status::BadLength);
        switch (p[0]) {
        case kTcStatus: case kTcArm: case kTcTrigger: case kTcDisarm: case kTcSave: case kTcList: case kTcClear:
            if (p.size() != 1) return reply(Status::BadLength);
            break;
        case kTcLive:
            if (p.size() != 5) return reply(Status::BadLength);
            break;
        case kTcStored:
            if (p.size() != 9) return reply(Status::BadLength);
            break;
        default:
            return reply(Status::BadArg);
        }
        return reply_n(backend.transcap(p, buf));
    }
    case Cmd::Log:
        if (p.size() > 1) return reply(Status::BadLength);
        if (p.size() == 1) backend.set_log(p[0] != 0);
        buf[0] = backend.log_enabled() ? 1 : 0;
        return reply_n(1);
    }
    return reply(Status::UnknownCmd);
}

}  // namespace k2::remote
