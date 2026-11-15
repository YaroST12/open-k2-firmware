// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The sink's answer to a request it has no handler of its own for (docs/PD_SINK_REPLIES.md has the whole table, one row per
// message). Pure: the header (and the first data byte that matters) in, the answer out, so the table is host-tested message
// by message. pd::Sink calls these after its own handlers (Get_Sink_Cap, Get_Sink_Cap_Extended, the swaps, Soft_Reset,
// Get_Revision, EPR_Get_Sink_Cap, the VDMs it speaks, Source_Capabilities); everything else a source may send ends here.
#pragma once
#include <cstdint>

namespace k2::pd {

// The control message that refuses `header` (Not_Supported, Reject), or 0 = no answer. `rev` is the wire revision in use
// (1 = PD 2.0, 2 = PD 3.x); `ecdb` is the Extended_Control type (data byte 0, read only for that message).
//  - PD 3 (USB PD R3.2 6.3.x / 6.5.x): a request the K2 does not support gets Not_Supported, a reserved type too.
//  - PD 2.0 (inferred: the revision has no Not_Supported): a request the revision defines gets Reject (Get_Source_Cap from a
//    sink-only port), a type it does not define is ignored.
//  - Replies, notifications and a far sink's messages (Request, EPR_Request, Sink_Capabilities) get nothing.
//  - 0 too for the types the sink answers itself (they never reach this) and for VCONN_Swap (pd::Vconn answers it).
constexpr std::uint8_t refusal_for(std::uint16_t header, std::uint8_t ecdb, std::uint8_t rev) {
    constexpr std::uint8_t kNotSupported = 16, kReject = 4;
    const unsigned type = header & 0x1F, num_do = (header >> 12) & 7;
    const bool ext = (header >> 15) & 1;
    const bool pd3 = rev >= 2;
    if (ext) {
        if (!pd3) return 0;   // no extended messages in PD 2.0
        switch (type) {
            case 1: case 2: case 5: case 7: case 9: case 11: case 12: case 13: case 14: case 15: case 17: case 18:
                return 0;     // Source_Caps_Ext, Status, Battery_Caps, Manufacturer_Info, Security / FW_Update_Response,
                              // PPS_Status, Country_Info / Codes, Sink_Caps_Ext, EPR_Source_Caps (own), EPR_Sink_Caps: replies
            case 16:          // Extended_Control: EPR_Get_Sink_Cap is the sink's own; KeepAlive_Ack is a reply; a sink-only
                return ecdb == 4 ? 0 : kNotSupported;   // port has no EPR source caps, sends the KeepAlive, knows no other type
            default:
                return kNotSupported;   // Get_Battery_Cap / Status, Get_Manufacturer_Info, Security_Request, Firmware_Update_Request,
                                        // Vendor_Defined_Extended, reserved
        }
    }
    if (num_do > 0) {
        switch (type) {
            case 1: case 2: case 3: case 4: case 5: case 6: case 9: case 10: case 11: case 12: case 15:
                return 0;   // Source_Caps (own), Request / EPR_Request / Sink_Caps (a far sink's), BIST, Battery_Status, Alert,
                            // EPR_Mode (own), Source_Info, Revision: replies or notifications; VDMs are answered by on_vdm
            default:
                return pd3 ? kNotSupported : 0;   // Get_Country_Info (7), Enter_USB (8), reserved
        }
    }
    switch (type) {
        case 7:  return pd3 ? kNotSupported : kReject;   // Get_Source_Cap: a sink-only port
        case 14: case 17: case 18: case 20: case 21: case 23:
            return pd3 ? kNotSupported : 0;   // Data_Reset, Get_Source_Cap_Ext, Get_Status, Get_PPS_Status, Get_Country_Codes,
                                              // Get_Source_Info (none of them exists in PD 2.0)
        case 0: case 25: case 26: case 27: case 28: case 29: case 30: case 31:
            return pd3 ? kNotSupported : 0;   // reserved
        default:
            return 0;   // GoodCRC, GotoMin, Accept, Reject, Ping, PS_RDY, Wait, Data_Reset_Complete, Not_Supported: replies or
                        // notifications; Get_Sink_Cap(_Ext), the swaps, Soft_Reset, Get_Revision: the sink's own; VCONN_Swap: pd::Vconn
    }
}

// The NAK that answers a structured VDM request no handler took (USB PD R3.2 6.4.4.2: a responder NAKs a command it does not
// support, a mode it does not have, an SVID it does not list): the request's header with Command Type NAK, SVID, version,
// object position and command echoed. 0 = no answer: an unstructured VDM, an ACK / NAK / BUSY, or an Attention (never answered).
constexpr std::uint32_t vdm_nak_for(std::uint32_t vdm_header) {
    const bool structured = (vdm_header >> 15) & 1;
    const unsigned cmd_type = (vdm_header >> 6) & 3, cmd = vdm_header & 0x1F;
    if (!structured || cmd_type != 0 || cmd == 6) return 0;
    return (vdm_header & ~(3u << 6)) | (2u << 6);
}

static_assert(vdm_nak_for(0x05ACA003u) == 0x05ACA083u, "Discover Modes REQ -> NAK, everything else echoed");
static_assert(vdm_nak_for(0xFF01A106u) == 0, "Attention is never answered");
static_assert(vdm_nak_for(0xFF01A043u) == 0, "an ACK is not a request");
static_assert(vdm_nak_for(0x27170201u) == 0, "an unstructured VDM is not NAKed");
static_assert(refusal_for(0x0047, 0, 2) == 16 && refusal_for(0x0047, 0, 1) == 4, "Get_Source_Cap: Not_Supported / PD 2.0 Reject");

}  // namespace k2::pd
