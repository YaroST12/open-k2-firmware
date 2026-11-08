// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Identity of the device(s) behind a capture: Discover Identity ACKs on SOP (VID:PID, split by the
// sender's power role) and the Source_Capabilities fingerprint used when no identity was sniffed.
#include <array>
#include <cstring>
#include <string>
#include <vector>

#include "k2/pd/pdtrace.hpp"
#include "k2/pd/pdtrace_view.hpp"
#include "test_util.h"

using namespace k2::pdtrace;
namespace v = k2::pdtrace::view;

namespace {
using Buf = std::vector<std::uint8_t>;
void add(Buf& b, Type t, std::uint32_t ts, std::vector<std::uint8_t> p) {
    b.push_back(std::uint8_t(6 + p.size()));
    b.push_back(std::uint8_t(t));
    for (int i = 0; i < 4; i++) b.push_back(std::uint8_t(ts >> (8 * i)));
    b.insert(b.end(), p.begin(), p.end());
}
// Received message: token, header, objects, 4 CRC bytes.
void rx(Buf& b, std::uint32_t ts, std::uint8_t token, unsigned hdr, std::vector<std::uint32_t> objs) {
    std::vector<std::uint8_t> p = {token, std::uint8_t(hdr), std::uint8_t(hdr >> 8)};
    for (auto o : objs) for (int i = 0; i < 4; i++) p.push_back(std::uint8_t(o >> (8 * i)));
    p.insert(p.end(), 4, 0);
    add(b, Type::Rx, ts, p);
}
constexpr unsigned kSrc = 1u << 8;                 // header bit 8: power role = source
constexpr std::uint32_t kVdmAck = 0xFF00A041;      // structured, svid FF00, ACK, Discover Identity
constexpr std::uint32_t kVdmReq = 0xFF008001;      // the request, not the answer
void identity(Buf& b, std::uint32_t ts, std::uint8_t token, unsigned role, std::uint32_t hdr0, unsigned vid, unsigned pid) {
    rx(b, ts, token, 0x4F | role | 4u << 12, {hdr0, vid, 0, pid << 16});
}
std::string key(const v::Identity& i) {
    char o[24];
    return std::string(o, v::format_key(i, o));
}
// 5V3A 9V3A 12V3A 15V3A 20V3.35A + two PPS (11V 3A, 21V 3.25A)
const std::vector<std::uint32_t> kCaps67 = {0x0001912C, 0x0002D12C, 0x0003C12C, 0x0004B12C, 0x0006414F,
                                            0xC0000000u | 110u << 17 | 33u << 8 | 60u,
                                            0xC0000000u | 210u << 17 | 33u << 8 | 60u};
}  // namespace

int main() {
    {   // a source that answers Discover Identity: Apple, product 0x1234
        Buf b;
        rx(b, 100, 0xE0, 0x7001 | kSrc, kCaps67);                   // 7 objects
        identity(b, 200, 0xE0, kSrc, kVdmAck, 0x05AC, 0x1234);
        const auto id = v::identify(b);
        CHECK_EQ(id.has_src, 1); CHECK_EQ(id.src_vid, 0x05AC); CHECK_EQ(id.src_pid, 0x1234);
        CHECK_EQ(id.has_dev, 0);
        CHECK_STR(key(id).c_str(), "05AC:1234");
        CHECK_STR(id.fp, "67W 5PDO+2PPS");
    }
    {   // the sink device answers too: it is the connected device; the source is kept apart
        Buf b;
        identity(b, 200, 0xE0, kSrc, kVdmAck, 0x2717, 0x0001);
        identity(b, 300, 0xE0, 0, kVdmAck, 0x18D1, 0x4EE1);
        const auto id = v::identify(b);
        CHECK_EQ(id.has_dev, 1); CHECK_EQ(id.dev_vid, 0x18D1); CHECK_EQ(id.dev_pid, 0x4EE1);
        CHECK_EQ(id.has_src, 1); CHECK_EQ(id.src_vid, 0x2717);
        CHECK_STR(key(id).c_str(), "18D1:4EE1");
    }
    {   // cable (SOP') answers, requests, and NAKs are not a device identity
        Buf b;
        identity(b, 200, 0xC0, kSrc, kVdmAck, 0x1111, 0x2222);       // SOP'
        identity(b, 300, 0xE0, kSrc, kVdmReq, 0x3333, 0x4444);       // a request
        identity(b, 400, 0xE0, kSrc, 0xFF00A0C1, 0x5555, 0x6666);    // NAK
        rx(b, 500, 0xE0, 0x4F | kSrc | 1u << 12, {kVdmAck});          // truncated ACK
        const auto id = v::identify(b);
        CHECK_EQ(id.has_src, 0); CHECK_EQ(id.has_dev, 0);
        CHECK_STR(key(id).c_str(), "?");
    }
    {   // no identity: the Source_Capabilities fingerprint is the key
        Buf b;
        rx(b, 100, 0xE0, 0x7001 | kSrc, kCaps67);
        const auto id = v::identify(b);
        CHECK_STR(key(id).c_str(), "67W 5PDO+2PPS");
        Buf c;
        rx(c, 100, 0xE0, 0x1001 | kSrc, {0x0001912C});                // plain 5V3A charger
        CHECK_STR(key(v::identify(c)).c_str(), "15W 1PDO");
        Buf d;
        rx(d, 100, 0xE0, 0x1001, {0x0001912C});                       // sent by a sink: not capabilities
        CHECK_STR(key(v::identify(d)).c_str(), "?");
    }
    {   // same_key
        Buf a, b, c;
        identity(a, 1, 0xE0, kSrc, kVdmAck, 0x05AC, 0x1234);
        identity(b, 1, 0xE0, kSrc, kVdmAck, 0x05AC, 0x1234);
        identity(c, 1, 0xE0, kSrc, kVdmAck, 0x05AC, 0x1235);
        CHECK_EQ(v::same_key(v::identify(a), v::identify(b)), 1);
        CHECK_EQ(v::same_key(v::identify(a), v::identify(c)), 0);
        Buf f1, f2;
        rx(f1, 1, 0xE0, 0x7001 | kSrc, kCaps67);
        rx(f2, 1, 0xE0, 0x7001 | kSrc, kCaps67);
        CHECK_EQ(v::same_key(v::identify(f1), v::identify(f2)), 1);
        CHECK_EQ(v::same_key(v::identify(f1), v::identify(a)), 0);
        CHECK_EQ(v::same_key(v::identify({}), v::identify({})), 1);   // all unknown share one group
    }
    {   // json_line: line 0 is the meta line with the identity, line 1 is the link line, the records follow from line 2
        Buf b;
        rx(b, 100, 0xE0, 0x7001 | kSrc, kCaps67);
        identity(b, 200, 0xE0, kSrc, kVdmAck, 0x05AC, 0x1234);
        identity(b, 300, 0xE0, 0, kVdmAck, 0x18D1, 0x4EE1);
        std::array<char, v::kJsonLine> ln;
        v::json_line(b, 0, ln);
        CHECK_EQ(ln[v::kJsonLine - 1], '\n');
        const std::string m(ln.data(), v::kJsonLine - 1);
        CHECK_EQ(m.find("{\"meta\":{") == 0, 1);
        CHECK_EQ(m.find("\"key\":\"18D1:4EE1\"") != std::string::npos, 1);
        CHECK_EQ(m.find("\"dev\":\"18D1:4EE1\"") != std::string::npos, 1);
        CHECK_EQ(m.find("\"src\":\"05AC:1234\"") != std::string::npos, 1);
        CHECK_EQ(m.find("\"fp\":\"67W 5PDO+2PPS\"") != std::string::npos, 1);
        v::json_line(b, 1, ln);                                         // line 1: the negotiated link
        CHECK_EQ(std::memcmp(ln.data(), "{\"link\":{", 9), 0);
        v::json_line(b, 2, ln);                                         // first record
        CHECK_EQ(std::memcmp(ln.data(), "{\"t\":100,", 9), 0);
        v::json_line({}, 0, ln);                                        // empty capture: key "?"
        CHECK_EQ(std::string(ln.data(), 40).find("\"key\":\"?\"") != std::string::npos, 1);
    }
    return test_report("pdtrace_id");
}
