// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// USB descriptors of the Open-K2 device. One image serves every link mode; the mode is the
// stored USB link setting (k2/drivers/usbcfg.hpp, `usbcfg::current`, loaded before usb::init()) and picks
// the device descriptor, the configuration descriptor and the strings at enumeration time.
#include <array>
#include <cstdint>

#include "tusb.h"
#include "k2/drivers/usb_pma.hpp"
#include "k2/drivers/usbcfg.hpp"
#include "k2/drivers/usb_webusb.hpp"

/*
 * Modes (every configuration is a single configuration, value 1):
 *   HID         interface 0: HID logger/remote, 64-byte interrupt IN+OUT on EP1, 1 ms
 *               (byte-compatible with the M1 logger, VID/PID 0716:5060)
 *   Drive       interface 0: Bulk-Only mass storage, EP2 OUT 0x02 / IN 0x82, 64 B bulk
 *   HID+Drive   interface 0 HID (as above), interface 1 MSC on the same EP2 pair
 *   HID+Net     (K2_NET=1 builds only; the default build has no network function and none of its descriptors)
 *               interface 0 HID (as above), interfaces 1/2 CDC-NCM (control + data)
 *               notification EP2 IN 0x82 (16 B interrupt, 50 ms), data EP3 OUT 0x03 / IN 0x83 (64 B bulk)
 *   +Shell      HID and HID+Drive get a CDC-ACM function (the debug shell, k2/core/shell.hpp) after their
 *               last interface: control 1 (or 2 with the drive), data next to it; notification EP4 IN
 *               0x84 (8 B), data EP5 OUT 0x05 / IN 0x85 (16 B bulk). The device descriptor then
 *               carries the IAD class triple too. Not in Drive-only and not with Net.
 *
 *   WebUSB      (K2_WEBUSB=1 builds only) a vendor-class interface (class FF, bulk OUT 0x03 / IN 0x83, 64 B) that carries
 *               the remote protocol instead of the HID interface; bcdUSB is 2.10 and the BOS descriptor announces the
 *               WebUSB landing page and the Microsoft OS 2.0 set (k2/drivers/usb_webusb.hpp). Interface 0, or 1 when
 *               the HID interface comes first (HID+WebUSB, the fallback kept for comparison). WebUSB+Drive+Shell is the
 *               HID-free end state; HID+WebUSB+Drive does not fit the packet memory. +Shell works as with HID.
 *
 * Why CDC-NCM and not RNDIS+ECM: the inbox drivers of Linux (cdc_ncm), macOS (NCM) and Windows 11
 * (usbncm.sys) all bind to it with one configuration, no MS OS 2.0 / BOS descriptors, no vendor
 * control requests and no INF. RNDIS+ECM needs two configurations and a BOS descriptor to cover
 * Windows 10 as well; that OS (end of life) is the price. The device descriptor of the net mode
 * carries the IAD class triple (EF/02/01) because the NCM function is an interface association;
 * the other modes keep class 0 exactly as before.
 *
 * PMA budget (512 B) per mode is in k2/drivers/usb_pma.hpp and checked by the host test
 * tests/test_usb_pma.cpp against the descriptor bytes built here.
 */

namespace {

namespace pma = k2::usbpma;

enum : std::uint8_t { ITF_NUM_HID = 0, ITF_NUM_MSC, ITF_NUM_TOTAL_DRIVE };
#if K2_NET
// HID+Net: HID 0, NCM control 1, NCM data 2 (the data interface follows the control one).
enum : std::uint8_t { ITF_NUM_NCM = 1, ITF_NUM_TOTAL_NET = 3 };
#endif
// +Shell: CDC control interface after HID (1) or after HID and MSC (2); two interfaces each.
enum : std::uint8_t { ITF_NUM_SHELL_HID = 1, ITF_NUM_SHELL_DRIVE = 2 };

constexpr std::uint8_t kEpHidOut = 0x01, kEpHidIn = 0x81;
constexpr std::uint8_t kEpMscOut = 0x02, kEpMscIn = 0x82;
constexpr std::uint8_t kEpShellNotif = 0x84, kEpShellOut = 0x05, kEpShellIn = 0x85;
#if K2_WEBUSB
constexpr std::uint8_t kEpVendorOut = 0x03, kEpVendorIn = 0x83;
#endif
#if K2_NET
constexpr std::uint8_t kEpNetNotif = 0x82, kEpNetOut = 0x03, kEpNetIn = 0x83;
constexpr std::uint8_t kNetNotifIntervalMs = 50;
constexpr std::uint8_t kNcmCaps = NCM_NETWORK_CAPS_ETH_FILTER | NCM_NETWORK_CAPS_NTB_INPUT_SIZE;
#endif

static_assert(pma::kHidEpSize == CFG_TUD_HID_EP_BUFSIZE);
#if K2_WEBUSB
static_assert(pma::kVendorEpSize == CFG_TUD_VENDOR_RX_EPSIZE && pma::kVendorEpSize == CFG_TUD_VENDOR_TX_EPSIZE);
#endif

// String descriptor indices (iManufacturer/iProduct/iSerialNumber in the device descriptor). STR_NET and STR_MAC
// belong to the network function and exist in a K2_NET=1 build only.
enum : std::uint8_t { STR_LANGID = 0, STR_MFG, STR_PROD, STR_SERIAL, STR_NET, STR_MAC };

// bcdUSB 2.00 (2.10 in the WebUSB modes: browsers ask for the BOS descriptor only from 2.10 on), class 0 for every mode
// but the ones with an interface association (the network or the shell function: IAD class triple).
constexpr tusb_desc_device_t make_device(bool iad, std::uint16_t bcd = 0x0200) {
    return {
        .bLength            = sizeof(tusb_desc_device_t),
        .bDescriptorType    = TUSB_DESC_DEVICE,
        .bcdUSB             = bcd,
        .bDeviceClass       = iad ? std::uint8_t(TUSB_CLASS_MISC) : std::uint8_t(0),
        .bDeviceSubClass    = iad ? std::uint8_t(MISC_SUBCLASS_COMMON) : std::uint8_t(0),
        .bDeviceProtocol    = iad ? std::uint8_t(MISC_PROTOCOL_IAD) : std::uint8_t(0),
        .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
        .idVendor           = 0x0716,  // WITRN
        .idProduct          = 0x5060,
        .bcdDevice          = 0x0200,
        .iManufacturer      = STR_MFG,
        .iProduct           = STR_PROD,
        .iSerialNumber      = STR_SERIAL,
        .bNumConfigurations = 1,
    };
}
const tusb_desc_device_t desc_device = make_device(false);
const tusb_desc_device_t desc_device_iad = make_device(true);   // the network or the shell function present
#if K2_WEBUSB
const tusb_desc_device_t desc_device_web = make_device(false, 0x0210);
const tusb_desc_device_t desc_device_web_iad = make_device(true, 0x0210);
#endif

// Unchanged from the original logger: 64-byte input and output reports.
const std::uint8_t desc_hid_report[] = {
    0x05, 0x01,  // Usage Page (Generic Desktop)
    0x09, 0x00,  // Usage (Undefined)
    0xA1, 0x01,  // Collection (Application)
    0x15, 0x00,  // Logical Minimum (0)
    0x25, 0xFF,  // Logical Maximum (255)
    0x75, 0x08,  // Report Size (8 bits)
    0x95, 0x40,  // Report Count (64 bytes)
    0x19, 0x01,  // Usage Minimum (1)
    0x29, 0x08,  // Usage Maximum (8)
    0x81, 0x06,  // Input (Data, Var, Rel) -> 64-byte IN from K2
    0x19, 0x01,  // Usage Minimum (1)
    0x29, 0x08,  // Usage Maximum (8)
    0x91, 0x02,  // Output (Data, Var, Abs) -> 64-byte OUT to K2
    0xC0         // End Collection
};

// HID interface shared by every configuration: 64-byte interrupt IN+OUT on EP1, 1 ms.
#define K2_HID_DESCRIPTOR                                                                   \
    TUD_HID_INOUT_DESCRIPTOR(ITF_NUM_HID, 0, HID_ITF_PROTOCOL_NONE, sizeof(desc_hid_report), \
                             kEpHidOut, kEpHidIn, CFG_TUD_HID_EP_BUFSIZE, 1)

// Configuration headers: bmAttributes 0x20 (as before) and 100 mA.

// HID + Drive: the MSC interface comes AFTER the HID one, so HID stays interface 0 with the same
// endpoints and report descriptor (tests/test_usb_desc_msc1.cpp compares the bytes).
constexpr std::uint16_t kConfigLen = TUD_CONFIG_DESC_LEN + TUD_HID_INOUT_DESC_LEN + TUD_MSC_DESC_LEN;
const std::uint8_t desc_configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL_DRIVE, 0, kConfigLen, 0x20, 100),
    K2_HID_DESCRIPTOR,
    TUD_MSC_DESCRIPTOR(ITF_NUM_MSC, 0, kEpMscOut, kEpMscIn, pma::kMscEpSize),
};

// HID only / Drive only: a single function (the drive alone is interface 0 on the same EP2 pair).
constexpr std::uint16_t kHidOnlyLen = TUD_CONFIG_DESC_LEN + TUD_HID_INOUT_DESC_LEN;
const std::uint8_t desc_config_hid_only[] = {
    TUD_CONFIG_DESCRIPTOR(1, 1, 0, kHidOnlyLen, 0x20, 100),
    K2_HID_DESCRIPTOR,
};
constexpr std::uint16_t kMscOnlyLen = TUD_CONFIG_DESC_LEN + TUD_MSC_DESC_LEN;
const std::uint8_t desc_config_msc_only[] = {
    TUD_CONFIG_DESCRIPTOR(1, 1, 0, kMscOnlyLen, 0x20, 100),
    TUD_MSC_DESCRIPTOR(0, 0, kEpMscOut, kEpMscIn, pma::kMscEpSize),
};

#if K2_NET
// HID + Net: HID interface 0, then the NCM function (its own IAD, interfaces 1 and 2).
constexpr std::uint16_t kNetLen = TUD_CONFIG_DESC_LEN + TUD_HID_INOUT_DESC_LEN + TUD_CDC_NCM_DESC_LEN;
const std::uint8_t desc_config_net[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL_NET, 0, kNetLen, 0x20, 100),
    K2_HID_DESCRIPTOR,
    TUD_CDC_NCM_DESCRIPTOR(ITF_NUM_NCM, STR_NET, STR_MAC, kEpNetNotif, pma::kNetNotifSize, kEpNetOut, kEpNetIn,
                           pma::kNetEpSize, CFG_TUD_NET_MTU, kNetNotifIntervalMs, kNcmCaps),
};
#endif

// HID + Shell, and HID + Drive + Shell (the shell is the last function).
constexpr std::uint16_t kHidShellLen = TUD_CONFIG_DESC_LEN + TUD_HID_INOUT_DESC_LEN + TUD_CDC_DESC_LEN;
const std::uint8_t desc_config_hid_shell[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_SHELL_HID + 2, 0, kHidShellLen, 0x20, 100),
    K2_HID_DESCRIPTOR,
    TUD_CDC_DESCRIPTOR(ITF_NUM_SHELL_HID, 0, kEpShellNotif, pma::kShellNotifSize, kEpShellOut, kEpShellIn,
                       pma::kShellEpSize),
};
constexpr std::uint16_t kHidDriveShellLen = kConfigLen + TUD_CDC_DESC_LEN;
const std::uint8_t desc_config_hid_drive_shell[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_SHELL_DRIVE + 2, 0, kHidDriveShellLen, 0x20, 100),
    K2_HID_DESCRIPTOR,
    TUD_MSC_DESCRIPTOR(ITF_NUM_MSC, 0, kEpMscOut, kEpMscIn, pma::kMscEpSize),
    TUD_CDC_DESCRIPTOR(ITF_NUM_SHELL_DRIVE, 0, kEpShellNotif, pma::kShellNotifSize, kEpShellOut, kEpShellIn,
                       pma::kShellEpSize),
};

#if K2_WEBUSB
// WebUSB modes. The vendor function is first (interface 0) except after the HID interface; the MSC and CDC interface
// numbers follow the functions before them. Bulk endpoints never repeat between functions (the PMA test relies on it).
constexpr std::uint16_t kVendorDescLen = TUD_VENDOR_DESC_LEN;
#define K2_VENDOR_DESCRIPTOR(itf) TUD_VENDOR_DESCRIPTOR(itf, 0, kEpVendorOut, kEpVendorIn, pma::kVendorEpSize)
#define K2_SHELL_DESCRIPTOR(itf)  TUD_CDC_DESCRIPTOR(itf, 0, kEpShellNotif, pma::kShellNotifSize, kEpShellOut, kEpShellIn, pma::kShellEpSize)

constexpr std::uint16_t kWebLen = TUD_CONFIG_DESC_LEN + kVendorDescLen;
const std::uint8_t desc_config_web[] = {
    TUD_CONFIG_DESCRIPTOR(1, 1, 0, kWebLen, 0x20, 100),
    K2_VENDOR_DESCRIPTOR(0),
};
constexpr std::uint16_t kWebShellLen = kWebLen + TUD_CDC_DESC_LEN;
const std::uint8_t desc_config_web_shell[] = {
    TUD_CONFIG_DESCRIPTOR(1, 3, 0, kWebShellLen, 0x20, 100),
    K2_VENDOR_DESCRIPTOR(0),
    K2_SHELL_DESCRIPTOR(1),
};
constexpr std::uint16_t kWebDriveLen = kWebLen + TUD_MSC_DESC_LEN;
const std::uint8_t desc_config_web_drive[] = {
    TUD_CONFIG_DESCRIPTOR(1, 2, 0, kWebDriveLen, 0x20, 100),
    K2_VENDOR_DESCRIPTOR(0),
    TUD_MSC_DESCRIPTOR(1, 0, kEpMscOut, kEpMscIn, pma::kMscEpSize),
};
constexpr std::uint16_t kWebDriveShellLen = kWebDriveLen + TUD_CDC_DESC_LEN;
const std::uint8_t desc_config_web_drive_shell[] = {
    TUD_CONFIG_DESCRIPTOR(1, 4, 0, kWebDriveShellLen, 0x20, 100),
    K2_VENDOR_DESCRIPTOR(0),
    TUD_MSC_DESCRIPTOR(1, 0, kEpMscOut, kEpMscIn, pma::kMscEpSize),
    K2_SHELL_DESCRIPTOR(2),
};
// HID + WebUSB (the comparison fallback): the HID interface keeps number 0 and its bytes.
constexpr std::uint16_t kHidWebLen = TUD_CONFIG_DESC_LEN + TUD_HID_INOUT_DESC_LEN + kVendorDescLen;
const std::uint8_t desc_config_hid_web[] = {
    TUD_CONFIG_DESCRIPTOR(1, 2, 0, kHidWebLen, 0x20, 100),
    K2_HID_DESCRIPTOR,
    K2_VENDOR_DESCRIPTOR(1),
};
constexpr std::uint16_t kHidWebShellLen = kHidWebLen + TUD_CDC_DESC_LEN;
const std::uint8_t desc_config_hid_web_shell[] = {
    TUD_CONFIG_DESCRIPTOR(1, 4, 0, kHidWebShellLen, 0x20, 100),
    K2_HID_DESCRIPTOR,
    K2_VENDOR_DESCRIPTOR(1),
    K2_SHELL_DESCRIPTOR(2),
};
#endif

// The WebUSB function is in this setting only when the build has it (K2_WEBUSB) and the setting asks for it; it
// replaces the network function (the packet memory has no room for both) and, with the Drive, the HID interface.
constexpr bool has_webusb(const k2::usbcfg::Setting& c) { return k2::usbcfg::kWebusbBuilt && c.webusb; }

// The network function is in this setting only when the build has it (K2_NET) and the setting asks for it. The
// descriptors never trust `net` alone: a K2_NET=0 build answers a stray net request as the HID configuration.
constexpr bool has_net(const k2::usbcfg::Setting& c) { return k2::usbcfg::kNetBuilt && c.net && !has_webusb(c); }

// The shell function is present in these modes only (HID or WebUSB on, Net off); `shell` is a derived flag.
constexpr bool has_shell(const k2::usbcfg::Setting& c) { return c.shell && (c.hid || has_webusb(c)) && !has_net(c); }

#if K2_NET
// Indexed by STR_*; null entries are answered with a stall (STR_LANGID, STR_MAC are special).
constexpr std::array<const char*, STR_NET + 1> kStrings{
    nullptr, "WITRN", "Open-K2 Logger", "0001", "Open-K2 Network",
};
#else
constexpr std::array<const char*, STR_SERIAL + 1> kStrings{
    nullptr, "WITRN", "Open-K2 Logger", "0001",
};
#endif

}  // namespace

#if K2_NET
// MAC the host sees on its side (locally administered, unicast), read by TinyUSB by name and
// announced in string STR_MAC. The device side of the link (src/net/net.cpp) uses the same address
// with the low bit flipped.
extern "C" std::uint8_t tud_network_mac_address[6];
std::uint8_t tud_network_mac_address[6] = {0x02, 0x4B, 0x32, 0x00, 0x00, 0x00};
#endif

// ---- TinyUSB descriptor callbacks (C linkage: looked up by name) ----
extern "C" {

const std::uint8_t* tud_descriptor_device_cb(void) {
    const k2::usbcfg::Setting c = k2::usbcfg::current;
#if K2_WEBUSB
    if (has_webusb(c)) return reinterpret_cast<const std::uint8_t*>(has_shell(c) ? &desc_device_web_iad : &desc_device_web);
#endif
    return reinterpret_cast<const std::uint8_t*>(has_net(c) || has_shell(c) ? &desc_device_iad : &desc_device);
}

#if K2_WEBUSB
// The BOS descriptor exists in the WebUSB modes only; every other mode stalls the request, as before.
const std::uint8_t* tud_descriptor_bos_cb(void) {
    return has_webusb(k2::usbcfg::current) ? k2::webusb::kBos.data() : nullptr;
}
#endif

const std::uint8_t* tud_descriptor_configuration_cb(std::uint8_t) {
    const k2::usbcfg::Setting c = k2::usbcfg::current;
#if K2_WEBUSB
    if (has_webusb(c)) {
        const bool sh = has_shell(c);
        if (c.drive) return sh ? desc_config_web_drive_shell : desc_config_web_drive;
        if (c.hid) return sh ? desc_config_hid_web_shell : desc_config_hid_web;
        return sh ? desc_config_web_shell : desc_config_web;
    }
#endif
#if K2_NET
    if (has_net(c)) return desc_config_net;
#endif
    if (has_shell(c)) return c.drive ? desc_config_hid_drive_shell : desc_config_hid_shell;
    if (c.hid && !c.drive) return desc_config_hid_only;
    if (c.drive && !c.hid) return desc_config_msc_only;
    return desc_configuration;
}

const std::uint8_t* tud_hid_descriptor_report_cb(std::uint8_t) { return desc_hid_report; }

const std::uint16_t* tud_descriptor_string_cb(std::uint8_t index, std::uint16_t) {
    static std::uint16_t desc[32];  // [0] = header, then UTF-16 code units (ASCII only here)
    std::uint8_t n = 0;
#if K2_NET
    const bool net = has_net(k2::usbcfg::current);
#endif

    if (index == STR_LANGID) {
        desc[1] = 0x0409;  // English (US)
        n = 1;
#if K2_NET
    } else if (index == STR_MAC && net) {
        // The NCM function wants the host-side MAC as 12 upper-case hex characters.
        constexpr char hex[] = "0123456789ABCDEF";
        for (; n < 12; n++) {
            const std::uint8_t b = tud_network_mac_address[n / 2];
            desc[1 + n] = hex[(n & 1) ? (b & 0x0F) : (b >> 4)];
        }
#endif
    } else {
        // The network name exists only in the net mode (and a K2_NET=1 build): the other modes answer it with a stall.
        if (index >= kStrings.size() || !kStrings[index]
#if K2_NET
            || (index == STR_NET && !net)
#endif
        ) return nullptr;
        for (const char* s = kStrings[index]; *s && n < 31; s++) desc[1 + n++] = *s;
    }

    desc[0] = (TUSB_DESC_STRING << 8) | (2 * n + 2);
    return desc;
}

}  // extern "C"
