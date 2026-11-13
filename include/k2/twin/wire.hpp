// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Device twin on the wire: the payload layouts of Cmd::Twin (the op codes and the dispatcher checks are in
// k2/core/remote_proto.hpp, the hardware side in src/core/remote.cpp, the host side in tools/k2_remote.py `twin`). Pure code
// over values and buffers, host-tested by tests/test_twin_wire.cpp. A response never exceeds kMaxRsp bytes (the remote frame's
// payload room), checked by a static_assert in remote.cpp.
//
// Every response starts with err: twin::Err (0 Ok, 1 Full, 2 Invalid, 3 NotFound, 4 Flash, 5 Exists) or one of the kErr* below.
//   LIST     [err, n, seq * n]
//   GET      [err, seq, part, parts, n, bytes * n]     the record of k2/twin/library.hpp, kChunk bytes per part
//   PREDICT  [err, seq, kind, limiter, flags, pos, mv u16, ma u16, mw u32, want_mw u32, claim_w u16, cable_ma u16, cable_mv u16]
//   USB      [err, seq, which, n, text * n]            which 0 declared, 1 observed; text without a terminator
//   LEARN    [err, how, seq, existed, unchanged, vid u16, pid u16, num_pdos, flags]
//   RUN      [refusal, n]                              refusal = twin::live::Refusal (0 = started)
//   STATUS   [err, running, reason, count, done, current, restoring, verdict mask u16]
//   RESULT   [err, seq, verdict, skip, measured_mv u16]
//   INLINE   [err, sub, ...]                         the inline interrogation, see encode_iq_status below
#pragma once
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>

#include "k2/twin/library.hpp"
#include "k2/twin/live.hpp"
#include "k2/twin/predict.hpp"
#include "k2/twin/twin.hpp"
#include "k2/twin/twin_hw.hpp"
#include "k2/twin/usbdata.hpp"

namespace k2::twin::wire {

inline constexpr std::size_t kMaxRsp = 58;       // the remote frame's payload room (remote::kMaxRspPayload)
inline constexpr std::size_t kChunk = 48;        // record bytes per GET part: 5 + 48 <= kMaxRsp
inline constexpr std::size_t kMaxParts = (kMaxFileBytes + kChunk - 1) / kChunk;   // 3
inline constexpr std::size_t kUsbText = 40;
inline constexpr std::uint8_t kErrBusy = 7;      // same value as the saved-cable library's busy
inline constexpr std::uint8_t kErrNoCaps = 8;    // predict: the charger offered no list
inline constexpr std::uint8_t kErrLearn = 9;     // learn: nothing to learn from, `how` says why
inline constexpr std::uint8_t kUsbDeclared = 0, kUsbObserved = 1;
inline constexpr std::uint8_t kPredMismatch = 1, kPredObserved = 2;

static_assert(5 + kChunk <= kMaxRsp);
static_assert(4 + kUsbText <= kMaxRsp);
static_assert(3 + kCapacity <= kMaxRsp);
static_assert(kMaxParts * kChunk >= kMaxFileBytes);

namespace detail {
inline void le16(std::uint8_t* p, std::uint16_t v) { p[0] = std::uint8_t(v); p[1] = std::uint8_t(v >> 8); }
inline void le32(std::uint8_t* p, std::uint32_t v) { for (int i = 0; i < 4; ++i) p[i] = std::uint8_t(v >> (8 * i)); }
}  // namespace detail

// LIST: the twin numbers (at most kCapacity).
inline std::size_t encode_list(std::span<const std::uint8_t> seqs, std::span<std::uint8_t> out) {
    const std::size_t n = std::min<std::size_t>(seqs.size(), kCapacity);
    if (out.size() < 2 + n) return 0;
    out[0] = 0;
    out[1] = std::uint8_t(n);
    for (std::size_t i = 0; i < n; ++i) out[2 + i] = seqs[i];
    return 2 + n;
}

// GET: part `part` of the record. An out-of-range part answers err Invalid with just the err byte.
inline std::size_t encode_chunk(std::uint8_t seq, std::uint8_t part, std::span<const std::uint8_t> record, std::span<std::uint8_t> out) {
    const std::size_t parts = (record.size() + kChunk - 1) / kChunk;
    if (out.empty()) return 0;
    if (part >= parts || out.size() < 5 + kChunk) {
        out[0] = std::uint8_t(Err::Invalid);
        return 1;
    }
    const std::size_t off = std::size_t(part) * kChunk, n = std::min(kChunk, record.size() - off);
    out[0] = 0;
    out[1] = seq;
    out[2] = part;
    out[3] = std::uint8_t(parts);
    out[4] = std::uint8_t(n);
    std::memcpy(&out[5], record.data() + off, n);
    return 5 + n;
}

inline constexpr std::size_t kPredictBytes = 24;
inline std::size_t encode_predict(std::uint8_t seq, const Outcome& o, const Facts& f, std::span<std::uint8_t> out) {
    if (out.size() < kPredictBytes) return 0;
    std::uint8_t* b = out.data();
    b[0] = 0;
    b[1] = seq;
    b[2] = std::uint8_t(o.kind);
    b[3] = std::uint8_t(o.limiter);
    b[4] = std::uint8_t((o.mismatch ? kPredMismatch : 0) | (o.observed ? kPredObserved : 0));
    b[5] = o.pos;
    detail::le16(&b[6], o.mv);
    detail::le16(&b[8], o.ma);
    detail::le32(&b[10], o.mw);
    detail::le32(&b[14], o.want_mw);
    detail::le16(&b[18], std::uint16_t(std::min<std::uint32_t>(f.claim_w, 0xFFFFu)));
    detail::le16(&b[20], f.cable_ma);
    detail::le16(&b[22], f.cable_mv);
    return kPredictBytes;
}

// USB: one line of the twin's USB data. Declared: the best declared link and the alternate modes, with the Thunderbolt class
// as an inference ("TB4 class"); observed: what the PD messages configured. Never a measured rate. Empty = nothing held.
// The SuperSpeed lanes cannot be measured by this device: a speed here is declared or negotiated, never a throughput.
inline void usb_text(const Twin& t, std::uint8_t which, std::span<char> out) {
    if (out.empty()) return;
    out[0] = 0;
    char b[kUsbText + 1]{};
    if (which == kUsbDeclared) {
        if (!t.decl.any()) return;
        declared_text(t.id_hi, t.decl, b);
        const char* tb = tb_class(declared_link(t.id_hi, t.decl));
        const std::size_t n = std::strlen(b);
        if (*tb != 0 && n + 1 + std::strlen(tb) + 1 <= sizeof b) {
            b[n] = ' ';
            std::memcpy(b + n + 1, tb, std::strlen(tb) + 1);
        }
    } else {
        if (!t.obs.valid) return;
        observed_text(t.obs, b);   // names TBT3/TBT4 itself when the Enter_Mode exchange saw it: nothing to infer here
    }
    const std::size_t n = std::min(std::strlen(b), out.size() - 1);
    std::memcpy(out.data(), b, n);
    out[n] = 0;
}

inline std::size_t encode_usb(std::uint8_t seq, std::uint8_t which, const Twin& t, std::span<std::uint8_t> out) {
    if (out.size() < 4 + kUsbText) return 0;
    char b[kUsbText + 1];
    usb_text(t, which, b);
    const std::size_t n = std::strlen(b);
    out[0] = 0;
    out[1] = seq;
    out[2] = which;
    out[3] = std::uint8_t(n);
    std::memcpy(&out[4], b, n);
    return 4 + n;
}

inline constexpr std::size_t kLearnBytes = 11;
// `how`: twin_hw::Learn as a number; store_err: twin::Err of the save.
inline std::size_t encode_learn(std::uint8_t how, std::uint8_t store_err, const Result& r, const Twin& t, std::span<std::uint8_t> out) {
    if (out.size() < kLearnBytes) return 0;
    std::uint8_t* b = out.data();
    b[0] = how == 0 ? 0 : how == 4 ? store_err : kErrLearn;   // Learn::Ok, Learn::Store
    b[1] = how;
    b[2] = r.seq;
    b[3] = r.existed ? 1 : 0;
    b[4] = r.unchanged ? 1 : 0;
    detail::le16(&b[5], t.vid);
    detail::le16(&b[7], t.pid);
    b[9] = t.num_pdos;
    b[10] = std::uint8_t((t.has_request() ? kFlagRequest : 0) | (t.decl.any() ? kFlagDeclared : 0) | (t.obs.valid ? kFlagObserved : 0));
    return kLearnBytes;
}

inline constexpr std::size_t kRunBytes = 2;
inline std::size_t encode_run(live::Refusal r, std::uint8_t n, std::span<std::uint8_t> out) {
    if (out.size() < kRunBytes) return 0;
    out[0] = std::uint8_t(r);
    out[1] = n;
    return kRunBytes;
}

// The run's request: [flags (bit0 confirm), n, seq * n]; n = 0 = every twin in the library.
inline constexpr std::uint8_t kRunConfirm = 1;
struct RunReq {
    bool confirm = false;
    std::uint8_t n = 0;
    std::array<std::uint8_t, kCapacity> seqs{};
};
inline bool parse_run(std::span<const std::uint8_t> p, RunReq& out) {
    if (p.size() < 2 || (p[0] & ~kRunConfirm) != 0 || p[1] > kCapacity || p.size() != 2u + p[1]) return false;
    RunReq r;
    r.confirm = (p[0] & kRunConfirm) != 0;
    r.n = p[1];
    for (std::size_t i = 0; i < r.n; ++i) {
        if (p[2 + i] == 0 || p[2 + i] > kCapacity) return false;
        for (std::size_t j = 0; j < i; ++j)
            if (r.seqs[j] == p[2 + i]) return false;   // a twin twice in one run proves nothing new
        r.seqs[i] = p[2 + i];
    }
    out = r;
    return true;
}

inline constexpr std::size_t kStatusBytes = 9;
// One bit per twin number 1..kCapacity (bit seq-1) whose slot holds a verdict from the last run.
inline std::uint16_t verdict_mask(const live::Runner& r) {
    std::uint16_t m = 0;
    for (unsigned s = 1; s <= kCapacity; ++s)
        if (r.slot(std::uint8_t(s)).verdict != live::Verdict::None) m = std::uint16_t(m | (1u << (s - 1)));
    return m;
}
inline std::size_t encode_status(const live::Runner& r, std::span<std::uint8_t> out) {
    if (out.size() < kStatusBytes) return 0;
    std::uint8_t* b = out.data();
    b[0] = 0;
    b[1] = r.running() ? 1 : 0;
    b[2] = std::uint8_t(r.reason());
    b[3] = r.count();
    b[4] = r.done_count();
    b[5] = r.current();
    b[6] = r.restoring() ? 1 : 0;
    detail::le16(&b[7], verdict_mask(r));
    return kStatusBytes;
}

inline constexpr std::size_t kSlotBytes = 6;
inline std::size_t encode_slot(std::uint8_t seq, const live::Slot& s, std::span<std::uint8_t> out) {
    if (out.size() < kSlotBytes) return 0;
    out[0] = 0;
    out[1] = seq;
    out[2] = std::uint8_t(s.verdict);
    out[3] = std::uint8_t(s.skip);
    detail::le16(&out[4], s.measured_mv);
    return kSlotBytes;
}

// ---- INLINE: the interrogation of a device inline (twin_hw::inline_*, k2/pd/pd_interrogate.hpp) ------------------------------
// [11, sub, ...]:
//   STATUS  [11, 0]                       -> kIqStatusBytes, below
//   START   [11, 1, flags, name...]       flags bit0 dry (learn, do not save), bit1 confirm, bit2 spoof -> [err, 1, refusal]
//                                         err 0 = started, kErrIqRefused = refused: refusal = pd::IqRefusal
//           with the spoof bit: [11, 1, flags, kind, id u16, name...]: the cable identity the K2 shows the charger during the run,
//           kind 0 = a built-in record (id = its index), 1 = a saved one (id = its library seq); a record that is not there or not
//           valid refuses with BadSpoof (the dispatcher checks the lengths, remote.cpp resolves the record)
//   ABORT   [11, 2]                       -> [0, 2, was_running]
//   ANSWERS [11, 3, part]                 -> [0, 3, part, ...] what the device answered:
//                                         part 0 sink PDOs [n, u32 * 7], 1 Sink_Capabilities_Extended [n, bytes * 24],
//                                         2 Discover Identity [valid, n, u32 * 6], 3 [n_svids, u16 * 12, n_modes, (u16 svid, u32 vdo) * 2],
//                                         4 EPR sink PDOs [n, partial, u32 * 13] (n = 0: none; partial: only the first of the two
//                                         26-byte chunks was read, so the PDOs past it are missing)
// STATUS: [err 0, sub 0, running, pending, done, phase, step, why, match, rev, asked, ok, refused, silent, sent, dev_acks,
//          resets_sent, resets_seen, skipped_first_reset, vid u16, pid u16, n_svids, n_modes, n_sink_pdos, sink_ext_len,
//          revision u32, first ready, first pos, first rdo u32, second ready, second pos, second rdo u32,
//          learn how, store err, store seq, existed, unchanged, ms u16, late mask, flags (bit0 spoof, bit1 EPR partial),
//          SOP' queries, spoof replies, n_epr_pdos]
//   pending: started, not learned from yet; done: the last run was learned from (learn how / store say what it stored);
//   masks are pd::iq_bit per IqStep (late: answered after the step had been given up as silent, also set in ok);
//   ms: the run's length once it ended (ended_at - started_at, capped), else 0.
inline constexpr std::uint8_t kInlineStatus = 0, kInlineStart = 1, kInlineAbort = 2, kInlineAnswers = 3;
inline constexpr std::uint8_t kInlineDry = 1, kInlineConfirm = 2, kInlineSpoof = 4;
inline constexpr std::uint8_t kInlineSpoofBuiltin = 0, kInlineSpoofSaved = 1;
inline constexpr std::uint8_t kInlineParts = 5;
inline constexpr std::uint8_t kErrIqRefused = 10;
inline constexpr std::size_t kIqStatusBytes = 55;
inline constexpr std::size_t kInlineSpoofHead = 6;   // [11, 1, flags, kind, id u16]
static_assert(kIqStatusBytes <= kMaxRsp && 4 + 1 + 2 * pd::kMaxSvids + 1 + 6 * pd::kIdModeSvids <= kMaxRsp);
static_assert(3 + 2 + 4 * pd::kIqEprMaxPdos <= kMaxRsp);

// The cable identity of a spoofed run, as the START op carries it (pure: remote.cpp resolves it to a record).
struct InlineSpoof {
    bool on = false;
    std::uint8_t kind = kInlineSpoofBuiltin;
    std::uint16_t id = 0;
};

inline std::size_t encode_iq_status(bool running, const twin_hw::InlineLearn& l, const pd::IqStatus& s, std::span<std::uint8_t> out) {
    if (out.size() < kIqStatusBytes) return 0;
    std::uint8_t* b = out.data();
    b[0] = 0;
    b[1] = kInlineStatus;
    b[2] = running ? 1 : 0;
    b[3] = l.pending ? 1 : 0;
    b[4] = l.done ? 1 : 0;
    b[5] = std::uint8_t(s.phase);
    b[6] = std::uint8_t(s.step);
    b[7] = std::uint8_t(s.why);
    b[8] = std::uint8_t(s.match);
    b[9] = s.rev;
    b[10] = s.asked_mask;
    b[11] = s.ok_mask;
    b[12] = s.refused_mask;
    b[13] = s.silent_mask;
    b[14] = s.sent;
    b[15] = s.dev_acks;
    b[16] = s.resets_sent;
    b[17] = s.resets_seen;
    b[18] = s.skipped_first_reset ? 1 : 0;
    detail::le16(&b[19], s.id.valid ? s.id.vid : 0);
    detail::le16(&b[21], s.id.valid ? s.id.pid : 0);
    b[23] = s.num_svids;
    b[24] = s.num_modes;
    b[25] = s.num_sink_pdos;
    b[26] = s.sink_ext_len;
    detail::le32(&b[27], s.revision);
    b[31] = s.first.ready ? 1 : 0;
    b[32] = s.first.pos;
    detail::le32(&b[33], s.first.rdo);
    b[37] = s.second.ready ? 1 : 0;
    b[38] = s.second.pos;
    detail::le32(&b[39], s.second.rdo);
    b[43] = std::uint8_t(l.result.how);
    b[44] = std::uint8_t(l.result.store.err);
    b[45] = l.result.store.seq;
    b[46] = l.result.store.existed ? 1 : 0;
    b[47] = l.result.store.unchanged ? 1 : 0;
    const std::uint32_t ms = pd::iq_finished(s.phase) && s.ended_at >= s.started_at ? s.ended_at - s.started_at : 0;
    detail::le16(&b[48], std::uint16_t(std::min<std::uint32_t>(ms, 0xFFFFu)));
    b[50] = s.late_mask;
    b[51] = std::uint8_t((s.spoof ? 1 : 0) | (s.epr_partial ? 2 : 0) | (s.sopp_real ? 4 : 0));
    b[52] = s.sopp_queries;
    b[53] = s.sopp_replies;
    b[54] = s.num_epr_pdos;
    return kIqStatusBytes;
}

// ANSWERS part `part` (0..kInlineParts-1); an unknown part answers err Invalid.
inline std::size_t encode_iq_answers(const pd::IqStatus& s, std::uint8_t part, std::span<std::uint8_t> out) {
    if (out.size() < kMaxRsp) return 0;
    std::uint8_t* b = out.data();
    b[0] = 0;
    b[1] = kInlineAnswers;
    b[2] = part;
    std::size_t n = 3;
    switch (part) {
        case 0: {
            const std::size_t k = std::min<std::size_t>(s.num_sink_pdos, s.sink_pdos.size());
            b[n++] = std::uint8_t(k);
            for (std::size_t i = 0; i < k; ++i, n += 4) detail::le32(&b[n], s.sink_pdos[i]);
            return n;
        }
        case 1: {
            const std::size_t k = std::min<std::size_t>(s.sink_ext_len, s.sink_ext.size());
            b[n++] = std::uint8_t(k);
            std::memcpy(&b[n], s.sink_ext.data(), k);
            return n + k;
        }
        case 2: {
            const std::size_t k = s.id.valid ? std::min<std::size_t>(s.id.num_vdos, s.id.vdos.size()) : 0;
            b[n++] = s.id.valid ? 1 : 0;
            b[n++] = std::uint8_t(k);
            for (std::size_t i = 0; i < k; ++i, n += 4) detail::le32(&b[n], s.id.vdos[i]);
            return n;
        }
        case 3: {
            const std::size_t k = std::min<std::size_t>(s.num_svids, s.svids.size());
            b[n++] = std::uint8_t(k);
            for (std::size_t i = 0; i < k; ++i, n += 2) detail::le16(&b[n], s.svids[i]);
            const std::size_t m = std::min<std::size_t>(s.num_modes, s.mode_svid.size());
            b[n++] = std::uint8_t(m);
            for (std::size_t i = 0; i < m; ++i, n += 6) {
                detail::le16(&b[n], s.mode_svid[i]);
                detail::le32(&b[n + 2], s.mode_vdo[i]);
            }
            return n;
        }
        case 4: {
            const std::size_t k = std::min<std::size_t>(s.num_epr_pdos, s.epr_pdos.size());
            b[n++] = std::uint8_t(k);
            b[n++] = s.epr_partial ? 1 : 0;
            for (std::size_t i = 0; i < k; ++i, n += 4) detail::le32(&b[n], s.epr_pdos[i]);
            return n;
        }
        default:
            b[0] = std::uint8_t(Err::Invalid);
            return 1;
    }
}

// ---- the command handler -----------------------------------------------------------------------------------------------------
// Op codes: the same numbers as remote::kTw* (static_asserted in src/core/remote.cpp).
inline constexpr std::uint8_t kOpList = 0, kOpGet = 1, kOpPredict = 2, kOpLearn = 3, kOpRename = 4, kOpDelete = 5, kOpRun = 6,
                              kOpStatus = 7, kOpResult = 8, kOpUsb = 9, kOpCancel = 10, kOpInline = 11;

// One Cmd::Twin request over an Api (the hardware one in src/core/remote.cpp, a fake in the host tests). The dispatcher has
// already checked the lengths and the argument ranges, the confirm bit and the busy guard (k2/core/remote_proto.hpp); this
// still refuses what it cannot parse, so it stands on its own. Returns the response size (0 = nothing to say).
//   unsigned list(std::span<std::uint8_t>)                          twin numbers, ascending
//   bool get(std::uint8_t, Twin&)                                   false: missing or unreadable
//   bool predict(const Twin&, Outcome&, Facts&)                     false: the charger offered no list
//   void log_predict(std::uint8_t seq, const Twin&, const Outcome&) the [TWIN] line, one per prediction
//   twin_hw::Learned learn(std::uint8_t how, std::string_view name, bool save)
//   Err rename(std::uint8_t, std::string_view)    Err remove(std::uint8_t)
//   live::Refusal run(std::span<const std::uint8_t> seqs, bool confirmed)
//   const live::Runner& runner() const    bool running() const    void cancel()
//   pd::IqRefusal inline_start(std::string_view name, bool save, bool confirmed, const InlineSpoof&)    void inline_cancel()
//   bool inline_running() const    const twin_hw::InlineLearn& inline_learn() const    const pd::IqStatus& inline_status() const
template <class Api>
std::size_t handle(Api& api, std::span<const std::uint8_t> p, std::span<std::uint8_t> out) {
    if (p.empty() || out.size() < kMaxRsp) return 0;
    const auto err1 = [&](std::uint8_t e) -> std::size_t { out[0] = e; return 1; };
    const auto need = [&](std::size_t n) { return p.size() >= n; };
    const auto text = [&](std::size_t from) {
        return std::string_view(reinterpret_cast<const char*>(p.data()) + from, p.size() - from);
    };
    switch (p[0]) {
        case kOpList: {
            std::array<std::uint8_t, kCapacity> s{};
            const unsigned n = api.list(s);
            return encode_list(std::span<const std::uint8_t>(s.data(), n), out);
        }
        case kOpGet: {
            if (!need(3)) return err1(std::uint8_t(Err::Invalid));
            Twin t;
            if (!api.get(p[1], t)) return err1(std::uint8_t(Err::NotFound));
            std::array<std::uint8_t, kMaxFileBytes> rec{};
            const std::size_t n = encode(t, p[1], rec);
            if (n == 0) return err1(std::uint8_t(Err::Invalid));
            return encode_chunk(p[1], p[2], std::span<const std::uint8_t>(rec.data(), n), out);
        }
        case kOpPredict: {
            if (!need(2)) return err1(std::uint8_t(Err::Invalid));
            Twin t;
            if (!api.get(p[1], t)) return err1(std::uint8_t(Err::NotFound));
            Outcome o;
            Facts f;
            if (!api.predict(t, o, f)) return err1(kErrNoCaps);
            api.log_predict(p[1], t, o);
            return encode_predict(p[1], o, f, out);
        }
        case kOpUsb: {
            if (!need(3)) return err1(std::uint8_t(Err::Invalid));
            Twin t;
            if (!api.get(p[1], t)) return err1(std::uint8_t(Err::NotFound));
            return encode_usb(p[1], p[2], t, out);
        }
        case kOpLearn: {
            if (!need(3)) return err1(std::uint8_t(Err::Invalid));
            const twin_hw::Learned l = api.learn(p[1], text(3), (p[2] & 1) == 0);
            return encode_learn(std::uint8_t(l.how), std::uint8_t(l.store.err), l.store, l.twin, out);
        }
        case kOpRename: {
            if (!need(3)) return err1(std::uint8_t(Err::Invalid));
            out[0] = std::uint8_t(api.rename(p[1], text(2)));
            out[1] = p[1];
            return 2;
        }
        case kOpDelete: {
            if (!need(3) || p[2] != 1) return err1(std::uint8_t(Err::Invalid));
            out[0] = std::uint8_t(api.remove(p[1]));
            out[1] = p[1];
            return 2;
        }
        case kOpRun: {
            RunReq q;
            if (!parse_run(p.subspan(1), q)) return err1(std::uint8_t(Err::Invalid));
            std::array<std::uint8_t, kCapacity> all{};
            unsigned n = q.n;
            if (n == 0) n = api.list(all);   // every twin in the library, in number order
            const std::uint8_t* seqs = q.n == 0 ? all.data() : q.seqs.data();
            const live::Refusal r = api.run(std::span<const std::uint8_t>(seqs, n), q.confirm);
            return encode_run(r, std::uint8_t(n), out);
        }
        case kOpStatus:
            return encode_status(api.runner(), out);
        case kOpResult:
            if (!need(2)) return err1(std::uint8_t(Err::Invalid));
            return encode_slot(p[1], api.runner().slot(p[1]), out);
        case kOpCancel: {
            const bool was = api.running();
            api.cancel();
            out[0] = 0;
            out[1] = was ? 1 : 0;
            return 2;
        }
        case kOpInline: {
            if (!need(2)) return err1(std::uint8_t(Err::Invalid));
            switch (p[1]) {
                case kInlineStatus:
                    return encode_iq_status(api.inline_running(), api.inline_learn(), api.inline_status(), out);
                case kInlineStart: {
                    if (!need(3) || (p[2] & ~(kInlineDry | kInlineConfirm | kInlineSpoof)) != 0) return err1(std::uint8_t(Err::Invalid));
                    const bool spoof = (p[2] & kInlineSpoof) != 0;
                    if (spoof && (!need(kInlineSpoofHead) || p[3] > kInlineSpoofSaved)) return err1(std::uint8_t(Err::Invalid));
                    const InlineSpoof sp = spoof ? InlineSpoof{true, p[3], std::uint16_t(p[4] | (p[5] << 8))} : InlineSpoof{};
                    const pd::IqRefusal r =
                        api.inline_start(text(spoof ? kInlineSpoofHead : 3), (p[2] & kInlineDry) == 0, (p[2] & kInlineConfirm) != 0, sp);
                    out[0] = r == pd::IqRefusal::None ? 0 : kErrIqRefused;
                    out[1] = kInlineStart;
                    out[2] = std::uint8_t(r);
                    return 3;
                }
                case kInlineAbort: {
                    const bool was = api.inline_running();
                    api.inline_cancel();
                    out[0] = 0;
                    out[1] = kInlineAbort;
                    out[2] = was ? 1 : 0;
                    return 3;
                }
                case kInlineAnswers:
                    if (!need(3)) return err1(std::uint8_t(Err::Invalid));
                    return encode_iq_answers(api.inline_status(), p[2], out);
                default:
                    return err1(std::uint8_t(Err::Invalid));
            }
        }
        default:
            return err1(std::uint8_t(Err::Invalid));
    }
}

}  // namespace k2::twin::wire
