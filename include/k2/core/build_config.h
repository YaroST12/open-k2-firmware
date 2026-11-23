// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
/* Build-time feature switches, set on the compiler command line by the Makefile (C and C++).
 *
 * K2_NET  1 = the USB network function is built in: TinyUSB's CDC-NCM class, lwIP, the DHCP server, the HTTP
 *             server and the web dashboard (src/net/net.cpp, src/net/http_server.cpp, tools/web/k2.html). Its RAM is the
 *             link.ld .mode_net overlay, the largest member of the USB-mode overlay.
 *         0 = none of that exists (the default: `make`, `make gate`). The stored USB link setting "HID+Net" then
 *             runs as HID. Build with `make K2_NET=1` (or `make docker-build K2_NET=1`) to get it back.
 * The default here is off, like the Makefile's; the host tests pass -DK2_NET=1 explicitly.
 *
 * K2_WEBUSB  1 = the WebUSB transport is built in: a vendor-class interface (bulk IN + OUT, TinyUSB's vendor class) that carries
 *             the same 64-byte remote frames as the HID channel, the BOS descriptor with the WebUSB platform capability and
 *             the Microsoft OS 2.0 set (so a browser and Windows bind it without a driver). It is meant to replace the HID
 *             remote channel and the K2_NET web path; the stored USB link setting picks it (Settings > USB Link, WebUSB).
 *         0 = none of that exists (the default): the setting's WebUSB request is dropped and the row is not shown.
 *             Build with `make K2_WEBUSB=1`; the host tests pass -DK2_WEBUSB=1 explicitly.
 *
 * K2_PDSTRESS  1 = Tools > PD Stress, its remote command (0x55) and the `pdstress` shell commands are built in.
 *              0 = none of it (the default since 2026-11-12: the tool found no charger fault, only its own Soft_Reset bug, see
 *                  ROADMAP). The Tools list ends at Loop probe, the remote and the shell answer "unsupported". The host tests
 *                  pass -DK2_PDSTRESS=1 so the logic keeps compiling; `make K2_PDSTRESS=1` builds the tool back.
 *
 * K2_PDSPEC  1 = the no-load PD conformance check (docs/PD_SPEC_CHECK.md) is built in: the charger test's Deep level runs it as
 *               one more section (the results grid, the journal and the web grid carry it), and the remote command (0x5A)
 *               starts it alone. There is no Tools page.
 *            0 = none of it (the default while flash is short): the remote answers UnknownCmd, a Deep run has no spec section.
 *                The host tests pass -DK2_PDSPEC=1; `make K2_PDSPEC=1` builds it in.
 *
 * K2_LOOPPROBE  1 = Tools > Loop probe (the HID-cable loop probe, docs/LOOP_PROBE.md): its page, session, stored result and
 *                   the remote command 0x57 (LOOP_PROBE) are built in.
 *               0 = none of it (the default while flash is short): the remote answers UnknownCmd, the Bench has no Loop probe row.
 *                   The host tests pass -DK2_LOOPPROBE=1; `make K2_LOOPPROBE=1` builds the tool in.
 *
 * K2_TWIN  1 = Device twins (TwinPage, twin_hw, /twin flash library, remote Cmd::Twin, shell `twin` commands) are built in.
 *          0 = none of it (the default): remote answers UnknownCmd, shell answers "unsupported", Device place has no Twins row.
 *              Inline interrogation (pd_interrogate, fusb302::interrogate_*) stays built in for the Device place.
 *              The host tests pass -DK2_TWIN=1; `make K2_TWIN=1` builds Device twins back in.
 *
 * K2_OPPO_PROBE 1 = the OPPO charger-protocol lab probe (docs/ROADMAP.md, lab docs): the probe sequencer (pd/oppo_probe), the
 *                   shell commands `pd oppo-probe` / `pd oppo-power`, the remote command 0x4D (OPPO_PROBE), the [OPPO] log lines,
 *                   the sink-initiated DR_Swap and the data-role plumbing it needs (the FUSB302 data-role bit in the transmit
 *                   header and the SWITCHES1 sync) are built in.
 *               0 = none of it (the default: a lab tool, it costs flash): the remote answers UnknownCmd, the PD sink never
 *                   sends a DR_Swap and the image is the one without the branch. The host tests of it run only with
 *                   `make test K2_OPPO_PROBE=1` (the flag is a whole-tree choice: its own host object directory).
 *
 * K2_EEPROM_READ 1 = the read-only 24Cxx EEPROM lab dump: src/drivers/eeprom.cpp and the remote command 0x2B (DEBUG_EEPROM; `k2_remote
 *                   debug eeprom probe|dump`). Read only. 0 = none of it: the remote answers UnknownCmd.
 *                   The Makefile default is 1 (about 620 B); the host tests pass -DK2_EEPROM_READ=1.
 *
 * K2_EEPROM_CAL_CLEAR 1 = the one EEPROM write: DEBUG_EEPROM op 2 (`k2_remote debug eeprom cal-clear --expect HEX --yes`) clears the
 *                   12 calibration trim bytes 0x04..0x0F of the 24C16 on the INA bus to zero, only when they read back equal to the
 *                   bytes the host expects (compare-and-swap). Address, length and value are constants (eeprom_calc.hpp); nothing
 *                   else can be written. The Makefile default is 0 (a one-unit repair, not for the shipping image); the host
 *                   tests pass -DK2_EEPROM_CAL_CLEAR=1. 0 = op 2 answers BadArg and the write code is not linked.
 *
 * K2_EEPROM_WRITE 1 = the backup restore's page write: DEBUG_EEPROM op 3 writes one aligned 16-byte page of the 24C16 on the INA bus,
 *                   only when the page reads back equal to the old bytes the host sends (compare-and-swap), then reads it back.
 *                   The Makefile default is 1; the host tests pass -DK2_EEPROM_WRITE=1. 0 = op 3 answers BadArg.
 *                   The settings store (Settings USB link, the armed spoof; k2/core/eekeep.hpp) is always built and writes only
 *                   its own pages 0x500..0x7DF.
 *
 * K2_SPECROWS  1 = the PD spec section keeps and shows its per-transition list (needs K2_PDSPEC=1): the engine marks each fixed
 *                  transition's measurements (TransRec::seen / fails, 18 -> 20 B each), `specsec` projects them into 10 byte
 *                  rows (spec_rows.hpp), a check's detail page lists them (a verdict band, one row per transition, PLUS / MINUS
 *                  scroll), the journal stores them in /jr/spt.rec and the remote reads them with 0x0B [8, off16].
 *              0 = none of it (the default: the shipping K2_WEBUSB=1 K2_PDSPEC=1 K2_PDSTRESS=1 image has no flash for it): a check's
 *                  detail is the summary table (Verdict, Worst, Limit, Count, Fails, At, Penalty), no spt.rec is written, the
 *                  remote [8] answers BadLength (what an older firmware answers; tools/k2_remote.py treats it as "no rows").
 *                  The journal format byte stays 5 either way (the file is additive). The host tests build it off by default and
 *                  on in a second pass (`make test-specrows`, host-build-rows/): it changes struct layouts, so never per object. */
/* K2_SCREENSHOT  1 = screenshot capture (src/ui/capture.cpp, src/ui/rle565.cpp), remote Screenshot (0x0A), HTTP
 *                    /api/screenshot.bmp and Settings > Screenshot are built in.
 *                0 = none of it (the default: saves the 1920 B strip buffer and ~3 KB Flash). Remote answers UnknownCmd
 *                    and screenshot_enabled() returns false. The host tests pass -DK2_SCREENSHOT=1 explicitly;
 *                    `make K2_SCREENSHOT=1` builds screenshot capture back in. */
/* K2_SELFTEST  1 = the on-device math selftest (src/core/math_selftest.cpp, docs/MATH_SELFTEST.md) is linked: the shell `selftest`
 *                  command, the cycle benchmark behind `make perf-check`, the link.ld placement canaries.
 *              0 = none of it (the default: 12 KB flash, pre-release builds only): the shell answers "selftest not available" and the
 *                  perf gate reports SKIPPED. The host tests pass -DK2_SELFTEST=1; `make K2_SELFTEST=1` builds it in. */
#ifndef K2_BUILD_CONFIG_H_
#define K2_BUILD_CONFIG_H_

#ifndef K2_NET
#define K2_NET 0
#endif

#ifndef K2_WEBUSB
#define K2_WEBUSB 0
#endif

#ifndef K2_PDSTRESS
#define K2_PDSTRESS 0
#endif

#ifndef K2_PDSPEC
#define K2_PDSPEC 0
#endif

#ifndef K2_LOOPPROBE
#define K2_LOOPPROBE 0
#endif

#ifndef K2_TWIN
#define K2_TWIN 0
#endif

#ifndef K2_EEPROM_READ
#define K2_EEPROM_READ 0
#endif

#ifndef K2_EEPROM_CAL_CLEAR
#define K2_EEPROM_CAL_CLEAR 0
#endif

#ifndef K2_EEPROM_WRITE
#define K2_EEPROM_WRITE 0
#endif

#ifndef K2_OPPO_PROBE
#define K2_OPPO_PROBE 0
#endif

#ifndef K2_SPECROWS
#define K2_SPECROWS 0
#endif
#if K2_SPECROWS && !K2_PDSPEC
#error "K2_SPECROWS=1 needs K2_PDSPEC=1 (the per-transition list belongs to the PD spec section)"
#endif

#ifndef K2_SCREENSHOT
#define K2_SCREENSHOT 0
#endif

#ifndef K2_SELFTEST
#define K2_SELFTEST 0
#endif

/* K2_HOST_REPORT  1 = the loop probe's Host report prototype (docs/LOOP_PROBE.md): src/loopprobe/host_report_hw.cpp, the
 *                     FUSB302 loop-host line, the USB enumeration tap and the Loop probe page's Host view.
 *                 0 = none of it (the default). The pure half (host_report.cpp, usb_fp.cpp) is host-tested either way. */
#ifndef K2_HOST_REPORT
#define K2_HOST_REPORT 0
#endif
#if K2_HOST_REPORT && !K2_LOOPPROBE
#error "K2_HOST_REPORT=1 needs K2_LOOPPROBE=1: the Host report is a view of the Loop probe"
#endif

#endif /* K2_BUILD_CONFIG_H_ */
