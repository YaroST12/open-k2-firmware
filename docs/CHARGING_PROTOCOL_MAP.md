# Charging Protocol Map — Proprietary Fast-Charge Families vs. Open Standards

A vendor-by-vendor map of the fast-charging protocol **families** in the phone/laptop ecosystem, written for the
WITRN K2 tester / QC+PD spoofer project. Where `docs/FASTCHARGE_PROTOCOLS.md` covers the **wire-level**
D+/D- protocols the K2 speaks, this document maps the **families**: who owns them, what transport they run on,
their power tiers, and — critically for a tester — whether they need a special cable or are just PD/PPS in disguise.

Sources are inline markdown links; community/Reddit claims are marked *(community)* and anything not verified
against a primary or reputable secondary source is marked *(uncertain)*. Compiled 2026-11 from web research.

---

## 1. Summary comparison table

| Family | Vendor(s) | Max power (wired) | Transport | Needs special cable? | Aligns with |
|---|---|---|---|---|---|
| VOOC / SuperVOOC | BBK Electronics brands: OPPO, OnePlus (Warp/Dash), realme (SuperDart/UltraDart) | 240W (20V/12A, realme GT Neo5); OnePlus 120W brick = 11V/11A @230V (80W @120V); PD on same port capped at 65W | **D+/D- serial on both USB-A and USB-C**: charger is clock master on D+ (CLK), sink answers on D- (DATA) — OPPO technical paper; K2 edge-capture of a OnePlus 15 on a 120W brick *(HW)*. SVID 0x22D9 is advertised on Discover SVIDs but no PD-VDM unlock is documented | **Yes** — proprietary A-to-C cable (extra GND_DRAIN pin + auth IC) or high-current C-to-C (e-marker VID 0x22D9, whatcable #525) | None (UFCS on new models) |
| Xiaomi HyperCharge / Surge (MiPPS) | Xiaomi, Redmi, POCO | 120W (11V/6A, 20V/6A); 210W (17–20V/10.5A); 300W demoed MWC 2023 | USB-C: **PD PPS + unstructured VDM auth under SVID 0x2717** (MiCode kernel); charger raises APDO current 5A→6A only after VERIFIED + 6A cable *(HW: CUKTECH 10 GaN 120W)* | **Yes** — proprietary 6A cable; 5A cable caps at 100W | Falls back to PD PPS / QC (~23–60W on generic PPS) |
| Huawei SuperCharge (SCP/FCP) | Huawei, (Honor until split) | phone-directed 100W (20V/5A; 66W tier = 11V/6A); laptop bricks SCP 3.3–21V up to 127W | **D+/D- ACCP single-wire register protocol** (D+ held 0.6V, frames on D-, 160µs UI, CRC-8) — now public in Huawei GPL kernel + CCSA draft App. B; PD/PPS on C bricks | USB-A: no special cable; high-W USB-C: 5A e-marker | PD/PPS/UFCS fallback on C bricks |
| Samsung AFC → SFC 1.0/2.0/3.0 | Samsung | 15W (AFC) → 45W (SFC 2.0) → 60W (SFC 3.0, Galaxy S26); 65W OEM brick = fixed 20V/3.25A PDO. **Never >5A** | AFC: D+/D- handshake; SFC 1.0/2.0/3.0: **pure PD PPS** (SFC 2.0 = 3.3–11V @ ≤4.5A, OEM 4.05A; S25+/Ultra also 45W via PPS 15V/3A) | SFC 2.0 needs a **5A (100W) e-marked C-to-C cable** (except the 15V/3A path) | **PD PPS** for SFC 1.0/2.0/3.0 |
| vivo FlashCharge / iQOO | vivo, iQOO | 200W (20V/10A) | D+/D- UART (USB-A bricks); CC on USB-C | **Yes** — 10A-rated proprietary C-to-C cable for 120–200W | PD fallback; UFCS on new models |
| Lenovo/Moto TurboPower | Lenovo, Motorola | 68W (11V/6.2A); 125W (20V/6.25A) | **PD PPS with a non-standard >5A APDO current** treated as a MOTO-private mode, gated by the 6.5A e-marker cable ("6.25A only when PSU, cable and device all agree"); Lenovo slim-tip is fully proprietary | Moto 125W ships with 6.5A e-marked cable (P/N 78811036); 3rd-party: TALIX Zeta 240W, Lenovo 100W GaN ("MOTO Max 105W"); Lenovo laptops: slim-tip only | **PD PPS** + private >5A extension |
| Honor SuperCharge / Magic Power | Honor | 100W phone (20V/5A PD + SCP on D+/D-); laptop bricks SCP 20V/6A = 120W (HN-200600C02), 127W (HN-200500CP0). VID 0x339B = Honor Device Co., Ltd. (USB VID, not PD SVID) | D+/D- SCP (USB-A); PD PPS / UFCS on USB-C | USB-C models: standard cables | PD PPS / UFCS |
| ASUS ROG HyperCharge / BoostMaster | ASUS | 65W = PD 3.0 PPS 3.3–21V @ 3.25A (rog.asus.com spec); 30W adapter | **PD 3.0 + PPS**, no proprietary >5A mode | No proprietary cable; needs PD+PPS charger | **PD PPS** |
| Nubia/ZTE RedMagic Super Flash Charge | ZTE Nubia (RedMagic); VID 0x19D2 | 165W = exactly **20V/8.25A** (NB-A20825C; rides PPS APDOs 3.6–11V/8.25A and 3.6–20V/8.25A; max fixed PDO only 20V/3.25A); 120W from 10 Pro+ on | proprietary, wire format not publicly documented *(uncertain)*; standard PD/PPS/QC/SCP modes confirmed by KM003C | **Yes** — bundled 9A-marked C-to-C whose e-marker still reads 20V/5A | PD/PPS fallback (3rd-party PPS reaches ~50–100W; >100W needs OEM brick) |
| Meizu Super mCharge | Meizu | 80W (11V/5A era 55W) | D+/D- handshake (USB-A, HVDC) | USB-A proprietary cable | None |
| Qualcomm Quick Charge 2.0–5 | Chipset-wide (all Android SoCs) | 18W (QC2) → 100W+ (QC5), EPR 240W | QC2/3: D+/D- HVDCP; QC4/4+/5: **PD PPS**; QC5 adds AVS | No — standard PD cables (5A for >100W/EPR) | **PD PPS / PD 3.1 AVS** for QC4+ |
| MediaTek Pump Express 1.0–5.0 | MediaTek (Helio/Dimensity OEMs) | ~24W (PE1/2) → 120W (PE4/5 via PPS) | PE1/2/3: D+/D-; PE 4.0/5.0: **PD PPS-based** | No for PPS-based tiers | **PD PPS** for PE4/5 |
| UFCS (fusion fast charging) | CAICT/TAF + Huawei, Xiaomi, OPPO, vivo, Honor, etc. | bands 5/10/20/30V (to 36V); 30V档 for >200W; no power cap (320W realme demo 2024-08); UFCS 2.0 (2025-05-22): no-auth ≤40W | **D+/D- full-duplex UART (8N1, 115200 default; 2022 adds 57600/38400) on both A and C ports** — per T/TAF 083 standard PDFs; CC carries plain PD/PPS alongside. Vendor SHA256 auth above a vendor power threshold (App. F); cable e-label mandatory >6.5A (§6.2). K2 implements the D+/D- form (USART2) | standard cables ≤4A; e-label recommended 4–6.5A, mandatory >6.5A | **Own T/TAF standard** (083-2021 → 083-2022 → 083-2024/T/CCSA 393-2024; phase-2 T/TAF 266-2025), PD-adjacent |
| USB PD SPR AVS / EPR AVS | USB-IF standard | SPR AVS to 100W; EPR AVS to 240W (48V) | CC-line BMC | EPR (>100W) **requires** 240W EPR e-marker cable | **The standard itself** |
| Apple (reference point) | Apple | 20W–30W+ (plain PD) | CC-line PD | Standard PD cable | **Plain PD, no proprietary protocol** |

---

## 2. OPPO / OnePlus / realme — VOOC family (all BBK Electronics brands, shared Flash Charge tech)

- **VOOC** ("Voltage Open Loop Multi-step Constant-Current Charging"), launched 2014: low-voltage **high-current**
  approach — 5V/4A (20W), later 5V/6A (30W, VOOC 3.0 / OnePlus "Warp Charge 30"). Uses a **dual-cell series
  battery** and charge pumps inside the phone so the cable never carries high voltage. Handshake and voltage
  stepping run as a serial/UART exchange on **D+/D-** over USB-A ([OPPO Flash Charging technical paper](https://www.oppo.com/content/dam/oppo/en/mkt/newsroom/story/flash-charging-technical-pape/OPPO%20Flash%20Charging%20Technical%20Paper.pdf); [Wikipedia VOOC](https://en.wikipedia.org/wiki/VOOC)).
- **SuperVOOC** (2016): 10V/5A = 50W into the series dual-cell pack; SuperVOOC 2.0 (2019) 10V/6.5A = 65W.
  USB-A bricks use a **proprietary USB-A→C cable with an extra signal pin and oversized contacts** rated up to
  ~9.1A ([tinkervault protocol library](https://www.tinkervault.com/usb-fast-charging-protocols/)).
- **SUPERVOOC 80W/100W/150W/160W**: 10V/10A (100W) up to **20V/7.5A (150W)** at the brick; the phone converts
  down (150W ≈ 20V/7.5A in → 10V/15A → 5V/15A per cell). 160W bricks also speak PD/PPS
  ([ET India Times on the 160W adapter](https://economictimes.indiatimes.com/tech/tech-trends/oneplus-10r-160w-supervooc-charger-supports-pps-pd-charging/articleshow/91178049.cms)).
- **240W SUPERVOOC** (2023, **realme GT Neo5** — realme GT3 for global markets; *not* a OnePlus-branded phone):
  **20V/12A** on the cable, ~10V/24A at the battery via three charge pumps, 4600 mAh 0–100% in ~9 min
  ([Engadget on the GT Neo5 240W](https://www.engadget.com/realme-gt-neo5-240w-supervooc-charging-100557408.html);
  [tinkervault SuperVOOC deconstruction](https://kodikion.substack.com/p/deconstructing-supervooc) is community-grade).
  OnePlus-branded phones cap lower: **100W** on flagships (OnePlus 12/13, 10V/10A; OnePlus 15 is 80W/100W) and
  **150W** on the Ace 5 Ultra (20V/7.5A).
- **Transport on USB-C**: still **D+/D-** (the USB 2.0 pair exists in C-to-C cables): OPPO's own technical paper
  states "D+ 用于传输握手信号的 CLK 数据，D– 用于传输握手信号的 DATA" — charger clocks D+, phone answers on
  D-. Confirmed on our hardware: 71k-edge D+/D- capture of a OnePlus 15 on a SUPERVOOC 120W brick
  (`docs/FASTCHARGE_PROTOCOLS.md` §3). The brick also advertises PD on CC (capped at 65W: 20V/3.25A +
  PPS 5–21V/3.25A — oneplus.com, ChargerLAB teardown of OSABBCBBAC) and lists **SVID 0x22D9** on Discover SVIDs
  *(HW capture, OSABBCBBAE)*; **no public source documents a 0x22D9 VDM unlock** — realme's GPL kernel
  (`oppo_vooc.c`) has no PD-VDM path, so treat 0x22D9 as discovery-only *(uncertain, open item §18.5)*.
- **Cable e-marker**: C-to-C SuperVOOC cables carry an e-marker with **VID 0x22D9**; whatcable #525 shows the
  12A cable's Cable VDO 0x700A2E40 = standard **5A @ 50V** field — the "12A" is marketing/vendor handshake, there
  is no PD-standard >5A field. USB-A cables use an extra plug pin (USB3 GND_DRAIN) + TI single-wire auth IC.
- **Cable requirement**: full power is gated on OPPO's own cable ("Without OPPO's cable, the system refuses to
  deliver high power" — [kodikion Substack deconstruction](https://kodikion.substack.com/p/deconstructing-supervooc)). USB-A SuperVOOC cables have the extra signal pin; C-to-C versions are high-current proprietary.
- **AirVOOC**: wireless only — 40W/50W Qi-derived proprietary wireless, 15W Qi fallback ([EFTM AirVOOC 50W review](https://eftm.com/2025/05/review-oppo-airvooc-50w-wireless-charger-wireless-speed-at-its-best-262181)). Not relevant to a wired tester.
- **UFCS**: recent OPPO/realme/OnePlus models advertise UFCS compatibility alongside SuperVOOC (first port UFCS up to 320W + SuperVOOC 150W demoed — [tinkervault UFCS page](https://www.tinkervault.com/usb-fast-charging-protocols/ufcs)).

## 3. Xiaomi HyperCharge / Surge Charge (MiPPS)

- Tiers: **67W, 90W, 120W** (USB-A and USB-C), **210W** (Redmi Note 12 Explorer/Discovery, 2022-10-27; 17–20V/10.5A
  per [tinkervault HyperCharge](https://www.tinkervault.com/usb-fast-charging-protocols/hypercharge)), **300W demoed
  at MWC 2023-02-28** (not production). mi.com official for the 120W combo: 11V/6A, 20V/6A.
- **Transport on USB-C (validated 2026-11)**: standard PD PPS **plus an unstructured-VDM authentication chain
  under SVID 0x2717** — primary code in [MiCode/Xiaomi_Kernel_OpenSource](https://github.com/MiCode/Xiaomi_Kernel_OpenSource)
  (`courbet-r-oss` branch, `drivers/usb/pd/usbpd.h`: `USB_PD_MI_SVID 0x2717`, uVDM cmds 1–10 = version/voltage/
  temp/session-seed/auth-digest/VERIFIED/…; 4-word payloads byte-swapped; on `pd_verifed=1` the phone sends
  Get_Source_Cap and re-reads caps). The charger raises its PPS APDO current **5A→6A only after VERIFIED**
  *(HW: CUKTECH 10 GaN 120W, VID 0x2B01 — opener `0x27170101`, chain `0x27170201..0A`, then APDOs at 6A)*.
  Public reimplementations with published digest keys: [ianchb/xiaomi-mipps-auth](https://github.com/ianchb/xiaomi-mipps-auth)
  (HMAC-SHA256 over challenge‖adapter_id) and forks. On USB-A it is the proprietary "Surge Charge / MiPPS"
  D+/D- protocol.
- **Cable gating**: 120W needs a **proprietary 6A USB-C cable**; with a standard 5A (100W/240W) cable the phone
  caps at 100W ([tinkervault HyperCharge](https://www.tinkervault.com/usb-fast-charging-protocols/hypercharge)).
  The 210W tier needs the bundled proprietary C-to-C (17V/10.5A, 20V/10.5A).
- Falls back to ~50W on generic PD/PPS chargers; Snapdragon Xiaomi phones also do QC.

## 4. Huawei SuperCharge (SCP / FCP)

- **FCP** (Fast Charge Protocol, 9V/2A) and **SCP** (SuperCharge Protocol) run a **D+/D- ACCP single-wire
  register protocol** (D+ held 0.6V at BC1.2 entry, frames on D-, 160µs UI, odd parity, CRC-8/0x39; regs 0x80 =
  SCP-supported, 0xA0 = control (0xC0 = output enable), 0xB0/B1 = voltage mV, 0xB2/B3 = current mA) — the K2 spec
  covers the wire patterns (`docs/FASTCHARGE_PROTOCOLS.md` §2) and the register map is now public in
  [Huawei's GPL kernel `adapter_protocol_scp.c`](https://github.com/Huawei-Dev/android_kernel_huawei_kirin970),
  the public [CCSA draft App. B](https://qxb-img-osscache.qixin.com/standards/cffee4628ca228767edbc6482c25ca32.pdf)
  and [kralonur/embedded-charging-protocols-rs](https://github.com/kralonur/embedded-charging-protocols-rs).
  Tiers: 22.5W (FCP 9V/2A), **40W** (10V/4A), **66W** (SCP 11V/6A), **100W** "Super Turbo" (= **20V/5A** on C)
  ([PowerRadix roundup](https://www.powerradix.com/blog/usb-fast-charging-protocols-everything-you-need-to-know/);
  [tinkervault FCP/SCP](https://www.tinkervault.com/usb-fast-charging-protocols/huawei-fcp-scp)). Laptop bricks go
  higher over C: SCP 3.3–21V up to **127W** (HONOR HN-200500CP0, MateBook X Pro 140W —
  [ChargerLAB teardown](https://www.chargerlab.com/teardown-of-honor-100w-gan-charger-for-laptop-hn-200500cp0/)).
- USB-C Huawei bricks (65W+) are largely **PD/PPS** with SCP layered on the C port. No proprietary cable needed on
  USB-C beyond a 5A e-marked cable for 100W. 0x12D1 is Huawei's **USB VID** (usb.ids), not a documented PD SVID —
  no public VDM usage. Newer bricks add UFCS (huawei.com product pages).

## 5. Samsung AFC / Super Fast Charging

- **AFC** (Adaptive Fast Charging, ~15W, 9V/1.67A): proprietary **D+/D- handshake** on USB-A/Micro-USB era; the K2
  implements the ACCP-style single-wire ping/bit-bang (`include/k2/fastcharge/accp.hpp`).
- **SFC 1.0 (25W)** and **SFC 2.0 (45W)** on USB-C are **pure PD PPS**: SFC 2.0 = PPS **3.3–11V @ 4.05A**
  (≈45W) and **requires a 5A/100W e-marked C-to-C cable**; with a 3A cable it drops to 15–25W *(community, r/samsung)*
  ([reddit r/samsung SFC 2.0](https://www.reddit.com/r/samsung/comments/1563e9f/question_about_super_fast_charging_20/)).
- **SFC 2.0 nuance**: S25+/Ultra also reach 45W via **PPS 15V/3A without a 5A cable** *(community, r/samsung 1iinaub)*;
  OEM 45W brick measures PPS 3.3–11V @ **4.05A** (the "5A" figure is the cable rating).
- **SFC 3.0 (60W)** debuted with the **Galaxy S26** series: PPS range to 20V at ≥3A — still just PD PPS
  ([tinkervault SFC page](https://www.tinkervault.com/usb-fast-charging-protocols/samsung-afc-sfc)). Samsung's
  separate 65W *adapter* (EP-T6530) achieves 65W via a fixed **20V/3.25A PDO** (teardown: post.smzdm.com/p/a606694g) —
  no Samsung mode ever exceeds 5A.
- **Notable surprise**: Samsung's "Super Fast Charging 2.0/3.0" is literally PD PPS — any PPS charger + 5A cable
  gets you there. No proprietary cable, no proprietary signalling on USB-C.

## 6. vivo / iQOO FlashCharge

- Tiers: 33W, 44W, 55W, **120W (20V/6A)**, **200W (20V/10A)** (iQOO 9/10 Pro era) with dual-cell batteries and
  in-phone charge pumps ([vopmart iQOO 120W adapter listing](https://www.vopmart.com/catalog/product/view/_ignore_category/1/id/17455/s/vivo-iqoo-120w-super-flashcharger/);
  [ChargerLAB 120W teardown](https://www.chargerlab.com/teardown-of-vivo-iqoo-120w-flash-charge-gan-charger-v12060l1b0-cn/)).
- USB-A bricks use **D+/D- UART** (the K2 spec §7 covers the vivo/UFCS UART-on-D+/D- pattern). The exact VFCP
  wire format on USB-C is **not publicly documented** *(uncertain — open item §18.5)*; VIVO VFC is a
  115200-8N1 UART on D+/D- *(HW)*, see `docs/FASTCHARGE_PROTOCOLS.md` §5. Charger side is measurable: 120W bricks also do PD 20V/5A,
  PPS 5–20V/5A and UFCS 33–44W ([tinkervault VFCP](https://www.tinkervault.com/usb-fast-charging-protocols/vfcp)).
  0x2D95 is vivo's **USB VID** (usb.ids); no PD SVID documented.
- **Cable**: 120W+ needs vivo's **10A-rated proprietary C-to-C cable**; 200W marketing bundles a 10A cable.
- Newer vivo/iQOO devices support **UFCS**.

## 7. Lenovo / Motorola TurboPower

- Motorola **TurboPower 68W/125W** USB-C chargers: 68W = 11V/6.2A; 125W = 20V/6.25A. Validated 2026-11: the brick
  advertises standard PPS APDOs at 5A **plus a MOTO-private 20V/6.25A and 5–20V/6.25A extension** (ChargerLAB /
  chongdiantou teardown of MC-1258: "MOTO 私有协议"; a tester requesting 20V/6.25A "violates PD/PPS spec as it
  exceeds 5A" — r/UsbCHardware 1g9u7kn). The >5A level is gated by the **6.5A e-marked C-to-C cable** shipped with
  the 125W kit (P/N 78811036) — "it will only do 6.2A when the Power Supply, cable, and device all report that they
  support it" *(community, r/UsbCHardware 10aeuo6)*. Third-party: TALIX Zeta 240W (MCZT240A2) and Lenovo Multi-Port
  100W GaN ("MOTO Max 105W", ChargerLAB) implement the MOTO mode ([tinkervault moto-turbopower](https://www.tinkervault.com/usb-fast-charging-protocols/moto-turbopower)).
  → **PD PPS + private >5A extension; 6.5A cable for full 125W.** Lenovo Legion Y90 phone is 68W-tier, not 100W.
- Lenovo laptop side: proprietary **slim-tip (rectangular yellow-tip) connector**, not USB-C PD — noted in the K2
  quirks log as `proprietary-power-path` (`docs/CHARGER_QUIRKS.md`). USB-C Lenovo laptops are plain PD.

## 8. Honor Magic Power / SuperCharge

- Honor (post-split from Huawei) SuperCharge: **100W adapter = PD 20V/5A plus SCP on D+/D-** (honor.com product
  page; tinkervault). USB-C models charge via **PD PPS and UFCS**. No 200W Honor product exists publicly.
  Above-5A exists only on Honor *laptop* bricks: **HN-200600C02 = SCP 120W (20V/6A)** over USB-C
  ([ChargerLAB teardown](https://www.chargerlab.com/teardown-of-honor-120w-gan-charger-hn-200600c02/)) and
  HN-200500CP0 = SCP 127W. **VID 0x339B = Honor Device Co., Ltd.** (usb.ids) — a USB VID, not a documented PD SVID.
  No proprietary cable on USB-C beyond standard rules.

## 9. ASUS ROG HyperCharge / BoostMaster

- ROG phones: **HyperCharge 65W** requires a charger doing **PD 3.0 + PPS** to feed both half-batteries
  simultaneously; with a generic PD charger you get ~30W ("HyperCharge 30") because only one cell is fed
  *(community, ASUS ZenTalk / r/UsbCHardware)* ([ZenTalk thread](https://zentalk.asus.com/t5/rog-phone-8/how-to-enable-hypercharge-65/td-p/430285/page/2)).
- → **PD PPS in disguise**, no proprietary cable.

## 10. Nubia / ZTE RedMagic "Super Flash Charge"

- RedMagic 8 Pro+ / 9 Pro+ (CN): **165W = exactly 20V/8.25A**, charger model **NB-A20825C**; the 165W rides PPS
  APDOs 3.6–11V/8.25A and 3.6–20V/8.25A (max fixed PDO is only 20V/3.25A) — chongdiantou/KM003C measurements
  ([chongdiantou 363737](https://www.chongdiantou.com/archives/363737.html),
  [GizChina 3C cert](https://www.gizchina.com/2022/02/01/nubia-red-magic-7-pro-with-165w-fast-charging-passed-3c-certification/)).
  From RedMagic 10 Pro+ on, max dropped to 120W; **there is no 210W RedMagic** (210W is Redmi). Wire format of the
  165W mode is not publicly documented *(uncertain — open item §18.5)*; standard PD3.0/PPS/QC/SCP/FCP/AFC modes
  are confirmed. **Requires the bundled 9A-marked proprietary C-to-C cable** — but its e-marker reads only 20V/5A
  to standard PD tools, so the >5A capability is out-of-band. Third-party PPS chargers reach ~50–100W; >100W needs
  the OEM brick *(community, r/RedMagic 1h4og4m; chongdiantou compatibility tests)*. USB VID 0x19D2 = ZTE/nubia.

## 11. Meizu Super mCharge

- 2017-era **Super mCharge 55W (11V/5A)** HVDC over USB-A with D+/D- handshake, charge-pump halves the voltage to
  double current ([GSMArena](https://www.gsmarena.com/super_mcharge_can_boost_your_meizu_from_0_to_60_in_10_minutes-blog-23697.php);
  [tinkervault mCharge](https://www.tinkervault.com/usb-fast-charging-protocols/mcharge)). Legacy; later Meizu phones moved to PD PPS.

## 12. Qualcomm Quick Charge 2.0 → 5

(Also covered in `docs/agents/QC_RESEARCH.md`; the QC3 stepping loop in `docs/FASTCHARGE_PROTOCOLS.md` §1.)

- **QC 2.0** (2013): HVDCP on **D+/D-**, fixed 5/9/12/20V. **QC 3.0** (2015): same D+/D- but **200mV steps**
  3.6–12V (Class A) / up to 20V (Class B). **QC 3+** (2020): **20mV steps**.
- **QC 4 / 4+** (2018/2020): mandates **USB PD 3.0 PPS** (20mV steps, 3.3–21V) — i.e. QC4+ chargers are PD PPS
  chargers with QC branding ([szapphone QC4 guide](https://www.szapphone.com/blog/qualcomm-qc-4-0-guide/)).
- **QC 5** (2020): up to **100W+**, 3.3–20V, PD PPS-compatible; QC 5 with **PD 3.1** supports **SPR AVS** (15–20V,
  100mV steps) and **EPR up to 240W** ([Qualcomm QC5 page](https://www.qualcomm.com/products/features/quick-charge);
  [androxus standards explainer](https://androxus.com/blog/images/ampere-watt-volt-explained/hero.webp/)). QC5's own
  finer-grained mode is sometimes described as AVS-style 10mV-class adjustment *(uncertain — Qualcomm does not
  publish the step size; treat 20mV PPS as the guaranteed floor)*.
- No special cable beyond standard PD rules (5A e-marker for >100W / EPR 240W).

## 13. MediaTek Pump Express

- **PE 1.0/Plus (2014)**: D+/D-, 5V→ up to 10.5V coarse steps (~24W). **PE 2.0**: 5/7/9/12V. **PE 3.0**: 10mV-class
  fine steps 3.6–12V via D+/D- with direct-charge. **PE 4.0 / 5.0**: moved to **USB PD PPS** (PE4.0 is PPS-based,
  up to 30W+; PE5 targets 120W-class on PPS) — [MediaTek PE white paper (PDF4PRO mirror)](https://pdf4pro.com/view/mediatek-pump-express-introduction-5974cf.html);
  [tinkervault Pump Express](https://www.tinkervault.com/usb-fast-charging-protocols/mediatek-pe).
- → Old PE = D+/D-; modern PE (Dimensity era) = **PD PPS in disguise**, no special cable.

## 14. UFCS — Universal Fast Charging (China MIIT/TAF standard)

- **T/TAF 083-2021 《移动终端融合快速充电技术规范》**, released 2021-05-28 by TAF/CAICT with Huawei, OPPO, vivo,
  Xiaomi; superseded by **T/TAF 083-2022**, then **T/TAF 083-2024 (T/CCSA 393-2024)** plus phase-2 **T/TAF 266-2025**;
  full PDFs public at [taf.org.cn](https://www.taf.org.cn/StdQuery.aspx?type=TAF&num=T%2FTAF+083-2021).
  **UFCS 2.0 announced 2025-05-22** (no-auth interop ≤40W, reverse charging, mandatory Power Change)
  ([tinkervault UFCS 1.0/2.0](https://www.tinkervault.com/usb-fast-charging-protocols/ufcs);
  [CAICT/TAF 2025 conference](https://gma.caict.ac.cn/plat/news/ufcs-2025-industry-promotion-meeting-held-in-shenzhen)).
  ITU-T **L.1004 (07/2025)** internationalized UFCS ([ITU-T L.1004](https://www.itu.int/rec/T-REC-L.1004)).
- **Signalling transport (validated against the standard PDFs 2026-11)**: **D+/D- full-duplex UART on both USB-A
  and USB-C ports** — the standard states devices communicate over the D+/D- channel (8N1 frame, 115200 bps
  default; the 2022 revision adds 57600/38400 fallbacks). CC is *not* used by UFCS; on C ports, plain PD/PPS runs
  on CC alongside. This matches what the K2 implements: UFCS on USART2 (PA2/PA3 = D-/D+, 115200 8N1) —
  `docs/FASTCHARGE_PROTOCOLS.md` §4. *(Earlier revision of this doc said "CC-line BMC" — that was wrong.)*
- Voltage档: **5V: 3.4–5.5V, 10V: 5.5–12V, 20V: 12–21V, 30V: 21–36V** (30V档 reserved for >200W supplies) —
  per the T/TAF 083 standard text.
- **Authentication**: per the standard, when UFCS power exceeds a vendor-set threshold the sink must perform
  vendor-defined auth using **SHA256** (App. F of T/TAF 083-2021); UFCS 2.0 (2025) allows **unauthenticated ≤40W**
  cross-brand ([ITU-T L.1004](https://www.itu.int/epublications/publication/itu-t-l-1004-2025-07-universal-fast-charging-solution-for-mobile-terminals)).
- Power: no standard power cap (the 2022 revision even removed the old 36V/30A output entry); realme demoed
  **320W over UFCS** on 2024-08-14 (port 1: UFCS 320W + SuperVOOC 150W; port 2: PD 65W —
  [GSMArena](https://www.gsmarena.com/realme_320w_supersonic_charge-news-64114.php)). TAF certification: 279
  products as of 2026-02. >5A is in-spec for UFCS.
- **Cable**: T/TAF 083 §6.2 — cable e-label **mandatory above 6.5A**, recommended 4–6.5A, none needed ≤4A.
  Standard PD cable rules otherwise apply ([Satechi e-marker guide](https://satechi.net/blogs/identifying-usb-c-e-mark-cables)).

## 15. PD 3.1 AVS / EPR (the standard underneath QC5 & 240W UFCS)

- **SPR** (PD 2.0/3.0): fixed 5/9/15/20V, ≤100W (20V/5A); **PPS**: 3.3–21V in **20mV steps**, current 50mA steps.
- **PD 3.1 EPR**: adds fixed **28/36/48V** → 140/180/**240W (48V/5A)**; **AVS** in EPR adjusts 15–48V in **100mV
  steps**; SPR AVS covers 15–20V. ([wowohcool PD 3.1 explainer](https://www.wowohcool.com/blog/usb-c-pd-3-1-explained/);
  [Plugable PD blog](https://plugable.com/); [androxus](https://androxus.com/blog/images/ampere-watt-volt-explained/hero.webp/)).
- **240W requires a 240W EPR e-marker cable** (50V/5A/EPR-capable bits, ≥53.65V working voltage); a 100W cable caps
  any EPR source at 100W ([r/UsbCHardware e-marker discussion](https://www.reddit.com/r/UsbCHardware/comments/1f3vhbt/emarker_says_5a_x_50v_but_240w_is_the_standard/);
  [usbconnectioninformation 100W vs 240W cables](https://usbconnectioninformation.com/guides/usb-c-100w-vs-240w-cables/)).

## 16. Plain-PD reference points

- **Apple**: 20W/30W/40W USB-C adapters are **plain USB PD (with PPS on some)** — no proprietary wired protocol;
  iPhone fast charge is just PD ([GearExplained iPhone charger guide](https://gearexplained.com/guides/usb-c-charger-for-iphone/)).
- **Samsung 45W/65W**: plain PD PPS + 5A cable (see §5). These are the "no-surprises" baselines a tester should
  always pass.

## 17. E-marker / special-cable matrix

| Protocol / tier | Cable requirement | Why |
|---|---|---|
| PD SPR ≤60W | none (3A cable) | USB-C base rule |
| PD SPR 100W (20V/5A) | **5A e-marker** mandatory (>3A) | USB-C/PD spec |
| PD EPR 140–240W / UFCS 240W / QC5-EPR | **240W EPR e-marker** mandatory | source refuses EPR without it (K2 quirks: `cable-gated-port`, `strict-ra-cable-check`) |
| VOOC/SuperVOOC 50–240W | **proprietary OPPO/OnePlus/realme cable** (extra pin on USB-A; high-current C-to-C) | vendor lock-in; brick refuses full power otherwise |
| Xiaomi HyperCharge 120W / 200W / 210W | **proprietary 6A (120W) / bundled 10.5A (210W) cable** | current >5A + vendor handshake in cable |
| vivo/iQOO 120W / 200W | **proprietary 10A C-to-C** | high current |
| RedMagic 120–165W+ | **proprietary 8A+ C-to-C** | high current |
| Samsung SFC 2.0/3.0 (45/65W) | standard **5A/100W e-marked** cable | plain PPS, no vendor lock |
| Lenovo/Moto TurboPower 125W | **6.5A e-marked** (bundled) | PPS at 20V/6.25A exceeds 5A |
| Huawei SCP USB-A / Honor USB-A / Meizu mCharge / QC2-3 / AFC / FCP | none — USB-A D+/D- protocols | no CC/e-marker exists on USB-A |
| QC4/4+/5, PE4/5, ROG HyperCharge, UFCS ≤100W, Apple | standard PD/PPS cables | open standards |

---

## 18. Above-PD (>5 A) modes behind vendor SVIDs — validated map (2026-11)

USB PD caps current at **5 A** (fixed PDOs with a 5A e-marker; PPS APDOs at 5A; EPR raises *voltage* to 48V,
not current). Some OEM chargers unlock more only after a vendor handshake. This section is the validated
version of the research brief; every claim was re-checked by a dedicated subagent against public sources
(validation log in §18.5). Tier marks: **[P]** primary/public (spec PDFs, GPL code, usb.ids, vendor pages),
**[F]** forum/teardown, **[HW]** our K2 captures, **[I]** inference.

### 18.1 The map

| Brand | Marketing name | Max power (V/A/W) | Transport | SVID / VID | Cable requirement | Sources |
|---|---|---|---|---|---|---|
| Xiaomi/Redmi/POCO | HyperCharge / Mi Turbo (C-port "Surge Charge" = MiPPS) | 120W (11V/6A, 20V/6A); 210W (17–20V/10.5A); 300W demo 2023 | **PD uVDM auth → APDO 5A→6A** | SVID **0x2717** | proprietary 6A C-to-C; 5A cable caps at 100W | [P] MiCode kernel; [HW] CUKTECH; [F] tinkervault |
| OPPO/OnePlus/realme | VOOC/SuperVOOC/Warp/Dash | 240W (20V/12A); 120W (11V/11A @230V) | **D+/D-** (D+=CLK master, D-=DATA); PD on same port ≤65W | SVID **0x22D9** advertised, no VDM unlock documented | original 10–12A cable (e-marker VID 0x22D9) | [P] OPPO paper, realme kernel; [HW]; [F] whatcable #525 |
| Huawei | SuperCharge SCP/FCP | 100W phone (20V/5A); 127W SCP on laptop bricks | **D+/D- ACCP** single-wire registers | VID 0x12D1 (USB VID only) | standard; 5A e-marker for 100W PD | [P-code] Huawei GPL kernel, CCSA draft |
| Honor | SuperCharge | 100W (20V/5A PD + SCP); laptop bricks SCP 120W (20V/6A), 127W | PD ≤5A + SCP on D+/D- | VID 0x339B (USB VID only) | standard | [P] usb.ids/honor.com; [F] ChargerLAB |
| vivo/iQOO | FlashCharge VFCP | 120W (20V/6A); 200W (20V/10A) | proprietary; wire format not public *(HW: UART on D+/D-)* | VID 0x2D95 (USB VID only) | proprietary high-A cable | [P] usb.ids; [F] tinkervault/ChargerLAB |
| Motorola/Lenovo | TurboPower | 68W (11V/6.2A); 125W (20V/6.25A) | **PPS APDO >5A as MOTO-private mode**, cable-gated | no VDM; VID 0x17EF (Lenovo) | 6.5A e-marker cable (P/N 78811036) | [F] ChargerLAB MC-1258, tinkervault |
| Nubia/RedMagic | "NeoCharger 3.0" | 165W = 20V/8.25A (NB-A20825C) | proprietary, not public *(uncertain)* | VID 0x19D2 (ZTE) | bundled 9A cable (e-marker reads 20V/5A) | [F] chongdiantou KM003C |
| Samsung | SFC/SFC2.0/SFC3.0 | 45W (PPS ≤4.5A); 60W (S26); 65W brick = fixed 20V/3.25A — **never >5A** | plain PD PPS (+AFC on D+/D-) | VID 0x04E8; no SVID | 5A e-marker for 45W (S25+/Ultra: PPS 15V/3A) | [F] tinkervault/teardowns |
| ASUS ROG | HyperCharge | 65W = PPS 3.3–21V/3.25A — within PD | PD PPS | VID 0x0B05; no SVID | standard | [P] rog.asus.com |
| Cross-brand | **UFCS** T/TAF 083 | to 36V档; no cap; 320W realme demo | **D+/D- full-duplex UART 8N1 115200**; vendor SHA256 auth above threshold | n/a | e-label mandatory >6.5A | [P] taf.org.cn PDFs |

### 18.2 PD-VDM vs D+/D- vs UFCS (Q2)
- **Only Xiaomi 0x2717 is a confirmed PD-VDM unlock** [P+HW]: MiCode kernel `courbet-r-oss`
  `usbpd.h` (`USB_PD_MI_SVID 0x2717`, uVDM type byte 1=REQ/2=ACK, cmds 1–10), 4-word byte-swapped seed/digest
  payloads, `pd_verifed=1` → Get_Source_Cap → charger re-advertises APDOs at 6A (our CUKTECH capture: opener
  `0x27170101`, chain `0x27170201..0A`, then 5–11V/6A and 5–20V/6A). The phone never requests above the
  advertised APDO — the "unlock" is the charger's reaction to VERIFIED + cable [I]. Public reimplementations
  with published digest keys: [ianchb/xiaomi-mipps-auth](https://github.com/ianchb/xiaomi-mipps-auth) and forks
  (rponeawa, blu-sharky/piano-mipps-auth, yzddmr6 mainline PRs).
- **OPPO 0x22D9**: SVID advertised (our OSABBCBBAE capture) but no public VDM unlock; SuperVOOC is D+/D-
  (OPPO paper; realme GPL `oppo_vooc.c` has no VDM path). PD-side role of 0x22D9 = **open** [I].
- **Huawei 0x12D1 / vivo 0x2D95 / Honor 0x339B / ZTE 0x19D2 / Zimi 0x2B01 are USB VIDs (usb.ids), not documented
  PD SVIDs** — their modes are D+/D- or private-PPS, not VDM.
- **Motorola** 6.25A and **RedMagic** 8.25A ride the PPS APDO current field (>5A = non-standard) as a private
  mode gated by the cable — no VDM.
- **UFCS** is a published standard on D+/D- UART with in-band vendor SHA256 auth — not VDM.

### 18.3 Third-party support (Q3)
- Xiaomi MiPPS/Surge Charge is licensed to third parties [F tinkervault]: **CUKTECH** (corroborated by cuktech.com
  + r/ChargingSheet; confirmed on our HW), **ASUS Adol** AC140-020A (ChargerLAB: two Xiaomi 120W private levels),
  **MIIIW** CA514 Pro 140W, **Sharge** HyperTower 170W, **Huntkey** S140 Pro 90W, **Baseus** 100W 7-in-1 90W,
  **UGREEN** PB550 67W, **Candysign** (exact wattages mostly tinkervault-only). Cables: CUKTECH/MIIIW 6A;
  **Mcdodo CA-4730** and **SOUNCE 6A** are the only third-party cables Reddit confirms for 120W *(community,
  r/Xiaomi 1657wbe)*; Baseus/Essager "120W" cables do **not** unlock 6A.
- SuperVOOC: **no known third-party charger/cable** *(community, r/oneplus 1dge739 — UGREEN Nexode 100W fails)*.
- Motorola: TALIX Zeta 240W (MCZT240A2), Lenovo Multi-Port 100W GaN ("MOTO Max 105W") [F ChargerLAB/tinkervault].

### 18.4 Public RE sources (Q4)
- GPL/primary code: [MiCode/Xiaomi_Kernel_OpenSource](https://github.com/MiCode/Xiaomi_Kernel_OpenSource)
  (0x2717 VDM chain), [Huawei-Dev android_kernel_huawei_kirin970](https://github.com/Huawei-Dev/android_kernel_huawei_kirin970)
  (`adapter_protocol_scp.c`), CCSA draft App. B PDF, [kralonur/embedded-charging-protocols-rs](https://github.com/kralonur/embedded-charging-protocols-rs),
  realme GPL kernel `oppo_vooc.c`, OPPO Flash Charging technical paper, [T/TAF 083 PDFs at taf.org.cn](https://www.taf.org.cn/StdQuery.aspx?type=TAF&num=T%2FTAF+083-2021).
- Reimplementations: [ianchb/xiaomi-mipps-auth](https://github.com/ianchb/xiaomi-mipps-auth) (+forks).
- Measurements: tinkervault per-protocol pages; ChargerLAB/chongdiantou teardowns (chip inventories: Injoinic
  IP2736/IP2756, NuVolta NU1520, Southchip SC9712A implement UFCS/SCP/FCP/AFC/PD — vendor modes live in commodity
  protocol ICs); whatcable e-marker reports (#525 OPPO 0x22D9; #138/#540/#628 Zimi 0x2B01).
- Forums: 4PDA 1102294/1062207, XDA 4659776, r/Xiaomi, r/PocoPhones, r/oneplus, r/UsbCHardware, r/motorola,
  r/RedMagic, CSDN UFCS dissection.
- Our own: `docs/FASTCHARGE_PROTOCOLS.md`, `docs/CHARGER_QUIRKS.md`, `docs/agents/research/xiaomi-10v5a.md`,
  `docs/captures/` [HW].

### 18.5 Cable marking for >5A (Q5) + validation log + open items
- PD has **no standard >5A cable field** [P]: e-marker Cable VDO VBUS-current bits 6..5 encode only
  default/3A/5A (`CABLE_CURR_DEF/3A/5A`, torvalds/linux `include/linux/usb/pd_vdo.h`); sources cap at 3A without
  a 5A e-marker. Vendor practice: vendor VID in the e-marker ID header (OPPO **0x22D9** — Cable VDO 0x700A2E40
  still reads 5A@50V, "12A" is marketing; Zimi/CUKTECH **0x2B01** PID 0x4051/0x4092 likewise; a Kowsi "Mi 6A"
  clone reports **0x2717** [HW]) + optional vendor SOP' VDM exchange with the cable (CUKTECH gates 6A on it
  [HW]). VOOC USB-A cables: extra GND_DRAIN pin + TI single-wire auth IC [F chongdiantou]. Xiaomi's official 6A
  cable contains an unmarked Xiaomi-custom chip (the "TUSB546" listing claim is a misidentification — TUSB546 is
  a DP-altmode redriver) [F teardowns]. [I] A compliant 5A e-marker is still required for any PD >3A.
- **Validation log (2026-11, 9 subagents, one per protocol)**: all claims re-checked publicly; corrections folded
  into §1–§14 above — key ones: UFCS PHY is D+/D- full-duplex UART (not CC BMC), standard is T/TAF **083** (not
  113), no 240W UFCS cap, e-label >6.5A; VOOC on C is D+/D- (not CC); Samsung SFC 3.0 = 60W on S26, SFC 2.0 ≤4.5A
  (OEM 4.05A); Honor 0x339B is a documented USB VID; Honor laptop bricks do SCP >5A (120W/127W); RedMagic 165W =
  exactly 20V/8.25A, no 210W RedMagic; Moto 125W = private 6.25A mode on top of standard PPS-5A APDOs; MiPPS
  digest keys are public in `ianchb/xiaomi-mipps-auth` (kernel itself closed); whatcable "12A/6A" cable labels
  are marketing — the e-marker field stays 5A@50V.
- **Open items**: OPPO 0x22D9 PD-VDM role (settle with a CC-line capture of a SuperVOOC session); vivo VFCP and
  RedMagic 165W wire formats (no public primary source; our [HW] RE is the only evidence); USB-IF SVID registry
  check for 0x12D1/0x2D95/0x339B/0x19D2/0x22D9 (all evidence says USB VIDs only, except 0x2717).

---

## Sources

- [OPPO Flash Charging Technical Paper (PDF)](https://www.oppo.com/content/dam/oppo/en/mkt/newsroom/story/flash-charging-technical-pape/OPPO%20Flash%20Charging%20Technical%20Paper.pdf)
- [Wikipedia — VOOC](https://en.wikipedia.org/wiki/VOOC)
- [TinkerVault protocol library — VOOC/SuperVOOC, HyperCharge, UFCS, Pump Express, mCharge, ASUS](https://www.tinkervault.com/usb-fast-charging-protocols/)
- [Kodikion Substack — Deconstructing SuperVOOC](https://kodikion.substack.com/p/deconstructing-supervooc) *(community-grade)*
- [ET India Times — OnePlus 160W adapter supports PPS/PD](https://economictimes.indiatimes.com/tech/tech-trends/oneplus-10r-160w-supervooc-charger-supports-pps-pd-charging/articleshow/91178049.cms)
- [r/Xiaomi — how Xiaomi charging protocol works](https://www.reddit.com/r/Xiaomi/comments/1gaet9g/how_does_the_xiaomi_charging_protocol_work_why/) *(community)*
- [r/samsung — SFC 2.0 PPS 4.05A/11V, 5A cable](https://www.reddit.com/r/samsung/comments/1563e9f/question_about_super_fast_charging_20/) *(community)*
- [PowerRadix — USB fast charging protocols roundup](https://www.powerradix.com/blog/usb-fast-charging-protocols-everything-you-need-to-know/)
- [ChargerLAB — vivo/iQOO 120W GaN teardown](https://www.chargerlab.com/teardown-of-vivo-iqoo-120w-flash-charge-gan-charger-v12060l1b0-cn/)
- [ChargerLAB — OnePlus SUPERVOOC 100W teardown](https://www.chargerlab.com/teardown-of-oneplus-supervooc-100w-dual-ports-charger-1a1c/)
- [vopmart — iQOO 120W adapter (20V/6A)](https://www.vopmart.com/catalog/product/view/_ignore_category/1/id/17455/s/vivo-iqoo-120w-super-flashcharger/)
- [HuaweiCentral — UFCS launch 2021](https://www.huaweicentral.com/huawei-oppo-vivo-and-xiaomi-launched-the-ufcs-first-fast-charging-standard-of-china/)
- [HuaweiCentral — Xiaomi 35W/67W UFCS charger](https://www.huaweicentral.com/xiaomi-launches-35w-67w-ufcs-1-0-charger-supports-huawei-oppo-vivo-phones/)
- [MobilityArena — What is UFCS](https://mobilityarena.com/universal-fast-charging-specification-ufcs/)
- [Qualcomm — Quick Charge 5](https://www.qualcomm.com/products/features/quick-charge)
- [szapphone — QC4 guide / QC↔PD PPS relationship](https://www.szapphone.com/blog/qualcomm-qc-4-0-guide/)
- [androxus — fast charging standards explainer (PPS 20mV, EPR 28/36/48V)](https://androxus.com/blog/images/ampere-watt-volt-explained/hero.webp/)
- [wowohcool — USB-C PD 3.1 EPR/AVS/240W](https://www.wowohcool.com/blog/usb-c-pd-3-1-explained/)
- [r/UsbCHardware — EPR e-marker 50V/5A bits](https://www.reddit.com/r/UsbCHardware/comments/1f3vhbt/emarker_says_5a_x_50v_but_240w_is_the_standard/) *(community)*
- [usbconnectioninformation — 100W vs 240W cables](https://usbconnectioninformation.com/guides/usb-c-100w-vs-240w-cables/)
- [Satechi — Identifying USB-C e-mark cables](https://satechi.net/blogs/identifying-usb-c-e-mark-cables)
- [Amazon — Motorola TurboPower 125W + 6.5A e-marked cable](https://www.amazon.com/Motorola-TurboPower-125W-Duo-Marked/dp/B0FL86MLBF)
- [ASUS ZenTalk — ROG HyperCharge 65W needs PD3.0+PPS](https://zentalk.asus.com/t5/rog-phone-8/how-to-enable-hypercharge-65/td-p/430285/page/2) *(community)*
- [GizChina — RedMagic 7 Pro 165W (20V/8.25A)](https://www.gizchina.com/2022/02/01/nubia-red-magic-7-pro-with-165w-fast-charging-passed-3c-certification/)
- [GSMArena — Meizu Super mCharge 55W (11V/5A)](https://www.gsmarena.com/super_mcharge_can_boost_your_meizu_from_0_to_60_in_10_minutes-blog-23697.php)
- [EFTM — OPPO AirVOOC 50W wireless review](https://eftm.com/2025/05/review-oppo-airvooc-50w-wireless-charger-wireless-speed-at-its-best-262181)
- [GearExplained — iPhone USB-C charger guide (plain PD)](https://gearexplained.com/guides/usb-c-charger-for-iphone/)
- [Alibaba buying guide — Samsung 65W SFC / PPS + 5A cable](https://electronics.alibaba.com/buyingguides/samsung-65w-super-fast-charger-guide)
- §18 primary/validated sources:
  [MiCode/Xiaomi_Kernel_OpenSource](https://github.com/MiCode/Xiaomi_Kernel_OpenSource) (`courbet-r-oss`, `usbpd.h`, `policy_engine.c`),
  [ianchb/xiaomi-mipps-auth](https://github.com/ianchb/xiaomi-mipps-auth),
  [Huawei-Dev kernel — adapter_protocol_scp.c](https://github.com/Huawei-Dev/android_kernel_huawei_kirin970),
  [kralonur/embedded-charging-protocols-rs](https://github.com/kralonur/embedded-charging-protocols-rs),
  [T/TAF 083-2021 PDF](https://www.taf.org.cn/upload/notice/2021-0528-091954-1436470269.pdf) / [T/TAF 083-2022 PDF](https://www.taf.org.cn/upload/AssociationStandard/TTAF%20083-2022%20%E7%A7%BB%E5%8A%A8%E7%BB%88%E7%AB%AF%E8%9E%8D%E5%90%88%E5%BF%AB%E9%80%9F%E5%85%85%E7%94%B5%E6%8A%80%E6%9C%AF%E8%A7%84%E8%8C%83.pdf),
  [usb.ids mirrors: 2717](https://the-sz.com/products/usbid/index.php?v=0x2717) [22D9](https://devicehunt.com/view/type/usb/vendor/22D9) [12D1](https://usb-ids.gowdy.us/read/UD/12D1) [2D95](https://the-sz.com/products/usbid/index.php?v=0x2D95) [339B](https://the-sz.com/products/usbid/index.php?v=0x339B) [19D2](https://the-sz.com/products/usbid/index.php?v=0x19D2) [2B01](https://the-sz.com/products/usbid/index.php?v=0x2B01) [17EF](https://the-sz.com/products/usbid/index.php?v=0x17EF),
  [torvalds/linux pd_vdo.h — CABLE_CURR_DEF/3A/5A](https://github.com/torvalds/linux/blob/master/include/linux/usb/pd_vdo.h),
  [whatcable #525 (OPPO 12A cable, VDO 0x700A2E40)](https://github.com/darrylmorley/whatcable/issues/525), [#138/#540/#628 (Zimi 0x2B01)](https://github.com/darrylmorley/whatcable/issues/138),
  [ChargerLAB — Moto MC-1258 125W teardown](https://www.chargerlab.com/teardown-of-original-125w-gan-charger-for-motorola-edge-30-ultra-mc-1258/),
  [ChargerLAB — HONOR HN-200500CP0 127W SCP](https://www.chargerlab.com/teardown-of-honor-100w-gan-charger-for-laptop-hn-200500cp0/),
  [ChargerLAB — HONOR HN-200600C02 120W](https://www.chargerlab.com/teardown-of-honor-120w-gan-charger-hn-200600c02/),
  [ChargerLAB — ASUS Adol AC140-020A Xiaomi 120W levels](https://www.chargerlab.com/teardown-of-asus-adol-140w-gan-quick-charger-ac140-020a/),
  [chongdiantou — RedMagic NB-A20825C 165W readout](https://www.chongdiantou.com/archives/363737.html),
  [GSMArena — realme 320W UFCS demo](https://www.gsmarena.com/realme_320w_supersonic_charge-news-64114.php),
  [ROG Phone 8 spec (PPS 3.3–21V/3.25A)](https://rog.asus.com/phones/rog-phone-8/spec/),
  [Motorola 6.5A e-marked cable P/N 78811036](https://www.motorola.com/us/p/motoaccessories/power-charging/78811036),
  [mi.com 120W combo specs (11V/6A, 20V/6A)](https://www.mi.com/global/product/xiaomi-120w-charging-combo-type-a/specs/),
  [tinkervault: hypercharge](https://www.tinkervault.com/usb-fast-charging-protocols/hypercharge) [moto-turbopower](https://www.tinkervault.com/usb-fast-charging-protocols/moto-turbopower) [ufcs](https://www.tinkervault.com/usb-fast-charging-protocols/ufcs) [samsung-afc-sfc](https://www.tinkervault.com/usb-fast-charging-protocols/samsung-afc-sfc) [huawei-fcp-scp](https://www.tinkervault.com/usb-fast-charging-protocols/huawei-fcp-scp) [vfcp](https://www.tinkervault.com/usb-fast-charging-protocols/vfcp)
- Repo-internal: `docs/FASTCHARGE_PROTOCOLS.md`, `docs/agents/QC_RESEARCH.md`, `docs/CHARGER_QUIRKS.md`, `docs/agents/research/xiaomi-10v5a.md`
