# USB PD Cable E-Marker Formats, Rules, and Discovery Reference

Reference compiled for a no-load USB-C tester reading electronically marked cables (both via a Source/Charger over PD `VCONN_Swap` -> `SOP'`/`SOP''` and standalone), checking EPR capability, and decoding `Discover Identity` VDOs across PD revisions.

All findings are marked **verified** (directly read in extracted official USB-IF specification text) or **unverified** (secondary source, inferred, or not locally available in raw spec text).

---

## 1. Sources & Version Baseline

| Short Ref | Document Title | Revision / Version | Issue / Release Date | Local Path | Status |
|---|---|---|---|---|---|
| **PD R3.3** | *Universal Serial Bus Power Delivery Specification* | Release 3.3, Version 1.0 (`r3.3 v1.0`) | October 16, 2026 | `/tmp/usb_specs/pd33.txt` | **verified** |
| **PD R3.2** | *Universal Serial Bus Power Delivery Specification* | Release 3.2, Version 1.2 (`r3.2 v1.2`) | August 19, 2026 | `/tmp/usb_specs/pd32.txt` | **verified** |
| **USB-C R2.5** | *Universal Serial Bus Type-C Cable and Connector Specification* | Release 2.5 | March 2026 | `/tmp/usb_specs/typec25.txt` | **verified** |
| **PD CTS** | *USB Power Delivery Compliance Test Specification* | Q4, 2026 OR | October 10, 2026 | `/tmp/usb_specs/pd3_cts.txt` | **verified** |
| **SPT CTS** | *USB Type-C and PD Source Power Requirements CTS* | Revision 1.2 | November 4, 2026 | `/tmp/usb_specs/spt_v12.txt` | **verified** |
| **PD R3.0 / R3.1** | *USB PD Specification Rev 3.0 / Rev 3.1* | Historical releases | Pre-2026 | Not present locally | Legacy VDO formats **verified** via PD R3.3 Section 6.4.12.3 legacy tables (ECN #22), USB-C R2.5 Annex F, and PD CTS PD2/PD3 rules; intermediate R3.0 `30V`/`40V` labels are **unverified** against original R3.0 PDF. |

---

## 2. `Discover Identity` on `SOP'` / `SOP''`

### 2.1 Packet Targeting (`SOP'` vs `SOP''`) & Response Length

| Item | Value / Rule | Spec, Revision, Section, Table/Figure | Status | Verbatim Quote |
|---|---|---|---|---|
| `Discover Identity` target | `SOP'` only (never `SOP''`); `SOP'` answers apply to `SOP''` as well | PD R3.3 V1.0, Sec 8.7.1.2 (`pd33.txt:15318-15321`) | **verified** | `"The Initiator Shall Not send discovery SVDM REQs to the Cable Plug at SOP''. The Initiator Shall consider any responses to discovery SVDM REQs from the Cable Plug at SOP' to apply to the Cable Plug at SOP''."` |
| Number of Data Objects (`Message Header`) | `5` objects (Passive Cable, VPD, or single-VDO legacy Active Cable) or `6` objects (Active Cable SVDM 2.0 v1.2 / SVDM 2.1 v1.3 with VDO1 + VDO2) | PD R3.3 V1.0, Sec 6.4.12.3, Fig 6.5; PD CTS Q4 2026 `TEST.PD.VDM.CBL.1` (`pd3_cts.txt:9179-9184`) | **verified** | `"If Product Type (ID Header B29…27) = Active Cable, the check fails if the Number of Data Objects is not 6"` (PD3 mode, `pd3_cts.txt:9181-9182`). |
| VDO sequence in `Discover Identity` ACK | `VDM Header` -> `ID Header VDO` -> `Cert Stat VDO` -> `Product VDO` -> `Passive Cable / Active Cable VDO1 (+ VDO2) / VPD VDO` | PD R3.3 V1.0, Sec 6.4.12.3, Fig 6.5 (`pd33.txt:8979-8988`) | **verified** | Fig 6.5 (`Response`): `Message Header`, `VDM Header`, `ID Header VDO`, `Cert Stat VDO`, `Product VDO`, `0..3 Product Type VDO(s)`. |
| `Message Header` `Cable Plug` bit (bit `8`) on `SOP'`/`SOP''` | `0` = Message from DFP or UFP; `1` = Message from Cable Plug or VPD | PD R3.3 V1.0, Sec 6.2.1, Table 6.2 (`pd33.txt:6710-6757`) | **verified** | `"Cable Plug"` (`Bit 8` for `SOP'/SOP''`): `0 = Port`, `1 = Cable Plug or VPD`. |
| `Message Header` `Specification Revision` (bits `7...6`) | `00b` = Rev 1.0 (deprecated, treat as 2.0); `01b` = Rev 2.0; `10b` = Rev 3.x; `11b` = Reserved | PD R3.3 V1.0, Sec 6.2.1, Table 6.2 | **verified** | `"00b - Revision 1.0... 01b - Revision 2.0... 10b - Revision 3.x... 11b - Reserved, Shall Not be used."` |

---

### 2.2 Structured VDM Header & SVDM Version (`1.0` vs `2.0` vs `2.1`)

**Source**: PD R3.3 V1.0, Sec 6.4.12.2, **Table 6.33** (`pd33.txt:8788-8886`) & Sec 6.4.12.2.1 (`pd33.txt:8891-8896`); identical bit layout in PD R3.2 V1.2, Table 6.33 (`pd32.txt:8543-8662`).

| Bit(s) | Field Name | Encoding / Values | Status | Verbatim Quote (PD R3.3 Table 6.33) |
|---|---|---|---|---|
| `31...16` | `Standard or Vendor ID (SVID)` | `FF00h` = PD SID; `FF01h` = DPTC SID; or 16-bit USB-IF VID/SID | **verified** | `"FF00h - PD SID allocated to this specification by USB-IF."` |
| `15` | `VDM Type` | `1` = Structured VDM | **verified** | `"1 = Structured VDM."` |
| `14...13` | `Structured VDM Version (Major)` | `00b` = Version 1.0 (PD R3.0 v1.0, deprecated)<br>`01b` = Version 2.x (PD R3.0 v1.0a+)<br>`10b...11b` = Invalid -> receiver uses `01b` | **verified** | `"00b - Version 1.0 (Used by USB PD implementations conforming to USB PD Revision 3.0, Version 1.0 - Deprecated for new designs). 01b - Version 2.x (Used by USB PD R3.x implementations starting with USB PD Revision 3.0, Version 1.0a). 10b...11b - Invalid, receiver uses 01b (Version 2.x)"` |
| `12...11` | `Structured VDM Version (Minor)` | When `Command = 0...15`:<br>`00b` = Version 2.0 (pre-PD R3.1 v1.6)<br>`01b` = Version 2.1 (starting PD R3.1 v1.6)<br>`10b...11b` = Invalid<br>When `Command = 16...31`: SVID-defined | **verified** | `"00b - Version 2.0 (Used for ports implemented prior to USB PD Revision 3.1, Version 1.6). 01b - Version 2.1 (Used for ports implemented starting with USB PD Revision 3.1, Version 1.6) 10b...11b - Invalid, Shall Not be used"` |
| `10...8` | `Object Position` | `000b` for `Discover Identity` (`Cmd 0...3, 7...15`) | **verified** | `"When Command = 0...3, 7...15: 000b - Shall be used. 001b...111b - Invalid; receiver Should ignore this field."` |
| `7...6` | `Command Type` | `00b` = REQ, `01b` = ACK, `10b` = NAK, `11b` = BUSY | **verified** | `"00b - REQ (Request from Initiator Port) 01b - ACK (Acknowledge Response from Responder Port) 10b - NAK ... 11b - BUSY"` |
| `5` | `Reserved` | Ignored by receiver | **verified** | `"Reserved, receiver Shall ignore this field."` |
| `4...0` | `Command` | `1` = Discover Identity; `2` = Discover SVIDs; `3` = Discover Modes; `4` = Enter Mode; `5` = Exit Mode; `6` = Attention; `0, 7...15` = Invalid (NAK); `16...31` = SVID-specific | **verified** | `"1 - Discover Identity 2 - Discover SVIDs 3 - Discover Modes 4 - Enter Mode 5 - Exit Mode 6 - Attention"` |

**SVDM Version Negotiation Rule** (PD R3.3 V1.0, Sec 8.5.1, `pd33.txt:14898-14905`): **verified**
> *"To ensure interoperability with existing USB PD products, USB PD products Shall support every Structured VDM Version number starting from Version 1.0. On receipt of a VDM Header with a higher Version number than it supports, a Responder Shall respond using the highest Version number that the Responder supports. On receipt of a VDM Header with a lower Version number than it supports, a Responder Shall respond using the same Version number it received."*

---

### 2.3 ID Header VDO, Cert Stat VDO, and Product VDO

**ID Header VDO** — PD R3.3 V1.0, Sec 6.4.12.3.1, **Table 6.34** (`pd33.txt:8998-9089`) & PD R3.2 V1.2, Table 6.34 (`pd32.txt:8772-8870`):

| Bit(s) | Field Name | Encoding for `SOP'` (Cable Plug / VPD) | Status | Verbatim Quote / Notes |
|---|---|---|---|---|
| `31` | `Communications Capable as USB Host` | `0b` = Not capable; `1b` = Capable | **verified** | `"0b - Product is not capable of enumerating USB Devices. 1b - Product is capable of enumerating USB Devices."` (Usually `0b` for cables.) |
| `30` | `Communications Capable as USB Device` | `0b` = Not capable; `1b` = Capable | **verified** | `"0b - Product is not capable of being enumerated as a USB Device."` (Usually `0b` for cables; `1b` possible on VPD.) |
| `29...27` | `Product Type (UFP/Cable Plug/VPD)` | **`011b` = Passive Cable**<br>**`100b` = Active Cable**<br>**`110b` = VCONN Powered USB Device (VPD)**<br>`000b` = Not a Cable Plug/VPD<br>`001b...010b`, `101b`, `111b` = Invalid | **verified** | `"SOP' (Cable Plug/VPD): 000b - Not a Cable Plug/VPD ... 011b - Passive Cable 100b - Active Cable 101b - Invalid ... 110b - VCONN Powered USB Device (VPD) 111b - Invalid, Shall Not be use"` |
| `26` | **R3.3**: `Alternate Modes Supported`<br>**R3.2**: `Modal Operation Supported` | `0b` = Does not support Alternate Modes / Modal Op<br>`1b` = Supports Alternate Modes / Modal Op | **verified** | PD R3.3 (`pd33.txt:9045-9055`): `"0b - Product is not capable of supporting Alternate Modes. 1b - Product is capable of supporting Alternate Modes."` (Renamed in R3.3 ECN #7 from `"Modal Operation Supported"` in PD R3.2 `pd32.txt:8819`.) |
| `25...23` | `Product Type (DFP)` | **`Reserved, Shall Not be used` (`000b`)** for `SOP'` | **verified** | `"SOP': Reserved, Shall Not be used."` (`pd33.txt:9072-9074`). Note: In `SOP` (DFP), `100b` was restored from *"Deprecated, AMC"* (R3.2) back to *"Alternate Mode Controller (AMC)"* (R3.3 ECN #27). |
| `22...21` | `Connector Type` | `00b` = Deprecated, Unknown (used in PD 2.0 / SVDM 1.0)<br>`01b` = Invalid<br>`10b` = USB Type-C Receptacle<br>`11b` = USB Type-C Plug | **verified** | PD R3.3 (`pd33.txt:9077-9083`): `"00b - Deprecated, Unknown connector type 01b - Invalid, Shall Not be used. 10b - USB Type-C Receptacle 11b - USB Type-C Plug"`. **PD2 difference** (`pd3_cts.txt:9194-9201`, USB-C R2.5 Table F-4 `typec25.txt:19035`): in PD R2.0 / SVDM 1.0, bits `25...16` were all Reserved `0`. |
| `20...16` | `Reserved` | Ignored by receiver (`00000b`) | **verified** | `"Reserved, receiver Shall ignore this field."` |
| `15...0` | `USB Vendor ID` | 16-bit USB-IF VID (`FFFFh` if vendor has no VID per Sec 6.1.5) | **verified** | `"Vendor ID assigned to the product vendor by USB-IF."` |

**Cert Stat VDO** — PD R3.3 V1.0, Sec 6.4.12.3.4, **Table 6.38** (`pd33.txt:9189-9197`) / PD R3.2 V1.2, Table 6.38: **verified**
- `Bits 31...0`: **`XID`** — *"Value provided by the USB-IF and assigned to the product by the vendor. If the vendor does not have an XID, then it Shall return zero in this field."* (Identical in R3.2 & R3.3.)

**Product VDO** — PD R3.3 V1.0, Sec 6.4.12.3.5, **Table 6.39** (`pd33.txt:9200-9211`) / PD R3.2 V1.2, Table 6.39: **verified**
- `Bits 31...16`: **`USB Product ID`** — 16-bit PID assigned by Vendor (`0000h` if VID = `FFFFh`, Sec 6.1.5 `pd33.txt:6646`).
- `Bits 15...0`: **`bcdDevice`** — Device Version assigned by Vendor. (Identical in R3.2 & R3.3.)

---

### 2.4 Passive Cable VDO Layouts (SVDM 2.0/2.1 v1.0 vs SVDM 1.0 vs PD 2.0)

#### A. Current Passive Cable VDO (SVDM 2.0 / 2.1, VDO Version `1.0`)
**Source**: PD R3.3 V1.0, Sec 6.4.12.3.10, **Table 6.45** (`pd33.txt:9586-9721`); **identical bit-for-bit** in PD R3.2 V1.2, Sec 6.4.12.3.8, **Table 6.42** (`pd32.txt:9183-9317`). *(Passive Cable VDO defines only `VDO Version = 000b` Version 1.0; there is no v1.1–v1.4 for Passive Cable VDO.)*

| Bit(s) | Field Name | Encoding | Status | Verbatim Quote (`pd33.txt` Table 6.45) |
|---|---|---|---|---|
| `31...28` | `Hardware Version` | `0000b...1111b` | **verified** | `"0000b...1111b - HW Version assigned by the VID owner."` |
| `27...24` | `Firmware Version` | `0000b...1111b` | **verified** | `"0000b...1111b - FW Version assigned by the VID owner."` |
| `23...21` | `VDO Version` | `000b` = Version 1.0; `001b...111b` = Invalid | **verified** | `"000b - Version 1.0 001b...111b - Invalid, Shall Not be used."` |
| `20` | `Reserved` | Ignored | **verified** | `"Reserved, receiver Shall ignore this field."` |
| `19...18` | `USB Type-C plug to USB Type-C/Captive (Passive Cable)` | `00b` = Deprecated, Type-A<br>`01b` = Deprecated, Type-B<br>`10b` = USB Type-C<br>`11b` = Captive | **verified** | `"00b - Deprecated, USB Type-A 01b - Deprecated, USB Type-B 10b - USB Type-C 11b - Captive"` |
| `17` | `EPR Capable (Passive Cable)` | `0b` = Not EPR Capable<br>`1b` = EPR Capable (up to 48V @ 5A) | **verified** | `"Indicates when the cable is specifically designed for safe operation when carrying up to 48 volts at 5 amps. 0b - Cable is not EPR Capable 1b - Cable is EPR Capable"` |
| `16...13` | `Cable Latency (Passive Cable)` | `0000b` = Invalid<br>`0001b` = `<10ns (~1m)`<br>`0010b` = `10ns to 20ns (~2m)`<br>`0011b` = `20ns to 30ns (~3m)`<br>`0100b` = `30ns to 40ns (~4m)`<br>`0101b` = `40ns to 50ns (~5m)`<br>`0110b` = `50ns to 60ns (~6m)`<br>`0111b` = `60ns to 70ns (~7m)`<br>`1000b` = `> 70ns (>~7m)`<br>`1001b...1111b` = Invalid | **verified** | `"0001b - <10ns (~1m) ... 0111b - 60ns to 70ns (~7m) 1000b - > 70ns (>~7m) 1001b....1111b - Invalid, Shall Not be used"` |
| `12...11` | `Cable Termination Type (Passive Cable)` | `00b` = VCONN not required (only supports Discover Identity)<br>`01b` = VCONN required<br>`10b...11b` = Invalid | **verified** | `"00b - VCONN not required. Cable Plugs that only support Discover Identity Commands Shall set these bits to 00b. 01b - VCONN required 10b...11b - Invalid, Shall Not be used."` |
| `10...9` | `Maximum VBUS Voltage (Passive Cable)` | `00b` = `20V`<br>`01b..10b` = Deprecated (receiver assumes `00b` `20V`; historically `30V`/`40V`)<br>`11b` = `50V` | **verified** | `"00b - 20V 01b..10b - Deprecated, receiver Shall assume 00b (20V). 11b - 50V"` / `"when the Maximum VBUS Voltage (Passive Cable) field is 50V, a Fixed Supply of 48V can be Negotiated... where the absolute maximum voltage that can be applied to the cable is 50.9V. EPR Sinks with a captive cable Shall report 50V."` |
| `8...7` | `Reserved` | Ignored | **verified** | `"Reserved, receiver Shall ignore this field."` |
| `6...5` | `VBUS Current Handling Capability (Passive Cable)` | `00b` = Invalid (assume `3A`)<br>`01b` = `3A`<br>`10b` = `5A`<br>`11b` = Invalid (assume `3A`) | **verified** | `"00b - Invalid; receiver Shall assume 01b (3A). 01b - 3A 10b - 5A 11b - Invalid; receiver Shall assume 01b (3A)."` |
| `4...3` | `Reserved` | Ignored | **verified** | `"Reserved, receiver Shall ignore this field."` |
| `2...0` | `USB Highest Speed (Passive Cable)` | `000b` = `[USB2]` only<br>`001b` = `[USB3]` Gen1<br>`010b` = `[USB3]/[USB4]` Gen2<br>`011b` = `[USB4]` Gen3<br>`100b` = `[USB4]` Gen4<br>`101b...111b` = Invalid | **verified** | `"000b - [USB2] only, no SuperSpeed support. 001b - [USB3] Gen1 010b - [USB3]/[USB4] Gen2 011b - [USB4] Gen3 100b - [USB4] Gen4 101b...111b - Invalid, Shall Not be used."` Note: `"The DFP Shall consider all values, including Invalid... higher than the highest value that the DFP recognizes as being Valid and functionally compatible with the highest speed that the DFP supports."` |

#### B. Legacy Passive Cable VDO (SVDM 1.0, PD R3.0 v1.0)
**Source**: PD R3.3 V1.0, Sec 6.4.12.3.11, **Table 6.46** (`pd33.txt:9724-9785`, added via ECN #22; not in PD R3.2 V1.2). Differs from Table 6.45 in:
- `23...21` **`Maximum VBUS Voltage (Passive Cable)`**: `00b` = `20V`; `01b..11b` = `Deprecated, receiver Shall assume 00b (20V)`. *(Verbatim 2-bit notation `00b`/`01b..11b` printed for the 3-bit `23...21` slot in Table 6.46, `pd33.txt:9742-9748`.)*
- `17` **`USB Type-C plug to Plug/Receptacle`**: `0b` = `Plug`; `1b` = `Deprecated, Receptacle` (`0` for Type-C/Captive). (No `EPR Capable` bit.)
- `10...7`: `Reserved` (`pd33.txt:9772`).
- `2...0` **`USB Highest Speed (Passive Cable)`**: Only `000b` (`USB2`), `001b` (`USB3 Gen1`), `010b` (`USB3/USB4 Gen2`); `011b...111b` = Invalid (`pd33.txt:9778-9784`).

#### C. PD Revision 2.0 Passive Cable VDO (Legacy Reference)
**Source**: USB-C R2.5 Annex F, Table F-2 (`typec25.txt:18948-18972`) & PD CTS `TEST.PD.VDM.CBL.1` (`pd3_cts.txt:9209-9239`): **verified**
- `23...20` = Reserved `0000b`; `17` = Reserved `0b`.
- `10...7` = `SSTX1 / SSTX2 / SSRX1 / SSRX2 Directionality Support` (replaced by `Max VBUS Voltage` + Reserved in PD3).
- `6...5` = `VBUS Current Handling Capacity` (`01b = 3A`, `10b = 5A`; `00b` Reserved if `VBUS through cable = 0` in PD2).
- `4` = `VBUS through cable` (`1 = Yes`); `3` = Reserved `0b` (`SOP''` not present on passive cable); `2...0` = `SuperSpeed USB Signaling Support`.

---

### 2.5 Active Cable VDO 1 & Active Cable VDO 2 Layouts

#### A. Current Active Cable VDO1 & VDO2 (SVDM 2.1, VDO Version `1.3`)
**Source**: PD R3.3 V1.0, Sec 6.4.12.3.12, **Table 6.47 & Table 6.48** (`pd33.txt:9787-10072`); **identical bit-for-bit** in PD R3.2 V1.2, Sec 6.4.12.3.9, **Tables 6.43–6.44** (`pd32.txt:9320-9596`). *(Max VDO Version is `1.3` = `011b`; no v1.4 exists.)*

**Active Cable VDO1 (`Table 6.47`)**:

| Bit(s) | Field Name | Encoding | Status | Verbatim Quote (`pd33.txt` Table 6.47) |
|---|---|---|---|---|
| `31...28` | `Hardware Version` | `0000b...1111b` | **verified** | `"0000b...1111b - HW Version assigned by the VID owner."` |
| `27...24` | `Firmware Version` | `0000b...1111b` | **verified** | `"0000b...1111b - FW Version assigned by the VID owner."` |
| `23...21` | `VDO Version` | `000b` = Deprecated v1.0<br>`001b` = Invalid<br>`010b` = Deprecated v1.2<br>`011b` = Version 1.3<br>`100b...111b` = Invalid | **verified** | `"000b - Deprecated, Version 1.0 001b - Invalid, Shall Not be used. 010b - Deprecated, Version 1.2 011b - Version 1.3 100b...111b - Invalid, Shall Not be used."` |
| `20` | `Reserved` | Ignored | **verified** | `"Reserved, receiver Shall ignore this field."` |
| `19...18` | `USB Type-C plug to USB Type-C/Captive` | `00b` Deprecated Type-A, `01b` Deprecated Type-B, `10b` USB Type-C, `11b` Captive | **verified** | Same values as Passive Cable VDO Table 6.45. |
| `17` | `EPR Capable (Active Cable)` | `0b` = Not EPR Capable; `1b` = EPR Capable | **verified** | `"0b - Cable is not EPR Capable 1b - Cable is EPR Capable"` |
| `16...13` | `Cable Latency` | `0001b`..`0111b`: `<10ns (~1m)`..`60–70ns (~7m)`<br>`1000b` = `1000ns (~100m)`<br>`1001b` = `2000ns (~200m)`<br>`1010b` = `3000ns (~300m)`<br>`0000b`, `1011b...1111b` = Invalid | **verified** | `"1000b - 1000ns (~100m) 1001b - 2000ns (~200m) 1010b - 3000ns (~300m) 1011b ....1111b - Invalid, Shall Not be used. Note: Includes latency of electronics in Active Cable."` |
| `12...11` | `Cable Termination Type (Active Cable)` | `00b...01b` = Invalid<br>`10b` = One end Active, one end passive, VCONN required<br>`11b` = Both ends Active, VCONN required | **verified** | `"00b...01b - Invalid, Shall Not be used. 10b - One end Active, one end passive, VCONN required. 11b - Both ends Active, VCONN required."` |
| `10...9` | `Maximum VBUS Voltage (Active Cable)` | `00b` = `20V`; `01b..10b` = Deprecated (assume `20V`); `11b` = `50V` | **verified** | `"00b - 20V 01b..10b - Deprecated, receiver Shall assume 00b (20V). 11b - 50V"` |
| `8` | `SBU Supported` | **`0b` = SBU supported; `1b` = SBU NOT supported** *(active-low!)* | **verified** | `"0b - SBU connections supported. 1b - SBU connections are not supported."` *(Contrast legacy USB-C R2.5 Table F-6 `1 = Yes` for TBT3 PD 3.0!)* |
| `7` | `SBU Type` | Valid if `B8 = 0b`: `0b` = SBU is passive; `1b` = SBU is active | **verified** | `"When SBU Supported = 0 (SBU connections are supported): 0b - SBU is passive 1b - SBU is active"` |
| `6...5` | `VBUS Current Handling Capability` | Ignored if `B4 = 0`: `00b` Invalid, `01b` = `3A`, `10b` = `5A`, `11b` Invalid | **verified** | `"Shall be ignored when VBUS Through Cable = 0 ... 01b - 3A 10b - 5A"` |
| `4` | `VBUS Through Cable` | `0b` = No; `1b` = Yes | **verified** | `"0b - No 1b - Yes"` |
| `3` | `SOP'' Controller Present` | `0b` = No SOP'' controller present; `1b` = SOP'' controller present | **verified** | `"0b - No SOP'' controller present 1b - SOP'' controller present"` |
| `2...0` | `USB Highest Speed (Active Cable)` | `000b` USB2 only; `001b` Gen1; `010b` Gen2; `011b` USB4 Gen3; `100b` USB4 Gen4; `101b...111b` Invalid | **verified** | Same table as Passive Cable VDO (`011b - [USB4] Gen3`, `100b - [USB4] Gen4`). |

**Active Cable VDO2 (`Table 6.48`)**:

| Bit(s) | Field Name | Encoding | Status | Verbatim Quote (`pd33.txt` Table 6.48) |
|---|---|---|---|---|
| `31...24` | `Maximum Operating Temperature` | 8-bit temp in °C | **verified** | `"Maximum allowable operating temperature inside the plug in °C. It may reflect the plug's skin temperature"` |
| `23...16` | `Shutdown Temperature` | 8-bit temp in °C | **verified** | `"Temperature inside the plug, in °C, at which the plug will shut down its active Signaling components."` |
| `15` | `Reserved` | Ignored | **verified** | `"Reserved, receiver Shall ignore this field."` |
| `14...12` | `U3/CLd Power` | `000b` `>10mW`, `001b` `5-10mW`, `010b` `1-5mW`, `011b` `0.5-1mW`, `100b` `0.2-0.5mW`, `101b` `50-200μW`, `110b` `<50μW`, `111b` Invalid (assume `>10mW`) | **verified** | `"000b - >10mW ... 110b - <50μW 111b - Invalid; receiver Should assume 000b (>10mW)"` |
| `11` | `U3 to U0 transition Mode` | `0b` = direct; `1b` = through U3S | **verified** | `"0b - U3 to U0 direct 1b - U3 to U0 through U3S"` |
| `10` | `Physical Connection` | `0b` = Copper; `1b` = Optical | **verified** | `"0b - Copper 1b - Optical"` |
| `9` | `Active Element` | `0b` = Active Re-driver; `1b` = Active Re-timer | **verified** | `"0b - Active Re-driver 1b - Active Re-timer"` |
| `8` | `USB4 Supported` | **`0b` = `[USB4]` supported; `1b` = not supported** *(active-low!)* | **verified** | `"0b - [USB4] supported 1b - [USB4] not supported."` |
| `7...6` | `USB 2.0 Hub Hops Consumed` | `0..3` hops (`0` if USB 2.0 unsupported) | **verified** | `"Number of USB 2.0 'Hub hops' that are lost due to the transmission time of the cable. Shall be set to zero if USB 2.0 not supported."` |
| `5` | `USB 2.0 Supported` | **`0b` = `[USB2]` supported; `1b` = not supported** *(active-low!)* | **verified** | `"0b - [USB2] supported 1b - [USB2] not supported."` |
| `4` | `USB 3.2 Supported` | **`0b` = `[USB3]` SuperSpeed supported; `1b` = not supported** *(active-low!)* | **verified** | `"0b - [USB3] SuperSpeed supported. 1b - [USB3] SuperSpeed not supported."` |
| `3` | `USB Lanes Supported` | `0b` = One lane; `1b` = Two lanes | **verified** | `"0b - One lane 1b - Two lanes"` |
| `2` | `Optically Isolated Active Cable` | `0b` = No; `1b` = Yes (no USB2, no VBUS) | **verified** | `"Optically Isolated Active Cables Shall have a re-timer or linear re-driver (LRD) as the active element and do not support [USB2] or carry VBUS."` |
| `1` | `[USB4] Asymmetric Mode Supported` | `0b` = No; `1b` = Yes | **verified** | `"0b - No 1b - Yes"` |
| `0` | `USB Gen` | `0b` = Gen 1 (`[USB3]` only); `1b` = Gen 2 or higher (see VDO1 `B2...0`) | **verified** | `"0b - Gen 1 (Shall only be used by [USB3] cables) 1b - Gen 2 or higher"` |

#### B. Legacy Active Cable VDO Tables (SVDM 1.0, SVDM 2.0 v1.0, SVDM 2.0 v1.2, PD 2.0)
**Source**: PD R3.3 V1.0 Sec 6.4.12.3.13–15 (**Tables 6.49–6.52**, `pd33.txt:10074-10256`, added via ECN #22), USB-C R2.5 Table F-5 (`typec25.txt:19055-19085`): **verified**
- **Active Cable VDO (SVDM 1.0)** (`Table 6.49`, single VDO, `pd33.txt:10074-10133`): `23...21` = Maximum VBUS Voltage (`00b` 20V, `01b..11b` Deprecated -> 20V); `17` = Plug/Receptacle; `10...7` = Reserved; `2...0` = USB Highest Speed (`000b..010b` only). No VDO2.
- **Active Cable VDO (SVDM 2.0 VDO 1.0)** (`Table 6.50`, single VDO, `pd33.txt:10135-10181`): `23...21` = VDO Version (`000b` v1.0); `17` = Reserved; `8...7` = Reserved; `2...0` = `000b..010b` only. No VDO2.
- **Active Cable VDO1/VDO2 (SVDM 2.0 VDO 1.2)** (`Tables 6.51–6.52`, 2 VDOs, `pd33.txt:10183-10256`):
  - **VDO1 (`Table 6.51`)**: `23...21` = VDO Version (`001b` v1.1, `010b` v1.2); `17` = Reserved (no EPR bit); `8` = `SBU Supported`, `7` = `SBU Type`; `2...0` = `000b..010b` only. *(Spec erratum in `pd33.txt:10216-10217`: row `6...5` `VBUS Current Handling Capability` was accidentally omitted in Table 6.51; USB-C R2.5 Table F-6 `typec25.txt:19105-19107` shows `6...5`: `01b = 3A`, `10b = 5A`.)*
  - **VDO2 (`Table 6.52`)**: `10...8` = Reserved (no `Physical Connection`, `Active Element`, or `USB4 Supported`); `2` = Reserved; `1...0` = **`SuperSpeed Signaling`** (`00b` Gen 1, `01b` Gen 2 or higher, `10b...11b` Invalid).
- **PD R2.0 Active Cable VDO** (`pd3_cts.txt:9240-9274`, `typec25.txt:19055-19085`): Single VDO; `23...20` = Reserved `0000b`; `17` = Reserved `0b`; `10...7` = `SSTX1/SSTX2/SSRX1/SSRX2 Directionality Support`.

---

### 2.6 VPD VDO (`VCONN Powered USB Device VDO`)

**Status**: **Active (not deprecated)** in both PD R3.2 V1.2 and PD R3.3 V1.0. **Source**: PD R3.3 V1.0, Sec 6.4.12.3.17, **Table 6.54** (`pd33.txt:10331-10397`); identical in PD R3.2 V1.2, Table 6.45 (`pd32.txt:9604-9671`).

| Bit(s) | Field Name | Encoding | Status | Verbatim Quote (`pd33.txt` Table 6.54) |
|---|---|---|---|---|
| `31...28` | `HW Version` | `0000b..1111b` | **verified** | `"HW Version assigned by the VID owner."` |
| `27...24` | `Firmware Version` | `0000b..1111b` | **verified** | `"FW Version assigned by the VID owner."` |
| `23...21` | `VDO Version` | `000b` = Version 1.0; `001b...111b` = Invalid | **verified** | `"000b - Version 1.0 001b...111b - Invalid, Shall Not be used"` |
| `20...17` | `Reserved` | Ignored | **verified** | `"Reserved, receiver Shall ignore this field."` |
| `16...15` | `Maximum VBUS Voltage` | `00b` = `20V`; `01b..11b` = Deprecated (assume `20V`) | **verified** | `"00b - 20V 01b..11b - Deprecated, receiver Shall assume 00b (20V)."` |
| `14` | `Charge Through Current Support` | Reserved `0` if `B0 = 0`; else `0b` = `3A`, `1b` = `5A` | **verified** | `"When Charge Through Support = 1b: 0b - the VPD is capable of carrying 3A. 1b - the VPD is capable of carrying 5A."` |
| `13` | `Reserved` | Ignored | **verified** | `"Reserved, receiver Shall ignore this field."` |
| `12...7` | `VBUS Impedance` | Reserved if `B0 = 0`; else VBUS impedance in **`2 mΩ`** increments (`< 10 mΩ` Reserved) | **verified** | `"VBUS impedance through the VPD in 2 mΩ increments. Values less than 10 mΩ are Reserved and Shall Not be used."` |
| `6...1` | `Ground Impedance` | Reserved if `B0 = 0`; else Ground impedance in **`1 mΩ`** increments (`< 10 mΩ` Reserved) | **verified** | `"Ground impedance through the VPD in 1 mΩ increments. Values less than 10 mΩ are Reserved and Shall Not be used."` |
| `0` | `Charge Through Support` | `0b` = No Charge Through; `1b` = Supports Charge Through | **verified** | `"0b - the VPD does not support Charge Through. 1b - the VPD supports Charge Through."` |

---

## 3. Decoded Field Meanings & Cross-Revision Changes

### 3.1 Field-by-Field Decoding Summary

| Concept | Decoding Rule | Primary Spec References | Status |
|---|---|---|---|
| **Max VBUS Voltage (`B10...9`, or `B23...21` in SVDM 1.0)** | `00b` -> **20 V** (abs max `21.55 V`).<br>`01b`, `10b` -> **Deprecated** in R3.2/R3.3 (*receiver Shall assume 20 V*). Historically **30 V** (`01b`) / **40 V** (`10b`) in PD R3.0 (`unverified` in raw R3.0 PDF, **verified** as deprecated in R3.3/R3.2).<br>`11b` -> **50 V** (allows 48 V Fixed EPR; abs max cable rating **`50.9 V`**). | PD R3.3 Tables 6.45, 6.47, 6.46; USB-C R2.5 Sec 3.11.1 | **verified** (`30V`/`40V` historical mapping **unverified**) |
| **VBUS Current (`B6...5`)** | `01b` -> **3 A**; `10b` -> **5 A**; `00b`/`11b` -> Invalid (Passive: receiver assumes **3 A**; Active: Ignored if `B4 = 0`). *(SPR 5 A non-EPR cables are deprecated in USB-C R2.5 Table 3-1.)* | PD R3.3 Tables 6.45, 6.47; USB-C R2.5 Table 3-1 | **verified** |
| **`EPR Capable` (`B17`)** | `1b` = Cable is EPR Capable (48 V @ 5 A safe). Only present in Passive Cable VDO Table 6.45 (`v1.0` SVDM 2.0/2.1) and Active Cable VDO1 Table 6.47 (`v1.3`). Reserved/`0`/Plug-Receptacle in SVDM 1.0 and Active `v1.0`/`v1.2`. | PD R3.3 Tables 6.45, 6.47 | **verified** |
| **USB Highest Speed (`B2...0`)** | `000b` = `[USB2]` only; `001b` = `[USB3]` Gen1; `010b` = `[USB3]/[USB4]` Gen2; `011b` = `[USB4]` Gen3; `100b` = `[USB4]` Gen4 (80 Gbps symmetric / 120G/40G asymmetric per `[USB4]`); `101b...111b` = Invalid *(DFP treats higher valid/invalid values as compatible with DFP max)*. SVDM 1.0 & Active `v1.0`/`v1.2` stop at `010b`. | PD R3.3 Tables 6.45, 6.47, 6.46, 6.50, 6.51 | **verified** (`80 Gbps` numeric label from USB4 context, not printed in PD Table 6.45) |
| **Cable Latency (`B16...13`)** | `0001b` `<10ns (~1m)` ... `0111b` `60–70ns (~7m)`. Passive `1000b` = `>70ns (>~7m)`. Active `1000b`/`1001b`/`1010b` = `1000ns (~100m)` / `2000ns (~200m)` / `3000ns (~300m)`. | PD R3.3 Tables 6.45, 6.47 | **verified** |
| **Termination Type (`B12...11`)** | Passive: `00b` = VCONN not required (Discover Identity only); `01b` = VCONN required.<br>Active: `10b` = 1 end Active + 1 passive, VCONN required; `11b` = Both ends Active, VCONN required. | PD R3.3 Tables 6.45, 6.47 | **verified** |
| **Connector Type (`ID Header B22...21` vs `Cable VDO B19...18`)** | ID Header `B22...21`: `10b` Receptacle, `11b` Plug, `00b` Unknown (PD2/SVDM1.0 = `00b`).<br>Cable VDO `B19...18`: `10b` USB Type-C opposite end, `11b` Captive (`00b`/`01b` Deprecated Type-A/B). | PD R3.3 Tables 6.34, 6.45, 6.47 | **verified** |
| **HW / FW Version (`B31...28`, `B27...24`)** | 4-bit nibbles assigned by VID owner (`0000b...1111b`). | PD R3.3 Tables 6.45, 6.47, 6.54 | **verified** |
| **Alternate Modes / Modal Op (`ID Header B26`)** | `0b` = No Alternate Modes; `1b` = Supports Alternate Modes (`Discover SVIDs` required). *(Not a "cable exit" bit — exit is via `Exit Mode` command `5` or Hard/Cable Reset.)* | PD R3.3 Table 6.34; PD R3.2 Table 6.34 | **verified** |
| **SBU & `SOP''` Presence (`Active VDO1 B8, B7, B3`)** | Only in Active Cable VDO1 (Passive cables are `SOP'` only and SBU is wire-through): `B8` `0b` = SBU supported / `1b` = not supported; `B7` `0b` = Passive SBU / `1b` = Active SBU; `B3` `1b` = `SOP''` controller present. | PD R3.3 Table 6.47 | **verified** |

### 3.2 Differences Between PD R3.3 V1.0 and PD R3.2 V1.2 (and R3.0 / R3.1 History)

1. **PD R3.3 V1.0 vs PD R3.2 V1.2**: **verified**
   - All **current** Cable & VPD VDO bit layouts (`Table 6.33` SVDM Header, `Table 6.38` Cert Stat, `Table 6.39` Product, Passive Cable `Table 6.45` = R3.2 `6.42`, Active Cable `Tables 6.47–6.48` = R3.2 `6.43–6.44`, VPD `Table 6.54` = R3.2 `6.45`) are **identical bit-for-bit**.
   - **ID Header VDO (`Table 6.34`)**: R3.3 renamed Bit `26` from `Modal Operation Supported` to `Alternate Modes Supported` (ECN #7) and un-deprecated `SOP (DFP)` `100b` `Alternate Mode Controller (AMC)` (ECN #27).
   - **Legacy VDO Tables restored in R3.3** (ECN #22, `pd33.txt:25709`): Added Tables `6.46` (Passive SVDM 1.0), `6.49` (Active SVDM 1.0), `6.50` (Active SVDM 2.0 v1.0), and `6.51–6.52` (Active SVDM 2.0 v1.2).
   - **SVDM state machine tweaks in R3.3**: Allowed `BUSY` on `Enter Mode` (ECN #32); deprecated `VDMBusyCounter` / `nBusyCount` limit (ECN #25); unified `tVconnSwapWait` -> `tAMSRepeat` (100 ms min, ECN #9); added `Not_Supported` stop condition on `SinkEPREnterTimer` and normative Step 1–12 Activity Diagrams for EPR entry (ECN #28); replaced `tVCONNValid` with `tVCONNPowerCycle` (25 ms max, ECN #24).
2. **PD R3.1 / R3.0 milestones reflected in R3.2 / R3.3**: **verified** (via Table 6.33 & USB-C R2.5)
   - `SVDM Major = 00b` (`1.0`) was introduced in PD R3.0 v1.0 (before `VDO Version` replaced `Max VBUS Voltage` at `B23...21` in PD R3.0 v1.0a SVDM `2.0`).
   - `SVDM Minor = 01b` (`2.1`) started with PD R3.1 v1.6 (Table 6.33 `pd33.txt:8824-8825`).
   - SPR 5A cables and `30V`/`40V` intermediate max-voltage encodings were deprecated/replaced by `50V`/`5A` EPR (`USB-C R2.5 Table 3-1`, `PD R3.3 Table 6.45`).

---

## 4. EPR Cable Rules & Mandatory Source Checks

| Requirement | Value / Rule | Spec, Revision, Section, Table | Status | Verbatim Quote |
|---|---|---|---|---|
| **EPR Cable Requirements** | **Must** be electronically marked (`SOP'`), **5 A** (`B6...5 = 10b`), **50 V** (`B10...9 = 11b`), and **`EPR Capable = 1b`** (`B17 = 1b`). Min functional voltage = **`50.9 V`** (bypass caps >= 63 V recommended). Both Passive & Active cables follow same V/I/`EPR Capable` bits (Active needs `VBUS Through Cable = 1b`). | USB-C R2.5 Sec 2.4 (`typec25.txt:2047-2049`), Sec 3.11.1 (`5700-5703`); PD R3.3 Tables 6.45, 6.47 | **verified** | `"USB Type-C cable assemblies designed for USB PD Extended Power Range (EPR) operation are required to have an electronic marking indicating EPR compatibility. These cables are required to be electronically marked for 5 A and 50 V and include the EPR Mode Capable bit set."` / `"The minimum functional voltage that a cable shall support is 50.9 V."` |
| **Detached Cable Power Tiers vs Port PDP (<240W vs 240W)** | Detached cables only come in **60 W** (unmarked / 3A), **deprecated 100 W** (SPR 5A), or **240 W (50 V / 5 A EPR)**. Even a `140 W` (`100W..240W`) EPR Source requires a 240 W EPR cable. Non-EPR cable caps EPR Source `Port Present PDP <= 100 W` (SPR Mode only). Captive EPR Sink cables **Shall** report 50 V (CTS checks 5A/50V/EPR=1). | USB-C R2.5 Sec 2.5 Table 3-1 (`2107`), Sec 3.4.1.1 (`3046`); PD R3.3 Sec 3.2.2 (`1990-1991`), Sec 3.2.5.3 (`2359-2382`), Table 6.45 (`9686`) | **verified** | `"Existing USB PD SPR 5 A cables have been deprecated and replaced by USB PD EPR cables."` / `"An EPR Capable Port has a Port Maximum PDP ≥ 100W and ≤ 240W... When the Attached cable is not an EPR Capable cable, an EPR Capable Source Shall only operate in SPR Mode."` |
| **Early EPR Source Cable Discovery Deadline** | Source **Shall** send `SOP'` `Discover Identity REQ` within **`tEPRSourceCableDiscovery` (`2 s max`)** after receiving `GoodCRC` for First Explicit Contract `PS_RDY` (including `VCONN_Swap` time if needed), and before offering `> 3 A` or `> 20 V`. | PD R3.3 V1.0, Sec 7.31.18.4 (`pd33.txt:14545-14555`), Table 7.10 (`14579`); PD CTS `TEST.PD.PROT.SRC3.13#1` (`pd3_cts.txt:7344-7346`) | **verified** | `"The EPR Source Shall send the Discover Identity REQ Command, to the Cable Plug, within tEPRSourceCableDiscovery of receiving the GoodCRC Message acknowledging the PS_RDY Message as part of the Explicit Contract Negotiation."` |
| **Mandatory Source Check During `EPR_Mode (Enter)`** | Sequence in Sec 7.30.1.2 (within `tEnterEPR` = **450 / 500 / 550 ms**): Step 5 `EPR_Mode(Ack)` -> Step 6–8 if Cable ID unknown & not VCONN Source, send `VCONN_Swap` (fail -> `Enter Failed` **`02h Source failed to become VCONN Source`**) -> Step 9 `SOP'` `Discover Identity` -> Step 10 verify `EPR Capable = 1b`, `50V`, `5A` (fail -> `Enter Failed` **`01h Cable not EPR Capable`**) -> Step 11 `EPR_Mode(Enter Succeeded)` -> Step 12 `EPR_Source_Capabilities`. | PD R3.3 V1.0, Sec 7.30.1.2 (`pd33.txt:13670-13709`), Table 6.28 (`8585-8596`), Table 7.10 (`14578`); PD CTS `TEST.PD.EPR.SRC3.2` / `SRC3.6` | **verified** | `"6. Determine cable status: a. If the Cable's Discover Identity response is already known, proceed to step 10. b. Otherwise, if the Source is the VCONN Source, proceed to step 9. c. Otherwise, proceed to step 7 [VCONN_Swap]... 10. Verify the cable is EPR Capable, supports 50V, and supports 5A."` |

---

## 5. SOP' / SOP'' Communication, VCONN Ownership & Sink-Side Tester Rules

### 5.1 Who Supplies VCONN & Who May Talk to `SOP'` / `SOP''`

| Rule | Detail | Spec, Revision, Section | Status | Verbatim Quote |
|---|---|---|---|---|
| **Initial VCONN Source at Attach** | **Source** (DFP) is initially VCONN Source (Rp/Rd/Ra -> turns on VCONN within `tVCONNON` = `2 ms max`); **Sink** (UFP) does **not** supply VCONN initially (`Attached.SNK` default). | PD R3.3 V1.0, Sec 1.2 (`pd33.txt:1065-1066`); USB-C R2.5 Sec 4.5.2.2.5.1 (`typec25.txt:7588-7597`), Table 4-32 (`10984-10986`) | **verified** | `"At the point of Attachment, the Source Port functions as both the Downstream Facing Port (DFP) and the VCONN Source, while the Sink Port functions as the Upstream Facing Port (UFP)."` |
| **ONLY VCONN Source May Talk to `SOP'`/`SOP''`** | **A Sink that is NOT the VCONN Source CANNOT legally communicate with `SOP'` or `SOP''`** (except pre-contract `SOP'` discovery is also restricted to Initial Sourcing Device / VCONN Source). Furthermore, when Sink IS VCONN Source, Rp **MUST** be `SinkTxOK` before initiating an `SOP'`/`SOP''` AMS. Non-VCONN-Source ports must not even send `GoodCRC` to `SOP'`/`SOP''`. | PD R3.3 V1.0, Sec 1.3.7 (`pd33.txt:1117-1119`), Sec 7.5 (`12512-12516`), Sec 7.13 (`13129-13139`); USB-C R2.5 Sec 4.9 (`typec25.txt:10606-10608`); PD CTS `TEST.PD.PHY.ALL.7#9/#10` | **verified** | `"The VCONN Source Port is the only port allowed to communicate with Cable Plugs. The VCONN Source is the only Port allowed to provide power to Cable Plugs."` / `"Note: Even when it is currently the VCONN Source, the Sink is not permitted to initiate an AMS with a Cable Plug unless Rp is set to SinkTxOK."` |
| **`VCONN_Swap` Make-Before-Break** | Either Port Partner may send `VCONN_Swap`. Data role, power role, and Rp/Rd remain unchanged; VCONN must remain continuously powered (`make before break`) across swap (`tVconnSourceOn` `<= 50 ms`, `tVconnSourceOff` `<= 25 ms`). Present VCONN Source **Shall Not** `Reject`. | PD R3.3 V1.0, Sec 7.13 (`pd33.txt:13101-13127`), Table 7.10 (`14622-14624`) | **verified** | `"VCONN Shall be continually sourced during the VCONN Swap process to maintain power to the Cable Plug(s) i.e., make before break."` / `"The Port that is presently the VCONN Source Shall Not send a Reject Message in response to a VCONN_Swap Message."` |
| **Permitted Commands on `SOP''` vs `SOP'`** | **`Discover Identity` is NOT allowed on `SOP''`**. Only `Soft Reset`, `Unsupported Sequence`, `Get Status` (Active Cables), `Enter USB` (Active Cables), and optional Mode/FW/other SVDMs are sent to `SOP''`. | PD R3.3 V1.0, Sec 7.5, Table 7.5 (`pd33.txt:12526-12615`); Sec 8.7.1.2 (`15318-15321`) | **verified** | `"The Initiator Shall Not send discovery SVDM REQs to the Cable Plug at SOP''."` (`Discover Identity`, `Discover SVIDs`, `Discover Modes`, `Get Manufacturer Info`, `Get Revision`, `Security Request`: `SOP'' Not Allowed` in Table 7.5.) |

### 5.2 Independent `MessageID` Counters, `Soft_Reset` to `SOP'`/`SOP''`, and `GoodCRC`

- **Independent `MessageID` & FSMs per `SOP*`**: PD R3.3 Sec 6.1.2 (`pd33.txt:6429-6433`) & Sec 9.1.2 (`15555-15562`): **verified**
  > *"Ports Shall keep a different MessageID counter for each supported SOP\*. Instead of using the MessageID counter, the MessageID field for GoodCRC Message Shall match the MessageID field of the received Message that the GoodCRC Message is acknowledging."*
  - Separate Protocol TX/RX/Hard-Reset FSMs exist for `SOP`, `SOP'`, and `SOP''`. `Cable Plugs Shall Not retry Messages` (Sec 7.32.2, `14748`).
- **Mandatory Targeted `Soft_Reset` on `SOP'`/`SOP''` After `VCONN_Swap`**: PD R3.3 Sec 7.1.1 (`pd33.txt:12022-12035`) & Sec 8.6.1.2.1 (`15052-15062`): **verified**
  > *"If after a VCONN Swap the VCONN Source wants to communicate with a Cable Plug using SOP' Packets, it Shall issue a Soft_Reset Message using a SOP' Packet in order to reset the Cable Plug's Protocol Layer. If the VCONN Source wants to communicate with a Cable Plug using SOP'' Packets, it Shall issue a Soft_Reset Message using a SOP'' Packet..."*
  - `Soft_Reset` on `SOP'`/`SOP''` resets only that Cable Plug's FSM & `MessageID` counter (does NOT reset `SOP` Contract).
  - **Pre-VCONN-Swap Exception**: Before cable PD capability is established during Initial Source startup, *"If a Port does not receive a GoodCRC Message for an SOP' Discover Identity REQ, it Shall not send an SOP' Soft Reset, because the Cable or VPD May Not be PD Capable."* (`pd33.txt:15052-15053`).

### 5.3 Procedure for a Sink-Side Tester to Read a Cable

1. **When Attached to a Charger / Source (`Attached.SNK`)**: **verified** (PD R3.3 Sec 7.13 `13101-13153`, Sec 7.1.1 `12026-12034`, Sec 8.6.1.2.1 `15060-15062`, Type-C R2.5 `10713`)
   - Complete Explicit Contract and wait until Charger Rp indicates `SinkTxOK`.
   - Send **`VCONN_Swap`**:
     - **`Accept` (compliant case)**: Turn on VCONN (`make before break`), send `PS_RDY` within `tVconnSourceOn` (`50 ms` max). Wait for VCONN stability if newly powered (`teMarkerReady` / `tVCONNStable` <= `50 ms`), send **`SOP'` `Soft_Reset`**, wait for `GoodCRC` + `Accept` (or note PD capability), then send `SOP'` `Discover Identity REQ` (under `SinkTxOK`). (Remember Source may later `VCONN_Swap` back before `EPR_Mode(Enter)`!)
     - **`Not_Supported`**: Per PD R3.3 Sec 7.13 (`pd33.txt:13117-13119, 13132-13133`), *"when a Not_Supported Message is received in response to a VCONN_Swap Message, the sender becomes the VCONN Source and May turn on VCONN"* until Hard Reset or Detach -> tester may turn on VCONN (`teMarkerReady <= 50 ms`), send `SOP'` `Soft_Reset` -> `Discover Identity`. *(Risk: non-compliant chargers that reply `Not_Supported` while still sourcing VCONN.)*
     - **`Wait`**: Valid only when both support PD >= R3.2 V1.1; wait >= **`tAMSRepeat` (`100 ms min`)** (was `tVconnSwapWait` in PD R3.2) before retrying `VCONN_Swap`.
     - **`Reject`**: Forbidden for current VCONN Source (Sec 7.13); if a non-compliant charger rejects anyway, the tester **must not** transmit `SOP'` while remaining an attached Sink.
2. **When Testing a Standalone Cable (No Charger Attached)**: **verified** (USB-C R2.5 Sec 4.5.2.2.7.2 `typec25.txt:7706-7708`; PD CTS `COMMON.PROC.BU.3` `pd3_cts.txt:1914-1928`)
   - A standard USB-C Source stays in `Unattached.SRC` if one CC pin shows `SRC.Ra` and the other shows `SRC.Open`.
   - A dedicated cable tester acts per `COMMON.PROC.BU.3`: apply `Rp` to both `CC1`/`CC2`, detect `Ra` (VCONN pin) and `Open` (CC pin), keep `Rp` **and** connect `Rd` on the CC pin (`tCCDebounce`), switch the VCONN (`Ra`) pin from `Rp` to VCONN + apply `vSafe5V` on VBUS, wait `tVCONNStable` (`teMarkerReady <= 50 ms`), then send `SOP'` `Discover Identity`.

---

## 6. Cable Discovery & VCONN Timing & Retries

### 6.1 Timing Parameters

| Parameter | Min | Typ/Nom | Max | Unit | Spec, Section, Table | Status | Verbatim Quote / Notes |
|---|---|---|---|---|---|---|---|
| **`tVCONNStable`** (`teMarkerReady` in USB-C R2.5) | — | — | **50** | ms | USB-C R2.5 Table 4-24 (`typec25.txt:10708-10715`); PD R3.3 `4822, 15030`; PD R3.2 `4622, 14253` | **verified** | USB-C R2.5 (`teMarkerReady`): `"The time between the application of VCONN or VBUS, whichever is later, until an eMarker shall be ready for communication."` Note: PD R3.3/R3.2 still call it `tVCONNStable (See [USB-C])` (CTS `TEST.PD.PROT.ALL.4#7` has typo `teMarerReady`). |
| **`tVCONNON`** | Note 1 | — | **2** | ms | USB-C R2.5 Table 4-32 (`typec25.txt:10984-10986`) | **verified** | From `vSafe5V` at `Attached.SRC` to min VCONN voltage (may be applied prior to VBUS). |
| **`tVCONNOFF`** | **0** | — | **35** | ms | USB-C R2.5 Table 4-32 (`typec25.txt:10994-10996`) | **verified** | From detach or as directed until VCONN disconnected. |
| **`tVCONNDischarge`** | — | — | **230** | ms | USB-C R2.5 Table 4-6 (`typec25.txt:6081-6083`) | **verified** | `"Time from cable disconnect to vVCONNDischarge met."` (`vVCONNDischarge <= 800 mV`). |
| **`tVconnSourceOn`** | — | — | **50** | ms | PD R3.3 Table 7.10 (`pd33.txt:14623`); PD R3.2 Table 7.9 | **verified** | New VCONN Source must turn on VCONN and send `PS_RDY` within 50 ms of `Accept` GoodCRC. |
| **`tVconnSourceOff`** | — | — | **25** | ms | PD R3.3 Table 7.10 (`pd33.txt:14622`); PD R3.2 Table 7.9 | **verified** | Initial VCONN Source must disable VCONN within 25 ms of `PS_RDY` GoodCRC. |
| **`tVconnSourceTimeout`** | **100** | **150** | **200** | ms | PD R3.3 Table 7.10 (`pd33.txt:14624`); PD R3.2 Table 7.9 | **verified** | Initiator wait for `PS_RDY` after `Accept` (`PE_VCS_Wait_for_VCONN`). |
| **`tVconnSourceDischarge`** | **160** | **200** | **240** | ms | PD R3.3 Table 7.10 (`pd33.txt:14621`); PD R3.2 Table 7.9 | **verified** | Time to discharge VCONN to `vSafe0V`/`vRaReconnect`. |
| **`tVCONNPowerCycle`** *(R3.3)* | — | — | **25** | ms | PD R3.3 Table 4.9 (`pd33.txt:4796-4815`) | **verified** | Replaced deprecated `tVCONNValid` (`0..5 ms` in R3.2 Table 4.8) for Data Reset; includes `tVCONNReapplied` = `10 ms min`. |
| **`tDiscoverIdentity`** | **40** | — | **50** | ms | PD R3.3 Table 7.10 (`pd33.txt:14575`); PD R3.2 Table 7.9 | **verified** | `DiscoverIdentityTimer` interval between `SOP'` `Discover Identity REQ` messages during Explicit Contract (Sec 7.31.12.4). |
| **`tVDMSenderResponse`** | **24** | — | **50** | ms | PD R3.3 Table 7.10 (`pd33.txt:14632`); PD R3.2 Table 7.9 | **verified** | `VDMResponseTimer` wait for Responder's ACK/NAK/BUSY. |
| **`tVDMReceiverResponse`** | — | — | **15** | ms | PD R3.3 Table 7.10 (`pd33.txt:14630`); PD R3.2 Table 7.9 | **verified** | Max Responder delay to start VDM reply. |
| **`tEnterEPR`** | **450** | **500** | **550** | ms | PD R3.3 Table 7.10 (`pd33.txt:14578`); PD R3.2 Table 7.9 | **verified** | `SinkEPREnterTimer` window for complete `EPR_Mode(Enter)` sequence. |
| **`tEPRSourceCableDiscovery`** | — | — | **2** | s | PD R3.3 Table 7.10 (`pd33.txt:14579`); PD R3.2 Table 7.9 | **verified** | Max delay after First Explicit Contract `PS_RDY` GoodCRC for EPR Source to send `SOP'` `Discover Identity REQ`. |

### 6.2 `nDiscoverIdentityCount` & Retry Mechanics

| Parameter / Rule | Value / Behavior | Spec, Revision, Section, Table | Status | Verbatim Quote / Notes |
|---|---|---|---|---|
| **`nDiscoverIdentityCount`** | **`20`** | PD R3.3 V1.0, Table 7.12 (`pd33.txt:14804-14811`); PD R3.2 V1.2, Table 7.11 | **verified** | `nDiscoverIdentityCount = 20`. (`nRetryCount = 2` PHY retries per message, `nMessageIDCount = 7`.) |
| **`DiscoverIdentityCounter` Lifecycle** | Incremented on each `PE_SRC_VDM_Identity_Request` (`Figure 9.93`, `23894-23907`); loops while `< 20` on `NAK`, `BUSY`, `VDMResponseTimer` timeout, or send failure without `GoodCRC`. **Reset** on **`VCONN_Swap`** (Sec 7.32.5 `14793`), Detach, Hard Reset, or Error Recovery. | PD R3.3 V1.0, Sec 7.31.12.4 (`14321-14328`), Sec 7.32.5 (`14790-14793`), Sec 8.6.1.2.1 (`15020-15056`) | **verified** | `"No more than nDiscoverIdentityCount Discover Identity Messages without a GoodCRC Message response Shall be sent. If no GoodCRC Message response is received after nDiscoverIdentityCount Discover Identity Command requests have been sent by a Port, the Port Shall Not send any further SOP'/SOP'' Messages."` (`Sec 7.31.12.4` — note `8.6.1.2.1` line `15046` prints *"SOP or SOP'"*, an apparent typo for *"SOP' or SOP''"*). |
| **PD 3.x -> PD 2.0 Fallback** | If cable ignores R3.x `Discover Identity REQ`, VCONN Source *May* retry using PD R2.0 before hitting `nDiscoverIdentityCount = 20`. | PD R3.3 V1.0, Sec 8.6.1.2.1 (`pd33.txt:15048-15050`) | **verified** | `"If a Cable Plug does not respond to a Revision 3.x Discover Identity REQ... the VCONN Source May repeat the Discover Identity REQ using PD Revision 2.0 before establishing that the Cable Plug is not PD Capable , until nDiscoverIdentityCount expires."` |
| **Cable Plug (`SOP'`) vs Port Partner (`SOP`) Discovery** | **Cable Plug (`SOP'`)**: Permitted **prior to** Explicit Contract (Default Contract) and during Explicit Contract (VCONN Source only); capped by `nDiscoverIdentityCount = 20` and `tDiscoverIdentity` (`40–50 ms`); no `Soft_Reset` if no `GoodCRC` during initial discovery.<br>**Port Partner (`SOP`)**: Permitted **only after** Explicit Contract; uses `VDMResponseTimer` (`24–50 ms`) without `nDiscoverIdentityCount`; `NAK` should not retry; `BUSY` waits `tVDMBusy >= 50 ms` (R3.3 deprecated `VDMBusyCounter`/`nBusyCount = 5`). | PD R3.3 V1.0, Sec 8.6.1.1, 8.6.1.2.1, 7.32.6 (`14795-14799`); PD CTS `COMMON.PROC.PD.7#1.f` | **verified** | `"There is no limit to the number of Responder BUSY responses a UFP or Cable Plug May send..."` (Sec 7.32.6 `pd33.txt:14795-14796`). |

---

## 7. URLs Used

| URL / Local Cache Source | Outcome | Notes |
|---|---|---|
| `https://usb.org/sites/default/files/USB_PD_R3.3_V1.0.zip` (`/tmp/usb_specs/pd33.txt`) | Succeeded | Official USB PD Specification R3.3 V1.0 (Oct 16, 2026) |
| `/tmp/usb_specs/pd32.txt` (from prior USB-IF fetch) | Succeeded | Official USB PD Specification R3.2 V1.2 (Aug 19, 2026) |
| `https://www.usb.org/sites/default/files/USB%20Type-C%202.5%20Release%20202603.zip` (`/tmp/usb_specs/typec25.txt`) | Succeeded | Official USB Type-C Spec R2.5 (March 2026) |
| `https://www.usb.org/sites/default/files/USB%20PD3%20CTS%20Q4%202026%20OR.zip` (`/tmp/usb_specs/pd3_cts.txt`) | Succeeded | Official USB PD3 CTS Q4 2026 OR (Oct 10, 2026) |
| `https://www.usb.org/sites/default/files/USB-C%20Source%20Power%20Test%20Specification%20V1.2%202026%2011%2004.pdf` (`/tmp/usb_specs/spt_v12.txt`) | Succeeded | Official Source Power Requirements CTS V1.2 (Nov 4, 2026) |
| `https://www.usb.org/sites/default/files/D2T2-1%20-%20USB%20Power%20Delivery.pdf` | Redirected via search | USB-IF presentation URL surfaced during search for PD R3.0 `30V`/`40V` encoding; not downloaded separately since PD R3.3 ECN #22 already provides legacy SVDM 1.0 & v1.0/v1.2 VDO tables. |
