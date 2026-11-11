// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Host tests for the HTTP/JSON API of the USB network link (k2/net/http_api.hpp): request-line
// parsing, JSON encoders, confirm gating and the 64-byte frame bridge, all against the fake backend.
#include "test_util.h"
#include "fake_remote_backend.hpp"
#include "k2/net/http_api.hpp"

#include <string>

using namespace k2;
using http::Body;
using http::Method;

static std::array<char, 2048> g_out;

struct Res {
    http::Reply r;
    std::string body;
};

// Runs one request through the router; `head` is the raw request head as lwIP would deliver it.
static Res run(Fake &f, const char *head) {
    const http::RequestLine rl = http::parse_request_line(head);
    Res res{};
    res.r = http::route(rl, f, std::span<char>(g_out));
    if (res.r.body == Body::Json) res.body.assign(g_out.data(), res.r.len);
    return res;
}

static bool has(const Res &r, const char *needle) { return r.body.find(needle) != std::string::npos; }

#define X "\r\nX-K2: 1\r\n\r\n"

static void test_request_line() {
    auto rl = http::parse_request_line("GET /api/state HTTP/1.1\r\nHost: k2\r\n\r\n");
    CHECK_EQ(rl.ok, 1); CHECK_EQ(rl.method == Method::Get, 1);
    CHECK_EQ(rl.path == "/api/state", 1); CHECK_EQ(rl.query.empty(), 1); CHECK_EQ(rl.x_k2, 0);

    rl = http::parse_request_line("POST /api/press?button=2&ms=100 HTTP/1.1\r\nx-k2: 1\r\n\r\n");
    CHECK_EQ(rl.ok, 1); CHECK_EQ(rl.method == Method::Post, 1);
    CHECK_EQ(rl.path == "/api/press", 1); CHECK_EQ(rl.query == "button=2&ms=100", 1); CHECK_EQ(rl.x_k2, 1);

    rl = http::parse_request_line("POST /x HTTP/1.1\r\nHost: a\r\nX-K2:1\r\n\r\n");
    CHECK_EQ(rl.x_k2, 1);
    rl = http::parse_request_line("POST /x HTTP/1.1\r\nX-K2-Other: 1\r\n\r\n");
    CHECK_EQ(rl.x_k2, 0);

    CHECK_EQ(http::parse_request_line("").ok, 0);
    CHECK_EQ(http::parse_request_line("GET\r\n\r\n").ok, 0);
    CHECK_EQ(http::parse_request_line("GET /x\r\n\r\n").ok, 0);
    CHECK_EQ(http::parse_request_line("GET /x FTP/1.0\r\n\r\n").ok, 0);
    CHECK_EQ(http::parse_request_line("DELETE /x HTTP/1.1\r\n\r\n").method == Method::Other, 1);
}

static void test_query() {
    CHECK_EQ(http::query_get("a=1&bb=22&c=", "bb").value() == "22", 1);
    CHECK_EQ(http::query_get("a=1&bb=22&c=", "c").value().empty(), 1);
    CHECK_EQ(http::query_get("a=1", "b").has_value(), 0);
    CHECK_EQ(http::query_get("ab=1", "b").has_value(), 0);
    CHECK_EQ(http::parse_uint("65535").value(), 65535);
    CHECK_EQ(http::parse_uint("").has_value(), 0);
    CHECK_EQ(http::parse_uint("12x").has_value(), 0);
    CHECK_EQ(http::parse_uint("-1").has_value(), 0);
    CHECK_EQ(http::parse_uint("4294967296").has_value(), 0);
    CHECK_EQ(http::parse_uint("4294967295").value(), 4294967295LL);
    const auto fr = http::parse_hex_frame("A54B010100" + std::string(118, '0'));
    CHECK_EQ(fr.has_value(), 1);
    if (fr) CHECK_EQ((*fr)[2], 1);
    CHECK_EQ(http::parse_hex_frame("A54B").has_value(), 0);
    CHECK_EQ(http::parse_hex_frame(std::string(127, '0') + "g").has_value(), 0);
}

static void test_json_writer() {
    std::array<char, 64> b;
    http::Json j{std::span<char>(b)};
    j.open('{'); j.key("a"); j.num(-5); j.key("s"); j.str("x\"y\\z\n"); j.key("l"); j.open('[');
    j.num(1); j.num(2); j.close(']'); j.key("t"); j.boolean(true); j.close('}');
    CHECK_EQ(j.over, 0);
    CHECK_STR(std::string(b.data(), j.n).c_str(), "{\"a\":-5,\"s\":\"x\\\"y\\\\z?\",\"l\":[1,2],\"t\":true}");
    std::array<char, 4> tiny;
    http::Json t{std::span<char>(tiny)};
    t.open('{'); t.key("long"); t.num(1);
    CHECK_EQ(t.over, 1);
}

static void test_state_pdos_ping() {
    Fake f;
    Res r = run(f, "GET /api/state HTTP/1.1\r\n\r\n");
    CHECK_EQ(r.r.status, 200); CHECK_EQ(r.r.body == Body::Json, 1);
    CHECK_EQ(has(r, "\"vbus_mv\":20000"), 1);
    CHECK_EQ(has(r, "\"current_ma\":-1500"), 1);
    CHECK_EQ(has(r, "\"temp_deci_c\":-25"), 1);
    CHECK_EQ(has(r, "\"cable_age_s\":258"), 1);

    f.snap.valid = 1; f.snap.spec_rev = 3; f.snap.num_pdos = 2;
    f.snap.pdo[0] = {0, 0x0001912C, 5000, 5000, 3000, 15000};
    f.snap.pdo[1] = {3, 0xC0000000, 5000, 20000, 3000, 60000};
    r = run(f, "GET /api/pdos HTTP/1.1\r\n\r\n");
    CHECK_EQ(r.r.status, 200);
    CHECK_EQ(has(r, "\"num_pdos\":2"), 1);
    CHECK_EQ(has(r, "{\"type\":0,\"raw\":102700,\"min_mv\":5000,\"max_mv\":5000,\"max_ma\":3000,\"max_mw\":15000}"), 1);
    CHECK_EQ(has(r, "\"max_mv\":20000"), 1);
    CHECK_EQ(has(r, "\"type\":3"), 1);

    r = run(f, "GET /api/ping HTTP/1.1\r\n\r\n");
    CHECK_EQ(r.r.status, 200);
    CHECK_EQ(has(r, "\"git\":\"abc1234\""), 1);
    CHECK_EQ(has(r, "\"uptime_ms\":16909060"), 1);
}

static void test_press() {
    Fake f;
    Res r = run(f, "POST /api/press?button=2&ms=120 HTTP/1.1" X);
    CHECK_EQ(r.r.status, 200); CHECK_EQ(f.last_btn, 2); CHECK_EQ(f.last_ms, 120);
    CHECK_EQ(has(r, "\"ok\":true"), 1);
    f.calls = 0;
    r = run(f, "POST /api/press?button=2&ms=120 HTTP/1.1\r\n\r\n");   // no X-K2 header
    CHECK_EQ(r.r.status, 403); CHECK_EQ(f.calls, 0);
    r = run(f, "GET /api/press?button=2 HTTP/1.1\r\n\r\n");
    CHECK_EQ(r.r.status, 405); CHECK_EQ(f.calls, 0);
    r = run(f, "POST /api/press?button=9&ms=120 HTTP/1.1" X);
    CHECK_EQ(r.r.status, 400); CHECK_EQ(f.calls, 0);
    r = run(f, "POST /api/press?ms=120 HTTP/1.1" X);
    CHECK_EQ(r.r.status, 400);
    r = run(f, "POST /api/press?button=1 HTTP/1.1" X);   // default press length
    CHECK_EQ(r.r.status, 200); CHECK_EQ(f.last_ms, 100);
    f.press_status = Status::Busy;
    r = run(f, "POST /api/press?button=1 HTTP/1.1" X);
    CHECK_EQ(r.r.status, 409);
}

static void test_vbus_confirm() {
    Fake f;
    // PD request: refused at the HTTP layer without confirm=1, the backend is never reached.
    Res r = run(f, "POST /api/pd/request?pos=2&mv=9000&ma=2000 HTTP/1.1" X);
    CHECK_EQ(r.r.status, 403); CHECK_EQ(f.calls, 0); CHECK_EQ(has(r, "confirm"), 1);
    r = run(f, "POST /api/pd/request?pos=2&mv=9000&ma=2000&confirm=0 HTTP/1.1" X);
    CHECK_EQ(r.r.status, 403); CHECK_EQ(f.calls, 0);
    r = run(f, "POST /api/pd/request?pos=2&mv=9000&ma=2000&confirm=1 HTTP/1.1" X);
    CHECK_EQ(r.r.status, 200); CHECK_EQ(f.rq_pos, 2); CHECK_EQ(f.rq_mv, 9000); CHECK_EQ(f.rq_ma, 2000);
    CHECK_EQ(f.rq_confirm, 1);
    r = run(f, "POST /api/pd/request?pos=2&mv=x&ma=2000&confirm=1 HTTP/1.1" X);
    CHECK_EQ(r.r.status, 400);

    f.calls = 0;
    r = run(f, "POST /api/pd/reset HTTP/1.1" X);
    CHECK_EQ(r.r.status, 403); CHECK_EQ(f.resets, 0);
    r = run(f, "POST /api/pd/reset?confirm=1 HTTP/1.1" X);
    CHECK_EQ(r.r.status, 200); CHECK_EQ(f.resets, 1);
    r = run(f, "POST /api/pd/release HTTP/1.1" X);
    CHECK_EQ(r.r.status, 403); CHECK_EQ(f.releases, 0);
    r = run(f, "POST /api/pd/release?confirm=1 HTTP/1.1" X);
    CHECK_EQ(r.r.status, 200); CHECK_EQ(f.releases, 1);

    // Fast charge: status is read only, everything else needs confirm.
    r = run(f, "GET /api/fc HTTP/1.1\r\n\r\n");
    CHECK_EQ(r.r.status, 200); CHECK_EQ(has(r, "\"target_mv\":9000"), 1); CHECK_EQ(f.fc_calls, 0);
    r = run(f, "POST /api/fc?op=trigger&proto=3&mv=5000 HTTP/1.1" X);
    CHECK_EQ(r.r.status, 403); CHECK_EQ(f.fc_calls, 0);
    r = run(f, "POST /api/fc?op=scan HTTP/1.1" X);
    CHECK_EQ(r.r.status, 403); CHECK_EQ(f.fc_calls, 0);
    r = run(f, "POST /api/fc?op=reset HTTP/1.1" X);
    CHECK_EQ(r.r.status, 403); CHECK_EQ(f.fc_calls, 0);
    r = run(f, "POST /api/fc?op=trigger&proto=3&mv=9000&confirm=1 HTTP/1.1" X);
    CHECK_EQ(r.r.status, 200); CHECK_EQ(f.fc_calls, 1); CHECK_EQ(f.fc_op, 3); CHECK_EQ(f.fc_proto, 3);
    CHECK_EQ(f.fc_mv, 9000); CHECK_EQ(f.fc_flags, 1);
    r = run(f, "POST /api/fc?op=trigger&proto=3&mv=9000&confirm=1&inline=1 HTTP/1.1" X);
    CHECK_EQ(f.fc_flags, 3);
    r = run(f, "POST /api/fc?op=scan&confirm=1 HTTP/1.1" X);
    CHECK_EQ(r.r.status, 200); CHECK_EQ(f.fc_op, 2);
    r = run(f, "POST /api/fc?op=bogus&confirm=1 HTTP/1.1" X);
    CHECK_EQ(r.r.status, 400);
    r = run(f, "POST /api/fc?op=trigger&proto=99&mv=9000&confirm=1 HTTP/1.1" X);
    CHECK_EQ(r.r.status, 400);
    f.fc_status = Status::Busy;
    r = run(f, "POST /api/fc?op=reset&confirm=1 HTTP/1.1" X);
    CHECK_EQ(r.r.status, 409);
}

static std::string frame_hex(std::initializer_list<uint8_t> req) {
    std::string s;
    std::array<uint8_t, 64> f{};
    size_t i = 0;
    for (uint8_t b : req) f[i++] = b;
    for (uint8_t b : f) { char t[3]; snprintf(t, sizeof t, "%02x", b); s += t; }
    return s;
}

static void test_frame_bridge() {
    Fake f;
    // ping frame: answered with the 64-byte response in hex
    Res r = run(f, ("POST /api/frame?hex=" + frame_hex({0xA5, 0x4B, 0x01, 0x07, 0x00}) + " HTTP/1.1" X).c_str());
    CHECK_EQ(r.r.status, 200); CHECK_EQ(has(r, "\"rsp\":\"a64b0107"), 1);
    CHECK_EQ(r.body.size() > 128, 1);
    // PD request frame with the confirm flag in the payload but no confirm=1 parameter: refused
    const std::string pd = frame_hex({0xA5, 0x4B, 0x41, 0x01, 0x06, 2, 1, 0x28, 0x23, 0xD0, 0x07});
    f.calls = 0;
    r = run(f, ("POST /api/frame?hex=" + pd + " HTTP/1.1" X).c_str());
    CHECK_EQ(r.r.status, 403); CHECK_EQ(f.calls, 0);
    r = run(f, ("POST /api/frame?hex=" + pd + "&confirm=1 HTTP/1.1" X).c_str());
    CHECK_EQ(r.r.status, 200); CHECK_EQ(f.rq_mv, 9000); CHECK_EQ(f.rq_confirm, 1);
    // The lab-only over-load flag (payload flags bit 1) never crosses the network: /api/frame strips it
    // before dispatch, so a network PdRequest reaches the backend with over_load=false even with bit 1 set.
    const std::string pd_ol = frame_hex({0xA5, 0x4B, 0x41, 0x02, 0x06, 2, 3, 0x28, 0x23, 0xD0, 0x07});   // flags = confirm | over-load
    r = run(f, ("POST /api/frame?hex=" + pd_ol + "&confirm=1 HTTP/1.1" X).c_str());
    CHECK_EQ(r.r.status, 200); CHECK_EQ(f.rq_mv, 9000); CHECK_EQ(f.rq_confirm, 1);
    CHECK_EQ(f.rq_over_load, false);           // RED before the fix: bit 1 reached the backend
    // pd attach takes the Rd and asks the source for a contract: same family as PdRequest, so the raw bridge
    // needs confirm=1 for it too (the payload confirm flag is the dispatcher's, the parameter is the deliberate-call gate)
    const std::string att = frame_hex({0xA5, 0x4B, 0x48, 0x03, 0x05, 0x28, 0x23, 0, 0, 1});   // 9000 mV, flags = confirm
    f.calls = 0; f.attaches = 0;
    r = run(f, ("POST /api/frame?hex=" + att + " HTTP/1.1" X).c_str());
    CHECK_EQ(r.r.status, 403); CHECK_EQ(f.calls, 0); CHECK_EQ(f.attaches, 0);
    r = run(f, ("POST /api/frame?hex=" + att + "&confirm=1 HTTP/1.1" X).c_str());
    CHECK_EQ(r.r.status, 200); CHECK_EQ(f.attaches, 1); CHECK_EQ(f.attach_mv, 9000); CHECK_EQ(f.attach_confirm, true);
    // fc trigger and scan frames need confirm=1 too; status does not
    f.calls = 0;
    r = run(f, ("POST /api/frame?hex=" + frame_hex({0xA5, 0x4B, 0x32, 1, 5, 3, 1, 0x88, 0x13, 1}) + " HTTP/1.1" X).c_str());
    CHECK_EQ(r.r.status, 403); CHECK_EQ(f.fc_calls, 0);
    r = run(f, ("POST /api/frame?hex=" + frame_hex({0xA5, 0x4B, 0x32, 1, 1, 0}) + " HTTP/1.1" X).c_str());
    CHECK_EQ(r.r.status, 200);
    // ACTION FcScan walks the output up to 9 V: a raw frame needs confirm=1 for it as for fc scan (RED before the
    // fix: the frame, payload confirm byte set, reached the backend with no deliberate-call parameter).
    const std::string act_scan = frame_hex({0xA5, 0x4B, 0x06, 0x04, 0x02, 4, 1});
    f.calls = 0; f.last_action = 0xFF;
    r = run(f, ("POST /api/frame?hex=" + act_scan + " HTTP/1.1" X).c_str());
    CHECK_EQ(r.r.status, 403); CHECK_EQ(f.calls, 0);
    r = run(f, ("POST /api/frame?hex=" + act_scan + "&confirm=1 HTTP/1.1" X).c_str());
    CHECK_EQ(r.r.status, 200); CHECK_EQ(f.last_action, 4);
    // an action that never raises VBUS needs no confirm=1 (PdRescan)
    f.calls = 0;
    r = run(f, ("POST /api/frame?hex=" + frame_hex({0xA5, 0x4B, 0x06, 0x05, 0x01, 1}) + " HTTP/1.1" X).c_str());
    CHECK_EQ(r.r.status, 200); CHECK_EQ(f.last_action, 1);
    // STOP (0x4B) only moves toward 5 V / idle: the raw bridge passes it without confirm=1 (a tab that lost its host must be able
    // to stop; pinned here because every other output-changing family is gated above)
    f.calls = 0; f.stops = 0;
    f.stop_result = {0x02, 5012, true};
    r = run(f, ("POST /api/frame?hex=" + frame_hex({0xA5, 0x4B, 0x4B, 0x09, 0x00}) + " HTTP/1.1" X).c_str());
    CHECK_EQ(r.r.status, 200); CHECK_EQ(f.stops, 1);
    CHECK_EQ(has(r, "\"rsp\":\"a64b4b09"), 1);
    // commands that cannot work or must not run over the network
    for (uint8_t cmd : {0x0A, 0x30, 0x31, 0x08, 0x33, 0x49}) {   // screenshot, DFU, fault, reboot, factory wipe, lab hand-back
        f.calls = 0;
        r = run(f, ("POST /api/frame?hex=" + frame_hex({0xA5, 0x4B, cmd, 1, 0}) + "&confirm=1 HTTP/1.1" X).c_str());
        CHECK_EQ(r.r.status, 403); CHECK_EQ(f.calls, 0); CHECK_EQ(f.shots, 0);
    }
    r = run(f, "POST /api/frame?hex=zz HTTP/1.1" X);
    CHECK_EQ(r.r.status, 400);
    r = run(f, ("POST /api/frame?hex=" + frame_hex({0x00, 0x00, 1, 0, 0}) + " HTTP/1.1" X).c_str());   // not a request
    CHECK_EQ(r.r.status, 400);
    r = run(f, ("GET /api/frame?hex=" + frame_hex({0xA5, 0x4B, 1, 0, 0}) + " HTTP/1.1\r\n\r\n").c_str());
    CHECK_EQ(r.r.status, 405);
}

static void test_blackbox_cables() {
    Fake f;
    Res r = run(f, "GET /api/blackbox?op=3 HTTP/1.1\r\n\r\n");
    CHECK_EQ(r.r.status, 200); CHECK_EQ(f.bb_op, 3); CHECK_EQ(has(r, "\"hex\":\"a578563412\""), 1);
    r = run(f, "GET /api/blackbox?op=0&arg=77 HTTP/1.1\r\n\r\n");
    CHECK_EQ(r.r.status, 200); CHECK_EQ(f.bb_arg, 77);
    r = run(f, "GET /api/blackbox?op=5&arg=65538 HTTP/1.1\r\n\r\n");   // capture 1, offset 2
    CHECK_EQ(r.r.status, 200); CHECK_EQ(f.bb_arg, 65538);
    f.calls = 0;
    r = run(f, "GET /api/blackbox?op=1 HTTP/1.1\r\n\r\n");   // clear must be a POST
    CHECK_EQ(r.r.status, 405); CHECK_EQ(f.calls, 0);
    r = run(f, "POST /api/blackbox?op=1 HTTP/1.1" X);
    CHECK_EQ(r.r.status, 200); CHECK_EQ(f.bb_op, 1);
    r = run(f, "POST /api/blackbox?op=3 HTTP/1.1" X);   // read ops are GET only
    CHECK_EQ(r.r.status, 405);
    r = run(f, "GET /api/blackbox?op=9 HTTP/1.1\r\n\r\n");
    CHECK_EQ(r.r.status, 400);

    f.em.valid = 1; f.em.vid = 0x05AC; f.em.max_a = 5; f.em.max_v = 50; f.em.max_w = 250;
    r = run(f, "GET /api/cables HTTP/1.1\r\n\r\n");
    CHECK_EQ(r.r.status, 200);
    CHECK_EQ(has(r, "\"r_mohm\":100"), 1); CHECK_EQ(has(r, "\"vid\":1452"), 1); CHECK_EQ(has(r, "\"max_w\":250"), 1);
    r = run(f, "POST /api/cables?op=reset HTTP/1.1" X);
    CHECK_EQ(r.r.status, 200); CHECK_EQ(f.cable_resets, 1);
}

static void test_static_routes() {
    Fake f;
    CHECK_EQ(run(f, "GET / HTTP/1.1\r\n\r\n").r.body == Body::Index, 1);
    CHECK_EQ(run(f, "GET /index.html HTTP/1.1\r\n\r\n").r.body == Body::Index, 1);
    CHECK_EQ(run(f, "GET /api/screenshot.bmp HTTP/1.1\r\n\r\n").r.body == Body::Bmp, 1);
    CHECK_EQ(run(f, "GET /api/screenshot.bmp HTTP/1.1\r\n\r\n").r.status, 200);
    CHECK_EQ(f.shots, 0);   // the HID screenshot path is not used
    f.shot_enabled = false;   // Settings > Screenshot off: no body, 503, nothing rendered
    const auto off = run(f, "GET /api/screenshot.bmp HTTP/1.1\r\n\r\n");
    CHECK_EQ(off.r.status, 503);
    CHECK_EQ(off.r.body == Body::Bmp, 0);
    f.shot_enabled = true;
    CHECK_EQ(run(f, "GET /nope HTTP/1.1\r\n\r\n").r.status, 404);
    CHECK_EQ(run(f, "garbage").r.status, 400);
    CHECK_EQ(run(f, "DELETE /api/state HTTP/1.1\r\n\r\n").r.status, 405);
}

static void test_small_buffer() {
    Fake f;
    std::array<char, 40> tiny;
    const auto rl = http::parse_request_line("GET /api/state HTTP/1.1\r\n\r\n");
    const auto r = http::route(rl, f, std::span<char>(tiny));
    CHECK_EQ(r.status, 500);
}

static void test_headers() {
    char b[256];
    size_t n = http::build_header(std::span<char>(b), {200, Body::Json, 12}, 12);
    std::string h(b, n);
    CHECK_EQ(h.rfind("HTTP/1.1 200 OK\r\n", 0), 0u);
    CHECK_EQ(h.find("Content-Type: application/json") != std::string::npos, 1);
    CHECK_EQ(h.find("Content-Length: 12\r\n") != std::string::npos, 1);
    CHECK_EQ(h.find("Connection: close\r\n") != std::string::npos, 1);
    CHECK_EQ(h.find("Cache-Control: no-store") != std::string::npos, 1);
    CHECK_EQ(h.substr(h.size() - 4) == "\r\n\r\n", 1);
    n = http::build_header(std::span<char>(b), {200, Body::Index, 0}, 500, true);
    h.assign(b, n);
    CHECK_EQ(h.find("Content-Encoding: gzip") != std::string::npos, 1);
    CHECK_EQ(h.find("text/html") != std::string::npos, 1);
    n = http::build_header(std::span<char>(b), {200, Body::Index, 0}, 500, false);
    h.assign(b, n);
    CHECK_EQ(h.find("Content-Encoding") == std::string::npos, 1);
    n = http::build_header(std::span<char>(b), {404, Body::Json, 5}, 5);
    CHECK_EQ(std::string(b, n).rfind("HTTP/1.1 404 Not Found", 0), 0u);
    n = http::build_header(std::span<char>(b), {403, Body::Json, 5}, 5);
    CHECK_EQ(std::string(b, n).rfind("HTTP/1.1 403 Forbidden", 0), 0u);
    n = http::build_header(std::span<char>(b, 20), {200, Body::Json, 5}, 5);
    CHECK_EQ(n, 0u);   // does not fit
    n = http::build_header(std::span<char>(b), {200, Body::Bmp, 0}, 115266);
    CHECK_EQ(std::string(b, n).find("image/bmp") != std::string::npos, 1);
}

static void test_bmp() {
    std::array<uint8_t, http::kBmpHeaderSize> h;
    http::bmp_header(h, 240, 240);
    CHECK_EQ(h[0], 'B'); CHECK_EQ(h[1], 'M');
    const uint32_t size = h[2] | h[3] << 8 | h[4] << 16 | static_cast<uint32_t>(h[5]) << 24;
    CHECK_EQ(size, http::kBmpHeaderSize + 240 * 240 * 2);
    CHECK_EQ(h[10], http::kBmpHeaderSize);           // pixel data offset
    CHECK_EQ(h[14], 40);                              // BITMAPINFOHEADER
    CHECK_EQ(h[18], 240); CHECK_EQ(h[19], 0);         // width
    CHECK_EQ(h[22], 0x10);                      // height -240 = 0xFFFFFF10: top-down
    CHECK_EQ(h[23], 0xFF); CHECK_EQ(h[24], 0xFF); CHECK_EQ(h[25], 0xFF);
    CHECK_EQ(h[26], 1); CHECK_EQ(h[28], 16);          // planes, bpp
    CHECK_EQ(h[30], 3);                               // BI_BITFIELDS
    CHECK_EQ(h[54], 0x00); CHECK_EQ(h[55], 0xF8);     // red mask 0xF800
    CHECK_EQ(h[58], 0xE0); CHECK_EQ(h[59], 0x07);     // green mask 0x07E0
    CHECK_EQ(h[62], 0x1F); CHECK_EQ(h[63], 0x00);     // blue mask 0x001F
    CHECK_EQ(http::bmp_size(240, 240), http::kBmpHeaderSize + 115200u);
}


static http::HeadScanner::State feed_all(http::HeadScanner &h, std::string_view s, size_t chunk) {
    http::HeadScanner::State st = h.state;
    for (size_t i = 0; i < s.size(); i += chunk) st = h.feed(s.substr(i, chunk));
    return st;
}

static void test_head_scanner() {
    using S = http::HeadScanner::State;
    const std::string_view req = "POST /api/press?button=1 HTTP/1.1\r\nHost: 192.168.7.1\r\nUser-Agent: " 
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\r\n"
        "x-K2: 1\r\nAccept: */*\r\n\r\n";
    for (size_t chunk : {size_t(1), size_t(2), size_t(7), size_t(64), size_t(1000)}) {
        http::HeadScanner h;
        CHECK_EQ(feed_all(h, req, chunk) == S::Done, 1);
        const auto rl = h.result();
        CHECK_EQ(rl.ok, 1); CHECK_EQ(rl.x_k2, 1); CHECK_EQ(rl.method == Method::Post, 1);
        CHECK_EQ(rl.path == "/api/press", 1); CHECK_EQ(rl.query == "button=1", 1);
    }
    http::HeadScanner a;
    CHECK_EQ(feed_all(a, "GET /api/state HTTP/1.1\r\nHost: k2\r\n", 5) == S::Need, 1);
    CHECK_EQ(a.feed("\r\n") == S::Done, 1);
    CHECK_EQ(a.result().x_k2, 0);
    // bare LF line ends are accepted for the blank line only
    http::HeadScanner b;
    CHECK_EQ(feed_all(b, "GET / HTTP/1.1\r\n\r\n", 3) == S::Done, 1);
    CHECK_EQ(b.result().ok, 1);
    // the request line is too long
    http::HeadScanner c;
    CHECK_EQ(feed_all(c, "GET /" + std::string(300, 'a') + " HTTP/1.1\r\n\r\n", 50) == S::TooLarge, 1);
    // endless headers
    http::HeadScanner d;
    feed_all(d, "GET / HTTP/1.1\r\n", 100);
    for (int i = 0; i < 400 && d.state == S::Need; i++) d.feed("X-Pad: aaaaaaaaaaaaaaaaaaaa\r\n");
    CHECK_EQ(d.state == S::TooLarge, 1);
    // data after Done is ignored
    http::HeadScanner e;
    feed_all(e, "GET / HTTP/1.1\r\n\r\nBODY", 4);
    CHECK_EQ(e.state == S::Done, 1);
    // a garbage first line parses as not ok
    http::HeadScanner g;
    feed_all(g, "hello\r\n\r\n", 3);
    CHECK_EQ(g.state == S::Done, 1); CHECK_EQ(g.result().ok, 0);
}

int main() {
    test_head_scanner();
    test_request_line();
    test_query();
    test_json_writer();
    test_state_pdos_ping();
    test_press();
    test_vbus_confirm();
    test_frame_bridge();
    test_blackbox_cables();
    test_static_routes();
    test_small_buffer();
    test_headers();
    test_bmp();
    return test_report("test_http_api");
}
