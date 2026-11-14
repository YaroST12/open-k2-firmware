// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#ifndef TUSB_CONFIG_H_
#define TUSB_CONFIG_H_

#include "k2/core/build_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* CFG_TUSB_MCU is passed on the compiler command line (Makefile). */
#ifndef CFG_TUSB_MCU
#error CFG_TUSB_MCU must be defined
#endif

#define CFG_TUSB_OS               OPT_OS_NONE
#define CFG_TUSB_DEBUG            0
#define CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_ALIGN        __attribute__((aligned(4)))

/* Device stack, full speed only (AT32F403A has an FS device controller) */
#define CFG_TUD_ENABLED           1
#define CFG_TUD_MAX_SPEED         OPT_MODE_FULL_SPEED
#define CFG_TUD_ENDPOINT0_SIZE    64

/* Every class the link modes use is compiled in; the stored USB link setting (k2/drivers/usbcfg.hpp) picks
 * the descriptors at boot (src/drivers/usb_descriptors.cpp), and TinyUSB opens only the classes that the
 * chosen configuration contains. HID: logger + remote; MSC: the FAT16 drive; NCM: network link, only in a
 * K2_NET=1 build (k2/core/build_config.h; the default build leaves the class, its buffers and lwIP out). */
#define CFG_TUD_CDC               1   /* the debug shell (k2/core/shell.hpp), one port */
#define CFG_TUD_MSC               1
#define CFG_TUD_HID               1
#define CFG_TUD_MIDI              0
#define CFG_TUD_VENDOR            K2_WEBUSB   /* the WebUSB remote channel (k2/core/build_config.h) */
#define CFG_TUD_ECM_RNDIS         0 /* NCM only: TinyUSB forbids enabling both network classes */
#define CFG_TUD_NCM               K2_NET

/* Debug shell: the endpoint transfer buffer, the receive FIFO (typed lines) and the transmit FIFO (a
 * few queued log lines). The bulk endpoints are 16 bytes (k2/drivers/usb_pma.hpp). ~450 bytes of RAM. */
#define CFG_TUD_CDC_EP_BUFSIZE    64
#define CFG_TUD_CDC_RX_BUFSIZE    64
#define CFG_TUD_CDC_TX_BUFSIZE    256

/* WebUSB vendor interface (K2_WEBUSB=1): 64-byte bulk endpoints (k2/drivers/usb_pma.hpp), FIFO mode. The receive FIFO holds
 * two remote frames, the transmit FIFO eight (the streamers pace themselves on usb::tx_free()). ~0.8 KB of RAM. */
#define CFG_TUD_VENDOR_RX_EPSIZE  64
#define CFG_TUD_VENDOR_TX_EPSIZE  64
#define CFG_TUD_VENDOR_RX_BUFSIZE 128
#define CFG_TUD_VENDOR_TX_BUFSIZE 512

/* One 64-byte report in each direction, as the original logger did. */
#define CFG_TUD_HID_EP_BUFSIZE    64

/* MSC transfer buffer: one 512-byte sector per SCSI read/write step (must be >= the 64-byte
 * bulk packet). The bulk endpoints themselves are 64 bytes (descriptor in usb_descriptors.cpp). */
#define CFG_TUD_MSC_EP_BUFSIZE    512

/* Network: one Ethernet frame is at most 1514 bytes; 2048 is the NTB size CDC-NCM 1.0 requires as
 * a minimum. One NTB each way keeps the static cost at 2 x 2048 (+ headers) bytes. */
#define CFG_TUD_NET_MTU                 1514
#define CFG_TUD_NCM_IN_NTB_MAX_SIZE     2048
#define CFG_TUD_NCM_OUT_NTB_MAX_SIZE    2048
#define CFG_TUD_NCM_IN_NTB_N            1
#define CFG_TUD_NCM_OUT_NTB_N           1

#ifdef __cplusplus
}
#endif

#endif /* TUSB_CONFIG_H_ */
