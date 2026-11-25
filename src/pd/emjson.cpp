// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#include "k2/pd/emjson.hpp"

#include "k2/pd/emname.hpp"
#include "k2/pd/pd.hpp"
#include "k2/core/textbuf.hpp"

namespace k2::emjson {

std::size_t render(const emlib::Entry& e, std::span<char> out) {
    TextBuf t(out);
    std::array<std::uint8_t, 5 * 4> raw{};
    std::array<std::uint32_t, 5> v{};
    for (unsigned i = 0; i < 5; i++) {
        v[i] = emrec::vdo(e.rec, i);
        for (unsigned b = 0; b < 4; b++) raw[4 * i + b] = static_cast<std::uint8_t>(v[i] >> (8 * b));
    }
    const pd::Emarker em = pd::parse_emarker(raw, 5);
    // The name is limited to [A-Za-z0-9 -_.] by emlib::name_valid, so it needs no JSON escaping;
    // anything else (a damaged entry) is replaced so the document stays valid.
    char name[emlib::kNameMax + 1];
    for (std::size_t i = 0; i <= emlib::kNameMax; i++) {
        const char c = e.name[i];
        name[i] = (c == '"' || c == '\\' || (c != 0 && (c < 0x20 || c > 0x7E))) ? '?' : c;
    }
    name[emlib::kNameMax] = 0;

    t.put_raw(K2_P("{\n  \"format\": \"k2-emarker\",\n  \"version\": 1,\n"));
    K2_TPUT(t, "  \"id\": %u,\n  \"revision\": %u,\n", static_cast<unsigned>(e.seq), static_cast<unsigned>(e.version));
    K2_TPUT(t, "  \"name\": \"%s\",\n", name);
    K2_TPUT(t, "  \"auto_name\": \"%s\",\n", emname::make(e.rec).data());
    t.put_raw(K2_P("  \"record\": \""));
    static const char* hex = "0123456789ABCDEF";
    for (std::uint8_t b : e.rec) {
        const char two[2] = {hex[b >> 4], hex[b & 15]};
        t.put_raw({two, 2});
    }
    t.put_raw(K2_P("\",\n  \"cable\": {\n"));
    K2_TPUT(t, "    \"vid\": \"0x%04X\",\n    \"vendor\": \"%s\",\n", static_cast<unsigned>(em.vid), em.vendor_str.c_str());
    K2_TPUT(t, "    \"pid\": \"0x%04X\",\n    \"xid\": \"0x%08X\",\n", static_cast<unsigned>(em.pid), static_cast<unsigned>(em.xid));
    K2_TPUT(t, "    \"type\": \"%s\",\n", em.is_active ? K2_P("active") : K2_P("passive"));
    K2_TPUT(t, "    \"speed\": \"%s\",\n", em.speed_str);
    K2_TPUT(t, "    \"current_a\": %u,\n    \"max_v\": %u,\n    \"max_w\": %u,\n", static_cast<unsigned>(em.max_current_a),
            static_cast<unsigned>(em.max_voltage_v), static_cast<unsigned>(em.max_power_w));
    K2_TPUT(t, "    \"latency\": \"%s\",\n", em.latency_str);
    K2_TPUT(t, "    \"vconn_required\": %s,\n    \"epr\": %s,\n", em.vconn_required ? "true" : "false", em.epr_capable ? "true" : "false");
    K2_TPUT(t, "    \"pd_rev\": %u,\n", static_cast<unsigned>(e.rec[0x14]));
    t.put_raw(K2_P("    \"vdos\": ["));
    for (unsigned i = 0; i < 5; i++) K2_TPUT(t, "%s\"0x%08X\"", i ? ", " : "", static_cast<unsigned>(v[i]));
    t.put_raw(K2_P("]\n  }\n}\n"));
    return t.size();
}

}  // namespace k2::emjson
