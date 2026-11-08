// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#include "k2/pd/pdtrace_view.hpp"

#include <algorithm>
#include <string_view>
#include "k2/pd/pdtrace.hpp"
#include "k2/pd/pdtrace_link.hpp"
#include "k2/pd/pd.hpp"
#include "k2/pd/pd_vdm.hpp"

namespace k2::pdtrace::view {

namespace {

constexpr std::uint32_t rd32(const std::uint8_t* p) {
    return p[0] | p[1] << 8 | p[2] << 16 | std::uint32_t(p[3]) << 24;
}

const char* msg_name(std::uint16_t h) {
    const unsigned type = h & 0x1F, nobj = h >> 12 & 7;
    if (h >> 15 & 1) return "Ext";
    if (nobj == 0) {
        static constexpr const char* kCtl[] = {"?", "GoodCRC", "GotoMin", "Accept", "Reject", "Ping", "PS_RDY", "GetSrcCap",
                                               "GetSnkCap", "DR_Swap", "PR_Swap", "VCONN_Swap", "Wait", "SoftRst", "DataRst",
                                               "DataRstDone", "NotSupp", "GetSrcExt", "GetStatus", "FR_Swap", "GetPPSSt"};
        return type < sizeof kCtl / sizeof kCtl[0] ? kCtl[type] : "Ctl?";
    }
    switch (type) {
    case 1: return "SrcCap";
    case 2: return "Request";
    case 3: return "BIST";
    case 4: return "SnkCap";
    case 5: return "BattStat";
    case 6: return "Alert";
    case 8: return "EnterUSB";
    case 9: return "EPRReq";
    case 10: return "EPRMode";
    case 11: return "SrcInfo";
    case 12: return "Revision";
    case 15: return "VDM";
    default: return "Data?";
    }
}

// The row a record makes, if any.
bool row_of(const std::uint8_t* r, Row& out) {
    const std::uint8_t len = r[0], type = r[1];
    const std::uint8_t* p = r + kHeader;
    const char* name = nullptr;
    char dir = '!';
    switch (Type(type)) {
    case Type::Rx:
        if (len < kHeader + 3) return false;
        name = msg_name(std::uint16_t(p[1] | p[2] << 8));
        dir = '<';
        break;
    case Type::Tx:
        if (len < kHeader + 2) return false;
        name = msg_name(std::uint16_t(p[0] | p[1] << 8));
        dir = '>';
        break;
    case Type::TxResult:
        if (len < kHeader + 1 || p[0] == std::uint8_t(TxResult::TxSent)) return false;
        name = p[0] == std::uint8_t(TxResult::RetryFail) ? "TX nocrc" : "TX timeout";
        break;
    case Type::Reset:
        if (len < kHeader + 1) return false;
        if (p[0] == std::uint8_t(ResetKind::HardRx)) name = "HardRst rx";
        else if (p[0] == std::uint8_t(ResetKind::HardTx)) name = "HardRst tx";
        else return false;
        break;
    case Type::Cc:
        if (len < kHeader + 1) return false;
        name = p[0] ? "ATTACH" : "DETACH";
        break;
    default:
        return false;
    }
    if (dir != '!' && name[0] == 'G' && name[4] == 'C') return false;   // GoodCRC
    std::size_t n = 0;
    for (; name[n] && n < sizeof out.name - 1; n++) out.name[n] = name[n];
    out.name[n] = '\0';
    out.dir = dir;
    out.t_us = rd32(r + 2);
    return true;
}

template <class F>
void each_record(std::span<const std::uint8_t> rec, F f) {
    std::size_t i = 0;
    while (i < rec.size() && rec[i] >= kHeader && i + rec[i] <= rec.size()) {
        f(&rec[i]);
        i += rec[i];
    }
}

constexpr std::uint16_t rd16(const std::uint8_t* p) { return std::uint16_t(p[0] | p[1] << 8); }

void put(std::span<char> out, std::size_t& n, char c) {
    if (n < out.size()) out[n++] = c;
}
void put_num(std::span<char> out, std::size_t& n, std::uint32_t v) {
    char t[10];
    int k = 0;
    do { t[k++] = char('0' + v % 10); v /= 10; } while (v);
    while (k) put(out, n, t[--k]);
}

}  // namespace

std::size_t count_rows(std::span<const std::uint8_t> records) {
    std::size_t n = 0;
    Row r;
    each_record(records, [&](const std::uint8_t* rec) { if (row_of(rec, r)) n++; });
    return n;
}

bool row_at(std::span<const std::uint8_t> records, std::size_t index, Row& out) {
    std::size_t n = 0;
    bool found = false, have_prev = false;
    std::uint32_t prev = 0;
    Row r;
    each_record(records, [&](const std::uint8_t* rec) {
        if (found || !row_of(rec, r)) return;
        if (n++ == index) {
            out = r;
            out.dt_us = have_prev ? r.t_us - prev : 0;
            found = true;
        }
        prev = r.t_us;
        have_prev = true;
    });
    return found;
}

std::size_t format_row(const Row& r, std::span<char> out) {
    std::size_t n = 0;
    put(out, n, r.dir);
    std::size_t k = 0;
    while (r.name[k]) put(out, n, r.name[k++]);
    while (n < 12) put(out, n, ' ');
    char t[12];
    std::size_t m = 0;
    std::span<char> ts(t, sizeof t);
    if (r.dt_us) {
        put(ts, m, '+');
        if (r.dt_us < 10000) {   // 0.0 .. 9.9 ms
            put_num(ts, m, r.dt_us / 1000);
            put(ts, m, '.');
            put_num(ts, m, r.dt_us / 100 % 10);
            put(ts, m, 'm'); put(ts, m, 's');
        } else if (r.dt_us < 1000000) {
            put_num(ts, m, r.dt_us / 1000);
            put(ts, m, 'm'); put(ts, m, 's');
        } else {
            put_num(ts, m, r.dt_us / 1000000);
            put(ts, m, '.');
            put_num(ts, m, r.dt_us / 10000 % 100 / 10);
            put_num(ts, m, r.dt_us / 10000 % 10);
            put(ts, m, 's');
        }
    }
    std::size_t width = 21;
    while (n + m < width) put(out, n, ' ');
    for (std::size_t i = 0; i < m; i++) put(out, n, t[i]);
    return n;
}

namespace {
void fingerprint(Identity& id, const std::uint8_t* objs, std::size_t n) {
    std::uint32_t best_mw = 0;
    unsigned fixed = 0, pps = 0, avs = 0;
    for (std::size_t i = 0; i < n; i++) {
        const pd::Pdo p = pd::decode_pdo(rd32(objs + 4 * i));
        if (p.max_mw > best_mw) best_mw = p.max_mw;
        switch (p.type) {
        case pd::PdoType::Pps: pps++; break;
        case pd::PdoType::Avs: avs++; break;
        case pd::PdoType::Unknown: break;
        default: fixed++; break;
        }
    }
    std::size_t k = 0;
    const std::span<char> out(id.fp, sizeof id.fp - 1);
    put_num(out, k, (best_mw + 500) / 1000);
    put(out, k, 'W'); put(out, k, ' ');
    put_num(out, k, fixed);
    for (const char c : {'P', 'D', 'O'}) put(out, k, c);
    if (pps) { put(out, k, '+'); put_num(out, k, pps); for (const char c : {'P', 'P', 'S'}) put(out, k, c); }
    if (avs) { put(out, k, '+'); put_num(out, k, avs); for (const char c : {'A', 'V', 'S'}) put(out, k, c); }
    id.fp[k] = '\0';
}

// A message in an Rx (token first) or Tx record: header, objects, object count; false when it does
// not hold a whole data message.
bool message_of(const std::uint8_t* r, bool& sop, std::uint16_t& hdr, const std::uint8_t*& objs, std::size_t& n) {
    const std::size_t len = r[0], skip = Type(r[1]) == Type::Rx ? 1 : 0;
    if (Type(r[1]) != Type::Rx && Type(r[1]) != Type::Tx) return false;
    const std::uint8_t* p = r + kHeader;
    if (len < kHeader + skip + 2) return false;
    sop = skip == 0 || (p[0] & 0xE0) == 0xE0;
    hdr = rd16(p + skip);
    n = hdr >> 12 & 7;
    objs = p + skip + 2;
    return n > 0 && !(hdr >> 15 & 1) && kHeader + skip + 2 + 4 * n <= len;
}
}  // namespace

Identity identify(std::span<const std::uint8_t> records) {
    Identity id;
    bool have_fp = false;
    each_record(records, [&](const std::uint8_t* r) {
        bool sop;
        std::uint16_t hdr;
        const std::uint8_t* o;
        std::size_t n;
        if (!message_of(r, sop, hdr, o, n)) return;
        if (!sop) return;
        const unsigned type = hdr & 0x1F;
        const bool source = hdr >> 8 & 1;
        if (type == 1 && source && !have_fp) { fingerprint(id, o, n); have_fp = true; return; }
        if (type != 15 || Type(r[1]) != Type::Rx || n < 4) return;
        const std::uint32_t vdm = rd32(o);
        if ((vdm >> 16) != pd::kSvidPd || !(vdm >> 15 & 1) || (vdm >> 6 & 3) != 1 || (vdm & 0x1F) != 1) return;
        const std::uint16_t vid = std::uint16_t(rd32(o + 4) & 0xFFFF), pid = std::uint16_t(rd32(o + 12) >> 16);
        if (source && !id.has_src) { id.has_src = true; id.src_vid = vid; id.src_pid = pid; }
        else if (!source && !id.has_dev) { id.has_dev = true; id.dev_vid = vid; id.dev_pid = pid; }
    });
    return id;
}

std::uint16_t key_vid(const Identity& id) { return id.has_dev ? id.dev_vid : id.has_src ? id.src_vid : 0; }

std::size_t format_key(const Identity& id, std::span<char> out) {
    std::size_t n = 0;
    if (id.has_dev || id.has_src) {
        const std::uint16_t vid = id.has_dev ? id.dev_vid : id.src_vid, pid = id.has_dev ? id.dev_pid : id.src_pid;
        for (const std::uint16_t w : {vid, pid}) {
            for (int s = 12; s >= 0; s -= 4) put(out, n, "0123456789ABCDEF"[w >> s & 15]);
            if (w == vid) put(out, n, ':');
        }
    } else if (id.fp[0]) {
        for (const char* c = id.fp; *c; c++) put(out, n, *c);
    } else {
        put(out, n, '?');
    }
    return n;
}

bool same_key(const Identity& a, const Identity& b) {
    const bool ia = a.has_dev || a.has_src, ib = b.has_dev || b.has_src;
    if (ia != ib) return false;
    if (ia) {
        const std::uint16_t av = a.has_dev ? a.dev_vid : a.src_vid, ap = a.has_dev ? a.dev_pid : a.src_pid;
        const std::uint16_t bv = b.has_dev ? b.dev_vid : b.src_vid, bp = b.has_dev ? b.dev_pid : b.src_pid;
        return av == bv && ap == bp;
    }
    std::size_t i = 0;
    while (a.fp[i] && a.fp[i] == b.fp[i]) i++;
    return a.fp[i] == b.fp[i];
}

std::size_t format_duration(std::uint32_t us, std::span<char> out) {
    std::size_t n = 0;
    if (us < 1000000u) {
        put_num(out, n, us / 1000);
        put(out, n, 'm'); put(out, n, 's');
    } else if (us < 10000000u) {
        put_num(out, n, us / 1000000);
        put(out, n, '.');
        put_num(out, n, us / 100000 % 10);
        put(out, n, 's');
    } else if (us < 60000000u) {
        put_num(out, n, us / 1000000);
        put(out, n, 's');
    } else {
        const std::uint32_t sec = us / 1000000;
        put_num(out, n, sec / 60);
        put(out, n, 'm');
        put(out, n, char('0' + sec % 60 / 10));
        put(out, n, char('0' + sec % 10));
        put(out, n, 's');
    }
    return n;
}

namespace {
std::uint32_t span_us(std::span<const std::uint8_t> records) {
    if (records.size() < kHeader) return 0;
    const std::uint32_t first = rd32(&records[2]);
    const std::uint32_t last = time_at(records, records.size());
    return last - first;
}
}  // namespace

std::size_t format_capture(std::span<const std::uint8_t> records, std::uint32_t seq, std::span<char> out) {
    std::size_t n = 0;
    put(out, n, '#');
    put_num(out, n, seq);
    put(out, n, ' ');
    char d[12];
    const std::size_t dn = format_duration(span_us(records), d);
    for (std::size_t i = 0; i < dn; i++) put(out, n, d[i]);
    put(out, n, ' ');
    put_num(out, n, static_cast<std::uint32_t>(count_rows(records)));
    for (const char ch : std::string_view(" msg")) put(out, n, ch);
    return n;
}

std::size_t format_capture_long(std::span<const std::uint8_t> records, std::uint32_t seq, std::span<char> out) {
    std::size_t n = 0;
    put(out, n, '#');
    put_num(out, n, seq);
    put(out, n, ' '); put(out, n, ' ');
    char d[12];
    const std::size_t dn = format_duration(span_us(records), d);
    for (std::size_t i = 0; i < dn; i++) put(out, n, d[i]);
    put(out, n, ' '); put(out, n, ' ');
    put_num(out, n, static_cast<std::uint32_t>(count_rows(records)));
    for (const char c : std::string_view(" msgs")) put(out, n, c);
    return n;
}

Strip vbus_strip(std::span<const std::uint8_t> records, std::span<std::uint8_t> heights, std::uint8_t h) {
    Strip s;
    std::uint32_t first = 0;
    bool any = false, have_t = false;
    each_record(records, [&](const std::uint8_t* r) {
        const std::uint32_t ts = rd32(r + 2);
        if (!have_t) { first = ts; have_t = true; s.t0_us = ts; }
        s.t1_us = ts;
        if (Type(r[1]) != Type::Vbus || r[0] < kHeader + 2) return;
        const std::uint16_t mv = std::uint16_t(r[kHeader] | r[kHeader + 1] << 8);
        if (!any || mv < s.mv_min) s.mv_min = mv;
        if (!any || mv > s.mv_max) s.mv_max = mv;
        any = true;
    });
    for (auto& v : heights) v = 0xFF;
    if (!any || heights.empty()) return s;
    if (s.mv_max - s.mv_min < 1000) {
        const std::uint16_t mid = std::uint16_t((s.mv_min + s.mv_max) / 2);
        s.mv_min = mid > 500 ? std::uint16_t(mid - 500) : 0;
        s.mv_max = std::uint16_t(s.mv_min + 1000);
    }
    s.valid = true;
    const std::size_t w = heights.size();
    const std::uint32_t span = s.t1_us - first ? s.t1_us - first : 1;
    each_record(records, [&](const std::uint8_t* r) {
        if (Type(r[1]) != Type::Vbus || r[0] < kHeader + 2) return;
        const std::uint16_t mv = std::uint16_t(r[kHeader] | r[kHeader + 1] << 8);
        // Timestamps need not rise (restored captures, edge-stamped records): clamp to the strip.
        const std::uint32_t ts = rd32(r + 2);
        const std::uint32_t dt = std::int32_t(ts - first) < 0 ? 0 : std::min(ts - first, span);
        const std::size_t c = std::size_t(std::uint64_t(dt) * (w - 1) / span);
        heights[c] = std::uint8_t(std::uint32_t(mv - s.mv_min) * h / (s.mv_max - s.mv_min));
    });
    // hold the last known value across columns without a sample
    std::uint8_t last = 0xFF;
    for (auto& v : heights) {
        if (v == 0xFF) v = last; else last = v;
    }
    return s;
}

std::size_t column_of(const Strip& s, std::uint32_t t_us, std::size_t w) {
    if (w == 0) return 0;
    const std::uint32_t span = s.t1_us - s.t0_us;
    if (!span || t_us <= s.t0_us) return 0;
    if (t_us - s.t0_us >= span) return w - 1;
    return std::size_t(std::uint64_t(t_us - s.t0_us) * (w - 1) / span);
}

std::uint32_t time_at(std::span<const std::uint8_t> records, std::size_t bytes) {
    std::uint32_t t = 0;
    std::size_t i = 0;
    while (i < records.size() && records[i] >= kHeader && i + records[i] <= bytes && i + records[i] <= records.size()) {
        t = rd32(&records[i + 2]);
        i += records[i];
    }
    return t;
}

void json_row(std::span<const std::uint8_t> records, std::size_t index, std::span<char, kJsonLine> out) {
    for (auto& c : out) c = ' ';
    out[kJsonLine - 1] = '\n';
    std::size_t off = 0, n = 0;
    while (off + 6 <= records.size()) {
        const std::size_t len = records[off];
        if (len < 6 || off + len > records.size()) return;
        if (n == index) {
            char* p = out.data();
            char* const end = p + kJsonLine - 1;
            auto put = [&](const char* s) { while (*s && p < end) *p++ = *s++; };
            auto num = [&](std::uint32_t v) {
                char t[10];
                int k = 0;
                do { t[k++] = static_cast<char>('0' + v % 10); v /= 10; } while (v);
                while (k && p < end) *p++ = t[--k];
            };
            put("{\"t\":");
            num(rd32(&records[off + 2]));
            put(",\"y\":");
            num(records[off + 1]);
            put(",\"d\":\"");
            for (std::size_t i = 6; i < len && p + 2 < end; i++) {
                *p++ = "0123456789abcdef"[records[off + i] >> 4];
                *p++ = "0123456789abcdef"[records[off + i] & 15];
            }
            put("\"}");
            return;
        }
        off += len;
        n++;
    }
}

void json_meta(std::span<const std::uint8_t> records, std::span<char, kJsonLine> out) {
    for (auto& c : out) c = ' ';
    out[kJsonLine - 1] = '\n';
    const Identity id = identify(records);
    char* p = out.data();
    char* const end = p + kJsonLine - 1;
    auto put = [&](const char* s) { while (*s && p < end) *p++ = *s++; };
    auto hex4 = [&](std::uint16_t w) { for (int s = 12; s >= 0 && p < end; s -= 4) *p++ = "0123456789ABCDEF"[w >> s & 15]; };
    auto vp = [&](const char* name, std::uint16_t vid, std::uint16_t pid) {
        put(",\""); put(name); put("\":\"");
        hex4(vid); put(":"); hex4(pid); put("\"");
    };
    put("{\"meta\":{\"key\":\"");
    char k[28];
    const std::size_t kn = format_key(id, std::span<char>(k, sizeof k - 1));
    k[kn] = 0;
    put(k); put("\"");
    if (id.has_dev) vp("dev", id.dev_vid, id.dev_pid);
    if (id.has_src) vp("src", id.src_vid, id.src_pid);
    if (id.fp[0]) { put(",\"fp\":\""); put(id.fp); put("\""); }
    put("}}");
}

void json_line(std::span<const std::uint8_t> records, std::size_t index, std::span<char, kJsonLine> out) {
    if (index == 0) json_meta(records, out);
    else if (index == 1) json_link(link_of(records), out);
    else json_row(records, index - 2, out);
}

}  // namespace k2::pdtrace::view
