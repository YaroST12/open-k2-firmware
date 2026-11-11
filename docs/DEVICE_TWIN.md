# Device twin

The question: **will this charger charge my device, and how fast?** The K2 learns a device once and keeps it as a *twin*. On any charger it then

1. **predicts**, per twin, the contract that device would get (volts, amps, watts, fixed or PPS) and what limits it (the device, the charger or the cable). Nothing is transmitted.
2. **proves it live**, on request: answers the charger as the twin (its sink capabilities), sends the Request the prediction chose, checks Accept, PS_RDY and VBUS in band, holds about one second and releases to 5 V.

The output is one screen (Tools > Device twins), one shell block and one JSON.

Status: built and host-tested (`tests/test_twin_*.cpp`, `tests/test_ui_twin.cpp`, `tests/test_twin_proto.py`). **Not yet run on hardware**; section 10 is the plan.

## 1. Where the code is

| Layer | Files | Notes |
| :--- | :--- | :--- |
| Profile and prediction (pure) | `include/k2/twin/twin.hpp`, `predict.hpp` | `predict(Twin, SourceCaps, Facts) -> Outcome` |
| Library record and learning (pure) | `library.hpp`, `learn.hpp`, `usbdata.hpp`, `present.hpp` | record codec, merge rules, USB-data decoders, the one-line texts |
| Live proof (pure state machine) | `live.hpp` | `Runner` over an `Env` seam, host-tested with a fake `Env` |
| Hardware glue | `include/k2/twin/twin_hw.hpp`, `src/twin/twin_hw.cpp` | flash library via `k2::storage`, Identify and sniff learning, the live proof over `fusb302` |
| Remote | `wire.hpp`, `Cmd::Twin = 0x63` | ops List 0, Get 1, Predict 2, Learn 3, Rename 4, Delete 5, Run 6, Status 7, Result 8, Usb 9, Cancel 10 |
| Shell | `twin list \| status \| cancel \| predict \| run \| learn \| rename \| delete` | `src/core/shell.cpp`, `src/core/shell_hw.cpp` |
| Host tool | `tools/k2_remote.py twin ...`, `tools/twin_proto.py` | `show`, `usb`, `export` are host-side views of the same records |
| Screen | `src/ui/ui_twin.cpp`, `include/k2/ui/ui_twin.hpp` | Tools > Device twins |

## 2. The prediction policy

The rules as built (`predict.hpp`, pinned by `tests/test_twin_predict.cpp`, one test per rule):

1. A fixed offer needs a sink fixed PDO **at the same voltage**.
2. The current is `min(what the source offers after the cable, what the sink PDO says)`.
3. A PPS offer needs a sink PPS range that overlaps; the voltage is the top of the overlap.
4. The **highest power wins**. A PPS above 5 V wins a tie; at 5 V the fixed PDO wins; then the higher voltage; then the lower position.
5. A sink whose PDO1 carries the **Higher Capability** bit and whose own PDO1 is 5 V gets `FiveV` with *mismatch*: the K2 never claims more than the device announced it can take.
6. **None** when the source caps are invalid, the twin has no sink PDOs (and no sniffed Request), or PDO1 is not 5 V.
7. A **sniffed Request** (what the device itself asked for on a real charger) wins when the same shape (same kind and voltage) exists on this charger.
8. Never above what the charger advertises. A cable that clips the current gives `Limiter::Cable`. The monitor ceiling (INA226 reads up to 36 V) is the live runner's business, not the prediction's: it is a `Skipped` verdict, never a changed prediction.
9. **Mismatch** is set when the source current after the cable is below the twin's need, or by rule 5.
10. **Limiter**: if the device gets what it needs, `Device` when the charger offers more (the device is the limit), else `None` ("full"); if it falls short, `Cable` when the cable clipped the offer (or the cable alone holds a bigger offer back), else `Charger`.

`Facts` carries what the K2 knows about this link: the charger's claimed power (`claim_w`, from Get_Source_Cap_Extended) and the physical cable's current and voltage rating from its e-marker (`cable_ma`, `cable_mv`; zero means "not read", never the 3 A / 20 V default). The "held by the cable" test of rule 10 is `rdintent::offer_held_by_cable(caps, claim_w)` (`include/k2/core/rd_intent.hpp`, which wraps `cable_limited(caps, claim_w)`): the charger's own claim when there is one, else the 20 V / 3.00 A signature.

### Prediction table, real data

Three stored devices against five bench chargers (`test_real_matrix` in `tests/test_twin_predict.cpp`; every cell is asserted). The twin wants: Pixel 33 W, OP15 55 W, Laptop 15 W.

| Device | CUKTECH C1, 3 A cable | C1, 5 A cable | ASUS 30 W | NOVOO RG100, 3 A | NOVOO, 5 A |
| :--- | :--- | :--- | :--- | :--- | :--- |
| Pixel | PPS pos 6, 11 V 3 A (33 W), device | same | PPS pos 6, 11 V 3 A, full | PPS pos 5, 11 V 3 A, device | same |
| OP15 | Fixed pos 3, 12 V 3 A (36 W), cable | PPS pos 6, 11 V 5 A (55 W), device | PPS pos 6, 11 V 3 A, charger, mismatch | PPS pos 5, 11 V 3 A, cable, mismatch | PPS pos 5, 11 V 5 A, device |
| Laptop | 5 V 3 A pos 1, device, mismatch | same | same | same | same |

Reading it: the Pixel is happy everywhere a PPS reaches 11 V. The OP15 needs a 5 A path: the 3 A cable holds both bricks down; the 5 A identity lifts them to the full 55 W; the ASUS 30 W brick is the limit. The Laptop's single sink PDO is 5 V 3 A with the Higher Capability bit, so it gets 5 V with a capability mismatch on every charger.

## 3. What a twin cannot predict

The prediction is a PD contract. It cannot say anything about:

- **Proprietary fast-charge.** A phone that charges faster over a vendor protocol (a proprietary Xiaomi 6 A mode, OPPO SVOOC, Apple's own charging steps) may get more than the PD contract says. The twin predicts the *standard* PD result only.
- **Battery-state-dependent PPS.** A phone's PPS current follows its battery temperature and state of charge; the twin predicts the contract at its best, not what the phone will pull at 80 %.
- **Any charger quirk** that depends on timing or history. That belongs to the charger test (`docs/CHARGER_QUIRKS.md`), not to a profile.

## 4. Learning a twin

A twin is learned from one of two observations. The record keeps only what is needed to predict (compact: a typical phone is **52 bytes**).

| Source | Command | When |
| :--- | :--- | :--- |
| Identify, device **alone** on the K2 | screen: Tools > Device twins > Learn row; shell `twin learn identify [dry] [name]`; host `k2_remote.py twin learn identify` | the device is attached to the K2 with no charger. Identify (`identify-device`, `pd::IdState`, `include/k2/pd/pd_identify.hpp`) asks it for identity, SVIDs, modes and its Sink_Capabilities. |
| A Request the K2 **sniffed** inline | shell `twin learn sniff [dry] [name]`; host `twin learn sniff` | a real charger and the real device, the K2 between them sending nothing. The sniffed Request (the contract the device really asked for) is stored next to the sink PDOs. |

`dry` shows what would be learned and saves nothing. Learning the same device again merges into its twin (same VID and PID): the library says `updated` or `unchanged`. The library holds 12 twins, in flash files `/twin/NN.twn`, names are 1 to 24 characters, an auto name is "<vendor> <product type>". A twin file written by a newer firmware is refused and never overwritten, and a damaged file is never replaced automatically.

### Record format

Magic "K2T", CRC16 CCITT, two versions:

- **v1**: 40 to 76 bytes: identity, sink PDOs, optionally the sniffed Request pair (flag bit 0).
- **v2**: v1 plus a **declared block** of 3 to 35 bytes (flag bit 1) and an **observed block** of 18 bytes (flag bit 2). A record without USB data is written as v1, byte-identical to what a firmware without the USB properties wrote, and old records stay readable. Any other flag bit, or flag bit 1 or 2 in a v1 record, is damage.
- Largest record 76 + 35 + 18 = 129 bytes; the typical phone (three sink PDOs) is 52 bytes.

Cross-check vectors (also in `tests/test_twin_lib.cpp` and `tests/test_twin_proto.py`):

```
PIXEL     (52 B) 4b3254010300d118e14ea0010203506978656c203900000000000000000000000000000000002c9101002cd102002c21dcc8e465
PIXEL_REQ (60 B) 4b3254010501d118e14ea0010203506978656c203900000000000000000000000000000000002c9101002cd102002c21dcc82cb104102cb10400a8b5
```

## 5. Running it: predict, prove, read

### On the screen

Tools > Device twins: one row per twin. The row shows the predicted wattage and a verdict cell, the focused row shows what limits it. The footer says how many twins would get all they need on this charger ("3 of 5 full on charger"), or "No twin yet. Learn one", or "No charger: no predictions".

| Action | Effect |
| :--- | :--- |
| +/- | move |
| OK on a twin | first OK: the live proof is requested; if the charger will be asked, the row arms ("Asks the charger. Confirm"). Second OK within 5 s starts it with confirmation. A cursor move or the window expiring disarms. |
| BACK while running | reads "Stop", cancels (5 V back) |
| OK on the Learn row | learns from the last Identify (footers say what happened: "Twin learned", "Twin updated", "Already known", "Run Identify first", "Nothing to learn from", "Library full") |

The proof refuses up front, with a footer, when the live proof cannot be run: a tool or test is running, a device is inline, the power monitor is not ready, there is no charger to run against. The page has no on-device delete (use the shell or the host tool).

### From the shell

```
twin list                          the library
twin predict [n|all]               what this charger gives each twin, no transmission
twin run [n|all] confirm           the live proof (needs a PD source already attached)
twin status | twin cancel
twin learn identify|sniff [dry] [name]
twin rename n name | twin delete n confirm
```

### From the host

```
python3 tools/k2_remote.py twin list
python3 tools/k2_remote.py twin show 1
python3 tools/k2_remote.py twin predict [ids]
python3 tools/k2_remote.py twin run [ids] --confirm [--wait S]
python3 tools/k2_remote.py twin status | result N | cancel
python3 tools/k2_remote.py twin learn identify|sniff [--name N] [--dry]
python3 tools/k2_remote.py twin rename N NAME | delete N --confirm
python3 tools/k2_remote.py twin usb N [declared|observed]
python3 tools/k2_remote.py twin export --out twins.json
```

Print format of `twin predict` (identical text on the screen, the shell and the table of the host tool):

```
#1 Pixel 9  33W PPS  the device takes no more  pos 6 11000mV 3000mA
#2 OP15  36W of 55W  limited by the cable  pos 3 12000mV 3000mA mismatch
```

`present::summary` writes `NNW PPS`, `NNW NV` (fixed), `NNW of MMW` (it gets less than it wants), `5V only`, or `no contract`.

### The live proof

`Runner` (`include/k2/twin/live.hpp`) refuses in this order: already running (Busy), no twin saved, a tool/test busy, a device inline, no power monitor, no source (`NoSource`: VBUS under 1 V or no valid source capabilities), and a missing confirmation (`NeedConfirm`). So **`twin run` needs an active sink first** (`k2_remote.py pd attach`).

Then per twin, in six steps (`live.hpp` header): plan (predict on the list the charger shows now; nothing usable, or a level above the monitor ceiling, ends the twin as Skipped before anything is touched), **attach afresh** with the Get_Sink_Cap answer pointed at the twin's PDOs (Rd off until VBUS is dark, then Rd on; a self-powered K2 does a Soft_Reset instead and keeps its Rd), wait for the charger's list and predict again on **that** list (a charger may list other levels for another sink; the second prediction is the one requested), send the Request (built by `pd::plan_request`, never more current than advertised), hold, release to 5 V. After the last twin the K2's own sink capabilities come back and it attaches once more, so the charger's picture of the sink is the K2's again. A cancel skips that last attach: it asks for 5 V and clears the override, and the charger keeps the twin's words until the next attach of any kind. The checks:

| Verdict | Meaning |
| :--- | :--- |
| Granted | Accept, PS_RDY, VBUS inside the band for the held second |
| Refused | the charger rejected or never accepted the Request |
| NoPsRdy | Accept came, no PS_RDY |
| OutOfBand | VBUS settled outside the band (the measured volts are shown) |
| NoCaps | the charger sent no usable source capabilities |
| Reset | the charger hard-reset |
| Unsafe | the monitor read over the safe cap (`pd::overcap_abort_mv`) |
| Skipped | the prediction is above the monitor ceiling (`pd::fits_monitor`): 48 V on a 36 V K2 is skipped with its reason, never requested |
| Cancelled | the user stopped it |

After every twin the K2 releases to 5 V, and after the whole run it restores its own sink capabilities. A self-powered K2 cannot drop its own supply, so it holds with a Soft_Reset and keeps its Rd. The run counts as a test for the stop report (`stop`), and a run started from the shell is cancelled when the shell session closes; a run started from the HID tool is left to its own lease. Judge VBUS with `power::vbus_fast_mv()` (the latest sample), never the display filter.

The verdicts stay in the runner until the next run; the page hides them when the charger's list changes.

## 6. USB data properties (item 7)

Besides the PD contract, a twin keeps what a device says about its USB data lines. Two blocks, with different trust:

- **Declared** (from Identify): product-type VDOs (Discover Identity words 4 and 5), up to six SVIDs, and the first mode VDO of the first two Discover Modes answers (DisplayPort capabilities, the Thunderbolt mode).
- **Observed** (from messages seen on the wire while learning or sniffing): the partner VID, the family the classifier assigned (`Family`, `Tbt{None,Tbt3,Tbt4,Tbt5}`), flags, the DisplayPort pin assignment seen, the data rate the partner declared or configured (`mbps`), the Enter_USB data object and the Thunderbolt Enter mode object.

Decoders are in `include/k2/twin/usbdata.hpp`; the class labels are the ones `include/k2/pd/usb_link.hpp` already uses on the Connection and Black Box link views. `twin usb N [declared|observed]` and the `show` view print them; `twin export` writes the raw record hex plus the same text.

### 6.1 The honesty text (what these numbers are not)

The host tool prints a short form of this with every USB view (`USB_HONESTY` in `tools/twin_proto.py`); it is the reason for the labels "declared" and "observed":

- The K2 sits on the CC wire. **SuperSpeed lanes cannot be measured**: there is no eye, no link training, no throughput.
- A **link speed is what PD messages declare or configure**, for example the USB highest-speed code in a UFP/DFP VDO or the data rate in Enter_USB. It says what the device asked for, not what a cable and a port actually reach.
- **USB 2.0 on D+/D-** is seen only as **activity** (a pull-up, a line state change). It is not a speed test.

### 6.2 Thunderbolt 4 / 5 is an inference

"Thunderbolt 4" and "Thunderbolt 5" are **certification levels**, not wire facts. They are not alternate modes: USB4 is entered with **Enter_USB**, while Thunderbolt 3 is an alternate mode (the 0x8087 SVID, entered with Enter_Mode). The K2 therefore never says "Thunderbolt 4 certified". It infers, and labels the result as inferred:

| Label | What the K2 saw |
| :--- | :--- |
| `USB4 40G TBT4` ("TB4 class") | the device declares USB4 at 40 Gbit/s **and** lists the 0x8087 mode |
| `USB4 80G` ("TB5 class") | the highest-speed code `100b` (USB4 Gen4, 80 Gbit/s), through the Usb4v2 class |

The provenance tag on these decodes is "inferred". A "TB4 class" device may still fail the certification; the K2 has no way to know.

## 7. Impersonating a device (out of scope)

Answering the charger with the twin's **sink capabilities** is built and is what the live proof does (a charger decides its offer from them; on a PD 3.2 charger the extended sink capabilities with Sink Modes 0x23 are what unlock SPR AVS). Claiming the device's **identity** (Discover Identity, SVIDs, Modes, as a host or a dock would) is **not built and not planned**. It would need all of the following, each a separate piece of work:

1. A role: the K2 would have to answer as a UFP or DFP with a product type, and keep answering every structured VDM, every message ID and every retry (a wrong or silent answer makes a host drop the link).
2. The twin's full Discover Identity words (`Identity.vdos` keeps six words today, so a third product-type VDO is lost) and its Modes beyond the first two SVIDs.
3. Enter_Mode / Enter_USB handling and a data role swap, which means the K2 acts as a data partner, not a power sink.
4. Safety: never with a laptop inline, never together with an active PD request on the same CC line.

The live proof therefore never changes who the K2 claims to be, only what it claims to need. What it also cannot reproduce of the twin (reverse-engineered limits):

- the **PD revision** of every message: the K2 answers in the revision the charger announced; the twin's own revision is stored but not spoken;
- the **Sink_Capabilities_Extended** block: the K2 sends its own, the twin keeps none;
- the **RDO** flags: `plan_request` builds the Request without Capability Mismatch, whatever the real device would set;
- anything a charger decides from D+/D-, or from a vendor protocol the device speaks.

## 8. Design only: "data and display match" (item 7d)

This section is design. Nothing in it is built.

**Question:** "Will this laptop show this monitor, and at what speed will this dock link?" With a twin of the laptop and a twin of the dock or monitor, the K2 can predict without either device present:

- **DisplayPort pin assignment and lane count.** The laptop's DFP capability VDO (the DP capabilities word of the DP mode) lists which pin assignments it can drive, and the dock's UFP word lists which it accepts. The intersection picks the assignment; a 4-lane assignment (C/E) gives full DP bandwidth, a 2-lane one (D/F) shares lanes with USB 3.
- **USB4 / Thunderbolt 3 from both sides plus the cable.** Both twins' USB4 or 0x8087 declarations, and the cable's e-marker (speed and active/passive) decide whether the link can reach 40G or 80G, falling back to USB 3.x, then USB 2.0.
- The output would be one line per pair: `4-lane DP, USB4 40G` or `2-lane DP + USB 3.2 10G`, plus the limiter (laptop, dock or cable) as for power.

What this needs that exists: the declared block (the DP capabilities and Thunderbolt mode VDOs) and the cable e-marker facts. What it needs that does not: a **pair view** on the screen (two twin pickers), a pure `predict_link(Twin host, Twin dock, CableFacts)` next to `predict.hpp` with its own tests, and real capture data of laptop and dock pairs to check the pin-assignment rules against.

**Replaying a twin's Discover Identity / SVIDs / Modes to a host** (making the K2 look like the dock): see section 7. It would reuse the e-marker spoof's responder shape (`k2::emk_spoof::respond()` with quirk rows) but on SOP, not SOP', and with the structured-VDM state the K2 does not keep today. It stays out until a host-side use case justifies the risk of a stuck link.

## 9. What the Identify interrogation does to the device's contract

Identify (Tools > Who's attached, `identify-device`, and the Learn row) is a conversation: the K2 sends Discover Identity and the later steps to the device, which means the K2 holds a port role and a contract during it.

- **Device alone on the K2** (no charger): the existing Identify-alone path waits for a Ready contract with the device, presents Rd, and the Rd gate keeps the link; at the end of the run the release drops the contract and the K2 hands back to 5 V where that hand-back already works. **Dropping the device's contract for the interrogation is acceptable** (operator decision) because the device is alone and nothing is being charged.
- **Inline** (a charger and a device with the K2 between them): the K2 **sends nothing**. Learning there is `twin learn sniff`, which only reads the device's Request. The Identify command is refused inline by the existing guard.
- **Physically impossible**: anything that would need VCONN is not attempted ("stop that part").

This is documented from the code paths and their tests. **Not hardware-verified**: what the device's contract does after the end-of-run release (whether it returns to 5 V, stays or resets) is the first thing the hardware plan below measures.

## 10. Hardware plan

For the main session; the K2 is on PC power, USB HID enabled, left port with a C-to-A cable. One device tool at a time, every call with a timeout, no load on the output, no `fault`.

**A. Learn a twin from a phone alone** (no charger)

1. `python3 tools/k2_remote.py ping` (hash without `-dirty`).
2. Attach the phone alone to the K2 top port. `python3 tools/k2_remote.py identify-device --wait 20` and confirm the sequence finished (Identity, SVIDs, Modes, Sink caps).
3. `python3 tools/k2_remote.py twin learn identify --dry --name "Phone"`. Expect `twin learned (dry, not saved) #N Phone vid=... pdos=3`.
4. Drop `--dry`: `twin learn identify --name Phone`. Expect `twin saved #1 Phone ...`; again: `... unchanged`.
5. `twin show 1`, `twin usb 1`, `twin export --out runs/twin-phone.json`. Check record size 52 B for three sink PDOs, and the USB text prints the honesty lines.
6. Record: `ping` before and after, VBUS and the contract at each step (`state`), and whether the phone's contract is back at 5 V or reset after the run (section 9).

**B. Predict, then prove, on NOVOO RG100 and CUKTECH C1** (K2 on PC power, charger on the bottom port, no load)

1. Attach the charger. `python3 tools/k2_remote.py pd attach` (5 V). Wait for a contract (`pd status`).
2. `python3 tools/k2_remote.py twin predict`. Expected on the C1 with a 3 A cable for a Pixel-class twin: `#N Pixel  33W PPS  the device takes no more  pos 6 11000mV 3000mA`; for an OP15-class twin: `36W of 55W  limited by the cable  pos 3 12000mV 3000mA`. On the NOVOO: PPS pos 5.
3. `python3 tools/k2_remote.py twin run --confirm --wait 60`. Expect each twin `Granted` with `ok 11.0V` (within the band), the contract back at 5 V after, and `twin status` listing `#N <name>  33W PPS ok 11.0V`.
4. Check mid-run: `ping` uptime against wall-clock (a reboot means stop and investigate).
5. `twin run 1 --confirm` without `pd attach` first must return `NoSource`; with a device inline it must return `Inline`.
6. Screen: `k2_remote.py page twin`, `screenshot --scale 2`; compare with the mock set in `runs/ui-twin/` of the main checkout.
7. Never request above 36 V; a 48 V prediction must show `Skipped` with its reason.

Pass: every verdict matches the prediction table above for the chargers and cable on the desk, no reboot, 5 V restored, no hard reset except the one a refused request causes.

## 11. Related

- `docs/ARCHITECTURE.md`: module map (Device twin row).
- `docs/USER_GUIDE.md`: the Tools reference.
- `docs/CHARGER_QUIRKS.md` and `docs/CHARGER_TEST_FLOW.md`: charger behaviour is detected by signature, never by VID:PID; a twin's VID labels a device, it never drives behaviour.
- Reused building blocks: Identify (`include/k2/pd/pd_identify.hpp`), `fusb302::sink_request`, `pd_sink`, `pd::plan_request`, the e-marker library's storage and UI pattern, the Who's attached match view.
