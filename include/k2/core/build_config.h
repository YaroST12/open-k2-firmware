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
 * K2_PDSTRESS  1 = Tools > PD Stress, its remote command (0x55) and the `pdstress` shell commands are built in.
 *              0 = none of it (the default since 2026-11-12: the tool found no charger fault, only its own Soft_Reset bug, see
 *                  ROADMAP). The Tools list ends at Loop probe, the remote and the shell answer "unsupported". The host tests
 *                  pass -DK2_PDSTRESS=1 so the logic keeps compiling; `make K2_PDSTRESS=1` builds the tool back. */
/* K2_WHOS_ATTACHED  1 = Tools > Charger ID / Cable read / Device ID open the old tabbed "Who's attached" page on that tab
 *                     (tab strip, Tab+/Tab-, the action pop-up).
 *                   0 = each opens its own single tool: one tab, no strip, OK runs the tool's action, the other actions are
 *                       rows under the result (the default since 2026-11-12: the tabbed page did not work well).
 *
 * K2_SCREENSHOT  1 = screenshot capture (src/ui/capture.cpp, src/ui/rle565.cpp), remote Screenshot (0x0A), HTTP
 *                    /api/screenshot.bmp and Settings > Screenshot are built in.
 *                0 = none of it (the default: saves the 1920 B strip buffer and ~3 KB Flash). Remote answers UnknownCmd
 *                    and screenshot_enabled() returns false. The host tests pass -DK2_SCREENSHOT=1 explicitly;
 *                    `make K2_SCREENSHOT=1` builds screenshot capture back in. */
#ifndef K2_BUILD_CONFIG_H_
#define K2_BUILD_CONFIG_H_

#ifndef K2_NET
#define K2_NET 0
#endif

#ifndef K2_PDSTRESS
#define K2_PDSTRESS 0
#endif

#ifndef K2_WHOS_ATTACHED
#define K2_WHOS_ATTACHED 0
#endif

#ifndef K2_SCREENSHOT
#define K2_SCREENSHOT 0
#endif

/* K2_HOST_REPORT  1 = the loop probe's Host report prototype (docs/LOOP_PROBE.md): src/loopprobe/host_report_hw.cpp, the
 *                     FUSB302 loop-host line, the USB enumeration tap and the Loop probe page's Host view.
 *                 0 = none of it (the default). The pure half (host_report.cpp, usb_fp.cpp) is host-tested either way. */
#ifndef K2_HOST_REPORT
#define K2_HOST_REPORT 0
#endif

#endif /* K2_BUILD_CONFIG_H_ */
