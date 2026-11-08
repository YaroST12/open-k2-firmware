// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#include <cstring>
#include <string>

#include "test_util.h"
#include "k2/ui/ui_labels.hpp"
#include "k2/ui/ui_pdtrig.hpp"
#include "k2/ui/ui_wizard.hpp"

using namespace k2;
#define CHECK_TRUE(c) CHECK_EQ(int(!!(c)), 1)

static pd::SourceCaps caps() {
    pd::SourceCaps c;
    c.valid = true;
    c.spec_rev = 2;
    const uint32_t raw[] = {0x0001912C, 0x0002D12C, 0x0004B12C, 0x000640C8, 0xC0DC2164, 0xC1A4213C};
    c.num_pdos = 6;
    for (int i = 0; i < 6; i++) c.pdos[i] = pd::decode_pdo(raw[i]);
    return c;
}

// Setup hints: every line fits the 22-column content area, uses printable ASCII, and the text says
// the live reason (which port, what must be absent) for the state the link reports.
static std::string hint_text(const SetupHint &h) {
    std::string s;
    for (int i = 0; i < h.lines; i++) { s += h.line[i]; s += '|'; }
    return s;
}

static void test_setup_hints() {
    const Tool tools[] = {Tool::Identify, Tool::Emarker, Tool::PdTrigger};
    for (Tool t : tools)
        for (int att = 0; att < 2; att++)
            for (int inl = 0; inl < 2; inl++)
                for (int caps_ok = 0; caps_ok < 2; caps_ok++)
                    for (int vb : {0, 5000}) {
                        SetupState st;
                        st.attached = att; st.inline_mode = att && inl; st.caps_valid = caps_ok;
                        st.vbus_known = true; st.vbus_mv = vb;
                        const SetupHint h = setup_hint(t, st);
                        CHECK_TRUE(h.lines >= 2 && h.lines <= 4);
                        for (int i = 0; i < h.lines; i++) {
                            const std::string l = h.line[i];
                            CHECK_TRUE(l.size() <= 22);
                            for (char ch : l) CHECK_TRUE(ch >= 0x20 && ch < 0x7F);
                        }
                    }
    SetupState none;   // nothing on INPUT
    SetupState src;    // a source on INPUT with caps, OUTPUT empty
    src.attached = true; src.caps_valid = true; src.vbus_known = true; src.vbus_mv = 5000;
    SetupState inl = src; inl.inline_mode = true;
    auto has = [](const SetupHint &h, const char *w) { return hint_text(h).find(w) != std::string::npos; };
    CHECK_TRUE(has(setup_hint(Tool::Identify, none), "Nothing connected"));
    CHECK_TRUE(!setup_hint(Tool::Identify, none).ready);
    CHECK_TRUE(has(setup_hint(Tool::Identify, none), "wake it"));
    CHECK_TRUE(setup_hint(Tool::Identify, src).ready);
    CHECK_TRUE(has(setup_hint(Tool::Identify, src), "other is empty"));
    CHECK_TRUE(!setup_hint(Tool::Identify, inl).ready);
    CHECK_TRUE(has(setup_hint(Tool::Identify, inl), "other end"));
    CHECK_TRUE(has(setup_hint(Tool::PdTrigger, inl), "other end"));
    CHECK_TRUE(!setup_hint(Tool::PdTrigger, inl).ready);
    CHECK_TRUE(has(setup_hint(Tool::PdTrigger, none), "either port"));
    SetupState nocaps = src; nocaps.caps_valid = false;
    CHECK_TRUE(!setup_hint(Tool::PdTrigger, nocaps).ready);
    CHECK_TRUE(setup_hint(Tool::PdTrigger, src).ready);
    // e-marker: no source = VCONN scan; source = listen only; inline = listen only, say why
    CHECK_TRUE(has(setup_hint(Tool::Emarker, none), "Scan"));
    CHECK_TRUE(setup_hint(Tool::Emarker, none).ready);
    CHECK_TRUE(has(setup_hint(Tool::Emarker, src), "Listen"));
    CHECK_TRUE(has(setup_hint(Tool::Emarker, inl), "Inline"));
    CHECK_TRUE(has(setup_hint(Tool::Emarker, inl), "listening only"));
}

// Tool template content: checklist rows, roles and footer reason per tool and live state.
static void test_setup_view() {
    SetupState none, src, inl, caps;
    src.attached = true;
    inl.attached = true; inl.inline_mode = true;
    caps.attached = true; caps.caps_valid = true;
    const SetupState vconn_none = none;   // no VBUS: the cable scan (VCONN)
    SetupState sniff = src; sniff.vbus_known = true; sniff.vbus_mv = 5000;

    // Identify: device on INPUT, OUTPUT empty -> both met, ready.
    SetupView v = setup_view(Tool::Identify, src);
    CHECK_EQ(v.n, 2); CHECK_TRUE(v.met[0]); CHECK_TRUE(v.met[1]); CHECK_TRUE(v.ready);
    CHECK_STR(v.top_role, "K2"); CHECK_STR(v.bottom_role, "DEV");
    CHECK_STR(v.footer, "Ready");
    v = setup_view(Tool::Identify, none);
    CHECK_TRUE(!v.met[0]); CHECK_TRUE(v.met[1]); CHECK_TRUE(!v.ready);
    CHECK_STR(v.footer, "Plug the device in");
    v = setup_view(Tool::Identify, inl);
    CHECK_TRUE(v.met[0]); CHECK_TRUE(!v.met[1]); CHECK_TRUE(!v.ready);
    CHECK_STR(v.footer, "Live sink: press twice");

    // E-marker: VCONN scan vs listen-only (a source on the cable) vs inline.
    v = setup_view(Tool::Emarker, vconn_none);
    CHECK_TRUE(v.ready); CHECK_STR(v.bottom_role, "CBL"); CHECK_TRUE(v.met[0]);
    v = setup_view(Tool::Emarker, sniff);
    CHECK_TRUE(v.ready); CHECK_STR(v.top_role, "CHG"); CHECK_STR(v.bottom_role, "CBL");
    CHECK_TRUE(!v.met[1]);   // the charger end still has to be replugged
    v = setup_view(Tool::Emarker, inl);
    CHECK_TRUE(!v.ready); CHECK_STR(v.footer, "Inline: listen only");

    // PD trigger: charger seen, caps read, OUTPUT empty.
    v = setup_view(Tool::PdTrigger, caps);
    CHECK_EQ(v.n, 3); CHECK_TRUE(v.met[0] && v.met[1] && v.met[2]); CHECK_TRUE(v.ready);
    v = setup_view(Tool::PdTrigger, src);
    CHECK_TRUE(v.met[0]); CHECK_TRUE(!v.met[1]); CHECK_STR(v.footer, "Charger gave no PD");
    v = setup_view(Tool::PdTrigger, none);
    CHECK_TRUE(!v.met[0]); CHECK_STR(v.footer, "Plug PD charger in");
    v = setup_view(Tool::PdTrigger, inl);
    CHECK_TRUE(!v.met[2]); CHECK_STR(v.footer, "Unplug the other device");

    // The ports are unmarked and symmetric: no text may name an INPUT or an OUTPUT.
    for (Tool t : {Tool::Identify, Tool::Emarker, Tool::PdTrigger}) for (const SetupState &st : {none, src, inl, caps, sniff}) {
        const SetupView w = setup_view(t, st);
        for (int i = 0; i < w.n; i++) CHECK_TRUE(std::string(w.check[i]).find("PUT") == std::string::npos);
        CHECK_TRUE(std::string(w.footer).find("PUT") == std::string::npos);
        CHECK_TRUE(hint_text(setup_hint(t, st)).find("PUT") == std::string::npos);
    }

    // Texts fit the card: rows <= 12 characters, footer <= 26 (182 px at 7 px).
    const Tool tools[] = {Tool::Identify, Tool::Emarker, Tool::PdTrigger};
    const SetupState states[] = {none, src, inl, caps, sniff};
    for (Tool t : tools) for (const SetupState &st : states) {
        const SetupView w = setup_view(t, st);
        for (int i = 0; i < w.n; i++) CHECK_EQ(int(std::strlen(w.check[i])) <= 12, 1);
        CHECK_EQ(int(std::strlen(w.footer)) <= 26, 1);
        CHECK_EQ(int(std::strlen(w.top_role)) <= 3 && std::strlen(w.bottom_role) <= 3, 1);
    }
}

// PD trigger list rows on the kit RowList: "TYPE volts amps" as the row text, the power as its tail. The row never
// truncates: the two together stay within kPdoLineCells, so the amps lose precision, then drop, before anything is cut.
static void test_pdo_line() {
    pd::SourceCaps c = caps();
    c.pdos[3] = pd::decode_pdo(0x00064145);                    // 20 V 3.25 A
    PdoLine l;
    pdo_line(c.pdos[0], l);
    CHECK_STR(l.text.c_str(), "FIX 5V 3A"); CHECK_STR(l.tail.c_str(), "15W");
    pdo_line(c.pdos[1], l); CHECK_STR(l.text.c_str(), "FIX 9V 3A"); CHECK_STR(l.tail.c_str(), "27W");
    pdo_line(c.pdos[3], l); CHECK_STR(l.text.c_str(), "FIX 20V 3.25A"); CHECK_STR(l.tail.c_str(), "65W");
    pdo_line(c.pdos[4], l); CHECK_STR(l.text.c_str(), "PPS 3.3-11V 5A");
    pdo_line(c.pdos[5], l); CHECK_STR(l.text.c_str(), "PPS 3.3-21V 3A");
    pd::Pdo avs{}; avs.type = pd::PdoType::Avs; avs.min_mv = 15000; avs.max_mv = 48000; avs.max_ma = 5000; avs.max_mw = 240000;
    pdo_line(avs, l);
    CHECK_STR(l.text.c_str(), "AVS 15-48V 5A"); CHECK_STR(l.tail.c_str(), "240W");
    // The widest EPR AVS: 25.5-51.1 V and 4.99 A would take 20 cells: the amps round to 5A.
    const auto widest_avs = pd::decode_pdo(0xD0000000u | (511u << 17) | (255u << 8) | 255u);
    pdo_line(widest_avs, l);
    CHECK_STR(l.text.c_str(), "AVS 25.5-51.1V 5A"); CHECK_STR(l.tail.c_str(), "255W");
    // 10 A and above: a whole number.
    pd::Pdo big{}; big.type = pd::PdoType::Fixed; big.max_mv = 48000; big.max_ma = 5000; big.max_mw = 240000;
    pdo_line(big, l); CHECK_STR(l.text.c_str(), "FIX 48V 5A");
    big.max_mv = 20000; big.max_ma = 10230; big.max_mw = 204000;
    pdo_line(big, l); CHECK_STR(l.text.c_str(), "FIX 20V 10A"); CHECK_STR(l.tail.c_str(), "204W");
    pd::Pdo var{}; var.type = pd::PdoType::Variable; var.max_mv = 12000; var.max_ma = 1000; var.max_mw = 12000;
    pdo_line(var, l); CHECK_STR(l.text.c_str(), "VAR 12V 1A");
    pd::Pdo none{}; none.type = pd::PdoType::Fixed; none.max_mv = 5000;
    pdo_line(none, l); CHECK_STR(l.text.c_str(), "FIX 5V"); CHECK_STR(l.tail.c_str(), "0W");   // no current: no amps
    // Every shape fits: text and tail together stay within the cells the row has.
    for (uint32_t mv : {3300u, 5000u, 5050u, 9000u, 12500u, 20000u, 25500u, 51100u})
        for (uint32_t ma : {0u, 1u, 500u, 1000u, 3250u, 4990u, 9990u, 10230u, 16000u})
            for (pd::PdoType t : {pd::PdoType::Fixed, pd::PdoType::Pps, pd::PdoType::Avs, pd::PdoType::Variable, pd::PdoType::Battery}) {
                pd::Pdo p{}; p.type = t; p.min_mv = 3300; p.max_mv = mv; p.max_ma = ma; p.max_mw = mv / 1000 * ma / 1000 * 1000;
                if (t == pd::PdoType::Avs && mv < 15000) continue;
                pdo_line(p, l);
                CHECK_EQ(int(l.text.view().size() + l.tail.view().size()) <= kPdoLineCells, 1);
            }
}

// A footer is a fact about the state, never the keys: the panel strip names what each button does, so the
// footer must not say OK / BACK / +/-. Covers every setup-card footer and every wizard step.
static void test_footers_do_not_repeat_the_strip() {
    const pd::SourceCaps c = caps();
    for (int armed = 0; armed < 2; armed++)
        for (int s = 0; s <= int(WizStep::Live); s++) {
            Wizard w;
            w.enter(c, 0);
            w.step = WizStep(s);
            for (uint8_t cur = 0; cur < c.num_pdos; cur++) {
                w.cursor = cur;
                StrBuf<32> f;
                w.footer(c, 36000, armed != 0, f);
                if (!footer_is_clean(f.c_str())) printf("footer repeats the strip: \"%s\"\n", f.c_str());
                CHECK_EQ(int(footer_is_clean(f.c_str())), 1);
                CHECK_EQ(int(f.view().size() > 0), 1);
            }
        }
    CHECK_EQ(int(footer_is_clean("OK next")), 0);
    CHECK_EQ(int(footer_is_clean("Press OK")), 0);
    CHECK_EQ(int(footer_is_clean("BACK = release")), 0);
    CHECK_EQ(int(footer_is_clean("+/- pick")), 0);
    CHECK_EQ(int(footer_is_clean("Okay")), 1);
    CHECK_EQ(int(footer_is_clean("Backs up")), 1);
}

int main() {
    test_setup_hints();
    test_footers_do_not_repeat_the_strip();
    const pd::SourceCaps c = caps();
    {   // confirm rule
        CHECK_EQ(int(trigger_needs_confirm(c, {1, 0, 0}, 36000)), 0);   // 5 V
        CHECK_EQ(int(trigger_needs_confirm(c, {2, 0, 0}, 36000)), 1);   // 9 V
        CHECK_EQ(int(trigger_needs_confirm(c, {5, 5000, 0}, 36000)), 0);   // PPS at 5 V
        CHECK_EQ(int(trigger_needs_confirm(c, {5, 9000, 0}, 36000)), 1);
        CHECK_EQ(int(trigger_needs_confirm(c, {4, 0, 0}, 5000)), 0);    // over the cap: refused, no confirm
        CHECK_EQ(int(trigger_needs_confirm(c, {0, 0, 0}, 36000)), 0);   // EPR list only
        CHECK_EQ(int(trigger_needs_confirm(pd::SourceCaps{}, {1, 0, 0}, 36000)), 0);
    }
    {   // identify rows
        pd::IdStatus st;
        pd::VdmInfo v;
        IdRow rows[kMaxIdRows];
        CHECK_EQ(identify_rows(st, v, rows), 0);
        StrBuf<24> s;
        identify_status_text(st, s);
        CHECK_STR(s.c_str(), "OK: identify device");
        v.identity.valid = true;
        v.identity.vid = 0x05AC;
        v.identity.pid = 0x1234;
        v.identity.vendor = "Apple";
        v.num_svids = 2;
        v.svids[0] = 0x05AC;
        v.svids[1] = 0xFF01;
        st.state = pd::IdState::Done;
        st.ok_mask = 0x0B;
        st.skipped_mask = 0x24;
        st.num_sink_pdos = 1;
        st.sink_pdos[0] = 0x0001912C;
        st.revision = 0x31200000;
        const int n = identify_rows(st, v, rows);
        CHECK_EQ(n, 11);
        CHECK_STR(rows[1].text.c_str(), "VID 05AC Apple");
        CHECK_STR(rows[4].text.c_str(), "SVID FF01 DisplayPort");
        CHECK_STR(rows[6].text.c_str(), "1 FIX 5V 3.0A");
        CHECK_STR(rows[7].text.c_str(), "PD 3.1 ver 2.0");
        CHECK_STR(rows[8].text.c_str(), "-- No reply --");   // names, not step numbers
        CHECK_STR(rows[9].text.c_str(), "Modes");
        CHECK_STR(rows[10].text.c_str(), "Revision");
        for (int i = 0; i < n; i++) CHECK_EQ(int(rows[i].text.view().size()) <= 21, 1);
        identify_status_text(st, s = {});
        CHECK_STR(s.c_str(), "Done, 3/6 answered");
        // Nobody home is not the same as a device that stayed silent on some steps.
        st.ok_mask = 0;
        identify_status_text(st, s = {});
        CHECK_STR(s.c_str(), "Device didn't answer");
        st.ok_mask = 0x0B;
        // Aborted always says why (and the text fits the 24 byte footer).
        st = {}; st.state = pd::IdState::Aborted;
        identify_status_text(st, s = {});
        CHECK_STR(s.c_str(), "Aborted: link dropped");
        st.why = pd::IdWhy::Detached;
        identify_status_text(st, s = {});
        CHECK_STR(s.c_str(), "Aborted: unplugged");
        st.why = pd::IdWhy::HardReset;
        identify_status_text(st, s = {});
        CHECK_STR(s.c_str(), "Aborted: hard reset");
        st.why = pd::IdWhy::Stopped;
        identify_status_text(st, s = {});
        CHECK_STR(s.c_str(), "Aborted: stopped");
    }
    {   // identify result card
        pd::IdStatus st;
        pd::VdmInfo v;
        IdCard card;
        CHECK_EQ(int(identify_card(st, v, card)), 0);          // nothing asked yet
        v.identity.valid = true; v.identity.vid = 0x05AC; v.identity.pid = 0x1A2B;
        v.identity.vendor = "Apple"; v.identity.model = "iPad Air 5";
        CHECK_EQ(int(identify_card(st, v, card)), 0);          // not Done
        st.state = pd::IdState::Done;
        CHECK_EQ(int(identify_card(st, v, card)), 1);
        CHECK_STR(card.name.c_str(), "iPad Air 5");
        CHECK_STR(card.vendor.c_str(), "Apple");
        CHECK_STR(card.pd.c_str(), "");
        CHECK_STR(card.wants.c_str(), "");
        st.revision = 0x31200000;
        st.num_sink_pdos = 2;
        st.sink_pdos[0] = 0x0001912C;                           // 5 V 3 A
        st.sink_pdos[1] = (uint32_t(400) << 10) | 150;          // 20 V 1.5 A
        v.identity.model = nullptr;
        identify_card(st, v, card);
        CHECK_STR(card.name.c_str(), "Apple");
        CHECK_STR(card.pd.c_str(), "PD 3.1");
        CHECK_STR(card.wants.c_str(), "wants 20V 1.5A");
        v.identity.vendor = nullptr;
        identify_card(st, v, card);
        CHECK_STR(card.name.c_str(), "Device");
        // key / value rows
        v.num_svids = 2; v.svids[0] = 0x05AC; v.svids[1] = 0x8087;
        st.skipped_mask = 0x24;
        CHECK_EQ(identify_kv_count(st, v), 4);                  // no type / role bits: VID, PID, 2 SVIDs
        v.identity.id_header = 0xC0000000u | (2u << 27);        // host+device capable, peripheral
        CHECK_EQ(identify_kv_count(st, v), 6);
        StrBuf<12> k; StrBuf<16> val;
        identify_kv(st, v, 0, k, val); CHECK_STR(k.c_str(), "VID"); CHECK_STR(val.c_str(), "05AC");
        identify_kv(st, v, 1, k = {}, val = {}); CHECK_STR(k.c_str(), "PID"); CHECK_STR(val.c_str(), "1A2B");
        identify_kv(st, v, 2, k = {}, val = {}); CHECK_STR(k.c_str(), "Type"); CHECK_STR(val.c_str(), "Peripheral");
        identify_kv(st, v, 3, k = {}, val = {}); CHECK_STR(k.c_str(), "USB"); CHECK_STR(val.c_str(), "Host+Device");
        identify_kv(st, v, 4, k = {}, val = {}); CHECK_STR(k.c_str(), "SVID"); CHECK_EQ(int(val.view().size()) <= 11, 1);
        {   // the unanswered steps are named in the footer, with no age and no step numbers
            StrBuf<64> r;
            identify_result_text(st, r);
            CHECK_STR(r.c_str(), "No reply: Modes, Revision");
            st.skipped_mask = 0x2E;
            identify_result_text(st, r = {});
            CHECK_STR(r.c_str(), "No reply: SVIDs, Modes, Sink caps, Revision");
            st.skipped_mask = 0;
            identify_result_text(st, r = {});
            CHECK_STR(r.c_str(), "Identified");
            st.skipped_mask = 0x24;
        }
        v.num_svids = 12;
        for (int i = 0; i < identify_kv_count(st, v); i++) {
            identify_kv(st, v, i, k = {}, val = {});
            CHECK_EQ(int(k.view().size()) <= 9, 1);
            CHECK_EQ(int(val.view().size()) <= 11, 1);
        }
    }
    test_setup_view();
    test_pdo_line();
    return test_report("test_ui_pdtrig");
}
