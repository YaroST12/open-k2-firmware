# Trigger Research Notes — OEM Fast-Charge Trigger Playbooks

> **Review status (2026-11-07):** written by another model and reviewed against the code and hardware results in
> `docs/agents/DETECTION_IMPLEMENTATION_PLAN.md` §1. Where they disagree, the plan wins. Known corrections: Xiaomi 120 W needs the
> SVID 0x2717 auth chain (the 6 A APDOs appear only after it); VOOC clocks without a load (frames are 9 clocks, accept
> 0x04D); UFCS: D+/D- on USB-A (what the K2 implements, USART2) and CC/BMC on USB-C per docs/CHARGING_PROTOCOL_MAP.md §14; DR_Swap/PR_Swap are control types 9/10; several file:line citations moved.


Public-source research to complement `FASTCHARGE_PROTOCOLS.md` (which holds the wire-level facts the K2 firmware implements and the captures made with it — **not duplicated here**). This file covers what that document does *not* contain, with a focus on exact trigger sequences a tester/spoofer must perform, organized per protocol as **Trigger sequence → Expected response → Gotchas**. Community/unverified claims are marked **[uncertain]**.

Sources used: TinkerVault protocol pages, ChargerLAB teardowns, kodikion substack, usbconnectioninformation.com measured guides, HandWiki, OPPO technical paper, Reddit r/UsbCHardware / r/AskElectronics, USB-IF PD spec.

---

## 1. Apple legacy USB-A resistor-divider codes (Apple 1A / 2.1A / 2.4A)

**What it is:** Before USB PD, Apple 5V bricks advertised current capability by holding fixed DC voltages on D+/D- (derived from VBUS through divider resistors, typically 43 kΩ / 49.9 kΩ class — see [usbconnectioninformation](https://usbconnectioninformation.com/guides/charge-macbook-with-old-apple-ipad-charger/) and [pinshop divider description](https://ptwebimg-9.pinshop.com/)). The sink (iPhone/iPad) reads both lines and picks a current limit. This is **not** BC 1.2 (no D+/D- short).

### Trigger sequence (source side — what the K2 must *apply* to emulate an Apple brick)

| Advertised current | D+ | D- | Notes |
|---|---|---|---|
| 0.5 A (5 W brick) | 2.0 V | 2.0 V | [uncertain — some sources list 5 W as D+/D- shorted (BC1.2-style); others 2.0/2.0] |
| 1.0 A (5 W/1 A) | 2.0 V | 2.7 V | Matches K2 classifier: `D+≈2.0V, D-≈2.6–2.8V → "APPLE 1.0A"` |
| 2.1 A (10 W brick) | 2.7 V | 2.0 V | Matches K2 classifier: `D+≈2.7V, 1.9–2.1V on D- → "APPLE 2.1A"` |
| 2.4 A (12 W brick / 29 W) | 2.7 V | 2.7 V | Matches K2 classifier: `D+≈2.7V, D-≈2.7V → "APPLE 2.4A"`; divider ≈ 43 kΩ/49.9 kΩ from 5 V VBUS |

Exact nominal is usually quoted as **2.75 V** (≈2.7 V bucket) and **2.0 V**; the K2 ADC buckets (decivolts 27 vs 20) match. iPhone allows drawing up to the advertised current at 5 V/5.1 V when it sees these codes ([pinshop](https://ptwebimg-9.pinshop.com/), [usbconnectioninformation measured guide](https://usbconnectioninformation.com/guides/charge-macbook-with-old-apple-ipad-charger/)).

### Expected response
- iPhone/iPad: raises draw to the coded limit (verified by usbconnectioninformation: 12 W brick + iPhone 17 Pro Max negotiates full 12 W via the legacy divider).
- **Modern USB-C MacBooks do NOT recognize the Apple divider codes** — through a USB-A→C adapter they fall back to BC 1.2 DCP and cap at **7.5 W** regardless of brick rating (measured on M1 MacBook Air with 5/10/12 W bricks, [usbconnectioninformation](https://usbconnectioninformation.com/guides/charge-macbook-with-old-apple-ipad-charger/)).

### Gotchas
- Voltages must be stable DC, not pulsed — Apple devices sample continuously; pulsing D+/D- is interpreted as other protocols.
- 2.0 V vs 2.7 V discrimination is tight (~0.7 V apart); use the K2's 2.0 V and 2.7 V divider rails, not 3.3 V.
- BC 1.2 DCP (D+/D- short) gives only 1.5 A to Apple devices — Apple codes are strictly better when emulated.

---

## 2. Apple over USB-C (iPhone / iPad / MacBook)

### Trigger sequence
- **Plain USB PD only.** iPhone (15+), iPad Pro/mini and Apple Silicon MacBooks negotiate charging purely over **standard USB PD on CC** — Source_Capabilities → Request → PS_RDY. No proprietary handshake is required or used for power negotiation. (Consensus across [HandWiki](https://handwiki.org/wiki/Engineering:VOOC) comparisons, [usbconnectioninformation](https://usbconnectioninformation.com/guides/charge-macbook-with-old-apple-ipad-charger/), r/UsbCHardware threads.)
- Apple VID **0x05AC** exists in the USB-IF SVID registry ([Infineon community thread](https://community.infineon.com/t5/USB-EZ-PD-Type-C/Discover-SVIDs-Command-response-for-Power-Delivery/td-p/80097)), and Apple devices do answer Discover Identity with modal operation, but **no Apple-specific VDM is used for optimized charging** on MacBooks — charging is standard PD up to the negotiated PDO (e.g. 20 V/5 A = 100 W on M1/M2 Pro/Max via 96/140 W bricks; the 140 W brick needs an EPR 240 W cable). **[uncertain]** whether any recent Mac firmware uses 0x05AC VDMs for anything power-related; none is required to trigger charging.
- Apple Silicon MacBook "reset": if a Mac stops charging, the documented user-level fix is an **SMC reset** (Apple Silicon: shut down, hold power 10 s, or just power-cycle — no cable magic); on the PD side the equivalent is a **hard reset / cable re-plug**, which re-runs the full PD contract. There is no secret CC-level reset sequence; treat "reset" as re-enumeration (Source_Capabilities after tTypeCSinkWaitCap or Hard_Reset).

### Expected response
- Standard PD: `Source_Capabilities` → `Request(RDO)` → `Accept` → `PS_RDY`, then steady VBUS at the contracted PDO. Nothing Apple-proprietary on the wire.

### Gotchas
- Apple 240 W (EPR) charging requires an **EPR e-marked 240 W cable** (Apple Woven Charge Cable or equivalent) — source checks the cable VDM before offering 28/36/48 V EPR PDOs ([usbconnectioninformation e-marker guide](https://usbconnectioninformation.com/guides/usb-c-cable-emarker-chips/)).
- 96/140 W MacBook Pro charging needs a **5 A (100 W+) e-marked cable**; a 3 A cable caps at 60 W even if the brick is 96 W.
- Lightning is irrelevant to the K2 (no CC); skipped as agreed.

---

## 3. OPPO VOOC / SuperVOOC (also OnePlus Warp/Dash, Realme SuperDart) — trigger direction is inverted vs QC/SCP

**Key structural fact (proven by K2 captures, `FASTCHARGE_PROTOCOLS.md` §3):** unlike QC/AFC/FCP/SCP where the *tester/phone is bus master*, in VOOC the **charger is the clock master on D+** and the **phone/tester is the slave answering on D-**. A trigger board therefore does *not* command the charger; it must (a) wake it and (b) answer its clocked frames.

### Trigger sequence (phone/tester side)
1. **Wake the charger with a load pulse**: some VOOC chargers are said to start clocking D+ only after seeing a current draw pulse on VBUS (K2 hardware note 2026-11-02: "needs load" — a PA8 load burst drew no current; the adapter clocked D+ without a load on some attempts, see `FASTCHARGE_PROTOCOLS.md` §3). Public teardowns (ChargerLAB, [HandWiki VOOC](https://handwiki.org/wiki/Engineering:VOOC)) confirm the MCU in the charger monitors VBUS current before enabling the D+ clock.
2. Charger then emits the **wake train + clock frames on D+** (K2 capture: 13× (0 V 506 µs / 3.3 V 510 µs / 0 V 1010 µs / 3.3 V 1510 µs), then 0 V 500 / 3.3 V 500 / 0 V 500 µs). The tester must listen on D+ (PA7 EXTI) with both lines Hi-Z.
3. For each **10-clock frame**, the slave samples/drives **D- 1 µs after each D+ falling edge** (K2 decoder `tools/vooc_edges.py`); command = frame >> 2. Observed init script from a real 120 W SVOOC adapter (OnePlus 15, 2026-11-02 capture): adapter sends `A8 98 98 9E 80 94 98 8A 98 8E 9A 82`; the phone answers with "golden words" (~11 commands over ~12 s) before the session ends.
4. `0xA8` = SuperVOOC adapter-identification command; ACK `0x0C`. The command table is in `FASTCHARGE_PROTOCOLS.md` §3.

### Expected response
- After a correct slave answer, the adapter raises VBUS to the VOOC level: **VOOC ≈ 5 V at 4–6 A** (low-voltage high-current, charge pump inside phone), **SuperVOOC 1.0 = 10 V/5 A (50 W)**, **SVOOC 2.0 up to 20 V/6.25 A (125 W)** with 3 parallel charge pumps ([HandWiki](https://handwiki.org/wiki/Engineering:VOOC)). On USB-C, SVOOC bricks advertise it as high-current PPS-class ranges, e.g. **5–11 V @ up to 10.95 A (120 W)** on USB-A with a 12 A cable, **5–11 V @ 9.1 A (100 W)** with a 10 A cable ([tinkervault VOOC page](https://www.tinkervault.com/usb-fast-charging-protocols/vooc-svooc-dash)).
- If the slave answer is wrong/absent, the adapter stays at 5 V (K2: "no response", next frame reads 0xFF).

### Gotchas
- **Cable is part of the auth**: SVOOC high power is only granted with the original BBK high-current cable (6 A/10 A/12 A USB-A→C, or 6 A USB-C→C). With a standard 5 A cable a 120 W charger caps at 100 W. **[uncertain]** whether the check is an e-marker read or a proprietary cable-ID resistor on CC — community believes resistor/e-marker hybrid; not publicly decoded.
- The D+ clock only starts after a **VBUS load pulse**; without it the charger sleeps (this is exactly the K2 "needs load" failure).
- Frame timing is tight: arm frame start after **0.7 ms of D+ high**, listen in **15 ms slices**, commands arrive **20–30 ms after each reply**, and **D- must float (Hi-Z) while listening** — a pull-down corrupts rising bits (0x98 read as 0x88). K2 capture lessons 2026-11-02.
- The standard **TLC 016-2019** defines VOOC but is paywalled — most public detail is teardown-derived; the kodikion "Deconstructing SuperVOOC" writeup exists ([kodikion.substack.com](https://kodikion.substack.com/p/deconstructing-supervooc)) but is Cloudflare-gated for scraping. **[uncertain]** details from it are not independently verified here.
- The K2 implements only the 5 V handshake — higher levels need the authenticated phone-side exchange.

---

## 4. Huawei SCP / FCP trigger (public view; K2 register-level detail in `FASTCHARGE_PROTOCOLS.md` §2)

### Trigger sequence
1. **BC1.2 HVDCP entry**: pull **D+ = 0.6 V** with D- Hi-Z and wait for the charger to pull D- low (charger closes its D+/D- detection switch). K2 waits up to 2000 ms for the D- drop.
2. Hold **D+ = 0.6 V** permanently; all protocol traffic is **bit-banged on D-** (ACCP single-wire): master ping = D- 2.0 V for **2570 µs**, TX bit `'1'` = 2.0 V for **162 µs**, `'0'` = 0 V; header L41/H41 pulses; frame ends with H 2570 / L 10; CRC-8 poly **0x39**, init 0x00, MSB-first. Bit timing: `include/k2/fastcharge/accp.hpp`.
3. **FCP (discrete 5/9/12 V)**: read capabilities reg **0x21** (non-zero = FCP), write target voltage to **0x2C** in 100 mV units (`0x5A`=9 V, `0x78`=12 V), current to **0x2D** in 100 mA units, then write **0x2B = 0x01** to commit/enable.
4. **SCP (direct charge, low V / high I)**: read **0x80** bit 4 = SCP supported, **0x90** max power, **0x92–0x97** V/I min/max/step; write **0xA0 = 0xC0** (output enable + direct charge), wait **22 ms**, write **0xA1 = 0x06**; clear watchdog bit in **0xA2**; then set target via 16-bit regs **0xB0/0xB1 = V in mV**, **0xB2/0xB3 = I in mA** (big-endian). Read back actual V/I at **0xB8/0xB9, 0xBA/0xBB**. Register map: `FASTCHARGE_PROTOCOLS.md` §2 and `include/k2/fastcharge/accp.hpp`.

### Expected response / power classes
- FCP: **9 V/2 A (20 W), 10 V/2 A** (HW-100200C00); 12 V/2 A exists but no shipping FCP-24 W device found ([tinkervault Huawei FCP/SCP](https://www.tinkervault.com/usb-fast-charging-protocols/huawei-fcp-scp)).
- SCP: **22.5 W (5 V/4.5 A)**, **40 W (10 V/4 A)**, **66 W (11 V/6 A)**, up to **100 W+ (11 V/10 A class, "SSCP" when I_max > 2.2 A per K2 heuristic)** ([tinkervault](https://www.tinkervault.com/usb-fast-charging-protocols/huawei-fcp-scp)).

### Gotchas
- Frames must be retried: K2 sends each AFC/FCP command **3 times, 10 ms apart, all three must succeed**; a malformed frame (wrong parity/inter-byte rule) makes the charger ignore the whole exchange.
- **Watchdog**: SCP has a watchdog (reg 0xA2 bit 2) — if the master stops writing, the charger drops back to 5 V. Keepalive = periodic V/I writes.
- Reset: pulling **D+ = D- = 0 V for 500 ms** returns the charger to 5 V.
- Huawei chargers are tolerant of the D+ 0.6 V bias being present for a long time, but AFC and FCP/SCP share the same physical layer — don't interleave.

---

## 5. Samsung AFC trigger + SFC via PD PPS

### Trigger sequence (AFC, wire detail in `include/k2/fastcharge/afc.hpp`)
1. HVDCP entry: **D+ = 0.6 V**, wait for charger D- pull-down.
2. ACCP master/slave ping exchange on **D-**, then send a **single command byte** `(V_code << 4) | I_code`: **9 V = `0x46`**, **12 V = `0x79`** (`include/k2/fastcharge/afc.hpp`). Each command is sent **3×, 10 ms apart, all must succeed**.
3. Classic AFC levels: **5 V/2 A, 9 V/1.67 A (15 W)**, 9 V/2 A (18 W).

### SFC (Super Fast Charging) — this is just PD PPS
- SFC is **standard USB PD with PPS**, no proprietary D+/D- needed on USB-C. Typical profiles ([tinkervault Samsung AFC/SFC](https://www.tinkervault.com/usb-fast-charging-protocols/samsung-afc-sfc)):
  - 25 W charger: **PD 9 V/2.77 A**, **PPS 3.3–5.9 V @ 3 A** and **3.3–11 V @ 2.25 A**.
  - 45 W (SFC 2.0): **PPS 3.3–11 V @ 5 A** and **3.3–20 V @ 2.25 A** plus 9 V PDO. **[uncertain]** exact 45 W APDO currents vary by model (EP-TA845 vs newer); verify against a specific charger's PDO list.
- Trigger = ordinary PD: request the PPS APDO with the desired voltage/current inside the advertised range.

### Gotchas
- AFC is USB-A only; on USB-C Samsung phones use PD PPS (SFC). AFC ping timing: master ping 2570 µs, bit 162 µs — marginal timing causes silent drops.
- Samsung phones accept QC as a fallback on many models, but SFC proper is PPS.

---

## 6. Xiaomi HyperCharge / MiPPS (120 W trigger)

### Trigger sequence
- **USB-C 120 W HyperCharge is plain PD PPS, gated by the cable**: the charger advertises **PPS 5–11 V @ 6 A** and **5–20 V @ 6 A** (120 W = 20 V/6 A or 11 V/6 A ≈ 66–120 W), but only offers the 6 A APDOs when the attached cable's **e-marker reports a 6 A cable** ([tinkervault HyperCharge](https://www.tinkervault.com/usb-fast-charging-protocols/hypercharge): "up to 120 W with a compatible 6 A USB-C→C cable; with a standard 5 A cable, capped at 100 W"). No proprietary PD VDM is needed for C-to-C; request the 6 A PPS APDO like any other PPS. **[uncertain]** community reports of Xiaomi-specific VDMs exist but none are required for 120 W on USB-C — the 6 A e-marker is the gate.
- **USB-A 120 W/90 W HyperCharge is proprietary on D+/D-** and requires Xiaomi's special **6 A USB-A→C cable** (the cable carries an ID chip/resistor; a generic A→C cable gets only ~20 W even on a Samsung phone per r/UsbCHardware). The exact D+/D- encoding of the A-port protocol is **not publicly decoded** — treat as **[uncertain]**; K2-style approach would be passive sniffing of a genuine Xiaomi A-cable session.
- MiPPS / Surge Charge (33–67 W range) is PPS-based: **3.6–11 V @ 6.1 A (67 W)**, **3.6–20 V @ 4–4.5 A (90 W A-port)** ([tinkervault](https://www.tinkervault.com/usb-fast-charging-protocols/hypercharge)).

### Expected response
- 120 W Xiaomi C charger + 6 A cable + Xiaomi phone: PD contract on **20 V/6 A (PPS 5–20 V 6 A)** reaching ~117–120 W at the wall. Non-Xiaomi phone on same charger: standard PD/PPS only (no 6 A unless cable allows; phone limits anyway).

### Gotchas
- The **6 A e-marker is mandatory**: spoofing 6 A without a 6 A cable risks overheating connectors — the source deliberately limits current to the cable rating. This is a source-side check the K2 must respect when *spoofing*.
- 210 W HyperCharge (Redmi Note 12 Discovery) uses 5 V/… high-current + proprietary; **[uncertain]** public trigger details.

---

## 7. Meizu mCharge / others (brief)

- **Meizu mCharge 3.0 ≈ 24 W**; **Super mCharge = 11 V/5 A (55 W)** with a charge pump that halves input voltage ([tinkervault mCharge](https://www.tinkervault.com/usb-fast-charging-protocols/mcharge), [ZDNET](https://www.zdnet.com/article/phone-battery-quick-charge-breakthrough-meizu-says-its-zero-to-full-in-20-minutes/)). The D+/D- wire encoding is **not publicly documented [uncertain]** — sniff-only. Low priority for the K2.
- **Lenovo/others**: no stable public trigger; mostly QC/PD clones. Skip.

---

## 8. Cross-protocol reliability gotchas (for the trigger playbook)

1. **E-marker / cable checks before high power (USB-C PD sources)**: the source reads the cable's VDM (Discover Identity SOP') before offering >3 A; >100 W additionally requires an EPR (240 W) cable; Xiaomi 6 A and VOOC 10/12 A need their specific e-marked/ID cables. A tester emulating a sink must present a matching e-marker or the source caps current. ([usbconnectioninformation e-marker guide](https://usbconnectioninformation.com/guides/usb-c-cable-emarker-chips/))
2. **VOOC needs a VBUS load pulse to wake the charger clock**; QC/AFC/FCP/SCP need the **D+ 0.6 V HVDCP entry** first. Wrong preamble = charger stays at 5 V.
3. **Timeouts/retries**: AFC/FCP/SCP commands are sent 3× with 10 ms spacing and all must ACK (K2 firmware behavior); VOOC sessions time out in seconds if the slave stops answering ("no response" on 0xFF).
4. **Keepalives**: SCP watchdog (reg 0xA2) drops the charger to 5 V if writes stop; VOOC needs continuous frame answers; PD needs periodic PD traffic or the source may hard-reset.
5. **Reset**: D+/D- = 0 V for 500 ms resets HVDCP/AFC/FCP/SCP chargers to 5 V; PD reset = Hard_Reset or re-plug. Apple Silicon Mac "reset" = SMC/power cycle, i.e. plain re-enumeration — no proprietary CC sequence.
6. **Voltage-bucket ambiguity**: 0.6 V on D+ is shared by HVDCP/QC/AFC/FCP/SCP — always verify the *response* (bus voltage change or slave ping), not just the line state, before claiming a protocol (K2 classifier does exactly this).
