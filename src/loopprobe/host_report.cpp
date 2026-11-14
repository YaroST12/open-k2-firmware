// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Loop probe "Host report", PD half: see k2/loopprobe/host_report.hpp.
#include "k2/loopprobe/host_report.hpp"

#include <algorithm>

#include "k2/core/bbstore.hpp"
#include "k2/pd/pd_ext.hpp"
#include "k2/pd/pd_identify.hpp"
#include "k2/pd/pd_probe.hpp"

namespace k2::loopprobe::host {
namespace {

constexpr std::uint8_t kCtlAccept = 3, kCtlReject = 4, kCtlWait = 12, kCtlNotSupported = 16, kCtlDrSwap = 9;
constexpr std::uint8_t kCtlGetStatus = 18, kCtlGetRevision = 24;
constexpr std::uint8_t kDataSourceCap = 1, kDataVdm = 15;
constexpr std::uint8_t kVdmIdentity = 1, kVdmSvids = 2, kVdmModes = 3;
constexpr std::uint16_t kPdSid = 0xFF00;

std::uint32_t le32(std::span<const std::uint8_t> p, std::size_t at) {
    return std::uint32_t(p[at] | p[at + 1] << 8 | p[at + 2] << 16 | std::uint32_t(p[at + 3]) << 24);
}
std::uint16_t sat16(std::uint32_t v) { return std::uint16_t(v > 0xFFFF ? 0xFFFF : v); }

pd::TxMsg vdm_req(std::uint16_t svid, std::uint8_t cmd, std::uint8_t rev) {
    pd::TxMsg m;
    const std::uint32_t h = pd::vdm_header(svid, cmd, rev);
    m.header = pd::make_header(kDataVdm, 1, rev);
    m.len = 4;
    for (int i = 0; i < 4; i++) m.data[std::size_t(i)] = std::uint8_t(h >> (8 * i));
    return m;
}

constexpr bool pd3_only(Q x) { return x == Q::IdUfp || x == Q::SrcCapExt || x == Q::Revision || x == Q::MfrInfo || x == Q::Status; }
constexpr bool is_swap(Q x) { return x == Q::DrSwap || x == Q::DrBack; }

}  // namespace

void Seq::start(std::uint32_t now, const PdFacts* resume) {
    f_ = resume ? *resume : PdFacts{};
    f_.dfp = 0;   // a reboot (or the Hard Reset behind it) put the roles back: the K2 is UFP
    end_ = End::None;
    q_ = 0;
    awaiting_ = cp_due_ = false;
    cp_done_ = resume != nullptr;   // the resumed run's journal is already on flash
    tries_ = 0;
    start_ = wait_from_ = now;
    ready_at_ = deadline_ = next_at_ = 0;
    if (resume) {
        // The question that was due when the journal was saved may be what took the K2 down: never asked again.
        for (QRec& r : f_.q)
            if (Ans(r.ans) == Ans::NotRun) {
                r.ans = std::uint8_t(Ans::Interrupted);
                r.skip = std::uint8_t(Skip::Rebooted);
                break;
            }
    }
    phase_ = Phase::WaitContract;
}

void Seq::stop(End why, std::uint32_t now) {
    if (!running()) return;
    if (awaiting_ || phase_ == Phase::Asking) {
        QRec& r = f_.q[q_ < kQueries ? q_ : 0];
        if (q_ < kQueries && Ans(r.ans) == Ans::NotRun && r.attempts > 0) {
            r.ans = std::uint8_t(Ans::Interrupted);
            r.skip = std::uint8_t(Skip::Ended);
            r.t_ms = ready_at_ ? sat16(now - ready_at_) : 0;
        }
    }
    awaiting_ = false;
    end_ = why;
    phase_ = Phase::Done;
}

bool Seq::askable(Q x, Skip& why) const {
    const QRec& r = f_.at(x);
    if (Ans(r.ans) != Ans::NotRun) { why = Skip::None; return false; }   // answered before a reboot / reset: keep it
    if (pd3_only(x) && f_.rev < 2) { why = Skip::Pd2; return false; }
    const bool have_id = f_.n_id > 0;
    switch (x) {
    case Q::DrSwap:
        if (have_id) { why = Skip::HaveId; return false; }
        if (f_.hard_resets) { why = Skip::HardReset; return false; }
        return true;
    case Q::IdDfp:
        if (have_id) { why = Skip::HaveId; return false; }
        if (!f_.dfp) { why = Skip::NoDfp; return false; }
        return true;
    case Q::Svids:
        if (!f_.dfp) { why = Skip::NoDfp; return false; }
        return true;
    case Q::Modes1:
    case Q::Modes2:
        if (!f_.dfp) { why = Skip::NoDfp; return false; }
        if (mode_svid(x == Q::Modes1 ? 0 : 1) == 0) { why = Skip::NoSvid; return false; }
        return true;
    case Q::DrBack:
        if (!f_.dfp || Ans(f_.at(Q::DrSwap).ans) != Ans::Ack) { why = Skip::NotSwapped; return false; }
        return true;
    default: return true;
    }
}

std::uint16_t Seq::mode_svid(unsigned idx) const {
    return pd::id_modes_svid({f_.svids.data(), f_.n_svids}, idx);
}

pd::TxMsg Seq::build(Q x) const {
    const std::uint8_t r = rev();
    pd::TxMsg m;
    switch (x) {
    case Q::IdUfp:
    case Q::IdDfp: return vdm_req(kPdSid, kVdmIdentity, r);
    case Q::Svids: return vdm_req(kPdSid, kVdmSvids, r);
    case Q::Modes1: return vdm_req(mode_svid(0), kVdmModes, r);
    case Q::Modes2: return vdm_req(mode_svid(1), kVdmModes, r);
    case Q::SrcCapExt: m.header = pd::make_header(pd::kCtlGetSourceCapExt, 0, r); break;
    case Q::Revision: m.header = pd::make_header(kCtlGetRevision, 0, r); break;
    case Q::Status: m.header = pd::make_header(kCtlGetStatus, 0, r); break;
    case Q::MfrInfo:   // chunked (extended messages always go chunked): data size 2, target 0 (port), ref 0
        m.header = pd::make_header(pd::kExtGetManufacturerInfo, 1, r, true);
        m.len = 4;
        m.data[0] = 2;
        m.data[1] = 0x80;
        break;
    case Q::DrSwap:
    case Q::DrBack: m.header = pd::make_header(kCtlDrSwap, 0, r); break;
    }
    return m;
}

void Seq::advance(std::uint32_t now) {
    awaiting_ = false;
    tries_ = 0;
    next_at_ = now;
    while (q_ < kQueries) {
        Skip why = Skip::None;
        if (askable(Q(q_), why)) return;
        QRec& r = f_.q[q_];
        if (Ans(r.ans) == Ans::NotRun) {
            r.ans = std::uint8_t(Ans::Skipped);
            r.skip = std::uint8_t(why);
        }
        q_++;
    }
    end_ = End::Done;
    phase_ = Phase::Done;
}

void Seq::complete(Ans a, std::uint32_t now, Skip why) {
    QRec& r = f_.q[q_];
    r.ans = std::uint8_t(a);
    r.skip = std::uint8_t(why);
    r.t_ms = sat16(now - ready_at_);
    if (a == Ans::Ack && Q(q_) == Q::DrSwap) { f_.dfp = 1; env_.set_dfp(true); }
    if (a == Ans::Ack && Q(q_) == Q::DrBack) { f_.dfp = 0; env_.set_dfp(false); }
    q_++;
    advance(now);
}

// Silence, BUSY or Wait: ask again after the back-off, up to kMaxAttempts, then the last outcome stands.
void Seq::retry_or(Ans a, std::uint32_t now, std::uint32_t wait_ms) {
    awaiting_ = false;
    if (tries_ < kMaxAttempts) {
        next_at_ = now + wait_ms;
        return;
    }
    complete(a, now);
}

// No GoodCRC (the chip's own retries are spent): retried like silence; when every attempt failed it is a protocol error:
// a Soft_Reset (bounded per run), and the question is asked again on the new contract.
void Seq::tx_failed(std::uint32_t now) {
    f_.tx_fails = std::uint8_t(f_.tx_fails < 0xFF ? f_.tx_fails + 1 : 0xFF);
    if (tries_ < kMaxAttempts) {
        next_at_ = now + kBackoffMs * tries_;
        return;
    }
    if (f_.soft_resets < kMaxSoftResets) {
        f_.soft_resets++;
        tries_ = 0;
        env_.soft_reset();
        phase_ = Phase::WaitContract;
        wait_from_ = now;
        return;
    }
    complete(Ans::TxFail, now);
}

void Seq::on_contract(bool ready, std::uint32_t now) {
    if (!running()) return;
    if (ready && phase_ == Phase::WaitContract) {
        phase_ = Phase::Settle;
        ready_at_ = now;
        if (!f_.ready_ms) f_.ready_ms = sat16(now - start_ > 0 ? now - start_ : 1);
    } else if (!ready && (phase_ == Phase::Settle || phase_ == Phase::Asking)) {
        // The contract went away (a Soft_Reset, a re-announce): the question in flight is asked again on the next one.
        awaiting_ = false;
        phase_ = Phase::WaitContract;
        wait_from_ = now;
    }
}

void Seq::on_hard_reset(std::uint32_t now) {
    if (!running()) return;
    f_.hard_resets = std::uint8_t(f_.hard_resets < 0xFF ? f_.hard_resets + 1 : 0xFF);
    if (f_.dfp) env_.set_dfp(false);
    f_.dfp = 0;   // a Hard Reset puts the data roles back: source DFP, sink UFP
    if (phase_ == Phase::Asking && q_ < kQueries && (awaiting_ || tries_ > 0)) {
        // What was in flight may be what the host did not like: never asked again.
        QRec& r = f_.q[q_];
        r.ans = std::uint8_t(Ans::Interrupted);
        r.skip = std::uint8_t(Skip::HardReset);
        r.t_ms = sat16(now - ready_at_);
        q_++;
    }
    awaiting_ = false;
    tries_ = 0;
    phase_ = Phase::WaitContract;
    wait_from_ = now;
}

bool Seq::matches_vdm(std::uint8_t cmd) const {
    switch (Q(q_)) {
    case Q::IdUfp:
    case Q::IdDfp: return cmd == kVdmIdentity;
    case Q::Svids: return cmd == kVdmSvids;
    case Q::Modes1:
    case Q::Modes2: return cmd == kVdmModes;
    default: return false;
    }
}

void Seq::on_message(std::uint16_t header, std::span<const std::uint8_t> p, std::uint32_t now) {
    const std::uint8_t type = header & 0x1F, ndo = (header >> 12) & 7;
    const bool ext = header & 0x8000;
    if (!ext && ndo > 0 && type == kDataSourceCap) {   // the host's offer: kept as advertised
        f_.rev = std::uint8_t((header >> 6) & 3);
        f_.n_pdos = std::uint8_t(std::min<std::size_t>({ndo, f_.pdos.size(), p.size() / 4}));
        for (std::size_t i = 0; i < f_.n_pdos; i++) f_.pdos[i] = le32(p, 4 * i);
        if (f_.caps_seen < 0xFF) f_.caps_seen++;
        return;
    }
    if (phase_ != Phase::Asking || q_ >= kQueries || tries_ == 0) return;
    const Q x = Q(q_);
    if (!ext && ndo == 0) {
        if (!awaiting_) return;   // a refusal only answers the message in flight
        if (type == kCtlNotSupported) return complete(Ans::NotSupported, now);
        if (type == kCtlReject) return complete(Ans::Reject, now);
        if (type == kCtlWait && is_swap(x)) return retry_or(Ans::Wait, now, kWaitRetryMs);
        if (type == kCtlAccept && is_swap(x)) return complete(Ans::Ack, now);
        return;
    }
    if (!ext && type == kDataVdm && p.size() >= 4) {
        const std::uint32_t vh = le32(p, 0);
        if (!(vh & 0x8000u)) return;                       // unstructured: not ours
        const std::uint8_t cmd = vh & 0x1F, kind = (vh >> 6) & 3;
        if (kind == 0 || !matches_vdm(cmd)) return;       // a REQ of the host's own, or another command
        if (kind == 2) return complete(Ans::Nak, now);
        if (kind == 3) return retry_or(Ans::Busy, now, kBackoffMs * tries_);
        const std::size_t n = std::min<std::size_t>(ndo, p.size() / 4) - 1;   // VDOs after the header
        if (x == Q::IdUfp || x == Q::IdDfp) {
            f_.n_id = std::uint8_t(std::min(n, f_.id.size()));
            for (std::size_t i = 0; i < f_.n_id; i++) f_.id[i] = le32(p, 4 * (i + 1));
        } else if (x == Q::Svids) {
            f_.n_svids = 0;
            for (std::size_t i = 0; i < n; i++) {
                const std::uint32_t v = le32(p, 4 * (i + 1));
                for (const std::uint16_t s : {std::uint16_t(v >> 16), std::uint16_t(v)})
                    if (s && f_.n_svids < f_.svids.size()) f_.svids[f_.n_svids++] = s;
            }
        } else {
            const std::size_t k = x == Q::Modes1 ? 0 : 1;
            f_.mode_svid[k] = std::uint16_t(vh >> 16);
            f_.mode_n[k] = std::uint8_t(n);
            f_.mode_vdo[k] = n ? le32(p, 4) : 0;
        }
        return complete(Ans::Ack, now);
    }
    if (!ext && type == pd::kDataRevision && x == Q::Revision && p.size() >= 4) {
        f_.revision = le32(p, 0);
        return complete(Ans::Ack, now);
    }
    if (ext && p.size() >= 2) {
        const std::size_t size = std::size_t(p[0] | (p[1] & 1) << 8);
        const std::span<const std::uint8_t> d = p.subspan(2, std::min(size, p.size() - 2));
        const auto keep = [&](auto& arr, std::uint8_t& n) {
            n = std::uint8_t(std::min(d.size(), arr.size()));
            std::copy_n(d.begin(), n, arr.begin());
            complete(Ans::Ack, now);
        };
        if (type == pd::kExtSourceCapsExt && x == Q::SrcCapExt) return keep(f_.scx, f_.n_scx);
        if (type == pd::kExtManufacturerInfo && x == Q::MfrInfo) return keep(f_.mfr, f_.n_mfr);
        if (type == pd::kExtStatus && x == Q::Status) return keep(f_.status, f_.n_status);
    }
}

void Seq::tick(std::uint32_t now, bool idle, bool usb_settled) {
    switch (phase_) {
    case Phase::Idle:
    case Phase::Done: return;
    case Phase::WaitContract:
        if (now - wait_from_ >= kContractWaitMs) stop(End::NoContract, now);
        return;
    case Phase::Settle:
        if (now - ready_at_ < kSettleMs) return;
        phase_ = Phase::Asking;
        if (tries_ == 0) advance(now);   // skips what cannot be asked (PD 2.0, a resumed run's answers, ...)
        next_at_ = now;
        if (phase_ != Phase::Asking) return;
        break;
    case Phase::Asking: break;
    }
    if (awaiting_) {
        if (std::int32_t(now - deadline_) >= 0) retry_or(Ans::Timeout, now, kBackoffMs * tries_);
        return;
    }
    if (phase_ != Phase::Asking || std::int32_t(now - next_at_) < 0 || !idle) return;
    const Q x = Q(q_);
    if (x == Q::DrSwap) {
        if (!usb_settled && now - ready_at_ < kUsbWaitMs) return;   // let the host finish enumerating first
        if (!cp_done_) {   // the journal goes to flash before the risky swap; the swap itself on the next tick
            cp_done_ = cp_due_ = true;
            return;
        }
    }
    const pd::TxMsg m = build(x);
    tries_++;
    QRec& r = f_.q[q_];
    if (r.attempts < 0xFF) r.attempts++;
    if (!env_.send(m)) return tx_failed(now);
    awaiting_ = true;
    deadline_ = now + kRespMs;
}

bool Seq::take_checkpoint() {
    const bool d = cp_due_;
    cp_due_ = false;
    return d;
}

const char* q_name(Q x) {
    switch (x) {
    case Q::IdUfp: return "id_ufp";
    case Q::SrcCapExt: return "src_cap_ext";
    case Q::Revision: return "revision";
    case Q::MfrInfo: return "mfr_info";
    case Q::Status: return "status";
    case Q::DrSwap: return "dr_swap";
    case Q::IdDfp: return "id_dfp";
    case Q::Svids: return "svids";
    case Q::Modes1: return "modes1";
    case Q::Modes2: return "modes2";
    case Q::DrBack: return "dr_swap_back";
    }
    return "?";
}

const char* ans_name(Ans a) {
    static constexpr const char* k[] = {"not run", "ack", "nak", "busy", "not supported", "reject", "wait", "timeout",
                                        "no goodcrc", "skipped", "interrupted"};
    return std::size_t(a) < std::size(k) ? k[std::size_t(a)] : "?";
}

const char* end_name(End e) {
    static constexpr const char* k[] = {"none", "done", "no contract", "cancelled", "foreign sink", "detached", "rebooted",
                                        "rebooted 3x", "VBUS high", "no loop"};
    return std::size_t(e) < std::size(k) ? k[std::size_t(e)] : "?";
}


// ---- the start refusal ------------------------------------------------------------------------------------------------------

Refuse refusal(const LoopFacts& f) {
    if (!f.usb_core) return Refuse::UsbOff;
    if (f.busy) return Refuse::Busy;
    if (f.own_rd) return Refuse::OwnRd;
    if (f.contract) return Refuse::Contract;
    if (f.far_request) return Refuse::FarSink;
    if (f.vbus_mv > kMaxVbusMv) return Refuse::VbusHigh;
    if (f.ibus_ma > kLoadMaxMa || f.ibus_ma < -kLoadMaxMa) return Refuse::Load;
    if (f.vbus_mv < kMinVbusMv) return Refuse::NoVbus;
    if (f.active_cc == 0) return Refuse::NoCc;
    if (!f.hid_host) return Refuse::NoHost;
    return Refuse::None;
}

const char* refuse_name(Refuse r) {
    static constexpr const char* k[] = {"none", "USB link off", "no host on HID", "no CC", "no 5V", "VBUS above 5.5V",
                                        "load on output", "another sink", "own Rd", "PD tool busy", "contract held"};
    return std::size_t(r) < std::size(k) ? k[std::size_t(r)] : "?";
}

// ---- the record -------------------------------------------------------------------------------------------------------------

namespace {

struct Put {
    std::span<std::uint8_t> out;
    std::size_t n = 0;
    bool ok = true;
    void b(std::uint8_t v) { if (n < out.size()) out[n] = v; else ok = false; n++; }
    void u8(std::uint8_t v) { b(v); }
    void u16(std::uint16_t v) { b(std::uint8_t(v)); b(std::uint8_t(v >> 8)); }
    void i16(std::int16_t v) { u16(std::uint16_t(v)); }
    void u32(std::uint32_t v) { u16(std::uint16_t(v)); u16(std::uint16_t(v >> 16)); }
    void chr(char c) { b(std::uint8_t(c)); }
};
struct Get {
    std::span<const std::uint8_t> in;
    std::size_t n = 0;
    std::uint8_t b() { return n < in.size() ? in[n++] : (n++, 0); }
    void u8(std::uint8_t& v) { v = b(); }
    void u16(std::uint16_t& v) { const std::uint8_t lo = b(); v = std::uint16_t(lo | b() << 8); }
    void i16(std::int16_t& v) { std::uint16_t u; u16(u); v = std::int16_t(u); }
    void u32(std::uint32_t& v) { std::uint16_t lo, hi; u16(lo); u16(hi); v = lo | std::uint32_t(hi) << 16; }
    void chr(char& c) { c = char(b()); }
};
// One walk for both directions: Put over a const record, Get over the one it fills (no copy: the record is ~600 bytes).
template <class IO, class R> void walk(IO& io, R& r) {
    io.u8(r.state); io.u8(r.boots); io.u8(r.end); io.u8(r.refuse); io.u8(r.os);
    for (auto& c : r.fw) io.chr(c);
    io.u32(r.start_ms); io.u8(r.cc); io.u8(r.rp); io.i16(r.vbus_mv); io.i16(r.dp_mv); io.i16(r.dm_mv);
    auto& p = r.pd;
    io.u8(p.rev); io.u8(p.n_pdos);
    for (auto& x : p.pdos) io.u32(x);
    io.u8(p.caps_seen); io.u16(p.ready_ms);
    for (auto& q : p.q) { io.u8(q.ans); io.u8(q.attempts); io.u8(q.skip); io.u16(q.t_ms); }
    io.u8(p.n_id);
    for (auto& x : p.id) io.u32(x);
    io.u8(p.n_svids);
    for (auto& x : p.svids) io.u16(x);
    for (auto& x : p.mode_svid) io.u16(x);
    for (auto& x : p.mode_n) io.u8(x);
    for (auto& x : p.mode_vdo) io.u32(x);
    io.u8(p.n_scx);
    for (auto& x : p.scx) io.u8(x);
    io.u32(p.revision); io.u8(p.n_mfr);
    for (auto& x : p.mfr) io.u8(x);
    io.u8(p.n_status);
    for (auto& x : p.status) io.u8(x);
    io.u8(p.dfp); io.u8(p.hard_resets); io.u8(p.soft_resets); io.u8(p.tx_fails);
    auto& u = r.usb;
    io.u8(u.n); io.u16(u.total); io.u8(u.flags);
    for (auto& e : u.ev) { io.u16(e.t_ms); io.u8(e.kind); io.u8(e.req_type); io.u8(e.req); io.u16(e.value); io.u16(e.index); io.u16(e.length); }
}

constexpr std::uint8_t kMagic[3] = {'L', 'P', 'H'};

}  // namespace

std::size_t encode(const Report& r, std::span<std::uint8_t> out) {
    if (out.size() < kReportBytes) return 0;
    Put io{out};
    io.u8(kVersion);
    walk(io, r);
    return io.ok && io.n == kReportBytes ? io.n : 0;
}

bool decode(std::span<const std::uint8_t> in, Report& r) {
    if (in.size() != kReportBytes || in[0] != kVersion) return false;
    Get io{in};
    io.b();
    r = {};
    walk(io, r);
    r.fw.back() = 0;
    return io.n == kReportBytes;
}

std::optional<Report> decode(std::span<const std::uint8_t> in) {
    Report r;
    if (!decode(in, r)) return std::nullopt;
    return r;
}

std::size_t encode_file(const Report& r, std::span<std::uint8_t> out) {
    if (out.size() < kFileBytes || encode(r, out.subspan(kFileHeader)) != kReportBytes) return 0;
    out[0] = kMagic[0]; out[1] = kMagic[1]; out[2] = kMagic[2]; out[3] = kVersion;
    out[4] = std::uint8_t(kReportBytes); out[5] = std::uint8_t(kReportBytes >> 8);
    const std::uint16_t crc = bbstore::crc16(0xFFFF, out.subspan(kFileHeader, kReportBytes));
    out[6] = std::uint8_t(crc); out[7] = std::uint8_t(crc >> 8);
    return kFileBytes;
}

bool decode_file(std::span<const std::uint8_t> in, Report& r) {
    if (in.size() != kFileBytes || in[0] != kMagic[0] || in[1] != kMagic[1] || in[2] != kMagic[2] || in[3] != kVersion)
        return false;
    if (std::size_t(in[4] | in[5] << 8) != kReportBytes) return false;
    const auto rec = in.subspan(kFileHeader, kReportBytes);
    if (bbstore::crc16(0xFFFF, rec) != std::uint16_t(in[6] | in[7] << 8)) return false;
    return decode(rec, r);
}

std::optional<Report> decode_file(std::span<const std::uint8_t> in) {
    Report r;
    if (!decode_file(in, r)) return std::nullopt;
    return r;
}

namespace {
// Appends into a fixed buffer, always NUL-terminated, cut at its end.
struct Text {
    std::span<char> out;
    std::size_t n = 0;
    Text& s(const char* t) {
        while (t && *t && n + 1 < out.size()) out[n++] = *t++;
        if (!out.empty()) out[n] = 0;
        return *this;
    }
    Text& u(unsigned v) {
        char d[10];
        int k = 0;
        do d[k++] = char('0' + v % 10); while ((v /= 10) && k < 10);
        while (k && n + 1 < out.size()) out[n++] = d[--k];
        if (!out.empty()) out[n] = 0;
        return *this;
    }
    Text& h4(std::uint16_t v) {
        static constexpr char kHex[] = "0123456789ABCDEF";
        for (int i = 12; i >= 0 && n + 1 < out.size(); i -= 4) out[n++] = kHex[(v >> i) & 15];
        if (!out.empty()) out[n] = 0;
        return *this;
    }
};
}  // namespace

const char* row_key(std::size_t row) {
    static constexpr const char* k[kRows] = {"Host", "PD", "Ident", "DR swap", "USB"};
    return row < kRows ? k[row] : "";
}

void row_text(const Report& r, std::size_t row, std::span<char> out) {
    if (out.empty()) return;
    out[0] = 0;
    Text t{out.first(std::min(out.size(), kRowChars + 1))};
    const PdFacts& p = r.pd;
    switch (row) {
    case 0:
        switch (JState(r.state)) {
        case JState::None: t.s("--"); break;
        case JState::Armed: t.s("armed: replug"); break;
        case JState::Running: t.s("running, boot ").u(r.boots); break;
        case JState::Done: t.s(End(r.end) == End::NoLoop ? refuse_name(Refuse(r.refuse)) : end_name(End(r.end))); break;
        }
        break;
    case 1:
        if (!p.caps_seen) { t.s("no caps"); break; }
        t.s(p.rev >= 2 ? "PD3 " : "PD2 ").u(p.n_pdos).s(" PDOs Rp");
        t.s(r.rp == 3 ? "3A" : r.rp == 2 ? "1.5A" : "-");
        break;
    case 2: {
        if (p.n_id) { t.s("VID ").h4(std::uint16_t(p.id[0])); break; }
        const Ans d = Ans(p.at(Q::IdDfp).ans), u = Ans(p.at(Q::IdUfp).ans);
        t.s(ans_name(d != Ans::NotRun && d != Ans::Skipped ? d : u));
        break;
    }
    case 3: t.s(ans_name(Ans(p.at(Q::DrSwap).ans))); break;
    case 4: t.s(usbfp::os_name(usbfp::Os(r.os))).s(" ").u(r.usb.total).s("ev"); break;
    default: break;
    }
}

Boot boot_decide(Report& r) {
    switch (JState(r.state)) {
    case JState::Armed:
        r.state = std::uint8_t(JState::Running);
        r.boots = 1;
        r.end = std::uint8_t(End::None);
        return Boot::Start;
    case JState::Running:
        if (r.boots >= kMaxBoots) {
            r.state = std::uint8_t(JState::Done);
            r.end = std::uint8_t(End::TooManyBoots);
            return Boot::Closed;
        }
        r.boots++;
        return Boot::Resume;
    default: return Boot::Nothing;
    }
}

}  // namespace k2::loopprobe::host
