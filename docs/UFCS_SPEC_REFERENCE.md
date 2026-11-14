# UFCS (Universal Fast Charging Specification) Reference

Reference compiled for a no-load USB-C tester acting as a UFCS **Sink (`充电设备`)** negotiating with and checking a **Source (`供电设备`)** over **D+ / D-**.

All findings are marked **verified** (read directly in extracted official standard PDFs) or **unverified** (secondary source, inferred, or unavailable). Where `T/TAF 083—2024` updated `T/TAF 083—2022`, both are noted explicitly.

---

## 1. Specification Availability, Versions & URLs Used

| Standard / Document | Version / Dates | Reachability & SHA-256 | Status |
|---|---|---|---|
| **T/CCSA 393—2024 \| T/TAF 083—2024** 《移动终端融合快速充电技术要求》 (*Universal fast charging specification for mobile devices*) | Replaced `083—2022`; Published `2024-04-01`, Effective `2024-04-08` (75 pp.) | **Succeeded** directly from `taf.org.cn` after chaining AIA intermediate CA `GeoTrustTLSRSACAG1.crt` (verified via `/etc/ssl/certs/ca-certificates.crt`). SHA-256: `1aa76e18fe8fb5b46a7688fe87b7e3135854c1f04379768c8809b8c287101359`. Extracted `/tmp/ufcs_specs/ttaf_083_2024.txt` (4,268 lines; entire doc read/indexed). | **verified** |
| **T/CCSA 394—2024 \| T/TAF 092—2024** 《移动终端融合快速充电测试方法》 (*Universal fast charging testing methods for mobile devices*) | Replaced `092—2022`; Published `2024-04-01`, Effective `2024-04-08` (81 pp.) | **Succeeded** directly from `taf.org.cn` over verified TLS. SHA-256: `bdbf4655aec57361d108660b982b807cfe1945ae7af345235a83daabf2781d5e`. Extracted `/tmp/ufcs_specs/ttaf_092_2024.txt` (3,941 lines; Sec `5` Source tests read/indexed). | **verified** |
| **T/TAF 083—2022** 《移动终端融合快速充电技术规范》 | Replaced `083—2021`; Published & Effective `2022-07-01` (81 pp.) | **Succeeded** from both `taf.org.cn` and GitHub mirror (byte-identical SHA-256: `bf125513f4f9cde1fffae8e860460ef43ea6604101a470dad2654f71f130bb33`). Extracted `/tmp/ufcs_specs/ttaf_083_2022.txt` (4,426 lines; entire doc mined for 2022→2024 delta tracking). | **verified** |
| **YD/T 7519-2026** & **YD/T 7520-2026** (Communications Industry Standards) | MIIT Announcement No. 23 (2026-08-18); Effective `2027-01-01` | Metadata **verified** via web search (`chongdiantou.com`, `taf.org.cn`); full PDF **not publicly downloadable** (published via People's Posts & Telecom Press / CCSA). | **unverified** (text unavailable) |
| **GB / GB/T** national standard | None found | Searched `openstd.samr.gov.cn` / `"GB/T"` `"融合快速充电"`: UFCS progressed from association standard (`T/TAF 083`, `T/CCSA 393`) straight to MIIT industry standard (`YD/T 7519-2026`). | **verified** (no GB/T exists) |

*Note on TLS*: Initial `curl` to `www.taf.org.cn` failed (`exit 60`, `Verify return code: 21`) because the server omits intermediate CA `GeoTrust TLS RSA CA G1` (listed in leaf cert AIA: `http://cacerts.geotrust.com/GeoTrustTLSRSACAG1.crt`). Fetching the intermediate DER, verifying it against the system CA store (`openssl verify` -> `OK`), and appending it to `/tmp/ufcs_specs/ca_bundle.pem` allowed full TLS-verified downloads without `--insecure`.

---

## 2. Physical Layer (`第 5–7 章`, `T/TAF 083—2024`)

### 2.1 Active Lines, Full-Duplex Roles & BC1.2/QC Coexistence

| Parameter | Value / Rule | Spec, Section, Table/Fig | Status | Verbatim Quote |
|---|---|---|---|---|
| **Transmission Medium & Duplex** | **Full-duplex UART over `D+` and `D-`** | `083—2024` Sec 5 (`L252-253`), Sec 7.2 Table 9 (`L408-442`) | **verified** | `"设备之间通过 D+、D-通道以全双工方式通信。"` |
| **Pin Mapping (`Sink` / `Source` / `Cable`)** | **Sink (`充电设备`)**: **`D-` = TX**, **`D+` = RX**.<br>**Source (`供电设备`)**: **`D+` = TX**, **`D-` = RX**.<br>**Cable e-marker**: `D- = TRX1`, `D+ = TRX2` (both initially `RX`; switches TX onto opposite pin of whichever pin received `Get_Cable_Info`, or `D+` TX if received on both simultaneously; resets to both `RX` on Cable HW Reset). | `083—2024` Sec 7.2 Table 9 (`L418-442`) | **verified** | Table 9: `充电设备 D-: 输出 串行数据发送 / D+: 输入 串行数据接收`; `供电设备 D-: 输入 串行数据接收 / D+: 输出 串行数据发送`; `7.2 a) 当从D+检测到Get_Cable_Info指令时，将D-引脚切换为发送状态（TX）` |
| **BC1.2 DCP Initial State & Transition** | Source starts in **Dedicated Charging Mode (`专有供电模式`)** with rated current `> 1.5 A`, **`D+`/`D-` shorted** (`Rin < 100 Ω`), `D+ > 0.4 V`. After detecting Sink's UFCS D- handshake pulses, Source opens the `D+`/`D-` short (`≥ 5 MΩ`) and pulls `D+` HIGH (`Idle`). | `083—2024` Sec 3.2 (`L187-189`), Sec 6.3 Table 7 (`L356-358`), Sec 9.2.1 (`L2305`) | **verified** | `"Rin为初始阻抗，D+/D-后断开后，阻抗不小于5MΩ。"` / `专有供电模式: "供电设备额定输出电流高于1.5A，在供电设备端D+和D-是短接的，在D+上检测到的电压大于0.4V。"` |
| **Coexistence with Other Fast Charge Protocols (QC, etc.)** | Sink executes **BC1.2 detection first**; only proceeds to UFCS handshake if **DCP** detected (`083—2022` Fig 5). During handshake, Source may detect UFCS concurrently with other D+/D- protocols. Multi-protocol Sinks **prioritize UFCS**; only fall back to other D+/D- or CC protocols if UFCS fails. | `083—2024` Sec 9.2.1 (`L2305`), Sec 9.2.2.1 (`L2452-2454`); `083—2022` Sec 6.3 Fig 5, Sec 8.2.1.1 (`L2653`) | **verified** | `"如果充电设备支持包括UFCS在内的多种快充协议，优先选择UFCS协议。如果充电设备的UFCS协议识别失败，充电设备可以根据自身需求选择D+D-或者CC线上的其他快充协议。"` |

### 2.2 Electrical Levels, Impedance & Edge Slew (`第 6 章`)

| Parameter | Min | Typ | Max | Unit | Spec, Section, Table | Status | Verbatim Quote / Conditions |
|---|---|---|---|---|---|---|---|
| **Sink Input `D+` High (`V_IH`)** | `2.31` | `3.30` | `3.60` | V | `083—2024` Sec 6.2.2 Table 3 (`L317-321`) | **verified** | `表3 充电设备侧输入信号（D+）电气规范`: `高电平 2.31 / 3.30 / 3.60 V` |
| **Sink Input `D+` Low (`V_IL`)** | `-0.30` | `0.00` | `0.54` | V | `083—2024` Sec 6.2.2 Table 3 (`L319-320`) | **verified** | `表3`: `低电平 -0.30 / 0.00 / 0.54 V` |
| **Sink Output `D-` High (`V_OH`)** | `1.44` | `3.30` | `3.60` | V | `083—2024` Sec 6.2.2 Table 4 (`L323-329`) | **verified** | Condition: `0uA ≥ Io ≥ -500uA` (negative = out of Sink) |
| **Sink Output `D-` Low (`V_OL`)** | `0.00` | `0.00` | **`0.60`** *(was `0.50` in 2022)* | V | `083—2024` Table 4 (`L326`) / `083—2022` Table 4 (`L486`) | **verified** | Condition: `500uA ≥ Io ≥ 0uA` (positive = into Sink) |
| **Source Input `D-` High / Low** | `1.40` / `-0.30` | `3.30` / `0.00` | `3.85` / `0.99` | V | `083—2024` Sec 6.2.1 Table 1 (`L301-305`) | **verified** | Used to verify Source RX thresholds in `092—2024` `5.1.1` |
| **Source Output `D+` High / Low** | `2.56` / `0.00` | `3.30` / `0.00` | `3.60` / `0.50` | V | `083—2024` Sec 6.2.1 Table 2 (`L307-313`) | **verified** | Conditions: `0uA ≥ Io ≥ -500uA` (High), `500uA ≥ Io ≥ 0uA` (Low) |
| **Series Line Resistance (`Rd+`, `Rd-`)** | — | — | `< 50` (each) | Ω | `083—2024` Sec 6.3 Table 7 (`L352-358`) | **verified** | `Rd+ < 50Ω`, `Rd- < 50Ω`, initial `Rin < 100Ω`, total `< 200Ω`; open `>= 5MΩ` |
| **Rise / Fall Time (`tRising`, `tFalling`)** | — | — | `1` | µs | `083—2024` Sec 6.4 Table 8 (`L360-379`) | **verified** | Measured at `CL = 200 pF`, `20% VOH ~ 80% VOH` |
| **Cable VBUS / GND Drop Limits** | — | — | `500` (VBUS) / `250` (GND) | mV | `083—2024` Sec 6.5 Fig 3 (`L381-401`) | **verified** | `VBUS两端压差要求 <= 500mV`, `GND两端压差要求 <= 250mV` |

### 2.3 Line Coding, Baud Rate, Tolerance & Framing (`7.4`)

| Parameter | Value / Rule | Spec, Section, Fig/Table | Status | Verbatim Quote |
|---|---|---|---|---|
| **UART Framing (`8N1`)** | **Idle**: logic `1` (High). **Start (`S`)**: 1 bit `0`. **Data**: 8 bits **LSB first, MSB last**. **Parity**: None. **Stop/End (`E`)**: 1 bit `1`. | `083—2024` Sec 3.1.8 (`L191-192`), Sec 7.4.1–7.4.5, Fig 8 (`L587-603`) | **verified** | `"每一帧数据包含8位逻辑‘0’或‘1’，在总线上先发送LSB，后发送MSB。"` / `"结束位用1位高电平来表示"` |
| **Inter-Frame & Inter-Packet Idle** | Within packet (between bytes): **`>= 1 bit`** High. Between TX packets: **`>= 2 ms`** (`tMsgTransDelay`). | `083—2024` Sec 7.4.2, 7.4.7.1, Fig 10–11 (`L597, L643-650`) | **verified** | `"帧与帧之间的空闲位需要大于等于1Bit宽度。"` / `"Tx发送数据包之间的IDLE态需要满足大于等于2ms"` |
| **`tFrameReceive`** *(RX timeout)* | Min `500 µs`, Typ `600 µs`, Max **`700 µs`** (intra-frame waiting for Stop bit & inter-frame waiting for next byte -> resets RX FSM to Idle). | `083—2024` Sec 7.4.7.2, Fig 12, Table 11 (`L660-675`) | **verified** | Table 11 `tFrameReceive`: `500 / 600 / 700 us` |
| **Supported Bit Rates** | **`115200` bps (default)**, **`57600` bps**, **`38400` bps** *(note: `19200` bps was removed in 2022)*. | `083—2024` Sec 7.4.6 (`L612-613`); `083—2022` Preface `e)` (`L120`) | **verified** | `"UFCS总线通信波特率支持如下档位：115200bps、57600bps、38400bps，其中115200bps为缺省支持档位。"` |
| **Training Byte (`0xAA`)** | Every packet starts with Training byte **`0xAA`** (bits LSB-first: `0,1,0,1,0,1,0,1`). RX measures its bit width for receiving that packet. | `083—2024` Sec 7.4.6 Fig 9 (`L607, L611, L617-619`) | **verified** | `"发送方首先以设定波特率发送Training序列（0xAA）"` |
| **Baud Rate Tolerance** | **TX**: `±10%` of nominal tier, intra-packet relative error `<= ±1%`.<br>**RX**: Must accept up to **`±15%`**; if exceeding **`±20%`**, judge baud error and **do not reply**. | `083—2024` Sec 7.4.6 (`L608-610`) | **verified** | `"数据发送波特率误差不超过基准挡位的±10%，在同一个数据包内，波特率相对误差不超过±1%"`; `"不超过±15%时，接收方需正常响应，当超过基准挡位的±20%时，判定波特率错误，不回复当前信号"` |
| **Baud Switching via `Ping`** | If TX sends `Ping` **`>= 3` times** *(was `>= 5` in 2022)* with no `ACK`/`NCK`, TX may switch baud tier (`115200 -> 57600 -> 38400`) and retry `Ping`. | `083—2024` Sec 7.4.6 (`L623-625`) / `083—2022` Sec 6.4.6 (`L844`) | **verified** | `"当发送方连续发送至少3次Ping消息未收到ACK或者NCK消息时，发送方主动切换为其它档位波特率并发送Ping消息"` |

---

## 3. Detection / Handshake & Initial Sequence (`7.3`, `9.2`, `Table 10`)

### 3.1 Electrical Handshake (`tDet1`–`tDet4`, `tDpDet`)

From the Sink's point of view after detecting `VBUS` and `BC1.2 DCP` (`D+`/`D-` shorted, `D+ > 0.4 V`):

| Step / Parameter | Min | Typ | Max | Unit | Spec, Section, Table/Fig | Status | Verbatim Quote |
|---|---|---|---|---|---|---|---|
| 1. `tDet1`: Sink drives **`D-` HIGH** | `1.5` | `2` | `2.5` | ms | `083—2024` Sec 7.3 Fig 5, Table 10 (`L567-584`) | **verified** | `"tDet1: 握手检测第一信号持续时间 1.5 / 2 / 2.5 ms"` |
| 2. `tDet2`: Sink drives **`D-` LOW** | `6` | `8` | `10` | ms | Same | **verified** | `"tDet2: 握手检测第二信号持续时间 6 / 8 / 10 ms"` |
| 3. `tDet3`: Sink drives **`D-` HIGH** | `1.5` | `2` | `2.5` | ms | Same | **verified** | `"tDet3: 握手检测第三信号持续时间 1.5 / 2 / 2.5 ms"` |
| 4. `tDet4`: Sink drives **`D-` LOW** (held `>= tDet4`) | `6` | `8` | `10` | ms | Same | **verified** | `"tDet4: 握手检测第四信号持续时间 6 / 8 / 10 ms"` (`"持续tDet4以上"`, `L480`) |
| 5. `tDataRoleSwitch`: Source releases short & pulls `D+` HIGH; Sink enters UFCS mode | — | — | `1` | ms | `083—2024` Sec 7.3 Table 10 (`L583`) | **verified** | `"tDataRoleSwitch: 数据引脚角色切换时间 max 1 ms"` |
| 6. `tDpDet`: Sink window from start of `tDet4` to sample `D+` HIGH | `11` | — | `15` | ms | `083—2024` Sec 7.3 Table 10 (`L561-579`) | **verified** | `"充电设备在握手检测第四信号开始时，启动对D+信号高电平检测... tDpDet: 11 ~ 15 ms"` |
| 7. `tdetRetry` & Max Attempts | `0` | — | `10` | ms | `083—2024` Sec 7.3 Fig 5, Table 10 (`L562-580`) | **verified** | `"可在tdetRetry时间内重新启动握手检测，即从第四信号开始计时11ms后，21ms内重新启动握手检测，最多可检测3次... 尝试3次后，依旧未握手检测成功，充电设备侧D+D-恢复到初始状态（高阻状态）"` |

### 3.2 Initial `Ping` / `ACK` Sequence & Source Fallback if Sink Never Answers

| Parameter / Rule | Value | Spec, Section, Table | Status | Verbatim Quote |
|---|---|---|---|---|
| **`tSendping`** (Sink deadline to send `Ping`) | `<= 100 ms` after handshake succeeds | `083—2024` Sec 7.3 Table 10 (`L560, L581`) | **verified** | `"充电设备在tSendping时间内需向供电设备发送Ping消息。"` (`max 100 ms`) |
| **`tWaitPing`** (Source wait window) | `110 ~ 120 ms` (`110` min / `120` max) | `083—2024` Sec 7.3 Table 10 (`L557-558, L582`) | **verified** | `"若在tWaitPing时间内，未收到来自充电设备的指令，供电设备首先发送充电设备硬件复位命令，再主动复位至初始状态。"` |
| **`Ping` Retries & Version Negotiation** | `Ping` retries are exempt from `nMsgRetryCount` (`3`). Sink sets Header Protocol Version to its newest supported version; Source replies `ACK` with `min(Sink_Ver, Source_Ver)` and starts **1 s Watchdog** (`图D.2`). | `083—2024` Sec 8.2.3.1, Sec 8.6 (`L1973-1987`), App `图D.2` (`L3150-3213`) | **verified** | `"供电设备回复ACK消息时，将该版本编号和自身协议版本编号取较小值填入消息头的协议版本编号字段中。"` |

---

## 4. Messages, Framing, CRC & Capabilities/Request Formats (`7.5`–`8.2`)

### 4.1 Packet Structure & CRC-8 (`7.5`, `7.6`, `8.2.1`, `8.2.2`, `附录 A`)

**Byte / Bit Order (`6.6.1`, `7.6`, `8.2.2`)**: Multi-byte fields (Header, Multi-byte Data fields) are sent **MSB byte first** (`先发送高字节，再发送低字节`); within each UART byte, bits are sent **LSB first**.

```text
Control Msg: [0xAA Training (1B)] [Header (2B)] [Cmd (1B)] [CRC-8 (1B)]
Data Msg:    [0xAA Training (1B)] [Header (2B)] [Cmd (1B)] [Len N (1B)] [Data (1..59B)] [CRC-8 (1B)]
Vendor Msg:  [0xAA Training (1B)] [Header (2B)] [VendorID (2B)] [Len N (1B)] [Data (1..58B)] [CRC-8 (1B)]
```
*(Note: In `083—2022` `6.6.3`, the 2B field after Header in Vendor Messages was `Reserved = 0x0000`; in `083—2024` `7.6` Fig 15, it became `VendorID`.)*

**Message Header (2 Bytes, `Bit 15..0`, `Table 13`, `L993-1019`)**: **verified**
- **`Bit 15..13` (`Device Address`, destination)**: `001b` (`1`) = Source, `010b` (`2`) = Sink, `011b` (`3`) = Cable e-marker, others Reserved.
- **`Bit 12..9` (`Message Number`)**: `0..15` cyclic (`MsgNumberCounter`). Note in **`083—2024` `8.2.2 b)` (`L1015-1016`)**: receivers do **not** verify whether MsgNum changed; senders are advised to set `0` (`建议设备在发送消息时，消息编号都设置为0，回复跟随`), while `ACK`/`NCK` **must** echo the received MsgNum.
- **`Bit 8..3` (`Protocol Version`)**: `000001b` = v1.0.0 (`2022` only had `000001b`); **`083—2024`** added `010001b` = v1.0.1 and `001001b` = v1.2.0 (`low 2b = Major`, `mid 2b = Minor`, `high 2b = Patch`).
- **`Bit 2..0` (`Message Type`)**: `000b` = Control, `001b` = Data, `010b` = Vendor-Defined, others Reserved.

**CRC-8 Algorithm (`7.5`, `附录 A`, `L676-680, L2913-2938`)**: **verified**
- **Coverage**: Message Header + Message Body (excludes `0xAA` Training byte and CRC byte).
- **Parameters**: Width = 8, Poly = **`0x29`** (`X^8 + X^5 + X^3 + 1`), Init = **`0x00`**, RefIn = **`false`** (MSB-first), RefOut = **`false`**, XorOut = **`0x00`**.

**Physical-Layer `ACK` / `NCK` (`7.7`, `L711-727`)**: **verified**
- Never reply `ACK`/`NCK` to an `ACK`/`NCK` or non-matching Device Address. Receivers use either Method 1 (Address + CRC -> `ACK`/`NCK`, then `Refuse(0x01)` if invalid Type/Len) or Method 2 (Address + MsgType + DataLen + CRC -> silently drop if invalid Type/Len).

### 4.2 Complete Message IDs (`Tables 14 & 15`, `083—2024`)

**Control Messages (`MsgType = 000b`, `8.2.3 Table 14`, `L1029-1061`)**: **verified**

| ID | Command | Sender -> Receiver | Key Semantics |
|---|---|---|---|
| `0x00` | `Ping` | Src/Snk -> All | Link/baud presence probe; reply `ACK`/`NCK` (unlimited retries). |
| `0x01` | `ACK` | All -> All | Valid CRC receipt; sent after `>= tACKtransmit` (`100 µs`). |
| `0x02` | `NCK` | All -> All | Bad CRC receipt; triggers immediate retransmission. |
| `0x03` | `Accept` | Src/Snk -> Src/Snk | Confirms agreement to `Request`, `Detect_Cable_Info`, `Start_Cable_Detect`, `Config_Watchdog`, or `Verify_Request`. |
| `0x04` | `Soft_Reset` | Src/Snk -> All | Keeps UFCS mode & current V/I unchanged; resets TX/RX FSM, timers, `MsgNumberCounter=0`, buffers (`8.2.3.5`). |
| `0x05` | `Power_Ready` | Src -> Snk | Output V/I reached `Request` target; sent within `tPowerSupply` (`550 ms`) of `Accept`. |
| `0x06` | `Get_Output_Capabilities` | Snk -> Src | Requests Source capability list (`Output_Capabilities`). |
| `0x07` | `Get_Source_Info` | Snk -> Src | Requests `Source_Information`. |
| `0x08` | `Get_Sink_Info` | Src -> Snk | Requests `Sink_Information`. |
| `0x09` | `Get_Cable_Info` | Src/Snk -> Cable | Requests `Cable_Information`. |
| `0x0A` | `Get_Device_Info` | Src/Snk -> Src/Snk | Requests `Device_Information` (`Vendor ID = 0xFFFF` indicates Test Mode in `图D.3`). |
| `0x0B` | `Get_Error_Info` | Src/Snk -> Src/Snk | Requests `Error_Information`. |
| `0x0C` | `Detect_Cable_Info` | Src/Snk -> Peer | Asks peer to query cable e-marker (`<= 1200 ms`). |
| `0x0D` | `Start_Cable_Detect` | Src/Snk -> Peer | Requests peer put TX in Hi-Z so initiator can read cable (`<= 1100 ms`). Required by Source if any mode `> 6.5 A`. |
| `0x0E` | `End_Cable_Detect` | Src/Snk -> Peer | Notifies peer to restore TX and resume normal comm. |
| `0x0F` | `Exit_UFCS_Mode` | Src/Snk -> Peer | Commands exit from UFCS back to BC1.2 DCP & default VBUS (`200 ms` check window). |
| `0x10` | `Get_Sink_Info_Extended` | Src -> Snk | *(New in 2024)* Requests `Sink_Information_Extended` (optional; `Refuse 0x02` if unsupported). |

*(Note: UFCS has **no `Wait` message**.)*

**Data Messages (`MsgType = 001b`, `8.2.4 Table 15`, `L1208-1233`)**: **verified**

| ID | Command | Data Len (B) | Sender -> Receiver | Key Semantics |
|---|---|---|---|---|
| `0x01` | `Output_Capabilities` | `8 * n` (`1..7`) | Src -> Snk | `1..7` contiguous programmable modes (ascending voltage). |
| `0x02` | `Request` | `8` | Snk -> Src | Mode index (`1..7`), Requested V (`10 mV`), Requested I (`10 mA`). |
| `0x03` | `Source_Information` | `8` | Src -> Snk | `63..52` rsvd; `51..48` *(2024)* `Sink_Info_Ext` min period (`×100ms`, `0`=off); `47..40` internal temp (`val-50` °C, `0`=N/A); `39..32` port temp (`val-50` °C); `31..16` Vout (`10 mV`); `15..0` Iout (`10 mA`). |
| `0x04` | `Sink_Information` | `8` | Snk -> Src | Battery/port temp (`val-50` °C), pre-IC Vchg (`10 mV`), Ichg (`10 mA`). |
| `0x05` | `Cable_Information` | **`10`** *(was `8` in 2022)* | Cable/Src/Snk -> Src/Snk | `2024 Table 20`: `79..64` Vendor ID; `63..48` Vendor-defined ID; `47..32` Impedance (`mΩ`); `31..16` Max V (**`10 mV`**); `15..0` Max I (**`10 mA`**). *(2022 Table 20 was 8B: 4B rsvd, 16b `mΩ`, 8b **`1 V`**, 8b **`1 A`**!)* |
| `0x06` | `Device_Information` | `8` | Src/Snk -> Src/Snk | `2024 Table 21`: `63..48` Vendor ID; `47..32` Vendor-defined ID; `31..16` HW Ver; `15..0` SW Ver. *(2022 had `63..32` Reserved `0`.)* |
| `0x07` | `Error_Information` | `4` | Src/Snk -> Src/Snk | Fault bitmask (`Table 22`, see Sec 6). |
| `0x08` | `Config_Watchdog` | `2` | Snk -> Src | `15..0` timeout in `ms` (`0` disables; default `1000 ms`). |
| `0x09` | `Refuse` | `4` | All -> Src/Snk | Rejected MsgNum/MsgType/CmdID + Reason (`Table 24`). |
| `0x0A` | `Verify_Request` | `17` | Src/Snk -> All | `1B` Key Index + `16B` Random A. |
| `0x0B` | `Verify_Response` | `48` | All -> Src/Snk | `32B` SHA-256(`A \|\| Key \|\| B`) + `16B` Random B. |
| `0x0C` | `Power_Change` | `3 * n` (`1..7`) | Src -> Snk | Dynamic derating: Mode#, fast/slow bit (`1` = reduce within `40 ms`), Max I (`10 mA`). |
| `0x0D` | `Sink_Information_Extended` | `3 * n` (`1..15`) | Snk -> Src | *(New in 2024)* Battery SoC (`0.01%`), Max power (`W`). |
| `0xFF` | `Test Request` | `2` | Tester -> All | Test Mode & V-accuracy (`+10%` I) enable + target Addr/Type/Cmd. |

### 4.3 `Output_Capabilities` (`0x01`) & `Request` (`0x02`) Layouts (`Tables 16–17`, `Table 38`)

**`Output_Capabilities` Mode Entry (64 bits per mode, up to 7 modes; `083—2024` `8.2.4.1` Table 16, `L1259-1281`)**: **verified**
- **`Bit 63..60`**: Output Mode Number (`1..7`, ascending; `Mode n Vmin == Mode (n-1) Vmax`).
- **`Bit 59..57`**: Current Step (`000b`=10mA, `001b`=20mA, `010b`=30mA, `011b`=40mA, `100b`=50mA, others reserved).
- **`Bit 56`**: Voltage Step (`0b`=10mV, `1b`=20mV).
- **`Bit 55..40`**: Max Output Voltage (`10 mV` units).
- **`Bit 39..24`**: Min Output Voltage (`10 mV` units).
- **`Bit 23..8`**: Max Output Current (`10 mA` units).
- **`Bit 7..0`**: Min Output Current (`10 mA` units).

**Source Power Tiers (`10.2.1 Table 38`, `L2681-2696`)**: **verified**
- `5V` programmable: `3.4 V – 5.5 V` (`M` >= 3A at 20W; optional >= 3A above 20W)
- `10V` programmable: `5.5 V – 11 V` (`M` >= 2A for `20..40W`, `M` >= 3A for `>= 40W`)
- `20V` programmable: `11 V – 21 V` (`O` >= 2A/3A `<90W`; `M` >= 3A `>= 90W`)
- `30V` programmable: `21 V – 36 V` (`O` >= 2A `90..120W`, `O` >= 3A `>= 120W`)

**`Request` (`083—2024` `8.2.4.2` Table 17, `L1292-1308`)**: **verified**
- `Bit 63..60`: Mode Number (`1..7`); `Bit 59..32`: Reserved (`0`); `Bit 31..16`: Requested Voltage (`10 mV` units); `Bit 15..0`: Requested Current (`10 mA` units).
- Sink constraint (`9.2.2.4`, `L2464-2467`): if `|ΔV| >= 3 V`, reduce current first (`<= 2 A` or `<= 80% Icc`).

---

## 5. Timing, Voltage Transitions, Watchdog & Resets (`8.3`, `8.4`, `10.3`)

### 5.1 Protocol & Reset Timers (`Tables 12, 30, 31`)

| Timer / Parameter | Min | Typ | Max | Unit | Spec Section | Status | Verbatim Quote / Rule |
|---|---|---|---|---|---|---|---|
| `tACKReceive` (`ACKReceiveTimer`) | — | — | `10` | ms | `8.3.1` Table 30 (`L1818`) | **verified** | Started on TX last bit (when RX idle) waiting for `ACK`/`NCK`. |
| `tACKtransmit` | `100` | — | — | µs | `8.3.1` Table 30 (`L1819`) | **verified** | Delay after RX last bit before sending `ACK`/`NCK` (`tMsgTransDelay` wins if conflict). |
| `tRetry` | — | — | `500` | µs | `8.3.1`–`8.3.2` Table 30 (`L1820`) | **verified** | Max delay before retransmission after `NCK` or timeout. |
| `tSenderResponse` (`SenderResponseTimer`) | — | — | `50` | ms | `8.3.2` Table 30 (`L1821`) | **verified** | Initiator wait for data/control response message. |
| `tReceiverResponse` | — | — | `40` | ms | `8.3.2` Table 30 (`L1822`) | **verified** | Responder deadline to send reply (`Output_Capabilities`, `Refuse`, etc.). |
| `tPowerSupply` (`PowerSupplyTimer`) | — | — | `550` | ms | `8.3.3` Table 30 (`L1823`) | **verified** | Sink wait from `Accept` to `Power_Ready`. Timeout -> Sink MUST send Hardware Reset (`7.9 d`). |
| `tMsgTransDelay` (`MsgTransDelayTimer`) | `2` | — | — | ms | `8.3.7` Table 30 (`L1827`) | **verified** | Min gap after finishing TX packet. |
| `tCableInfoResponse` / `tRestartTrans` / `tCableTrans` / `tVerifyResponse` | — | — | `1200` / `1100` / `1000` / `1000` | ms | `8.3.4`–`8.3.8` Table 30 (`L1824-1828`) | **verified** | `tVerifyResponse` added in 2024. |
| **`tResetCable` / `tResetSource` / `tResetSink`** | `1000` / `2000` / `2000` | — | `1500` / — / — | µs | `7.9` Table 12 (`L946-965`) | **verified** | Pull target's **RX** LOW (`D-` for Source reset, `D+` for Sink reset). Source HW reset acts like unplug: disconnects VBUS **`> 1 s`**, then returns to default (`5.10 V`, BC1.2 DCP, `表35 #10`). |
| RX Idle Before Active TX / Post-`Soft_Reset` | `> 12` ms idle / `5` ms Sink (`15` ms Src) | — | — | ms | `8.7.3` (`L2141-2145`) | **verified** | Avoids bus collision; excludes `ACK`/`NCK`. |
| `nMsgRetryCount` | — | `3` | — | retries | `8.4.2` Table 32 (`L1860-1869`) | **verified** | After `3` TX failures -> send `Soft_Reset`; if `Soft_Reset` also fails `3`x -> Hardware Reset. |

### 5.2 Dynamic Voltage Transition & Overshoot (`10.3 Tables 39–40`)

- **`TDelay`** (msg RX to `ACK`/`Accept` TX): **`< 10 ms`** (`Table 40`, `L2712`).
- **Overshoot / Undershoot** (`Table 39`, `L2707-2708`): `vRealValid(Max) <= 1.05 * vSetNew`, `vRealValid(Min) >= 0.95 * vSetNew` (`±5%`).
- **Settle (`TPowSettle`) & `Power_Ready` (`tPowReady`, includes `TPowSettle`)** (`Table 40`, `L2727-2755`): **verified**
  - `30V < vSetNew` or `30V < Starting V`: `TPowSettle <= 470 ms`, `tPowReady <= 500 ms`
  - `20V < vSetNew <= 30V` or `20V < Starting V <= 30V`: `TPowSettle <= 370 ms`, `tPowReady <= 400 ms` *(was `350 / 400 ms` for `<= 30V` in 2022)*
  - `11V < vSetNew <= 20V` or `11V < Starting V <= 20V`: `TPowSettle <= 250 ms`, `tPowReady <= 275 ms`
  - `vSetNew <= 11V` and `Starting V <= 11V`: `TPowSettle <= 100 ms`, **`tPowReady <= 100 ms`** *(was `100 / 120 ms` in 2022; note `T/TAF 092—2024` `5.5.7` still checks `<= 120 ms`)*
  - Additional condition `|vSetNew - Starting V| <= 1 V`: `TPowSettle <= 50 ms`, `tPowReady <= 70 ms` *(text also requires `|ΔV| <= 1V` simultaneously with the tier above).*

### 5.3 Watchdog (`8.2.4.8 Table 23`) & `Soft_Reset` / `Exit_UFCS_Mode`

- **Watchdog Default & Behavior**: Starts immediately after `Ping`/`ACK` success; **default timeout = `1 s` (`1000 ms`)**; `Config_Watchdog(0)` disables it. Reset on every received Sink message; paused while Source prepares/sends a non-ACK reply (`Request` -> `Power_Ready`). **On overflow**: Source cuts VBUS **`> 1 s`** (`<= 0.8 V` in `092—2024 5.4.2.8`), sends **Hardware Reset (`>= 2000 µs` LOW on `D+`)**, and resets to default VBUS (`4.85–5.50 V`) + BC1.2 DCP (`083—2024` `L1466-1473, L2605-2611`; **verified**).
- **`Soft_Reset`**: Keeps UFCS mode and current VBUS/I unchanged; resets FSMs and `MsgNumberCounter = 0` (`L1085-1091`).
- **`Exit_UFCS_Mode`**: Source returns to BC1.2 DCP & default VBUS (`4.85–5.50 V`). Sink waits `200 ms` to verify `ACK`, VBUS in default range, and DCP short (`L2330-2334`).

---

## 6. Errors & No-Load Sink-Side Compliance Checks

### 6.1 `Refuse` Codes (`8.2.4.9 Table 24`, `L1486-1526`) & `Error_Information` (`8.2.4.7 Table 22`)

| `Refuse` Reason Code (`Bit 7..0`) | Meaning | Trigger Conditions (`083—2024 Sec 8.2.4.9`) | Status |
|---|---|---|---|
| `0x01` | `无法识别的命令或数据` | Mode-1 `ACK` followed by undefined `MsgType` (e.g. `011b`) or wrong `Data Length` | **verified** |
| `0x02` | `不支持的命令或数据` | Wrong receiver role (Source receives `Get_Sink_Info`), bad Key Index in `Verify_Request`, undefined `Cmd ID`, or unsupported optional command (`Detect_Cable_Info`, `Start_Cable_Detect`, `Verify_Request`, `Get_Sink_Info_Extended`, unknown vendor msg) | **verified** |
| `0x03` | `设备忙，暂无法响应` | Device busy, temporarily unable to respond | **verified** |
| `0x04` | `充电设备请求的输出电压、电流或功率超出范围` | Sink's `Request` mode index, V, I, or P out of Source's advertised `Output_Capabilities` bounds | **verified** |
| `0x05` | `其它原因` | Other reasons (e.g., Source unable to read cable e-marker) | **verified** |

**`Error_Information` (`Bit 31..0`, `083—2024 Table 22` vs `083—2022 Table 22`)**: **verified**
- In **`083—2024`**: `Bit 8` = `D+OVP`, `Bit 7` = `D-OVP`, `Bit 6` = `CCOVP`; all other bits (`31..9`, `5..0`) are **`Reserved`**.
- *(Note: **`083—2022`** had populated `Bit 14` Watchdog overflow, `Bit 13` CRC output shutoff, `Bit 12` Input power loss, `Bit 11` Leakage > 300 mA, `Bit 10` Input UVP, `Bit 9` Input OVP, `Bit 5` Int/Bat OTP, `Bit 4` Port OTP, `Bit 3` SCP, `Bit 2` OCP, `Bit 1` UVP, `Bit 0` OVP — so real chargers built to 2022 firmware may still set those bits.)*

### 6.2 Concrete No-Load Compliance Checks (`083—2024` & `T/TAF 092—2024`)

1. **Default VBUS & BC1.2 DCP (`083—2024 Table 37`, `L2655-2659`)**: At **no-load (`空载下测量`)**, verify `4.85 V <= VBUS <= 5.50 V` (typ `5.10 V`) and `D+/D-` shorted. **verified**
2. **Handshake & `tWaitPing` (`092—2024 5.2.1.1–5.2.1.3`)**: Verify `D+` HIGH within `tDpDet` (`11–15 ms`), accept/reject boundary pulse widths, and verify withholding `Ping` past `tWaitPing` (`110–120 ms`, tested at `160 ms`) causes `D+` HW Reset (`>= 2 ms` LOW) + BC1.2 DCP fallback, while `Ping` at `105 ms` gets `ACK`. **verified**
3. **Baud Rates & Framing (`092—2024 5.2.2.1–5.2.2.4`)**: Verify `Ping`/`ACK` across `115200`, `57600`, and `38400` bps, `0xAA` training tracking within `±15%` (`> ±20%` ignored), and `tACKReceive <= 10 ms` / `tSenderResponse <= 50 ms` / `tReceiverResponse <= 40 ms`. **verified**
4. **Capabilities & No-Load Voltage Accuracy (`083—2024 Table 41`, `092—2024 5.5.4.1–5.5.4.4 Sources 5005–5008`)**: Sweep `Output_Capabilities` at **no-load** in `100 mV` steps (`V_actual` via DMM/INA226, `V_adc` via `Get_Source_Info`): **verified**
   - `3.4V < V <= 5.5V`: `|V_set - V_actual| <= 150 mV`, `|V_adc - V_actual| <= 100 mV`
   - `5.5V < V <= 11V`: `|V_set - V_actual| <= 300 mV`, `|V_adc - V_actual| <= 100 mV`
   - `11V < V <= 21V`: `|V_set - V_actual| <= 600 mV`, `|V_adc - V_actual| <= 100 mV`
   - `21V < V <= 36V`: `|V_set - V_actual| <= 900 mV`, `|V_adc - V_actual| <= 100 mV`
5. **Step Monotonicity & Resolution (`092—2024 5.5.10 Source.5038`, `083—2024 10.3.3`)**: Issue 10 consecutive minimum voltage steps (`10` or `20 mV` per `Output_Capabilities`); verify strictly monotonic `V1 < V2 < ... < V10`. **verified**
6. **Dynamic Ramp Timing & Overshoot (`092—2024 5.5.7 Source.5017`, `083—2024 Table 40`)**: At **no-load**, sweep `5V <-> 7/9/11/12/15/18/20/30V`; verify `TDelay < 10 ms`, `Power_Ready` within `tPowReady` (`120 / 275 / 500 ms` in `092—2024`; `100 / 275 / 400 / 500 ms` and `70 ms` for `|ΔV|<=1V` in `083—2024`), and overshoot/undershoot within `±5%` (`0.95..1.05 * vSetNew`). **verified**
7. **Watchdog, `Soft_Reset`, `Exit_UFCS_Mode`, & Error/Retry Checks (`092—2024 5.3.11–5.3.15, 5.4.2.8–5.4.2.9`)**: **verified**
   - Default `1 s` Watchdog & custom `2 s` `Config_Watchdog`: silence past timeout -> Source sends `>= 2 ms` HW reset, drops VBUS `<= 0.8 V` for `> 1 s`, restores default VBUS & BC1.2 DCP (`Source.3014`, `Source.4009`).
   - `Soft_Reset` at Vmin & Vmax (`Source.4010`): VBUS holds steady, UFCS stays active, `Ping` succeeds.
   - `Exit_UFCS_Mode` (`5.3.9`): returns to default VBUS & BC1.2 DCP within `200 ms`.
   - Out-of-range `Request` -> `Refuse (0x04)` (`Source.3015`); 3 `NCK`s -> `Soft_Reset` (`Source.3017`).

---

## 7. URLs Log

| URL | Outcome | Details |
|---|---|---|
| `https://www.taf.org.cn/StdDetail.aspx?uid=c076e8c2-f28f-4462-9f53-ec5324e0b02a&stdType=TAF` | **Succeeded** | Discovered official `T/TAF 083—2024` PDF path (verified TLS using AIA chain `/etc/ssl/certs/ca-certificates.crt` + `GeoTrustTLSRSACAG1.crt`). |
| `https://www.taf.org.cn/upload/AssociationStandard/TTAF%20083%E2%80%942024%EF%BC%88TCCSA%20393-2024%EF%BC%89%20%E7%A7%BB%E5%8A%A8%E7%BB%88%E7%AB%AF%E8%9E%8D%E5%90%88%E5%BF%AB%E9%80%9F%E5%85%85%E7%94%B5%E6%8A%80%E6%9C%AF%E8%A6%81%E6%B1%82.pdf` | **Succeeded** | Official `T/TAF 083—2024` PDF (verified TLS, SHA-256 `1aa76e18...`). |
| `https://www.taf.org.cn/StdDetail.aspx?uid=fa8f8bf5-1472-4e3b-9b30-95846809afab&stdType=TAF` & `.../upload/AssociationStandard/TTAF%20092%E2%80%942024...pdf` | **Succeeded** | Official `T/TAF 092—2024` Test Methods PDF (verified TLS, SHA-256 `bdbf4655...`). |
| `https://www.taf.org.cn/upload/AssociationStandard/TTAF%20083-2022%20%E7%A7%BB%E5%8A%A8%E7%BB%88%E7%AB%AF%E8%9E%8D%E5%90%88%E5%BF%AB%E9%80%9F%E5%85%85%E7%94%B5%E6%8A%80%E6%9C%AF%E8%A7%84%E8%8C%83.pdf` | **Succeeded** | Official `T/TAF 083—2022` PDF (verified TLS; byte-identical to GitHub mirror `bf125513...`). |
| `https://github.com/392625227/UFCS-protocol-compare-with-USB-PD/raw/main/TTAF%20083-2022%20%E7%A7%BB%E5%8A%A8%E7%BB%88%E7%AB%AF%E8%9E%8D%E5%90%88%E5%BF%AB%E9%80%9F%E5%85%85%E7%94%B5%E6%8A%80%E6%9C%AF%E8%A7%84%E8%8C%83.pdf` | **Succeeded** | Byte-identical mirror of `T/TAF 083-2022` (`bf125513...`). |
| `https://www.taf.org.cn/StandardList.aspx` | **Failed (App Error)** | Returned `Error.aspx` ("访问异常，请稍后重试"); `StdQuery.aspx?type=TAF` worked instead. |
| `https://www.ufcs.com` | **Succeeded** | Confirmed FCA alliance announcements & `YD/T 7519-2026` publication info (no direct PDF links on inspected category pages). |
