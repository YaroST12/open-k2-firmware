// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// HTTP/JSON API of the USB network link, as pure code over buffers (no lwIP, no hardware).
//
// The TCP server (src/net/http_server.cpp) hands the request head to parse_request_line() and then
// to route(), which answers from a remote::Backend: the same duck-typed seam as the HID remote,
// so the host tests drive it with the fake backend. Anything that moves VBUS goes through
// remote::handle_request() and therefore through the same confirm rules as the HID path; on top
// of that every such endpoint needs an explicit confirm=1 query parameter.
//
// Cross-site protection: every POST must carry an "X-K2: 1" header. A web page on another origin
// cannot add a custom header without a CORS preflight, which the K2 never answers.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

#include "k2/core/remote_proto.hpp"

namespace k2::http {

enum class Method : std::uint8_t { Get, Post, Other };

struct RequestLine {
    bool ok = false;
    Method method = Method::Other;
    std::string_view path{};
    std::string_view query{};   // without the '?'
    bool x_k2 = false;          // an "X-K2:" header is present
};

namespace detail {
// string_view::substr throws on a bad position, which would drag the exception machinery into
// the image (-fno-exceptions); this one clamps instead.
constexpr std::string_view sub(std::string_view s, std::size_t pos, std::size_t n = std::string_view::npos) {
    if (pos > s.size()) pos = s.size();
    if (n > s.size() - pos) n = s.size() - pos;
    return std::string_view(s.data() + pos, n);
}
constexpr char lower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c; }
constexpr bool starts_with_nocase(std::string_view s, std::string_view prefix) {
    if (s.size() < prefix.size()) return false;
    for (std::size_t i = 0; i < prefix.size(); ++i)
        if (lower(s[i]) != prefix[i]) return false;   // prefix is lower case
    return true;
}
}  // namespace detail

// "METHOD SP target SP HTTP/1.x CRLF" plus the header lines that matter to us.
constexpr RequestLine parse_request_line(std::string_view head) {
    RequestLine r;
    const std::size_t eol = head.find("\r\n");
    if (eol == std::string_view::npos) return r;
    const std::string_view line = detail::sub(head, 0, eol);
    const std::size_t sp1 = line.find(' ');
    if (sp1 == std::string_view::npos) return r;
    const std::size_t sp2 = line.find(' ', sp1 + 1);
    if (sp2 == std::string_view::npos || sp2 == sp1 + 1) return r;
    if (!detail::starts_with_nocase(detail::sub(line, sp2 + 1), "http/1.")) return r;
    const std::string_view m = detail::sub(line, 0, sp1);
    r.method = m == "GET" ? Method::Get : m == "POST" ? Method::Post : Method::Other;
    const std::string_view target = detail::sub(line, sp1 + 1, sp2 - sp1 - 1);
    const std::size_t q = target.find('?');
    r.path = detail::sub(target, 0, q);
    if (q != std::string_view::npos) r.query = detail::sub(target, q + 1);
    for (std::size_t pos = eol + 2; pos < head.size();) {
        std::size_t end = head.find("\r\n", pos);
        if (end == std::string_view::npos) end = head.size();
        if (detail::starts_with_nocase(detail::sub(head, pos, end - pos), "x-k2:")) r.x_k2 = true;
        pos = end + 2;
    }
    r.ok = true;
    return r;
}

// Incremental head reader for the TCP receive callback: keeps the first line (the request line)
// and a flag for the X-K2 header, nothing else, so a connection needs ~200 bytes whatever the
// browser sends. Fed with arbitrary chunks; Done once the blank line has arrived.
struct HeadScanner {
    enum class State : std::uint8_t { Need, Done, TooLarge };
    static constexpr std::size_t kFirstMax = 192;
    static constexpr std::size_t kHeadMax = 8192;   // total head bytes accepted

    std::array<char, kFirstMax + 2> first{};   // request line including its CRLF
    std::size_t first_len = 0;
    bool first_done = false;
    bool x_k2 = false;
    State state = State::Need;

    State feed(std::span<const char> d) {
        for (const char c : d) {
            if (state != State::Need) break;
            if (++total_ > kHeadMax) { state = State::TooLarge; break; }
            if (!first_done) {
                if (first_len < kFirstMax) first[first_len++] = c;
                else if (c != '\n') { state = State::TooLarge; break; }
                if (c == '\n') { first_done = true; }
                continue;
            }
            if (c == '\n') {
                if (col_ == 0 || (col_ == 1 && pre_[0] == '\r')) { state = State::Done; break; }   // blank line
                if (col_ >= 5 && detail::starts_with_nocase(std::string_view(pre_, 5), "x-k2:")) x_k2 = true;
                col_ = 0;
                continue;
            }
            if (col_ < sizeof pre_) pre_[col_] = c;
            ++col_;
        }
        return state;
    }

    RequestLine result() const {
        RequestLine r = parse_request_line(std::string_view(first.data(), first_len));
        r.x_k2 = x_k2;
        return r;
    }

private:
    std::size_t total_ = 0;
    std::size_t col_ = 0;
    char pre_[5]{};
};

// Value of `key` in "a=1&b=2" (no percent decoding: every value we take is decimal or hex).
constexpr std::optional<std::string_view> query_get(std::string_view q, std::string_view key) {
    while (!q.empty()) {
        const std::size_t amp = q.find('&');
        const std::string_view pair = detail::sub(q, 0, amp);
        const std::size_t eq = pair.find('=');
        if (detail::sub(pair, 0, eq) == key)
            return eq == std::string_view::npos ? std::string_view{} : detail::sub(pair, eq + 1);
        if (amp == std::string_view::npos) break;
        q.remove_prefix(amp + 1);
    }
    return std::nullopt;
}

constexpr std::optional<std::uint32_t> parse_uint(std::string_view s) {
    if (s.empty() || s.size() > 10) return std::nullopt;
    std::uint64_t v = 0;
    for (const char c : s) {
        if (c < '0' || c > '9') return std::nullopt;
        v = v * 10 + static_cast<unsigned>(c - '0');
    }
    if (v > 0xFFFFFFFFull) return std::nullopt;
    return static_cast<std::uint32_t>(v);
}

constexpr int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

constexpr std::optional<std::array<std::uint8_t, remote::kFrameSize>> parse_hex_frame(std::string_view s) {
    if (s.size() != remote::kFrameSize * 2) return std::nullopt;
    std::array<std::uint8_t, remote::kFrameSize> f{};
    for (std::size_t i = 0; i < f.size(); ++i) {
        const int hi = hex_digit(s[2 * i]), lo = hex_digit(s[2 * i + 1]);
        if (hi < 0 || lo < 0) return std::nullopt;
        f[i] = static_cast<std::uint8_t>(hi << 4 | lo);
    }
    return f;
}

// Streaming JSON writer into a fixed buffer; `over` is set instead of writing past the end.
struct Json {
    std::span<char> buf;
    std::size_t n = 0;
    bool over = false;
    bool comma = false;

    void put(char c) {
        if (n < buf.size()) buf[n++] = c;
        else over = true;
    }
    void raw(std::string_view s) {
        for (const char c : s) put(c);
    }
    void sep() {
        if (comma) put(',');
        comma = false;
    }
    void open(char c) { sep(); put(c); }
    void close(char c) { put(c); comma = true; }
    void key(std::string_view k) {
        sep();
        put('"');
        raw(k);
        put('"');
        put(':');
    }
    void num(std::int64_t v) {
        sep();
        char t[21];
        int i = 0;
        std::uint64_t u = v < 0 ? 0 - static_cast<std::uint64_t>(v) : static_cast<std::uint64_t>(v);
        do { t[i++] = static_cast<char>('0' + u % 10); u /= 10; } while (u);
        if (v < 0) put('-');
        while (i) put(t[--i]);
        comma = true;
    }
    void boolean(bool b) { sep(); raw(b ? "true" : "false"); comma = true; }
    // Printable ASCII only; quotes and backslashes are escaped, anything else becomes '?'.
    void str(std::string_view s) {
        sep();
        put('"');
        for (const char c : s) {
            if (c == '"' || c == '\\') { put('\\'); put(c); }
            else put(c >= 0x20 && c < 0x7F ? c : '?');
        }
        put('"');
        comma = true;
    }
    void hex(std::span<const std::uint8_t> d) {
        sep();
        put('"');
        for (const std::uint8_t b : d) { put("0123456789abcdef"[b >> 4]); put("0123456789abcdef"[b & 15]); }
        put('"');
        comma = true;
    }
    void field(std::string_view k, std::int64_t v) { key(k); num(v); }
};

enum class Body : std::uint8_t { Json, Index, Bmp };

struct Reply {
    std::uint16_t status = 500;
    Body body = Body::Json;
    std::size_t len = 0;   // Json: bytes written to the output buffer
};

constexpr std::string_view reason(unsigned status) {
    switch (status) {
        case 200: return "OK";
        case 400: return "Bad Request";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 408: return "Request Timeout";
        case 409: return "Conflict";
        case 431: return "Request Header Fields Too Large";
        case 503: return "Service Unavailable";
        default: return "Internal Server Error";
    }
}

// Status line and headers; 0 when `out` is too small. `content_length` is the body size.
inline std::size_t build_header(std::span<char> out, const Reply& r, std::size_t content_length, bool gzip = false) {
    Json j{out};
    j.raw("HTTP/1.1 ");
    j.num(r.status);
    j.comma = false;
    j.put(' ');
    j.raw(reason(r.status));
    j.raw("\r\nContent-Type: ");
    j.raw(r.body == Body::Json ? "application/json" : r.body == Body::Index ? "text/html; charset=utf-8" : "image/bmp");
    j.raw("\r\nContent-Length: ");
    j.num(static_cast<std::int64_t>(content_length));
    j.comma = false;
    if (gzip) j.raw("\r\nContent-Encoding: gzip");
    j.raw("\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n");
    return j.over ? 0 : j.n;
}

// 16 bits per pixel BMP (BI_BITFIELDS, RGB565), top-down: the strips go out in screen order.
constexpr std::size_t kBmpHeaderSize = 14 + 40 + 12;

constexpr std::uint32_t bmp_size(unsigned w, unsigned h) { return static_cast<std::uint32_t>(kBmpHeaderSize + w * h * 2); }

inline void bmp_header(std::span<std::uint8_t, kBmpHeaderSize> h, unsigned w, unsigned rows) {
    for (auto& b : h) b = 0;
    auto w16 = [&](std::size_t o, std::uint16_t v) { h[o] = v & 0xFF; h[o + 1] = v >> 8; };
    auto w32 = [&](std::size_t o, std::uint32_t v) { w16(o, v & 0xFFFF); w16(o + 2, static_cast<std::uint16_t>(v >> 16)); };
    h[0] = 'B'; h[1] = 'M';
    w32(2, bmp_size(w, rows));
    w32(10, kBmpHeaderSize);
    w32(14, 40);
    w32(18, w);
    w32(22, 0u - rows);   // negative height: top-down
    w16(26, 1);
    w16(28, 16);
    w32(30, 3);           // BI_BITFIELDS
    w32(34, w * rows * 2);
    w32(54, 0xF800);
    w32(58, 0x07E0);
    w32(62, 0x001F);
}

namespace detail {

using remote::Cmd;
using remote::Status;
constexpr std::size_t kHexOut = 128;

template <class B>
struct Call {
    Status status = Status::BadLength;
    bool handled = false;
    std::uint8_t n = 0;
    std::array<std::uint8_t, remote::kMaxRspPayload> data{};
};

// Builds a request frame and runs it through the shared dispatcher.
template <remote::Backend B>
Call<B> call(B& b, std::uint8_t cmd, std::span<const std::uint8_t> payload) {
    std::array<std::uint8_t, remote::kFrameSize> req{}, rsp{};
    req[0] = remote::kReqMagic0; req[1] = remote::kReqMagic1; req[2] = cmd; req[3] = 0;
    req[4] = static_cast<std::uint8_t>(payload.size());
    for (std::size_t i = 0; i < payload.size() && remote::kReqHeader + i < req.size(); ++i) req[remote::kReqHeader + i] = payload[i];
    Call<B> c;
    c.handled = remote::handle_request(req, b, rsp);
    if (!c.handled) return c;
    c.status = static_cast<Status>(rsp[4]);
    c.n = rsp[5] > c.data.size() ? static_cast<std::uint8_t>(c.data.size()) : rsp[5];
    for (std::size_t i = 0; i < c.n; ++i) c.data[i] = rsp[remote::kRspHeader + i];
    return c;
}

inline Reply finish(const Json& j, unsigned status) {
    if (j.over) return {500, Body::Json, 0};
    return {static_cast<std::uint16_t>(status), Body::Json, j.n};
}

inline Reply error(std::span<char> out, unsigned status, std::string_view msg) {
    Json j{out};
    j.open('{'); j.key("error"); j.str(msg); j.close('}');
    return finish(j, status);
}

inline unsigned http_status(Status s) {
    switch (s) {
        case Status::Ok: return 200;
        case Status::UnknownCmd: return 404;
        case Status::BadLength:
        case Status::BadArg: return 400;
        case Status::Busy: return 409;
        case Status::NeedConfirm: return 403;
    }
    return 500;
}

inline bool confirmed(const RequestLine& rl) {
    const auto v = query_get(rl.query, "confirm");
    return v && *v == "1";
}

// Result of a command that has no data of its own: {"ok":true} or the error.
template <class B>
Reply plain(std::span<char> out, const Call<B>& c) {
    if (c.status != Status::Ok) return error(out, http_status(c.status), c.status == Status::NeedConfirm ? "confirm required" : "refused");
    Json j{out};
    j.open('{'); j.key("ok"); j.boolean(true); j.close('}');
    return finish(j, 200);
}

template <remote::Backend B>
Reply do_state(B& b, std::span<char> out) {
    const remote::StateInfo s = b.state();
    Json j{out};
    j.open('{');
    j.field("page", s.page); j.field("active_cc", s.active_cc); j.field("spec_rev", s.spec_rev);
    j.field("num_pdos", s.num_pdos); j.field("pd_valid", s.pd_valid); j.field("chip", s.chip);
    j.field("pm_online", s.pm_online); j.field("log_on", s.log_on);
    j.field("vbus_mv", s.vbus_mv); j.field("current_ma", s.current_ma);
    j.field("power_mw", s.power_mw); j.field("energy_mwh", s.energy_mwh);
    j.field("dp_mv", s.dp_mv); j.field("dm_mv", s.dm_mv); j.field("cc1_mv", s.cc1_mv);
    j.field("cc2_mv", s.cc2_mv); j.field("aux_mv", s.aux_mv);
    j.field("temp_deci_c", s.temp_deci_c); j.field("mcu_temp_deci_c", s.mcu_temp_deci_c);
    j.field("fc", s.fc); j.field("adc_flags", s.adc_flags); j.field("link_flags", s.link_flags);
    j.field("age_s", s.age_s); j.field("cable_age_s", s.cable_age_s);
    j.close('}');
    return finish(j, 200);
}

template <remote::Backend B>
Reply do_ping(B& b, std::span<char> out) {
    const remote::PingInfo p = b.ping();
    Json j{out};
    j.open('{');
    j.field("proto", remote::kProtoVersion); j.field("uptime_ms", p.uptime_ms);
    j.key("git"); j.str(p.git); j.key("version"); j.str(p.version);
    j.close('}');
    return finish(j, 200);
}

template <remote::Backend B>
Reply do_pdos(B& b, std::span<char> out) {
    const remote::PdoSnapshot s = b.pdos();
    Json j{out};
    j.open('{');
    j.field("valid", s.valid); j.field("spec_rev", s.spec_rev); j.field("num_pdos", s.num_pdos);
    j.key("pdos"); j.open('[');
    for (std::size_t i = 0; i < s.num_pdos && i < s.pdo.size(); ++i) {
        const auto& p = s.pdo[i];
        j.open('{');
        j.field("type", p.type); j.field("raw", p.raw); j.field("min_mv", p.min_mv);
        j.field("max_mv", p.max_mv); j.field("max_ma", p.max_ma); j.field("max_mw", p.max_mw);
        j.close('}');
    }
    j.close(']');
    j.close('}');
    return finish(j, 200);
}

template <remote::Backend B>
Reply do_cables(const RequestLine& rl, B& b, std::span<char> out) {
    if (rl.method == Method::Post) {   // reset / mode change of the passive cable meter
        const auto op = query_get(rl.query, "op");
        std::uint8_t id = 0xFF;
        if (op && *op == "reset") id = remote::kCableReset;
        else if (op && *op == "passive") id = remote::kCablePassive;
        else if (op && *op == "kelvin") id = remote::kCableKelvin;
        if (id == 0xFF) return error(out, 400, "op: reset|passive|kelvin");
        const std::uint8_t p[1] = {id};
        const auto c = call(b, static_cast<std::uint8_t>(Cmd::CableR), p);
        if (c.status != Status::Ok) return plain(out, c);
    }
    const remote::CableRInfo r = b.cable_r_info();
    const remote::EmarkerSnapshot e = b.emarker();
    Json j{out};
    j.open('{');
    j.key("cable_r"); j.open('{');
    j.field("flags", r.flags); j.field("grade", r.grade); j.field("r_mohm", r.r_mohm);
    j.field("idle_mv", r.idle_mv); j.field("idle_ma", r.idle_ma); j.field("load_mv", r.load_mv);
    j.field("load_ma", r.load_ma); j.field("pos_mohm", r.pos_mohm); j.field("gnd_mohm", r.gnd_mohm);
    j.close('}');
    j.key("emarker"); j.open('{');
    j.field("valid", e.valid); j.field("is_active", e.is_active); j.field("vid", e.vid); j.field("pid", e.pid);
    j.field("max_a", e.max_a); j.field("max_v", e.max_v); j.field("max_w", e.max_w);
    j.key("vendor"); j.str(e.vendor); j.key("speed"); j.str(e.speed);
    j.close('}');
    j.close('}');
    return finish(j, 200);
}

template <remote::Backend B>
Reply do_fc_json(B& b, std::span<char> out, bool with_ok) {
    const remote::FcInfo i = b.fc_info();
    Json j{out};
    j.open('{');
    if (with_ok) { j.key("ok"); j.boolean(true); }
    j.field("state", i.state); j.field("proto", i.proto); j.field("fail", i.fail);
    j.field("scan_flags", i.scan_flags); j.field("scan_step", i.scan_step);
    j.field("target_mv", i.target_mv); j.field("vbus_mv", i.vbus_mv);
    j.key("results"); j.open('[');
    for (std::size_t k = 0; k < i.num_results && k < i.results.size(); ++k) {
        j.open('{'); j.field("proto", i.results[k].proto); j.field("outcome", i.results[k].outcome);
        j.field("mv", i.results[k].mv); j.close('}');
    }
    j.close(']');
    j.close('}');
    return finish(j, 200);
}

inline void put16(std::uint8_t* p, std::uint32_t v) { p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; }

}  // namespace detail

// Runs one parsed request. JSON bodies are written to `out`; Index and Bmp are streamed by the
// caller. A buffer that is too small gives a 500, never a truncated body.
template <remote::Backend B>
Reply route(const RequestLine& rl, B& b, std::span<char> out) {
    using namespace detail;
    if (!rl.ok) return error(out, 400, "bad request");
    if (rl.method == Method::Other) return error(out, 405, "method not allowed");
    const bool post = rl.method == Method::Post;
    const std::string_view path = rl.path;

    if (post && !rl.x_k2) return error(out, 403, "X-K2 header required");
    auto arg = [&](std::string_view k) { return query_get(rl.query, k).and_then(parse_uint); };

    if (path == "/" || path == "/index.html") {
        if (post) return error(out, 405, "GET only");
        return {200, Body::Index, 0};
    }
    if (path == "/api/screenshot.bmp") {
        if (post) return error(out, 405, "GET only");
        if (!b.screenshot_enabled()) return error(out, 503, "screenshot disabled in Settings");
        return {200, Body::Bmp, 0};
    }
    if (path == "/api/state" || path == "/api/pdos" || path == "/api/ping") {
        if (post) return error(out, 405, "GET only");
        return path == "/api/state" ? do_state(b, out) : path == "/api/pdos" ? do_pdos(b, out) : do_ping(b, out);
    }
    if (path == "/api/cables") return do_cables(rl, b, out);

    if (path == "/api/press") {
        if (!post) return error(out, 405, "POST only");
        const auto button = arg("button");
        const auto ms = query_get(rl.query, "ms") ? arg("ms") : std::optional<std::uint32_t>(100);
        if (!button || !ms || *button > 3 || *ms > 0xFFFF) return error(out, 400, "button=0..3 [ms=30..10000]");
        std::uint8_t p[3] = {static_cast<std::uint8_t>(*button), 0, 0};
        put16(&p[1], *ms);
        return plain(out, call(b, static_cast<std::uint8_t>(Cmd::Button), p));
    }

    if (path == "/api/blackbox") {
        const auto op = arg("op");
        if (!op) return error(out, 400, "op required");
        const bool read_op = *op == remote::kBbRead || *op == remote::kBbStatus || *op == remote::kBbCapList || *op == remote::kBbCapRead;
        const bool write_op = *op == remote::kBbClear || *op == remote::kBbFreeze || *op == remote::kBbCapClear;
        if (!read_op && !write_op) return error(out, 400, "unknown op");
        if (read_op == post) return error(out, 405, read_op ? "GET only" : "POST only");
        const std::uint32_t a = arg("arg").value_or(0);
        std::array<std::uint8_t, 5> p{};
        p[0] = static_cast<std::uint8_t>(*op);
        std::size_t n = 1;
        switch (*op) {
            case remote::kBbRead:
                if (query_get(rl.query, "arg")) { for (int i = 0; i < 4; ++i) p[1 + i] = static_cast<std::uint8_t>(a >> (8 * i)); n = 5; }
                break;
            case remote::kBbFreeze: p[1] = static_cast<std::uint8_t>(a); n = 2; break;
            case remote::kBbCapRead: p[1] = static_cast<std::uint8_t>(a >> 16); put16(&p[2], a & 0xFFFF); n = 4; break;
            default: break;
        }
        const auto c = call(b, static_cast<std::uint8_t>(Cmd::BlackBox), std::span<const std::uint8_t>(p.data(), n));
        if (c.status != Status::Ok) return plain(out, c);
        Json j{out};
        j.open('{'); j.key("ok"); j.boolean(true); j.key("hex"); j.hex(std::span<const std::uint8_t>(c.data.data(), c.n)); j.close('}');
        return finish(j, 200);
    }

    // ---- VBUS-changing endpoints: explicit confirm=1, then the shared dispatcher ----
    if (path == "/api/pd/request" || path == "/api/pd/reset" || path == "/api/pd/release") {
        if (!post) return error(out, 405, "POST only");
        if (!confirmed(rl)) return error(out, 403, "confirm required: add confirm=1");
        if (path == "/api/pd/request") {
            const auto pos = arg("pos"), mv = arg("mv"), ma = arg("ma");
            if (!pos || !mv || !ma || *pos > 255 || *mv > 0xFFFF || *ma > 0xFFFF) return error(out, 400, "pos, mv, ma required");
            std::uint8_t p[6] = {static_cast<std::uint8_t>(*pos), remote::kPdFlagConfirm, 0, 0, 0, 0};
            put16(&p[2], *mv);
            put16(&p[4], *ma);
            const auto c = call(b, static_cast<std::uint8_t>(Cmd::PdRequest), p);
            if (c.status != Status::Ok) return plain(out, c);
            Json j{out};
            j.open('{'); j.key("ok"); j.boolean(true); j.field("err", c.n ? c.data[0] : 0); j.close('}');
            return finish(j, 200);
        }
        const auto c = call(b, static_cast<std::uint8_t>(path == "/api/pd/reset" ? Cmd::PdReset : Cmd::PdRelease), {});
        return plain(out, c);
    }

    if (path == "/api/fc") {
        const std::string_view op = query_get(rl.query, "op").value_or("status");
        if (op == "status") {
            if (post) return error(out, 405, "GET only");
            return do_fc_json(b, out, false);
        }
        if (!post) return error(out, 405, "POST only");
        if (!confirmed(rl)) return error(out, 403, "confirm required: add confirm=1");
        std::uint8_t flags = remote::kFcConfirm;
        const auto inl = query_get(rl.query, "inline");
        if (inl && *inl == "1") flags |= remote::kFcAllowInline;
        std::array<std::uint8_t, 5> p{};
        std::size_t n = 0;
        if (op == "reset") { p[0] = remote::kFcReset; n = 1; }
        else if (op == "scan") { p[0] = remote::kFcScan; p[1] = flags; n = 2; }
        else if (op == "trigger") {
            const auto proto = arg("proto"), mv = arg("mv");
            if (!proto || !mv || *proto > 255 || *mv > 0xFFFF) return error(out, 400, "proto, mv required");
            p[0] = remote::kFcTrigger; p[1] = static_cast<std::uint8_t>(*proto); put16(&p[2], *mv); p[4] = flags; n = 5;
        } else return error(out, 400, "op: status|reset|scan|trigger");
        const auto c = call(b, static_cast<std::uint8_t>(Cmd::Fc), std::span<const std::uint8_t>(p.data(), n));
        if (c.status != Status::Ok) return plain(out, c);
        return do_fc_json(b, out, true);
    }

    // ---- raw bridge: one 64-byte request frame in, the 64-byte response out ----
    if (path == "/api/frame") {
        if (!post) return error(out, 405, "POST only");
        const auto hex = query_get(rl.query, "hex");
        const auto frame = hex ? parse_hex_frame(*hex) : std::nullopt;
        if (!frame) return error(out, 400, "hex=<128 hex digits>");
        const auto pr = remote::parse_request(*frame);
        if (!pr.header_valid) return error(out, 400, "not a request frame");
        // Screenshot streams over the HID endpoint, the rest cannot be undone from a browser tab.
        switch (static_cast<Cmd>(pr.cmd)) {
            case Cmd::Screenshot: case Cmd::DfuEnter: case Cmd::FactoryWipe: case Cmd::DebugFault: case Cmd::Reboot:
                return error(out, 403, "command not available over the network");
            case Cmd::PdRequest: case Cmd::PdReset: case Cmd::PdRelease:
                if (!confirmed(rl)) return error(out, 403, "confirm required: add confirm=1");
                break;
            case Cmd::Fc:
                if (!pr.payload.empty() && (pr.payload[0] == remote::kFcScan || pr.payload[0] == remote::kFcTrigger || pr.payload[0] == remote::kFcReset) && !confirmed(rl))
                    return error(out, 403, "confirm required: add confirm=1");
                break;
            default: break;
        }
        std::array<std::uint8_t, remote::kFrameSize> rsp{};
        if (!remote::handle_request(*frame, b, rsp)) return error(out, 400, "not a request frame");
        Json j{out};
        j.open('{'); j.key("rsp"); j.hex(rsp); j.close('}');
        return finish(j, 200);
    }

    return error(out, 404, "not found");
}

}  // namespace k2::http
