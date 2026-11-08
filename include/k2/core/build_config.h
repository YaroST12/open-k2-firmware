// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
/* Build-time feature switches, set on the compiler command line by the Makefile (C and C++).
 *
 * K2_NET  1 = the USB network function is built in: TinyUSB's CDC-NCM class, lwIP, the DHCP server, the HTTP
 *             server and the web dashboard (src/net/net.cpp, src/net/http_server.cpp, tools/web/k2.html). Its RAM is the
 *             link.ld .mode_net overlay, the largest member of the USB-mode overlay.
 *         0 = none of that exists (the default: `make`, `make gate`). The stored USB link setting "HID+Net" then
 *             runs as HID. Build with `make K2_NET=1` (or `make docker-build K2_NET=1`) to get it back.
 * The default here is off, like the Makefile's; the host tests pass -DK2_NET=1 explicitly. */
#ifndef K2_BUILD_CONFIG_H_
#define K2_BUILD_CONFIG_H_

#ifndef K2_NET
#define K2_NET 0
#endif

#endif /* K2_BUILD_CONFIG_H_ */
