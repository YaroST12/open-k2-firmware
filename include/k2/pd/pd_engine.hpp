// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// USB-PD reading engine, pure logic (no I/O, host-tested in tests/test_pd_engine.cpp).
// The FUSB302 driver hands every received frame to Engine::on_message(); the remote
// `debug pd-msg` command feeds the same entry point. Frames are header + data objects as
// they come off the wire, without the SOP token and the CRC. The engine only reads: it
// builds no message to transmit; the sink and Identify engines (pd_sink.hpp, pd_identify.hpp) do.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

#include "k2/pd/pd.hpp"
#include "k2/pd/pd_ext.hpp"
#include "k2/pd/pd_vdm.hpp"
#include "k2/pd/usb_link.hpp"

namespace k2::pd {

enum class Sop : std::uint8_t { Sop, SopP, SopPP };  // FIFO token 0xE0, 0xC0, 0xA0

// Extended message header (USB PD 3.1 6.2.1.2): the first two payload bytes of a message
// whose header has bit 15 set.
struct ExtHeader {
    std::uint16_t data_size = 0;  // bits 8:0, total message bytes over all chunks
    bool request = false;         // bit 10: a chunk request, no data
    std::uint8_t chunk_number = 0;  // bits 14:11
    bool chunked = false;         // bit 15
};
ExtHeader decode_ext_header(std::uint16_t raw);

inline constexpr std::uint8_t kExtEprSourceCaps = 0x11;  // extended message type
inline constexpr std::size_t kChunkBytes = 26;           // payload of a full chunk (28 - 2)

// Apple cable metadata: the VDOs (after the VDM header) of up to six SOP' VDMs of VID 0x05AC,
// stored in arrival order, 24 bytes per message. We never request them (passive rule), so this
// only fills when something else asks the cable.
struct AppleCable {
    static constexpr std::uint8_t kChunks = 6;
    std::array<std::uint8_t, kChunks * 24> bytes{};
    std::uint8_t chunks = 0;
    bool complete() const { return chunks == kChunks; }
};

// Contract seen on the line: the sink's Request (position, current, PPS voltage) and the
// source's PS_RDY. Decoded passively, so an existing contract is visible even when the
// Source_Capabilities were missed.
struct Contract {
    bool valid = false;
    bool ready = false;       // PS_RDY seen after the latest Request
    std::uint8_t pos = 0;     // 1-based object position
    std::uint32_t rdo = 0;    // the sink's latest RDO as sent (Request, or the RDO of an EPR_Request)
    std::uint16_t mv = 0;     // fixed: the PDO voltage when known; PPS/AVS: the requested voltage
    std::uint16_t op_ma = 0, max_ma = 0;
    std::uint8_t flags = 0;   // kRdo* bits of the latest RDO
    bool accepted = false;    // Accept seen after the latest Request
    bool rejected = false;    // Reject or Wait seen after the latest Request
    std::uint32_t requests = 0;  // sink Requests decoded since boot (never cleared by reset_source)
};

// The sink's own Sink_Capabilities (power role sink), seen only when something asked for them: we never
// ask while inline, so this stays empty unless the source sent a Get_Sink_Cap.
struct SinkCapsSeen {
    bool valid = false;
    std::uint8_t n = 0;
    std::array<std::uint32_t, 7> raw{};
};

class Engine {
public:
    Contract contract;
    SinkCapsSeen sink;
    // RED (k2/pps-overload review): declared only, nothing records it yet. Any control message seen on
    // SOP from a sink (header bit 8 clear: its GoodCRC, its Get_Source_Cap, ...) is PD traffic from a far
    // sink, so the passive-load gate (pd::over_load_allowed over fusb302::over_load_facts()) can refuse
    // it. Cleared with the attach / detach like the other sink-role records (reset_sink / reset_source).
    bool sink_ctl_seen = false;
    SourceCaps caps;
    // The list an SPR-mode EPR_Get_Source_Cap returned (informational, no EPR entry): decoded here while `epr_info_only`
    // is set (the glue sets it while the sink's own query is pending), so the live `caps` never claims EPR.
    SourceCaps epr_info;
    bool epr_info_only = false;
    // Identity, SVIDs and modes seen on SOP, filed by who sent them (header bit 8, the power role): `vdm` is what the
    // source (the charger) said, `sink_vdm` what the sink said. Passive inline sees both partners answer on SOP.
    // A completed PR_Swap exchanges the power roles, so the two records are swapped with them (each partner keeps
    // its own data under its new role; a partner that never answered stays unknown).
    VdmInfo vdm;
    VdmInfo sink_vdm;
    ExtInfo ext;  // latest extended messages seen from the source
    Emarker cable;      // Discover Identity ACK from the cable (SOP' or SOP'')
    std::uint32_t cable_acks = 0;  // Discover Identity ACKs accepted from a cable since boot
    VdmInfo cable_vdm;  // cable SVIDs and modes (SOP' / SOP'' ACKs)
    AppleCable apple;
    usblink::Classifier usb;  // USB / Thunderbolt link seen on SOP, SOP' and SOP'' (cleared with the source). One
                              // combined record, not per side: a PR_Swap exchanges power roles only, the data
                              // roles and the link stay, so it is kept across the swap.

    // `payload` = the data objects (for an extended message: ext header first). `cc` = the
    // CC line the frame arrived on (0 none, 1, 2).
    void on_message(Sop sop, std::uint16_t header, std::span<const std::uint8_t> payload, std::uint8_t cc);

    void reset() { std::construct_at(this); }   // in place: `*this = Engine{}` builds a ~1.2 KB temporary on the stack
    // New charger: drop its caps, identity, extended data and contract; the counters keep running.
    void reset_source();
    void reset_sink() { sink = {}; }
    void reset_cable() { cable = {}; cable_vdm = {}; apple = {}; }
    bool cable_thunderbolt() const;  // the cable advertised SVID 0x8087

private:
    void on_request(std::uint16_t header, std::span<const std::uint8_t> payload);
    void on_cable_message(Sop sop, std::uint16_t header, std::span<const std::uint8_t> payload);
    void on_ext_message(std::uint8_t type, std::span<const std::uint8_t> payload);
    void on_epr_caps_chunk(std::uint16_t header, std::span<const std::uint8_t> payload, std::uint8_t cc);
    void on_sop_control(std::uint8_t type);

    std::array<std::uint8_t, kMaxPdos * 4> epr_buf_{};
    std::uint16_t epr_total_ = 0;  // data_size of the message in progress, 0 = none
    std::uint8_t epr_mask_ = 0;    // chunks received, bit n = chunk n
    bool pr_swap_pending_ = false;   // a PR_Swap was seen on SOP and not answered yet
    bool pr_swap_accepted_ = false;  // it was Accepted; PS_RDY completes the swap
};

}  // namespace k2::pd
