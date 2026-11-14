// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The vendor control requests of the WebUSB modes (K2_WEBUSB=1 builds; the file is empty otherwise): the landing-page URL
// and the Microsoft OS 2.0 descriptor set. The bytes and the request matching are pure code in k2/drivers/usb_webusb.hpp
// (host-tested); this is the thin TinyUSB glue. TinyUSB routes every vendor-type control request here; only the SETUP
// stage answers, the data stage needs nothing more.
#include "k2/core/build_config.h"

#if K2_WEBUSB
#include "tusb.h"
#include "k2/drivers/usb_webusb.hpp"
#include "k2/drivers/usbcfg.hpp"

extern "C" bool tud_vendor_control_xfer_cb(std::uint8_t rhport, std::uint8_t stage, const tusb_control_request_t* request) {
    if (stage != CONTROL_STAGE_SETUP) return true;
    if (request->bmRequestType_bit.type != TUSB_REQ_TYPE_VENDOR) return false;
    if (request->bmRequestType_bit.direction != TUSB_DIR_IN) return false;
    const k2::usbcfg::Setting c = k2::usbcfg::current;
    const std::uint8_t itf = k2::webusb::vendor_interface(c.hid, c.drive);
    const k2::webusb::Reply r = k2::webusb::control_reply(request->bRequest, request->wValue, request->wIndex, itf);
    if (!r.data) return false;
    const std::uint16_t n = r.len < request->wLength ? r.len : request->wLength;
    return tud_control_xfer(rhport, request, const_cast<std::uint8_t*>(r.data), n);
}
#endif
