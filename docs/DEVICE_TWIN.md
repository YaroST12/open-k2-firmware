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
| Hardware glue | `include/k2/twin/twin_hw.hpp`, `src/twin/twin_hw.cpp` | flash library via `k2::storage`, Identify, sniff and inline-interrogation learning, the live proof over `fusb302` |
| Inline interrogation (pure sequencer) | `include/k2/pd/pd_interrogate.hpp`, `src/pd/pd_interrogate.cpp` | reset, sniff, ask, reset; driven by `fusb302::interrogate_*` (section 9) |
| Remote | `wire.hpp`, `Cmd::Twin = 0x63` | ops List 0, Get 1, Predict 2, Learn 3, Rename 4, Delete 5, Run 6, Status 7, Result 8, Usb 9, Cancel 10, Inline 11 |
| Shell | `twin list \| status \| cancel \| predict \| run \| learn \| rename \| delete` | `src/core/shell.cpp`, `src/core/shell_hw.cpp` |
| Host tool | `tools/k2_remote.py twin ...`, `tools/twin_proto.py` | `show`, `usb`, `export` are host-side views of the same records |
| Screen | `src/ui/ui_twin.cpp`, `include/k2/ui/ui_twin.hpp` | Tools > Device twins |

## 2. The prediction policy

The rules as built (`predict.hpp`, pinned by `tests/test_twin_predict.cpp`, one test per rule):

1. A fixed offer needs a sink fixed PDO **at the same voltage**, or a sink **Variable** or **Battery** PDO whose voltage range contains it (a laptop that lists a 5 V level and a 5-20 V 3.25 A Variable entry takes a 20 V level; a Battery entry gives the current as its power over the level's voltage). Both, a PPS range and the sniffed Request count toward **what the device wants**: that laptop wants 65 W, not 15 W, and against a 20 V 3 A charger gets 60 W of 65 W. Variable and Battery entries on the *charger's* side are not modelled.
2. The current is `min(what the source offers after the cable, what the sink PDO says)`.
3. A PPS offer needs a sink PPS range that overlaps; the voltage is the top of the overlap.
4. **Kind first, then power.** A PPS level above 5 V beats every fixed level whatever the power: a device that lists a PPS range takes it (direct charge), so a PPS offer that overlaps the twin's range wins even against a fixed level of more watts (the OnePlus 15 on a 3 A charger with PPS 5-11 V is PPS, not 12 V 3 A). Inside a kind the highest power wins, then the higher voltage, then the lower position. A PPS range that tops out at 5 V is no step up and ranks with the 5 V level. A PPS answer is a **range**, not an operating point: it reads "PPS up to 33W" (the top of the overlap), because the device picks its own voltage inside it (a direct-charge phone runs near 8-9 V). The live proof asks for that top, which is always inside the charger's APDO and the twin's range.
5. A sink whose PDO1 carries the **Higher Capability** bit and whose own PDO1 is 5 V gets `FiveV` with *mismatch*: the K2 never claims more than the device announced it can take.
6. **None** when the source caps are invalid, the twin has no sink PDOs (and no sniffed Request), or PDO1 is not 5 V.
7. A **sniffed Request** (what the device itself asked for on a real charger) wins over rules 4, 5 and 11 when the same shape (same kind and voltage) exists on this charger.
8. Never above what the charger advertises. A cable that clips the current gives `Limiter::Cable`. The monitor ceiling (INA226 reads up to 36 V) is the live runner's business, not the prediction's: it is a `Skipped` verdict, never a changed prediction.
9. **Mismatch** is set when the source current after the cable is below the twin's need, or by rule 5.
10. **Limiter**: if the device gets what it needs, `Device` when the charger offers more (the device is the limit), else `None` ("full"); if it falls short, `Cable` when the cable clipped the offer (or the cable alone holds a bigger offer back), else `Charger`.
11. A **learned kind preference** replaces rule 4's default order. When a sniff sees the device choose while a charger offers both a fixed level above 5 V and a PPS range above 5 V, the twin stores which kind it took (`Twin::pref`, `KindPref`). A twin that took a fixed level ranks fixed above 5 V before PPS (it still falls back to the other kind when its own has no usable level); a twin that took PPS, and a twin never seen choosing, rank PPS first. The preference applies to any charger, not only one with an exact-shape match; rule 7 still wins when it applies. Taking the 5 V level, or a charger that offers one kind only, teaches nothing and leaves the stored preference alone. The preference lives in two flag bits of the record (section 5), so it adds no bytes; the interrogation path does not learn it (it has no source list to compare against).

`Facts` carries what the K2 knows about this link: the charger's claimed power (`claim_w`, from Get_Source_Cap_Extended) and the physical cable's current and voltage rating from its e-marker (`cable_ma`, `cable_mv`; zero means "not read", never the 3 A / 20 V default). The "held by the cable" test of rule 10 is `rdintent::offer_held_by_cable(caps, claim_w)` (`include/k2/core/rd_intent.hpp`, which wraps `cable_limited(caps, claim_w)`): the charger's own claim when there is one, else the 20 V / 3.00 A signature.

### Prediction table, real data

Three stored devices against five bench chargers (`test_real_matrix` in `tests/test_twin_predict.cpp`; every cell is asserted). The twin wants: Pixel 33 W, OP15 55 W, Laptop 15 W.

| Device | CUKTECH C1, 3 A cable | C1, 5 A cable | ASUS 30 W | NOVOO RG100, 3 A | NOVOO, 5 A |
| :--- | :--- | :--- | :--- | :--- | :--- |
| Pixel | PPS pos 6, 11 V 3 A (33 W), device | same | PPS pos 6, 11 V 3 A, full | PPS pos 5, 11 V 3 A, device | same |
| OP15 | PPS up to 11 V 3 A (33 W) pos 6, cable, mismatch | PPS pos 6, 11 V 5 A (55 W), device | PPS pos 6, 11 V 3 A, charger, mismatch | PPS pos 5, 11 V 3 A, cable, mismatch | PPS pos 5, 11 V 5 A, device |
| Laptop | 5 V 3 A pos 1, device, mismatch | same | same | same | same |

Reading it: the Pixel is happy everywhere a PPS reaches 11 V. Every PPS cell is a range ("PPS up to 33 W"): the position and 11 V are the top of the overlap the live proof asks for, not the voltage the phone will run. The OP15 lists a 12 V level too, but rule 4 puts its PPS range first, so on the 3 A C1 it is PPS (33 W of the 55 W it wants), not 12 V 3 A. The OP15 needs a 5 A path: the 3 A cable holds both bricks down; the 5 A identity lifts them to the full 55 W; the ASUS 30 W brick is the limit. The Laptop's single sink PDO is 5 V 3 A with the Higher Capability bit, so it gets 5 V with a capability mismatch on every charger.

## 3. What a twin cannot predict

The prediction is a PD contract. It cannot say anything about:

- **Proprietary fast-charge.** A phone that charges faster over a vendor protocol (a proprietary Xiaomi 6 A mode, OPPO SVOOC, Apple's own charging steps) may get more than the PD contract says. The twin predicts the *standard* PD result only.
- **Battery-state-dependent PPS.** A phone's PPS current follows its battery temperature and state of charge; the twin predicts the contract at its best, not what the phone will pull at 80 %.
- **Any charger quirk** that depends on timing or history. That belongs to the charger test (`docs/CHARGER_QUIRKS.md`), not to a profile.

## 4. Learning a twin

A twin is learned from one of three observations. The record keeps only what is needed to predict (compact: a typical phone is **52 bytes**).

| Source | Command | When |
| :--- | :--- | :--- |
| Identify, device **alone** on the K2 | screen: Tools > Device twins > Learn row; shell `twin learn identify [dry] [name]`; host `k2_remote.py twin learn identify` | the device is attached to the K2 with no charger. Identify (`identify-device`, `pd::IdState`, `include/k2/pd/pd_identify.hpp`) asks it for identity, SVIDs, modes and its Sink_Capabilities. |
| A Request the K2 **sniffed** inline | shell `twin learn sniff [dry] [name]`; host `twin learn sniff` | a real charger and the real device, the K2 between them sending nothing. The sniffed Request (the contract the device really asked for) is stored next to the sink PDOs. |
| The device **interrogated** inline | screen: the Learn row with a device inline (confirm first); shell `twin learn inline confirm [dry] [name]`; host `twin learn inline --confirm [--dry] [--name N]`, lab `pd interrogate --inline --confirm [--save]` | a real charger and the real device, the K2 between them. The K2 resets the link, asks the device as its source and resets it again (section 9). The twin gets the identity, SVIDs, modes, sink PDOs, the sniffed Request, the Revision and Sink_Capabilities_Extended. |

`dry` shows what would be learned and saves nothing. Learning the same device again merges into its twin: the library says `updated` or `unchanged`.

### Same device (the merge key)

`twin::same_device` (`include/k2/twin/learn.hpp`) decides whether a new observation belongs to a saved twin:

1. Both carry an identity (VID not 0): the same VID and PID, nothing else.
2. One carries an identity and the other does not: never the same. A sink PDO list or a Request does not prove which identified device it was (two phones of one vendor list the same PDOs).
3. Neither carries one: the same sink PDO list when both have one, otherwise the same sniffed Request (the RDO and the source PDO it pointed at). With nothing to compare the two are different devices: two devices that never answered Discover Identity do not merge just because both read 0000:0000.

What the new observation knows replaces the saved value, what it does not know leaves the saved value alone, and the saved name stays.

The library holds 12 twins The library holds 12 twins, in flash files `/twin/NN.twn`, names are 1 to 24 characters, an auto name is the vendor plus the most the device asks for in whole watts ("Google 33W", "VID 1234 33W", "Device 33W"; the figure is `want_mw`, so Variable and Battery sink PDOs and the sniffed Request count), with " 2", " 3" on repeats. A saved name still in the old auto form (the watts before those counted, e.g. "Lenovo 15W" for a 65 W laptop) is corrected the next time that device is saved (any learn of it); a name the user typed never changes. A twin file written by a newer firmware is refused and never overwritten, and a damaged file is never replaced automatically.

### Record format

Magic "K2T", CRC16 CCITT, three versions:

- **v1**: 40 to 76 bytes: identity, sink PDOs, optionally the sniffed Request pair (flag bit 0).
- **v2**: v1 plus a **declared block** of 3 to 35 bytes (flag bit 1) and an **observed block** of 18 bytes (flag bit 2). A record without USB data is written as v1, byte-identical to what a firmware without the USB properties wrote, and old records stay readable. Any other flag bit, or flag bit 1 or 2 in a v1 record, is damage.
- **v3**: v2 plus the PD 3 answers of an inline interrogation: the **Revision** data object, 4 bytes (flag bit 3), and the **Sink_Capabilities_Extended** block, a length byte (1 to 24) and that many bytes (flag bit 4). Written only when one of them is present, so every older record stays byte-identical; flag bit 3 or 4 in a v1 or v2 record is damage.
- **v3, kind preference**: flag bit 5 = the device took PPS, flag bit 6 = it took a fixed level, when both kinds were offered (rule 11). The bits carry the whole fact, no bytes follow, so a record never grows. A twin with a preference is written as v3; a record without either bit loads as "no preference". Both bits at once, or either in a v1 or v2 record, is damage; flag bit 7 stays unused.
- Largest record 76 + 35 + 18 = 129 bytes, for every version: twelve of them are what the volume budget holds for `/twin` (one byte more each spills the directory into another metadata pair). The v3 blocks go in only while the record stays within 129 bytes, the extended block left out first, then the Revision; only a twin that also carries full USB blocks loses them. The typical phone (three sink PDOs) is 52 bytes, 89 with a Request and both v3 blocks.

Cross-check vectors (also in `tests/test_twin_lib.cpp` and `tests/test_twin_proto.py`):

```
PIXEL     (52 B) 4b3254010300d118e14ea0010203506978656c203900000000000000000000000000000000002c9101002cd102002c21dcc8e465
PIXEL_REQ (60 B) 4b3254010501d118e14ea0010203506978656c203900000000000000000000000000000000002c9101002cd102002c21dcc82cb104102cb10400a8b5
CANON_V3  (65 B) 4b3254030619000000000000020243616e6f6e000000000000000000000000000000000000002c9101002cb104002cb144432cb10400000010320401020304b427
```

## 5. Running it: predict, prove, read

### On the screen

Tools > Device twins (layout and wording: `docs/UI_SYSTEM.md` section 9). Two views on one page.

**List.** One row per twin, its full name, with a 12 px shape marker for the prediction against this charger: solid = full power, half = limited, hollow = 5 V only (half and hollow in caution ink). Once a live proof ran on this charger the marker is a verdict cell. The focused row opens a note: "gets 33W, full", "gets 36 of 55W, cable", "5V only, device", or the proof result ("proved 33W", "charger refused 11V"). The last row is "Learn from device". Footer: the charger's top offer and the count ("PD 20V3A: 2 of 5 full"), after proofs "1 of 2 proved", or "No twin yet. Learn one", "No charger: no predictions".

**Detail** (OK on a twin). TopBar "Twin"; the name; "gets" against "wants" in watts with a bar, the contract and what limits it ("limit: cable (3A)"); InfoBlock Wants / Offer / USB; then the actions Prove live, Re-learn from device, Delete twin. The page scrolls by whole lines so the focused action stays on screen.

| Action | Effect |
| :--- | :--- |
| +/- | move (list cursor, or the detail's action) |
| OK on a twin | opens its detail |
| OK on Prove live | arms the kit ActionCell (CONFIRM, 5 s drain, footer "Asks the charger for 12V"); a second OK starts the proof, which shows Asked / VBUS in the same box. A move or the window expiring disarms. |
| OK on Delete twin | arms ("Deletes this twin for good"); a second OK deletes it and returns to the list ("Twin deleted") |
| OK on Re-learn | as the Learn row; a different device is saved as another twin ("Saved as another twin") |
| BACK | stops a read or a proof (5 V back), disarms, closes the detail, then leaves the page |
| OK on the Learn row ("Learn from device") | a device alone on the K2 (a dual-role phone: VBUS dark, no source caps, its Rp toggling) is read first: the page starts the Sink-tab Identify, shows "Reading the device" (Back stops it), and learns when it ends ("Device did not answer" when it never did). A device **inline** behind its charger is interrogated: the first OK arms ("Resets the link. Confirm", or the refusal: "Tool or test running", "K2 holds the contract", "K2 runs on this VBUS"), the second starts it; the footer follows it ("Resetting the link", "Asking the device"), Back stops it ("Stopped. Link reset", the final reset still goes out), the end learns ("Twin learned", "Learned. Contract changed" in amber when the device asked differently after the reset, "Device did not answer"). VBUS dark with no charger listing and nothing of the above: "No power: add a charger". Anything else learns from the last Identify. Footers: "Twin learned", "Twin updated", "Already known", "Plug a device in alone", "Nothing to learn from", "Library full" |

The proof refuses up front, with a footer, when the live proof cannot be run: a tool or test is running, a device is inline, the power monitor is not ready, there is no charger to run against. Renaming stays off the device (shell or host tool).

### From the shell

```
twin list                          the library
twin predict [n|all]               what this charger gives each twin, no transmission
twin run [n|all] confirm           the live proof (wakes a charger that has not offered)
twin status | twin cancel
twin learn identify|sniff [dry] [name]
twin learn inline confirm [dry] [name]   interrogate the device inline (two Hard Resets); `twin status` follows it
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
python3 tools/k2_remote.py twin learn inline --confirm [--name N] [--dry] [--wait S]
python3 tools/k2_remote.py pd interrogate --inline --confirm [--save] [--name N]   (lab face: dry unless --save)
python3 tools/k2_remote.py twin rename N NAME | delete N --confirm
python3 tools/k2_remote.py twin usb N [declared|observed]
python3 tools/k2_remote.py twin export --out twins.json
```

Print format of `twin predict` (identical text on the screen, the shell and the table of the host tool):

```
#1 Pixel 9  PPS up to 33W  the device takes no more  pos 6 11000mV 3000mA
#2 OP15  PPS up to 33W  limited by the cable  pos 6 11000mV 3000mA mismatch
```

`present::summary` writes `PPS up to NNW` (a predicted PPS range: the ceiling, never an operating point), `NNW PPS` (the Request the K2 saw, repeated: a fact), `NNW NV` (fixed), `NNW of MMW` (it gets less than it wants), `5V only`, or `no contract`.

### The live proof

`Runner` (`include/k2/twin/live.hpp`) refuses in this order: already running (Busy), no twin saved, a tool/test busy, a device inline, no power monitor, no source (`NoSource`: VBUS under 1 V or no valid source capabilities), and a missing confirmation (`NeedConfirm`). With no list yet, the runner **wakes the charger itself** (UI_SYSTEM.md 9.9): when a charger waits on the plug (`WaitKind::ChargerWaiting`) or VBUS is up without caps, a confirmed `twin run` (or the page's Prove live) presents Rd through `fusb302::rd_for_run(true)`, waits up to 8 s for the list (the 5 V contract is taken by that same Rd path, never more before the proof), predicts on it and goes on. No offer ends the run with reason `no offer` and Rd goes back at once; a device inline or a dual-role port (a phone toggling its Rp) is never woken and still gets `NoSource`/`Inline`. `twin status` shows `waking` during the wake. The explicit route still works: `k2_remote.py pd attach` presents Rd and returns (no request), the capabilities arrive, a second `pd attach` requests 5 V. The twin runner does not call `pd attach` itself: it re-attaches afresh with the twin's sink capabilities. A started run arms the host lease (it holds the output above 5 V); the lease settles by itself once the run has released and VBUS is back at 5 V.

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
- **Inline** (a charger and a device with the K2 between them): by default the K2 **sends nothing**; `twin learn sniff` only reads the device's Request and the Identify command is refused inline by the existing guard. The one exception is the **inline interrogation** below (user decision 2026-11-13: "for the device ID we have to be able to interrogate things"); it runs only on an explicit confirm.
- **Physically impossible**: anything that would need VCONN is not attempted ("stop that part").

This is documented from the code paths and their tests. **Not hardware-verified**: what the device's contract does after the end-of-run release (whether it returns to 5 V, stays or resets) is the first thing the hardware plan below measures.

### 9.1 The inline interrogation

The pure sequencer is `pd::Interrogate` (`include/k2/pd/pd_interrogate.hpp`, host-tested in `tests/test_pd_interrogate.cpp`); the driver (`fusb302::interrogate_start/abort/running/status`) owns the wire, `twin_hw::inline_*` learns from it, and the Device twins page, the shell and the remote start it.

| Phase | What happens |
| :--- | :--- |
| Reset1 | a Hard Reset from the K2; skipped when a fresh contract was sniffed (caps, Request, Accept, PS_RDY with no reset since) |
| Sniff1 | the renegotiation, sniffed: Source_Capabilities, the device's Request, Accept, PS_RDY (5 s at most) |
| Settle | 250 ms after PS_RDY and 150 ms of a quiet line (3 s at most): the charger's own post-contract questions go first, and what the device answers them is kept like an answer to the K2 |
| Ask | Get_Sink_Cap, Get_Sink_Cap_Extended and Get_Revision (PD 3.x only) first, then the vendor messages last: Discover Identity, Discover SVIDs and Discover Modes (the last two only when the Identity ACK says modal operation); a step already answered is not asked again |
| Reset2 | a second Hard Reset: the charger's MessageID bookkeeping and what it overheard are left behind |
| Sniff2 | the renegotiated contract, compared with the first (`same contract`, `contract changed`, `no contract after the reset`) |

Rules (all in the sequencer, each pinned by a host test):

- **Roles and revision.** The K2 speaks as the source: power role source, data role DFP (or what the source's header says), the contract's revision (the lower of the device's Request and the source's header). A PD 2.0 device is asked with SVDM version 1.0 and is never asked Get_Sink_Cap_Extended or Get_Revision.
- **MessageID.** The next ID after the newest non-GoodCRC SOP message with the source power role on the wire (the charger's or the K2's own), a fresh ID per attempt, so the device's duplicate filter never drops a question as a retransmission. Risk: the charger's own next message reuses that ID, and a device that filters strictly by ID may drop it once; the final reset makes that harmless.
- **Answers.** Only a message with the sink power role answers (the charger may also answer a question it overheard; ignored). A step gets two attempts 200 ms apart; NAK, Not_Supported and Reject end it.
- **Transmit.** Auto-CRC stays off (the K2 never GoodCRCs inline: the charger already does), the frame goes out once with no hardware retry (CONTROL3 retries 0 for that frame, restored after), and the transmit result does not count: the device's and the charger's GoodCRCs collide on the wire and the device answers anyway.
- **Bounded.** 20 s of asking from the start, at most 2 Hard Resets from the charger or the device mid-run; the final reset always goes out once a question was sent, also after BACK / `twin cancel`. While a reset or its renegotiation is in flight the driver's VBUS detach rule is held off (VBUS is expected to drop); the CC rule still ends the run on a real unplug.
- **A source that resets when asked.** A Hard Reset from the far end within 150 ms of a question we transmitted is blamed on that question (a dual-role source, such as a laptop, takes a message of the downstream-facing role as a role clash; hardware 2026-11-13: the reset came within 3 ms of our Discover Identity). The run ends at once with `charger resets when asked`: no final reset of ours, no resync, no further question, later Hard Resets ignored. One budget, never a loop. Because that is the likeliest cause of a first-question reset, the vendor messages go last, so Get_Sink_Cap and its siblings are asked before a source can react to them. The twin page says "Charger resets when asked" and the shell prints the same sentence; asking through that charger is not possible, the device has to be asked on a charger that tolerates it (or alone, with Learn from a device alone).
- **Each question is logged with what came back.** `[IQ] t=<ms> Q <step> id <n> try <k>: <result> (device msgs <a>, GoodCRC <0|1>, charger msgs <b>)`; the result is one of `device answered`, `device refused`, `only the charger spoke`, `device GoodCRC only`, `nothing`, `charger Hard Reset`. The transmit line carries the time the frame took in microseconds. The first Canon recording (a dual-battery charger behind a PD charger) showed the device never answering Get_Sink_Cap, Get_Sink_Cap_Extended or Get_Revision while the charger's own replies reached the FIFO; these lines say per question which side spoke. The cause of that silence is not proven; a CRC-failed reply would only be visible with a further INTERRUPT read, which the driver does not do.
- **Refused** while another PD tool runs (Identify, probe, re-read, trigger prepare, VCONN, an armed Get_Source_Cap, an SOP' responder such as the e-marker spoof), without a CC line, without a device inline, while a charger test, PD Stress, the SuperVOOC trigger, the FC scan, an FC release, a loop probe or a twin proof owns the ports, while the K2 holds or makes its own contract, and when the K2 is powered from this VBUS (the reset would cut its own supply).
- **Evidence.** `[IQ]` log lines (start, each phase, each transmitted question, the end with the asked / answered / refused / silent masks, both contracts, the answers) and a Black Box mark `interrogate` per phase change; `[TWIN] learn inline ...` for the learn.

The learned twin (`twin::from_interrogated`): the identity, product-type VDOs, SVIDs and modes, the sink PDOs, the device's revision, the Request of the first contract (else the second), the Revision data object and the Sink_Capabilities_Extended block (record v3). A run that read no contract and no sink PDOs teaches nothing ("Device did not answer").

On the page, a twin made from a sniffed Request alone (no sink PDOs) shows `asked 45W` (the contract's power: what the device requested, not what it draws) against a live `draws 0.3W` from the power monitor; "wants" is shown only when sink PDOs were read, never as `0W`. Inline, Connection > Read again only re-reads what the sniff holds and says `Inline: listens only`; Ask device is what transmits.

**Not hardware-verified.** Hardware, 2026-11-13: behind a PD charger the Canon dual-battery charger answered none of the questions (root cause not proven, see Evidence), behind a laptop the source Hard Reset at once (handled, above). First hardware check: a phone behind a bench charger, `pd interrogate --inline --confirm` (dry), then `twin learn inline --confirm`; record `ping` uptime, VBUS and both contracts, and that the phone charges again after the final reset.

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

1. Attach the charger in two calls. The first `python3 tools/k2_remote.py pd attach` (5 V default) only presents Rd when there is no capability list yet and returns without a request (no lease armed); wait until `pd status` shows the source capabilities. A second `pd attach` then selects the 5 V PDO and requests it (a 5 V attach never arms the lease either). Wait for the 5 V contract (`pd status`).
2. `python3 tools/k2_remote.py twin predict`. Expected on the C1 with a 3 A cable for a Pixel-class twin: `#N Pixel  PPS up to 33W  the device takes no more  pos 6 11000mV 3000mA`; for an OP15-class twin: `PPS up to 33W  limited by the cable  pos 6 11000mV 3000mA`. On the NOVOO: PPS pos 5.
3. `python3 tools/k2_remote.py twin run --confirm --wait 60`. Expect each twin `Granted` with `ok 11.0V` (within the band), the contract back at 5 V after, and `twin status` listing `#N <name>  PPS ok 11.0V`.
4. Check mid-run: `ping` uptime against wall-clock (a reboot means stop and investigate).
5. `twin run 1 --confirm` with a charger alone on the plug (passive boot, VBUS 0 V, Rp steady) wakes it: `twin status` shows `waking`, then the proof runs. With nothing on the plug or a dual-role phone it must return `NoSource`; with a device inline it must return `Inline`.
6. Screen: `k2_remote.py page twin`, `screenshot --scale 2`; compare with the mock set in `runs/ui-twin/` of the main checkout.
7. Never request above 36 V; a 48 V prediction must show `Skipped` with its reason.

Pass: every verdict matches the prediction table above for the chargers and cable on the desk, no reboot, 5 V restored, no hard reset except the one a refused request causes.

## 11. Related

- `docs/ARCHITECTURE.md`: module map (Device twin row).
- `docs/USER_GUIDE.md`: the Tools reference.
- `docs/CHARGER_QUIRKS.md` and `docs/CHARGER_TEST_FLOW.md`: charger behaviour is detected by signature, never by VID:PID; a twin's VID labels a device, it never drives behaviour.
- Reused building blocks: Identify (`include/k2/pd/pd_identify.hpp`), `fusb302::sink_request`, `pd_sink`, `pd::plan_request`, the e-marker library's storage and UI pattern, the Who's attached match view.
