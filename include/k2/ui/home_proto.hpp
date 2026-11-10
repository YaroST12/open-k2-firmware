// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Home v7 protocol row source: turns what the firmware knows about the charger (the passively decoded PD
// contract and source capabilities, the D+/D- verdict, the Type-C Rp current) into one ProtoInfo for the
// row and the cells' scales and limits. Pure: the caller fetches the inputs, tests/test_home_proto.cpp
// covers the mapping.
#pragma once
#include <cstdint>
#include "k2/fastcharge/fastcharge_classify.hpp"
#include "k2/pd/pd.hpp"
#include "k2/pd/pd_engine.hpp"
#include "k2/pd/pd_sink.hpp"
#include "k2/ui/home_model.hpp"

namespace k2::home {

// A sink Request repeated at least this often with no Source_Capabilities on record is the PPS
// keep-alive pattern (a phone holding its PPS contract re-requests every ~0.5-1 s): label it PPS
// from the measured VBUS instead of a guessed fixed RDO.
inline constexpr uint32_t kPpsKeepaliveMs = 10000;

struct ProtoSource {
    pd::Contract ctr{};                 // the sink's latest Request (valid = a PD contract exists)
    const pd::SourceCaps *caps = nullptr;   // the source's capabilities when seen
    bool pd_link = false;               // CC attached and not waiting: the contract belongs to this source
    const pd::SinkStatus *sink = nullptr;   // the K2's OWN sink (fusb302::sink_status()); the K2 never hears its own Requests
    fastcharge::Protocol fc = fastcharge::Protocol::Pending;
    int32_t vbus_mv = 0;                // live VBUS, only rounded to a nominal level for protocols that carry none
    uint32_t typec_ma = 0;              // the Rp current of a source that sent no PD caps (0 = undecided)
    uint32_t req_period_ms = 0;         // period of the last sink Request (0 = fewer than two), the PPS keep-alive
};

ProtoInfo derive_proto(const ProtoSource &s);

}  // namespace k2::home
