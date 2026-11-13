// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// lwIP raw-API HTTP/1.1 server, see k2/net/http_server.hpp.
//
// Memory: kConns connection records (request line + flags) and one shared reply area, since only
// one reply is produced at a time: a 2 KB JSON buffer, the header+BMP-prefix buffer, and for the
// screenshot the capture module's 1.9 KB strip buffer. Replies are closed with FIN
// (Connection: close), so there is no keep-alive state.
#include "k2/core/build_config.h"
#include "k2/core/log.hpp"
#include "k2/net/http_server.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <string_view>

#include "k2/net/http_api.hpp"
#include "k2/drivers/systick.hpp"
#include "k2/drivers/usb.hpp"
#include "k2/net/web_asset.hpp"

#include "lwip/tcp.h"

// Defined once in remote.cpp: this object is built without LTO and would otherwise carry its own
// copy of the 4 KB request dispatcher.
extern template bool k2::remote::handle_request<k2::Remote>(std::span<const std::uint8_t>, k2::Remote &,
                                                            std::span<std::uint8_t, k2::remote::kFrameSize>);

namespace k2::http_server {
namespace {

constexpr int kConns = 3;
constexpr std::uint16_t kPort = 80;
constexpr std::uint32_t kHeadTimeoutMs = 5000;   // request not complete
constexpr std::uint32_t kSendTimeoutMs = 10000;  // no progress while sending
#if K2_SCREENSHOT
constexpr int kStrips = 60;
constexpr std::size_t kStripBytes = 240 * 4 * 2;
#endif

enum class St : std::uint8_t { Free, Reading, Pending, Sending };

struct Conn {
    tcp_pcb* pcb = nullptr;
    St st = St::Free;
    http::HeadScanner head;
    std::uint32_t t_ms = 0;   // last progress
};

struct Counters {
    std::uint32_t accepted, refused, requests, ok_replies, aborted, shots, bad_head;
};

constinit Counters ctr{};
constinit std::array<Conn, kConns> conns{};
constinit Remote* g_remote = nullptr;
#if K2_SCREENSHOT
constinit Shot g_shot{};
#endif

// The one reply in flight.
struct Reply {
    Conn* conn = nullptr;
    std::array<std::uint8_t, 320> prefix{};   // status line + headers (+ BMP header)
    std::size_t prefix_len = 0, prefix_off = 0;
    const std::uint8_t* body = nullptr;
    std::size_t body_left = 0;
    bool shot = false;       // screenshot session open: strips are pulled one at a time
    int strip = 0;
    bool closing = false;    // everything queued, waiting for tcp_close() to succeed
};
constinit Reply g_reply{};
constinit std::array<char, 2048> g_json{};

void release(Conn& c) {
    if (g_reply.conn == &c) {
#if K2_SCREENSHOT
        if (g_reply.shot) g_shot.end();
#endif
        g_reply = {};
    }
    c = Conn{};
}

// Detach the callbacks and close (or abort) the pcb, then forget the connection.
void drop(Conn& c, bool abort_it) {
    if (tcp_pcb* p = c.pcb) {
        tcp_arg(p, nullptr);
        tcp_recv(p, nullptr);
        tcp_err(p, nullptr);
        tcp_sent(p, nullptr);
        if (abort_it || tcp_close(p) != ERR_OK) tcp_abort(p);
        c.pcb = nullptr;
    }
    if (abort_it) ctr.aborted++;
    release(c);
}

void on_err(void* arg, err_t) {   // the pcb is already gone
    if (auto* c = static_cast<Conn*>(arg)) {
        c->pcb = nullptr;
        release(*c);
    }
}

err_t on_recv(void* arg, tcp_pcb* pcb, pbuf* p, err_t) {
    auto* c = static_cast<Conn*>(arg);
    if (!p) {   // peer closed its side
        if (c->st == St::Reading) drop(*c, false);
        return ERR_OK;
    }
    tcp_recved(pcb, p->tot_len);
    if (c->st == St::Reading) {
        for (const pbuf* q = p; q; q = q->next) {
            const auto s = c->head.feed(std::span<const char>(static_cast<const char*>(q->payload), q->len));
            if (s != http::HeadScanner::State::Need) break;
        }
        c->t_ms = systick::millis();
        if (c->head.state != http::HeadScanner::State::Need) c->st = St::Pending;
    }
    pbuf_free(p);
    return ERR_OK;
}

err_t on_accept(void*, tcp_pcb* pcb, err_t) {
    for (Conn& c : conns) {
        if (c.st != St::Free) continue;
        c = Conn{};
        c.pcb = pcb;
        c.st = St::Reading;
        c.t_ms = systick::millis();
        tcp_arg(pcb, &c);
        tcp_recv(pcb, on_recv);
        tcp_err(pcb, on_err);
        ctr.accepted++;
        return ERR_OK;
    }
    ctr.refused++;
    return ERR_MEM;
}

void put_error(unsigned status, std::string_view msg, http::Reply& r) {
    http::Json j{std::span<char>(g_json)};
    j.open('{'); j.key("error"); j.str(msg); j.close('}');
    r = {static_cast<std::uint16_t>(status), http::Body::Json, j.n};
}

void start(Conn& c) {
    ctr.requests++;
    const http::RequestLine rl = c.head.state == http::HeadScanner::State::Done ? c.head.result() : http::RequestLine{};
    http::Reply r;
    if (c.head.state == http::HeadScanner::State::TooLarge) {
        ctr.bad_head++;
        put_error(431, "request head too large", r);
    } else {
        r = http::route(rl, *g_remote, std::span<char>(g_json));
    }

    std::size_t content = r.len;
    bool gzip = false;
    g_reply = {};
    g_reply.conn = &c;
    if (r.body == http::Body::Index) {
        const WebAsset a = web_asset();
        g_reply.body = a.data;
        g_reply.body_left = content = a.len;
        gzip = a.gzip;
    } else if (r.body == http::Body::Bmp) {
#if K2_SCREENSHOT
        if (g_shot.begin()) {
            ctr.shots++;
            g_reply.shot = true;
            content = http::bmp_size(240, 240);
        } else {
            put_error(409, "screen capture busy", r);
            content = r.len;
        }
#else
        put_error(503, "screenshot disabled in build", r);
        content = r.len;
#endif
    }
    if (r.body == http::Body::Json) {
        g_reply.body = reinterpret_cast<const std::uint8_t*>(g_json.data());
        g_reply.body_left = r.len;
    }

    const std::size_t h = http::build_header(
        std::span<char>(reinterpret_cast<char*>(g_reply.prefix.data()), g_reply.prefix.size() - http::kBmpHeaderSize), r, content, gzip);
    if (h == 0) {
        drop(c, true);
        return;
    }
    g_reply.prefix_len = h;
    if (g_reply.shot) {
        http::bmp_header(std::span<std::uint8_t, http::kBmpHeaderSize>(g_reply.prefix.data() + h, http::kBmpHeaderSize), 240, 240);
        g_reply.prefix_len += http::kBmpHeaderSize;
    }
    if (r.status == 200) ctr.ok_replies++;
    c.st = St::Sending;
    c.t_ms = systick::millis();
}

// Queue as much of the reply as the send buffer takes; close once all of it is queued.
void pump(Conn& c) {
    Reply& rp = g_reply;
    tcp_pcb* p = c.pcb;
    if (!p) { release(c); return; }

    if (!rp.closing) {
        for (;;) {
            std::size_t avail = tcp_sndbuf(p);
            if (avail == 0) break;
            const std::uint8_t* src;
            std::size_t n;
            if (rp.prefix_off < rp.prefix_len) {
                src = rp.prefix.data() + rp.prefix_off;
                n = rp.prefix_len - rp.prefix_off;
            } else {
#if K2_SCREENSHOT
                if (rp.body_left == 0 && rp.shot && rp.strip < kStrips) {
                    rp.body = reinterpret_cast<const std::uint8_t*>(g_shot.strip(rp.strip++));
                    rp.body_left = kStripBytes;
                }
#endif
                if (rp.body_left == 0) { rp.closing = true; break; }
                src = rp.body;
                n = rp.body_left;
            }
            n = std::min({n, avail, std::size_t{1460}});
            const err_t e = tcp_write(p, src, static_cast<std::uint16_t>(n), TCP_WRITE_FLAG_COPY);
            if (e == ERR_MEM) break;   // queue full: try again after some ACKs
            if (e != ERR_OK) { drop(c, true); return; }
            c.t_ms = systick::millis();
            if (rp.prefix_off < rp.prefix_len) rp.prefix_off += n;
            else { rp.body += n; rp.body_left -= n; }
        }
        tcp_output(p);
    }
    if (rp.closing) {
        tcp_arg(p, nullptr);
        tcp_recv(p, nullptr);
        tcp_err(p, nullptr);
        if (tcp_close(p) == ERR_OK) {   // FIN follows the queued data
            c.pcb = nullptr;
            release(c);
        } else {
            tcp_err(p, on_err);         // out of memory: retry on the next pass
            tcp_arg(p, &c);
        }
    }
}

}  // namespace

void init(Remote& remote, const Shot& shot) {
    g_remote = &remote;
#if K2_SCREENSHOT
    g_shot = shot;
#else
    (void)shot;
#endif
    tcp_pcb* l = tcp_new();
    if (l && tcp_bind(l, IP_ADDR_ANY, kPort) == ERR_OK) {
        l = tcp_listen(l);
        if (l) tcp_accept(l, on_accept);
    }
    K2LOG("[HTTP] listening on :%d %s\n", int(kPort), l ? "ok" : "FAILED");
}

void service() {
    if (!g_remote) return;
    const std::uint32_t now = systick::millis();
    for (Conn& c : conns) {
        if (c.st == St::Reading && now - c.t_ms > kHeadTimeoutMs) drop(c, true);
        else if (c.st == St::Sending && now - c.t_ms > kSendTimeoutMs) drop(c, true);
    }
    if (g_reply.conn) {
        pump(*g_reply.conn);
        return;
    }
    for (Conn& c : conns) {
        if (c.st == St::Pending) {
            start(c);
            if (g_reply.conn) pump(c);
            return;
        }
    }
}

void log_stats() {
    K2LOG("[HTTP] acc=%d ref=%d req=%d ok=%d abrt=%d shots=%d\n", int(ctr.accepted), int(ctr.refused),
                 int(ctr.requests), int(ctr.ok_replies), int(ctr.aborted), int(ctr.shots));
}

}  // namespace k2::http_server
