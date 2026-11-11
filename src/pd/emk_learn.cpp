// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#include "k2/pd/emk_learn.hpp"

namespace k2::emk_learn {
namespace {

enum class Cls : std::uint8_t { None, Ack, Nak, Busy, NotSupported, Other };

constexpr std::uint8_t kCtlNotSupported = 16;
constexpr std::uint8_t kMsgVdm = 0x0F;
constexpr std::uint8_t kSweepFirst = 16, kSweepLast = 31;   // the SVID-specific command range

// What a reply is, for the request header it answers: a structured ACK/NAK/BUSY of the same SVID and command, a
// Not_Supported control message, or something else (a retransmission of an earlier answer, a different command).
Cls classify(std::uint32_t req_hdr, const Reply& rep) {
    if (!rep.got) return Cls::None;
    const unsigned type = rep.header & 0x1Fu;
    if (rep.n == 0) return type == kCtlNotSupported ? Cls::NotSupported : Cls::Other;
    if (type != kMsgVdm) return Cls::Other;
    const std::uint32_t h = rep.w[0];
    if ((h & 0x8000u) == 0 || (h >> 16) != (req_hdr >> 16) || (h & 0x1Fu) != (req_hdr & 0x1Fu)) return Cls::Other;
    switch ((h >> 6) & 3u) {
        case 1: return Cls::Ack;
        case 2: return Cls::Nak;
        case 3: return Cls::Busy;
        default: return Cls::Other;
    }
}

struct Run {
    ExchangeFn fn;
    void* ctx;
    Result& out;
    emk_variant::Variant var = emk_variant::kFirst;    // the variant every request goes out in; the answered one after identity()
    bool aborted = false;
    bool saw_not_supported = false, saw_nak = false;   // from the probes of requests the cable has no row for
    std::array<std::uint16_t, kMaxSvids> svid{};
    unsigned nsvid = 0;                                // explored (at most kMaxSvids)

    // One request at the current revision. False when the exchange was not acknowledged.
    bool probe(Step step, std::span<const std::uint32_t> words, Reply& rep) {
        Request req;
        req.spec_rev = var.spec_rev;
        req.n = static_cast<std::uint8_t>(words.size() > kMaxObjs ? kMaxObjs : words.size());
        for (unsigned i = 0; i < req.n; i++) req.w[i] = words[i];
        rep = {};
        out.probes++;
        if (!fn(ctx, req, rep)) {
            aborted = true;
            return false;
        }
        if (!rep.got) out.silent++;
        if (rep.got || step != Step::Sweep) {
            out.push_log(step, req, rep);
        }
        return true;
    }

    void add(const emk_quirk::Row& r) {
        if (!emk_quirk::add_row(out.quirks, r)) out.truncated = true;
    }

    // The row a reply makes: ACK with its VDOs, NAK, BUSY.
    void add_reply_row(std::uint32_t req_hdr, Cls c, const Reply& rep) {
        const std::uint16_t svid_ = static_cast<std::uint16_t>(req_hdr >> 16);
        const std::uint8_t cmd = req_hdr & 0x1Fu;
        if (c == Cls::Ack) {
            add(emk_quirk::svdm_row(svid_, cmd, emk_quirk::Kind::Ack, std::span<const std::uint32_t>(rep.w.data() + 1, rep.n - 1)));
        } else if (c == Cls::Nak) {
            add(emk_quirk::svdm_row(svid_, cmd, emk_quirk::Kind::Nak));
        } else if (c == Cls::Busy) {
            add(emk_quirk::svdm_row(svid_, cmd, emk_quirk::Kind::Busy));
        }
    }

    bool identity() {
        for (const emk_variant::Variant cand : emk_variant::kTable) {   // the scan's attempt table, in its order
            var = cand;
            const std::uint32_t w[1] = {vdm_request(0xFF00, 1, var)};
            Reply rep;
            aborted = false;
            if (!probe(Step::Identity, w, rep)) continue;
            if (classify(w[0], rep) != Cls::Ack) continue;
            std::array<std::uint32_t, 5> v{};
            for (unsigned i = 0; i < 5 && i < rep.n; i++) v[i] = rep.w[i];
            std::uint8_t sr = emrec::spec_rev_from_pd(static_cast<std::uint8_t>((rep.header >> 6) & 3u));
            if (sr == 0) sr = var.spec_rev;
            out.rec = emrec::make(v, sr);
            return emrec::valid(out.rec);
        }
        aborted = false;
        return false;
    }

    // Parses the SVIDs of a Discover SVIDs ACK: two per data object, upper half first, a zero half ends the list.
    void take_svids(const Reply& rep) {
        for (unsigned i = 1; i < rep.n; i++) {
            for (const std::uint32_t half : {rep.w[i] >> 16, rep.w[i] & 0xFFFFu}) {
                if (half == 0) return;
                if (out.svids < 255) out.svids++;
                if (nsvid < kMaxSvids) svid[nsvid++] = static_cast<std::uint16_t>(half);
            }
        }
    }

    bool svids_and_modes() {
        const std::uint32_t w[1] = {vdm_request(0xFF00, 2, var)};
        Reply rep;
        if (!probe(Step::Svids, w, rep)) return false;
        const Cls c = classify(w[0], rep);
        add_reply_row(w[0], c, rep);
        if (c == Cls::Ack) take_svids(rep);
        for (unsigned i = 0; i < nsvid; i++) {
            const std::uint32_t m[1] = {vdm_request(svid[i], 3, var)};
            if (!probe(Step::Modes, m, rep)) return false;
            add_reply_row(m[0], classify(m[0], rep), rep);
        }
        return true;
    }

    void note_policy(Cls c) {
        if (c == Cls::NotSupported) saw_not_supported = true;
        else if (c == Cls::Nak) saw_nak = true;
    }

    bool seeds(const Options& opt) {
        for (unsigned i = 0; i < opt.nseeds && i < kMaxSeeds; i++) {
            const Seed& s = opt.seeds[i];
            std::array<std::uint32_t, 3> w{};
            w[0] = vdm_request(s.svid, s.cmd, var);
            const unsigned nv = s.nvdo > 2 ? 2 : s.nvdo;
            for (unsigned k = 0; k < nv; k++) w[1 + k] = s.vdo[k];
            Reply rep;
            if (!probe(Step::Seed, {w.data(), 1 + nv}, rep)) return false;
            const Cls c = classify(w[0], rep);
            add_reply_row(w[0], c, rep);
            note_policy(c);
        }
        return true;
    }

    bool is_seed(const Options& opt, std::uint16_t sv, std::uint8_t cmd) const {
        for (unsigned i = 0; i < opt.nseeds && i < kMaxSeeds; i++)
            if (opt.seeds[i].svid == sv && (opt.seeds[i].cmd & 0x1Fu) == cmd) return true;
        return false;
    }

    // The vendor command range on the cable's SVIDs and the seeds' SVIDs. An ACK or BUSY is a row; a NAK or
    // Not_Supported only moves the unmatched policy (a row per refused command would fill the table).
    bool sweep(const Options& opt) {
        std::array<std::uint16_t, kMaxSvids> list{};
        unsigned n = 0;
        auto push = [&](std::uint16_t sv) {
            for (unsigned i = 0; i < n; i++)
                if (list[i] == sv) return;
            if (n < kMaxSvids) list[n++] = sv;
        };
        for (unsigned i = 0; i < nsvid; i++) push(svid[i]);
        for (unsigned i = 0; i < opt.nseeds && i < kMaxSeeds; i++) push(opt.seeds[i].svid);
        for (unsigned i = 0; i < n; i++) {
            for (unsigned cmd = kSweepFirst; cmd <= kSweepLast; cmd++) {
                if (is_seed(opt, list[i], static_cast<std::uint8_t>(cmd))) continue;
                const std::uint32_t w[1] = {vdm_request(list[i], static_cast<std::uint8_t>(cmd), var)};
                Reply rep;
                if (!probe(Step::Sweep, w, rep)) return false;
                const Cls c = classify(w[0], rep);
                if (c == Cls::Ack || c == Cls::Busy) add_reply_row(w[0], c, rep);
                else note_policy(c);
            }
        }
        return true;
    }
};

}  // namespace

bool learn(ExchangeFn fn, void* ctx, const Options& opt, Result& out) {
    out.outcome = Outcome::NoIdentity;
    out.quirks = {};
    out.rec = {};
    out.svids = out.probes = out.silent = 0;
    out.truncated = false;
    out.clear_log();
    Run run{fn, ctx, out};
    if (!run.identity()) return false;
    const bool done = run.svids_and_modes() && run.seeds(opt) && (!opt.sweep || run.sweep(opt));
    if (run.saw_not_supported) out.quirks.unmatched = emk_quirk::Unmatched::NotSupported;
    else if (run.saw_nak) out.quirks.unmatched = emk_quirk::Unmatched::Nak;
    out.outcome = done ? Outcome::Learned : Outcome::Aborted;
    return done;
}

namespace {
constexpr std::uint8_t kFlagSweep = 1, kFlagConfirm = 2;
constexpr std::uint8_t kErrNotFound = 3;

std::uint32_t get32(const std::uint8_t* p) {
    return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}
void put32(std::uint8_t* p, std::uint32_t v) {
    for (unsigned i = 0; i < 4; i++) p[i] = static_cast<std::uint8_t>(v >> (8 * i));
}
void put16(std::uint8_t* p, unsigned v) {
    p[0] = static_cast<std::uint8_t>(v);
    p[1] = static_cast<std::uint8_t>(v >> 8);
}
}  // namespace

bool parse_options(std::span<const std::uint8_t> p, Options& out, bool& confirm) {
    if (p.size() < 2 || (p[0] & ~(kFlagSweep | kFlagConfirm)) != 0 || p[1] > kMaxSeeds) return false;
    Options o;
    o.sweep = (p[0] & kFlagSweep) != 0;
    o.nseeds = p[1];
    std::size_t at = 2;
    for (unsigned i = 0; i < o.nseeds; i++) {
        if (p.size() < at + 4) return false;
        Seed& s = o.seeds[i];
        s.svid = static_cast<std::uint16_t>(p[at] | (p[at + 1] << 8));
        s.cmd = p[at + 2];
        s.nvdo = p[at + 3];
        at += 4;
        if (s.cmd > 31 || s.nvdo > 2 || p.size() < at + 4u * s.nvdo) return false;
        for (unsigned k = 0; k < s.nvdo; k++, at += 4) s.vdo[k] = get32(&p[at]);
    }
    if (at != p.size()) return false;
    out = o;
    confirm = (p[0] & kFlagConfirm) != 0;
    return true;
}

std::size_t encode_summary(const Result* last, std::span<std::uint8_t> out) {
    if (out.size() < kSummaryBytes) return 0;
    for (std::size_t i = 0; i < kSummaryBytes; i++) out[i] = 0;
    if (last == nullptr) return kSummaryBytes;
    out[1] = static_cast<std::uint8_t>(last->outcome) + 1;
    out[2] = last->svids;
    out[3] = last->probes;
    out[4] = last->silent;
    out[5] = static_cast<std::uint8_t>((last->truncated ? 1 : 0) | (last->log_full ? 2 : 0));
    out[6] = last->nlog;
    out[7] = last->quirks.count;
    out[8] = static_cast<std::uint8_t>(last->quirks.unmatched);
    put16(&out[9], emrec::vdo(last->rec, 1) & 0xFFFFu);
    put16(&out[11], emrec::vdo(last->rec, 3) >> 16);
    return kSummaryBytes;
}

namespace {
constexpr std::size_t kLogHead = 7;   // step, spec_rev, req.n, got, rep.n, header u16
}

bool Result::push_log(Step step, const Request& req, const Reply& rep) {
    const std::size_t len = kLogHead + 4u * req.n + 4u * rep.n;
    if (log_full || nlog == 255 || log_len + len > kLogBytes) {
        log_full = true;   // sticky: later, smaller entries must not leave a gap in the sequence
        return false;
    }
    std::uint8_t* p = &logbuf[log_len];
    p[0] = static_cast<std::uint8_t>(step);
    p[1] = req.spec_rev;
    p[2] = req.n;
    p[3] = rep.got ? 1 : 0;
    p[4] = rep.n;
    put16(&p[5], rep.header);
    p += kLogHead;
    for (unsigned k = 0; k < req.n; k++, p += 4) put32(p, req.w[k]);
    for (unsigned k = 0; k < rep.n; k++, p += 4) put32(p, rep.w[k]);
    log_len = static_cast<std::uint16_t>(log_len + len);
    nlog++;
    return true;
}

Xchg Result::entry(unsigned i) const {
    Xchg x;
    if (i >= nlog) return x;
    std::size_t at = 0;
    for (unsigned k = 0; k < i; k++) at += kLogHead + 4u * logbuf[at + 2] + 4u * logbuf[at + 4];
    const std::uint8_t* p = &logbuf[at];
    x.step = static_cast<Step>(p[0]);
    x.req.spec_rev = p[1];
    x.req.n = p[2];
    x.rep.got = p[3] != 0;
    x.rep.n = p[4];
    x.rep.header = static_cast<std::uint16_t>(p[5] | (p[6] << 8));
    p += kLogHead;
    for (unsigned k = 0; k < x.req.n; k++, p += 4) x.req.w[k] = get32(p);
    for (unsigned k = 0; k < x.rep.n; k++, p += 4) x.rep.w[k] = get32(p);
    return x;
}

std::size_t encode_entry(const Result& r, unsigned i, std::span<std::uint8_t> out) {
    if (out.empty()) return 0;
    if (i >= r.nlog) {
        out[0] = kErrNotFound;
        return 1;
    }
    const Xchg x = r.entry(i);
    const std::size_t len = 9 + 4u * x.req.n + 4u * x.rep.n;
    if (out.size() < len) return 0;
    out[0] = 0;
    out[1] = static_cast<std::uint8_t>(i);
    out[2] = static_cast<std::uint8_t>(x.step);
    out[3] = x.req.spec_rev;
    out[4] = x.req.n;
    out[5] = x.rep.got ? 1 : 0;
    out[6] = x.rep.n;
    put16(&out[7], x.rep.header);
    std::size_t at = 9;
    for (unsigned k = 0; k < x.req.n; k++, at += 4) put32(&out[at], x.req.w[k]);
    for (unsigned k = 0; k < x.rep.n; k++, at += 4) put32(&out[at], x.rep.w[k]);
    return len;
}

}  // namespace k2::emk_learn
