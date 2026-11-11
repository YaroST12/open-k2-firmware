// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Learning a real cable's behaviour: replay the requests a source makes to a powered cable and turn its replies
// into a clone (pure logic over an exchange callback, host-tested in tests/test_emk_learn.cpp).
//
// The K2 powers the cable (VCONN) and plays the source's side on SOP': Discover Identity, Discover SVIDs, Discover
// Modes for every SVID the cable lists, then the seeds (requests the caller knows a charger or brick makes, for
// instance the vendor command an OPPO brick sends) and optionally a sweep of the vendor command range (16..31) on
// the cable's SVIDs. Discover Identity walks the attempt table (emk_variant.hpp: PD 3.x + SVDM 2.0, PD 3.x +
// SVDM 1.0, PD 2.0 + SVDM 1.0) and every later request uses the variant that was answered, header revision and
// SVDM version both. Every reply becomes a Row of an emk_quirk::Quirks table, the Discover Identity answer becomes
// the identity record, so the result is a complete saved clone (k2/pd/emlib.hpp). The wire side is behind one
// callback (fusb302::cable_exchange in the firmware, a fake cable in the tests).
#pragma once
#include <array>
#include <cstdint>
#include <span>

#include "k2/pd/emk_quirk.hpp"
#include "k2/pd/emk_variant.hpp"
#include "k2/pd/emrec.hpp"

namespace k2::emk_learn {

inline constexpr unsigned kMaxSeeds = 4;
inline constexpr unsigned kMaxSvids = 3;     // SVIDs whose modes (and sweep) are explored
inline constexpr unsigned kLogBytes = 256;   // packed transcript bytes kept
inline constexpr unsigned kMaxObjs = 7;      // data objects of a PD message

// One request: `n` data objects (the VDM header first), sent as SOP' with this PD specification revision field
// (1 = PD 2.0, 2 = PD 3.x).
struct Request {
    std::uint8_t spec_rev = 2;
    std::uint8_t n = 0;
    std::array<std::uint32_t, kMaxObjs> w{};
};

// What came back after the cable's GoodCRC: nothing (`got` false), a control message (n 0, header type in the
// low 5 bits: Not_Supported 16, Reject 6, ...) or a VDM.
struct Reply {
    bool got = false;
    std::uint8_t n = 0;
    std::uint16_t header = 0;
    std::array<std::uint32_t, kMaxObjs> w{};
};

// One exchange on the wire. False: the request was not acknowledged at all (no GoodCRC, the link or the cable is
// gone) and the run stops; true: `rep.got` tells whether an answer followed.
using ExchangeFn = bool (*)(void* ctx, const Request& req, Reply& rep);

struct Seed {
    std::uint16_t svid = 0;
    std::uint8_t cmd = 0;                         // structured command 0..31
    std::uint8_t nvdo = 0;                        // data objects after the VDM header (0..2)
    std::array<std::uint32_t, 2> vdo{};
};

struct Options {
    std::array<Seed, kMaxSeeds> seeds{};
    std::uint8_t nseeds = 0;
    bool sweep = false;                           // also try vendor commands 16..31 on the cable's SVIDs and the seeds' SVIDs
};

enum class Step : std::uint8_t { Identity, Svids, Modes, Seed, Sweep };

// One transcript entry. Silent probes of a sweep are not kept, everything else is.
struct Xchg {
    Step step = Step::Identity;
    Request req;
    Reply rep;
};

enum class Outcome : std::uint8_t {
    Learned,        // identity read, the rest explored
    NoIdentity,     // the cable never answered Discover Identity (no VCONN, wrong port, plain cable)
    Aborted,        // an exchange was not acknowledged mid-run
};

// The learned clone (record and quirk table) lives in storage the caller lends: the firmware passes the spoof's own
// buffers (a learned clone and an armed one are never needed together), so a Result is only the packed transcript and
// a few counters. Transcript entry, `7 + 4 * (req.n + rep.n)` bytes: [step, spec_rev, req.n, got, rep.n, header u16,
// request words u32, reply words u32] (little endian). It stops, with `log_full`, at the first entry that no longer
// fits; the quirk table does not depend on it.
struct Result {
    Result(emrec::Record& r, emk_quirk::Quirks& q) : rec(r), quirks(q) {}
    Result(const Result&) = delete;
    Result& operator=(const Result&) = delete;

    Outcome outcome = Outcome::NoIdentity;
    emrec::Record& rec;                           // the Discover Identity answer as a saved-clone record
    emk_quirk::Quirks& quirks;
    emk_variant::Answered answered;               // the scan orientation and table row the cable answered (set by the caller; learn() leaves it)
    std::uint8_t svids = 0;                       // SVIDs the cable listed
    std::uint8_t probes = 0;                      // exchanges made
    std::uint8_t silent = 0;                      // probes the cable did not answer
    bool truncated = false;                       // more answers than quirk rows: the rest was dropped
    bool log_full = false;
    std::uint8_t nlog = 0;                        // whole transcript entries kept
    std::uint16_t log_len = 0;                    // bytes of `logbuf` in use
    std::array<std::uint8_t, kLogBytes> logbuf{};

    void clear_log() { nlog = 0; log_len = 0; log_full = false; }
    bool push_log(Step step, const Request& req, const Reply& rep);
    Xchg entry(unsigned i) const;                 // entry `i` decoded; an empty Xchg past `nlog`
};

// A Result that owns its record and table, for hosts with RAM to spare (the tests).
struct Standalone : Result {
    emrec::Record rec_store{};
    emk_quirk::Quirks quirks_store{};
    Standalone() : Result(rec_store, quirks_store) {}
};

// Runs the whole plan. True when the identity was learned (Outcome::Learned); the quirk table holds what else
// the cable answered. Never touches the wire except through `fn`.
bool learn(ExchangeFn fn, void* ctx, const Options& opt, Result& out);

// ---- remote wire forms (Cmd::EmLib learn ops; tools/k2_remote.py and tools/web/k2proto.js mirror them) ----
// Options, after the op byte and the run sub-op: [flags, nseeds, seed * nseeds]. Flags bit0 = also sweep the vendor
// commands, bit1 = the scan may run inline (confirmation, like the e-marker scan). A seed is svid u16, cmd (0..31),
// nvdo (0..2), vdo u32 * nvdo, all little endian. Exact length, anything else is false.
inline constexpr std::size_t kOptionsMax = 2 + kMaxSeeds * (4 + 4 * 2);
bool parse_options(std::span<const std::uint8_t> p, Options& out, bool& confirm);

// Summary (the status and run response): [err, outcome (0 nothing yet, 1 learned, 2 no identity, 3 aborted), svids,
// probes, silent, flags (bit0 truncated, bit1 log full), nlog, rows, unmatched policy, vid u16, pid u16,
// then the answered tail: orientation (0 none, 1 A, 2 B), PD header revision (1 PD 2.0, 2 PD 3.x), SVDM (1, 2),
// attempt (1-based within the orientation)]. The tail was added after the 13-byte form; a host tool accepts both
// lengths and reads the tail as unknown when it is absent. `last` null = nothing learned yet.
inline constexpr std::size_t kSummaryBytes = 17;
std::size_t encode_summary(const Result* last, std::span<std::uint8_t> out);

// One transcript entry: [err, i, step, spec_rev, req_n, got, rep_n, reply PD header u16, request words, reply words]
// (words u32 little endian, VDM header first, rep_n counts it). Past the log: the one byte [3].
inline constexpr std::size_t kEntryMax = 9 + 4 * 3 + 4 * kMaxObjs;
std::size_t encode_entry(const Result& r, unsigned i, std::span<std::uint8_t> out);

// Structured VDM request header: SVID, structured, SVDM version of the variant (2.0: major 1; 1.0: 0), command,
// object position 0. The PD header revision of the same variant goes in Request::spec_rev.
constexpr std::uint32_t vdm_request(std::uint16_t svid, std::uint8_t cmd, emk_variant::Variant v) {
    return emk_variant::vdm_header(svid, cmd, v);
}

}  // namespace k2::emk_learn
