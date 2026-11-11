# How a Mi 11 gets 10 V / 5 A over USB-C PD: what Xiaomi's kernel shows

Research date: 2026-11-07. Source: https://github.com/micode/xiaomi_kernel_opensource, shallow sparse clones under
`runs/xiaomi-kernel/` (gitignored):

| branch | phone | SoC | commit |
|---|---|---|---|
| `venus-r-oss` | Mi 11 | SM8350 (lahaina) | `7db0abb6726f611eedbbe92bbf8a4291c0dcdb69` |
| `star-r-oss` | Mi 11 Pro / Ultra | SM8350 | `a9244def8a5178edee9ed4a30d60034de8286cbb` |
| `courbet-r-oss` | Mi 11 Lite 4G | SM7150 (sdmmagpie) | `de8d0006e5c9c7aeae530438813a019e4df5e21f` |

Citations are `branch:path:line`. Everything not marked **inferred** comes straight from the code. This doc describes
the logic in prose and does not copy the GPL source.

## 0. The main limitation: on the Mi 11, the PD stack is not in the kernel

- `venus` and `star` build no kernel PD policy engine. `CONFIG_USB_PD_POLICY` is absent from
  `venus:arch/arm64/configs/vendor/venus_*.config` and `star:.../star_*.config`. Instead they enable
  `CONFIG_QTI_PMIC_GLINK`, `CONFIG_QTI_BATTERY_CHARGER`, `CONFIG_UCSI_QTI_GLINK` and `CONFIG_QTI_ALTMODE_GLINK`
  (`venus:arch/arm64/configs/vendor/venus_GKI.config:43-44,74-77`). On SM8350, the Type-C/PD state machine, the
  Xiaomi VDM transport and the PPS loop run in the **ADSP firmware (closed)**. The kernel driver
  `qti_battery_charger.c` only passes properties between userspace and the ADSP over glink.
- Who computes what is still visible. A userspace daemon (closed) drives the authentication through sysfs: it writes
  `request_vdm_cmd`, reads the adapter's answer back, and writes `pd_verifed` (`venus:drivers/power/supply/qti_battery_charger.c:3268-3499`).
  The ADSP then uses the verified flag (`XM_PROP_PD_VERIFED`) when it picks the charging mode.
- `courbet` (Mi 11 Lite) still runs the **Qualcomm kernel policy engine with Xiaomi's additions**
  (`courbet:arch/arm64/configs/vendor/courbet_user_defconfig:487` `CONFIG_USB_PD_POLICY=y`). It has the same command enum
  and the same sysfs interface as venus. It is the only branch where the wire format and the request logic can be read.
  **Inferred:** the Mi 11 ADSP speaks the same wire protocol. The `uvdm_state` enum is identical, values 0-8
  (`venus:drivers/power/supply/qti_battery_charger.c:74-84` vs `courbet:drivers/usb/pd/usbpd.h:128-140`), and so are the
  sysfs names and the 4-word byte-swapped seed/auth payloads.

## 1. The unlock message sequence

### Wire format (courbet, read)

- The Xiaomi SVID is `0x2717` (`courbet:drivers/usb/pd/usbpd.h:142`).
- The messages are **unstructured** VDMs. The header is SVID<<16, bit 15 = 0, one "type" byte in bits 15..8 and the
  command in bits 7..0 (`usbpd.h:147-150`). The phone sends type `USBPD_VDM_REQUEST` = 1. `USBPD_ACK` = 2 is defined
  next to it (`usbpd.h:152-154`). So a phone request reads `0x271701cc`, and **inferred:** the adapter answers
  `0x271702cc`. The phone's receive callback looks only at the low byte and never checks the type byte
  (`courbet:drivers/usb/pd/policy_engine.c:5054-5063`).
- Commands (`usbpd.h:128-140`; the send logic is `policy_engine.c:4811-4856`; the answers are parsed at `policy_engine.c:5064-5121`):

| cmd | name | phone sends | adapter answer, as parsed |
|---|---|---|---|
| 1 | CHARGER_VERSION | header only | VDO0 = adapter version |
| 2 | CHARGER_VOLTAGE | header only | VDO0 low 16 bits × 10 mV. The phone compares this with its own VBUS and current to estimate the cable resistance (debug log only) |
| 3 | CHARGER_TEMP | header only | VDO0 low 16 bits × 10 (temperature) |
| 4 | SESSION_SEED | 4 VDOs, each word byte-swapped | 4 VDOs stored as `s_secert` |
| 5 | AUTHENTICATION | 4 VDOs, each word byte-swapped | 4 VDOs stored as `digest` (128 bits) |
| 6 | VERIFIED | 1 VDO (the result) | none parsed |
| 7 | REMOVE_COMPENSATION | 1 VDO | none parsed |
| 8 | CONNECT | not sent as a command; it is a local state | - |
| 9 | NAN_ACK | local state: Discover Identity/SVIDs was NAKed | - |
| 10 (0x0A) | SINK_CAPACITY | 1 VDO = battery SoC 1-100 | answer = the peer's SoC (phone-to-phone "smart interchange") |

  Source: the SINK_CAPACITY send is at `policy_engine.c:664-665`; the delays (1300 ms send, 400 ms swap) are at
  `usbpd.h:168-169`. Commands 9 and 10 exist only on courbet. The venus enum stops at 8.

### Who computes what

- The kernel only moves bytes. It byte-swaps each of the 4 words, then sends them (`courbet:policy_engine.c:4841-4844`,
  `venus:qti_battery_charger.c:3285-3294,3311-3313`). It hands the adapter's 4-word digest back to userspace through
  `request_vdm_cmd` (`courbet:policy_engine.c:4911-4917`, `venus:qti_battery_charger.c:3380-3392`).
- The seed and the challenge come from userspace, and so does the digest check. The log tags call that daemon
  "batterysecret" (`courbet:policy_engine.c:4697,4751`). **Inferred:** it is a challenge-response with a secret that only
  Xiaomi adapters and the phone's secure side hold. The digest is 128 bits and the code names a "sha256" bit-swap
  (`policy_engine.c:4796-4809`). The key and the algorithm are not in the kernel.
- When userspace accepts the adapter, it writes `usbpd_verifed=1`. The kernel then **sends Get_Source_Cap**
  (`courbet:policy_engine.c:4687-4710`) and the phone re-reads the source capabilities. **Inferred:** the adapter
  learns the outcome only from the VERIFIED VDM (cmd 6, 1 VDO). It cannot tell whether the phone really checked the digest.

### Discover Identity / SVIDs (courbet, read)

- As a sink (UFP), the phone sends Discover Identity only when it is DFP (`policy_engine.c:1963-1974`, also after a
  DR_Swap to DFP at `2607-2625`). A PD 3.0 source's Discover Identity / SVIDs request gets Not_Supported, and a PD 2.0
  request gets a NAK (`policy_engine.c:2196-2211`). **The phone does not present a Xiaomi identity to the charger.**
- When the phone does discover, it stores `adapter_id` = the low 16 bits of the **last** VDO of the Discover Identity
  ACK (`policy_engine.c:2261-2265`) and `adapter_svid` = the last SVID listed (`policy_engine.c:2333`). Both are exported
  to userspace. The Mi SVID handler's `connect` callback runs only after Discover SVIDs listed 0x2717
  (`policy_engine.c:2351-2364, 5030-5036`).
- **Inferred:** a UFP Mi 11 Lite on a charger therefore normally never discovers. Userspace sends cmd 1 "blind" through
  `request_vdm_cmd` after the explicit contract, because `usbpd_send_vdm` does not check discovery
  (`policy_engine.c:2079-2109`). On venus the ADSP exposes `adapter_id`/`adapter_svid` (`venus:qti_battery_charger.c:3427-3463`),
  which suggests the ADSP firmware does discover. Its order is not visible.

### Source-cap checks

- A source cap list must start with a Fixed PDO (`courbet:policy_engine.c:1092-1095`). On a PD 3.0 source, any PPS APDO
  marks the port `PD_PPS_ACTIVE` (`1104-1118`). An APDO under 18 W sets the "weak PPS" limit, which caps requests at
  1.5 A (`1137-1147`, `usbpd.h:163-164`).

## 2. What the phone actually requests

- **Fixed PDOs are capped hard.** The phone refuses any fixed PDO above 9 V and caps 9 V at 2 A
  (`courbet:policy_engine.c:359-361, 922-935`). High power is **PPS only**.
- **The phone never asks for more than the APDO advertises.** If the requested mV/mA is outside the APDO limits,
  the request is refused with -EINVAL before any RDO is built (`policy_engine.c:940-946, 1034-1040`). Xiaomi's own
  clamps can only lower the current (`pd_max_curr_limit`, the weak-PPS limit) or the voltage (the unverified clamp).
  **No code path requests more than an advertised PDO allows.**
- The courbet PPS loop (`courbet:drivers/power/supply/ti/pd_policy_manager.c`) works like this:
  - It picks the PPS APDO with max V ≥ 10 V (`min_adapter_volt_required` = 10000, `:79-80`), max V ≤ 11 V
    (`APDO_MAX_VOLT` = 11000, `:39`) and the highest current (`:777-790`).
  - It reports `apdo_max` = V × A, in W, to the charger driver (`:797-801`).
  - The starting request is 2 × Vbat + 400 mV for the div-2 charge pump. The current is the lower of the APDO max and
    half the FCC limit (`:1110-1115`). The voltage then steps by 20 mV (`:991`).
  - **An 11 V APDO is treated as 10 V max**: `PPS_VOL_MAX` 11000 − `PPS_VOL_HYS` 1000 (`:993-997`,
    `pd_policy_manager.h:84-85`).
  - So on an "11 V 5 A" APDO the phone ends up at **≤ 10 V, ≤ 5 A**. **Inferred:** this is where the "10 V / 5 A" label
    comes from.
- Courbet itself is configured for 3 A bus current (`courbet:arch/arm64/boot/dts/qcom/courbet-sdmmagpie.dtsi:96-98`
  `mi,pd-bus-curr-max = <3000>`) and a 4 A PD current limit (`:525-526`). It is a 33 W phone. The 5 A numbers for the
  Mi 11 are in venus DT/ADSP, and neither is in this tree: venus has no device-tree file of its own.
- **What the verified flag changes, phone side only:**
  - Unverified PPS VBUS is clamped to 9.5 V (`policy_engine.c:1053-1055`; DT `mi,limit_pd_unverified_pps_vbus = <1>`,
    `mi,pd_unverified_pps_max_vbus_limit = <9500000>` at `courbet-sdmmagpie.dtsi:527-528`).
  - Unverified FCC is held at 4.8 A and the float voltage at 4.45 V (`courbet:drivers/power/supply/qcom/smb5-lib.h:134-135`,
    `smb5-lib.c:973-977, 5813-5818`).
  - Verified PPS is labelled "Turbo" in the UI. On venus/star, PD_PPS + `pd_verifed`==1 gives `QUICK_CHARGE_TURBE`, and
    unverified PPS gives `QUICK_CHARGE_FAST` (`venus:qti_battery_charger.c:1291-1303`, `star:...:1374-1375`).
  - The `ADAPTER_XIAOMI_PD_40W/50W/60W` codes on the same lines are the **wireless** TX adapter type (`XM_PROP_TX_ADAPTER`
    sits in the "wireless charge infor" block, `venus:...:176-188`; star `wls_power_max` at `star:...:4022-4037`). They are
    not about the wired PD source.
- **Does the source re-send changed PDOs?** The phone asks for them with Get_Source_Cap right after verification
  (`policy_engine.c:4701-4707`). Nothing in the kernel requires the list to change. **Inferred:** a Xiaomi adapter may
  raise its APDO current after VERIFIED (our CUKTECH C1 does: PPS 5 A → 6 A), but the phone works either way. It
  re-evaluates whatever list it gets and never over-requests.

## 3. Cable / e-marker (answers to the coordinator's questions a-c)

**(a) Who reads the e-marker?**
- **Not the phone as a sink**, on the readable branch. The courbet engine sends SOP' Discover Identity only in
  `PE_SRC_STARTUP` with VCONN on, i.e. when the phone is the **source** (reverse/OTG) (`courbet:policy_engine.c:1655-1691`).
  As a sink it enables the SOP' receive filter only if it sources VCONN itself (`1893-1895`), which a sink on a charger
  does not.
- There is no code that parses a cable VDO, cable current, or a Xiaomi cable VID/PID. Grepping for passive/active cable
  and cable current finds nothing. `pd_policy_manager.c`, `smb5-lib.c` and the venus/star glink drivers contain no
  e-marker logic. The only "cable" strings are Type-C partner-type names (`venus:qti_battery_charger.c:461-464`).
- **Inferred:** the cable check belongs to the **source**, as USB PD requires: a source may offer more than 3 A only
  after SOP' identifies a 5 A cable. The phone simply sees an APDO with 3 A or 5 A and stays inside it. On venus the
  ADSP could read SOP' too. That is not visible here, and nothing in the venus kernel interface mentions a cable.

**(b) Any check beyond the standard e-marker?**
- None on the phone side in the kernel. There is no VDM to SOP' and no cable ID resistor. `XM_PROP_RESISTANCE_ID`
  is the battery ID (`venus:qti_battery_charger.c:160`).
- **Our own hardware evidence, not the kernel:** chargers send Xiaomi-specific requests to SOP'.
  - CUKTECH C1 sent Xiaomi-specific SOP' requests to a VID-0x2717 cable clone and capped at 5 A when the clone did not
    answer. The built-in CUKTECH 6 A identity unlocked 6 A (`docs/CHARGER_QUIRKS.md:164-168`).
  - CUKTECH capped at 5 A over an EPR cable read as "Xiaomi 5 A" and gave 6 A over a cable with no readable e-marker
    (`CHARGER_QUIRKS.md:150`).
  - So a source can gate **6 A** on a vendor SOP' exchange. For **5 A** the standard e-marker VBUS-current field should
    be enough (**inferred**, per the spec).

**(c) What happens with a 3 A cable?**
- The source should advertise ≤ 3 A (inferred, spec). The phone picks the best APDO from what is offered: with
  PPS 11 V 3 A it requests ≤ 10 V at ≤ 3 A, so about 30 W (`pd_policy_manager.c:777-790, 993-999`). An APDO under 2 A
  becomes a 1.5 A request (`:797-798`).
- No kernel code makes the phone react to the cable itself.

**Which cable VIDs/PIDs or e-marker fields does the kernel accept?**
- **None are listed, on any of the three branches.** No cable allow-list exists. I searched `drivers/` and
  `include/linux/usb` on venus, star and courbet for 0x2B01, 0x2E87, the cable VID/PID and the cable-current fields.
- The only cable-current constants are the upstream generic ones in `venus:include/linux/usb/pd_vdo.h:166-172`
  (`CABLE_CURR_1A5/3A/5A`, `VDO_CABLE`). Only upstream drivers include that header, and none of them uses
  `CABLE_CURR_5A`. Those drivers (tcpm, typec altmodes/ucsi displayport) are not the Mi 11 PD path, and
  `CONFIG_TYPEC_TCPM` is not in `venus_*.config`.
- So when we read the CUKTECH cable on the K2, there is nothing in the phone kernel to compare it with. The comparison
  that matters is on the source side, against our own records:
  - the e-marker Cable VDO VBUS-current field (bits 6..5: 01 = 3 A, 10 = 5 A);
  - the cable VID in the ID Header VDO (CUKTECH/Xiaomi-ecosystem 0x2B01; the Kowsi "Mi 6A" clone uses 0x2717);
  - whether the source sends vendor SOP' VDMs to the cable (`docs/CHARGER_QUIRKS.md:150, 164-168`, where CUKTECH's
    6 A depends on such an exchange).
  - **Inferred:** the 20K bank needs only the 5 A current field for a 5 A APDO, if it has a 5 A mode at all.

**Would faking a 5 A e-marker be enough?**
- **Phone side: yes.** The phone never looks at the cable.
- **Source side:** it is enough for a source that follows the standard e-marker rule and has a 5 A mode at all. It is
  not enough if the source also wants the Xiaomi SOP' extension; CUKTECH does that for 6 A.
- **Caveat:** the K2 spoof answers SOP' only on the **top port**, the one that faces the source
  (`memory: emk-top-port-parked`).

## 4. Timing and ordering

- **VDM transport timing (courbet, read):**
  - The VDMs go out only in an explicit contract. They are queued and sent from `PE_SNK_READY`, after a 2 ms kick
    (`policy_engine.c:2079-2109, 1963-1970, 3529-3541`).
  - Only one VDM is in flight at a time, and a newer one replaces a queued one (`2083-2089`).
  - The discovery messages are retried on BUSY after tVDMBusy (`2383-2400`).
  - The Xiaomi command order is driven by userspace and is not in the kernel.
- **PPS loop timing (read):**
  - The policy manager starts 5 s after a PPS contact (`pd_policy_manager.c:1339-1349`), or at verified contact
    (`:1395-1400`).
  - The digest gate in the PPS loop is short-circuited to `true` on courbet (`pd_policy_manager.c:231-235`). Charging
    does not wait for the auth there; the auth only lifts the clamps.
- `select_pdo` from userspace is ignored while `verify_process` is set (`policy_engine.c:4355-4356`). **Inferred:** the
  phone holds its PPS requests while the auth chain runs. That matches our "do not Request during the chain" guard
  (`pd_sink.cpp xiaomi_chain_active`).
- SINK_CAPACITY goes out 1.3 s after source caps that show exactly 7.5 W, i.e. a phone acting as source
  (`policy_engine.c:1152-1153`, `usbpd.h:168`). This is irrelevant for chargers.

## 5. venus vs star vs courbet

| | venus (Mi 11) | star (Mi 11 Pro/Ultra) | courbet (Mi 11 Lite) |
|---|---|---|---|
| PD stack | ADSP (closed) | ADSP (closed) | kernel `policy_engine.c` |
| Xiaomi VDM cmds | 1-8 | 1-8 | 1-10 (adds NAN_ACK, SINK_CAPACITY) |
| verified → UI | PPS+verified = Turbo | same, plus a wireless 60 W code (0x0f → 67 W) | `pd_verifed` lifts the FCC/FV clamps |
| PPS policy | in ADSP, not visible | in ADSP, not visible | `ti/pd_policy_manager.c`, APDO 10-11 V, 3 A bus |
| unverified clamp | not visible | not visible | 9.5 V PPS, FCC 4.8 A |

- star's `qti_battery_charger.c` has 5442 lines against venus's 4252. The PD-auth part is the same; star only adds the
  60 W wireless code (`star:qti_battery_charger.c:1338-1388`).

## 6. Our firmware compared

What the K2 does today (`src/pd/pd_sink.cpp:893-1047`, constants in `include/k2/pd/pd_sink.hpp:150-152, 362-368`):

1. It answers the source's Discover Identity with a Xiaomi identity (`0xFF00A041, 0x05C02717, 0, 0x0000A486`) and the
   source's Discover SVIDs with `[0x2717]`.
2. It sends its own Discover SVIDs after about 220 ms.
3. It sends the opener `0x27170101` about 200 ms after 0x2717 appears.
4. It replies to each `0x271702cc` from the source with the next phone request:
   `0x27170102`, `0x27170103`, `0x27170104`+4 fixed words, `0x27170105`+4 fixed words, `0x27170106`+`1`,
   `0x27170107`+`1`, `0x27170108`+4 words, `0x2717010A`+`1`.
5. On `0x2717020A` it sends Get_Source_Cap.

Mapped onto the kernel enum, this is exactly the phone's request order: version, voltage, temp, session seed,
authentication challenge, VERIFIED=1, REMOVE_COMPENSATION=1, cmd 8 (not a command in either kernel), SINK_CAPACITY=1 %.
The chain itself is consistent with Xiaomi's protocol.

### Gap list

1. **Our 6 A "over-offer" rows have no counterpart on the phone.** The phone never requests above the APDO's max
   current (`courbet:policy_engine.c:940-946`). On a bank that advertises PPS 5-11 V **3 A**, a 6 A RDO is a protocol
   violation, and the Hard Reset is the correct answer. To reproduce a Mi 11, the K2 must get the source to *advertise*
   ≥ 5 A and then request inside that APDO.
2. **The bank's list probably reflects the cable, not the auth.** The Xiaomi 20K advertised 3 A everywhere,
   PPS 5-11 V 3 A and 5-20 V 2 A (`docs/CHARGER_QUIRKS.md:101-104`), and the runs used no e-marked cable. A compliant
   source must cap at 3 A without a 5 A e-marker. **Inferred:** that cap, not a failed auth, is why the post-chain list
   did not change. If the bank's top mode is 3 A only, then no unlock exists on it.
3. **No 5 A cable or spoof was used on the bank.** The top-port 5 A e-marker spoof (memory `emk-top-port-parked`)
   or a real 5 A / 6 A cable has not been tried against it.
4. **Our identity answers differ from a phone's.** The courbet phone answers a source's Discover Identity/SVIDs with
   Not_Supported/NAK (`policy_engine.c:2196-2211`) and has no Xiaomi identity of its own. We answer with a Xiaomi
   identity. This is not wrong (CUKTECH needed it, `CHARGER_QUIRKS.md:149`), but a Not_Supported variant is the closer
   phone mimic if a source behaves differently.
5. **The hunt voltage is not the phone's voltage.** A Mi 11 requests ≤ 10 V on an 11 V APDO (`pd_policy_manager.c:993-994`),
   starting near 2 × Vbat and stepping 20 mV. Our hunt asks for 11 V / 20 V directly. A "Mi 10 V 5 A" row
   (PPS, 10.0 V, the APDO's own max current) would match the phone.
6. **SINK_CAPACITY = 1 % and the cmd 8 payload are replayed fixed bytes.** Cmd 8 has no meaning in either kernel. This
   is harmless to the auth (it comes after VERIFIED) but should be documented as replay. Real phones send their SoC.
7. **We label the source's `0x271702cc` as "REQ from source"** (comments in `pd_sink.cpp:969-972`). By the kernel's
   defines they are **ACKs** (type 2) to our type-1 requests. This changes only naming and docs, not behaviour.
8. The 600 ms quiet / 2.5 s bound chain guard has no kernel counterpart. The closest one is the phone's `verify_process`
   gate (`policy_engine.c:4355`), which is set and cleared by userspace. Keep ours; it is empirical.

## 7. Test plan: K2 against the Xiaomi 20K bank

Put all artefacts in `runs/xiaomi20k-5a/`.

1. **Baseline caps, no auth relevance.** Use a plain C-to-C cable and `--units pd`. Record PDOs and confirm the known
   list: PPS 11 V 3 A, PPS 20 V 2 A.
2. **5 A cable, real.** Repeat with a real 5 A e-marked cable (the Baseus EPR cable from `chargers-on-hand`), then with the
   Mi 11 6 A cable if available. Record `pdos` before and after `0x2717020A` (`pd-reread`).
   - Pass: any APDO > 3 A appears, before or after the auth.
   - If the 5 A shows up before the auth, the auth is not what unlocks it.
3. **5 A spoof on the top port.** Plug the K2 top plug straight into the bank and use the PC13 Ra + 5 A e-marker spoof.
   Repeat step 2.
   - Log every SOP' frame the bank sends. A vendor SVID 0x2717 SOP' request means the bank wants the Xiaomi cable
     extension (compare the CUKTECH notes).
4. **Mi 11 request shape.** On whichever list shows an APDO ≥ 5 A, run `pd attach 10000 <apdo_max_mA> --confirm`, then step
   9.0 → 10.0 V in 20 mV steps under a 4-5 A load.
   - Never exceed the advertised current.
   - Watch for Reject, Hard Reset and VBUS sag.
5. **Auth on/off A/B.** With the same cable, run step 4 once with the 0x2717 chain disabled (no opener) and once with it
   enabled. Expected (inferred): an identical list, so the auth matters only to a phone.
6. **Hard-Reset sanity.** On the 3 A list, request PPS 10 V at exactly 3 A (expect Accept), then 3.05 A (expect Reject
   or Hard Reset). Record which one. This calibrates the `hard-reset-on-6a-over-offer` pattern against a plain 1-step
   over-offer.
7. **If nothing gives > 3 A:** read the bank's label or manual for its PPS rating. If it is a 33 W / 45 W bank, record
   in `docs/CHARGER_QUIRKS.md` that it has no 5 A mode. That is a property of the device, not an unlock failure.

## Open questions (not answerable from these trees)

- The venus ADSP firmware's exact order and timing of discovery and VDMs, and its PPS current limit.
- The digest algorithm and key (closed userspace / secure side).
- Whether a Xiaomi adapter changes its PDO list after VERIFIED, or only after a 5 A cable is seen.
- The Mi 11 device tree (5 A / 11 V limits). It is not in `venus-r-oss`, which has no venus `.dts`.
