# Saved-cable JSON (`k2-emarker`, format version 1)

Every cable saved in the e-marker library (`include/k2/pd/emlib.hpp`) shows up on the USB drive as
`CABLES/<name>.JSN` (long name `<name>.json`). The flash format stays binary; this text is
generated on read by `k2::emjson::render()` (`src/pd/emjson.cpp`, host test `tests/test_emjson.cpp`).

```json
{
  "format": "k2-emarker",
  "version": 1,
  "id": 3,
  "revision": 2,
  "name": "Desk USB4",
  "auto_name": "Apple-USB4G3-240W",
  "record": "<132 hex digits>",
  "cable": {
    "vid": "0x05AC",
    "vendor": "Apple",
    "pid": "0x1234",
    "xid": "0x00001234",
    "type": "passive",
    "speed": "USB4 Gen3 (40G)",
    "current_a": 5,
    "max_v": 50,
    "max_w": 240,
    "latency": "10-20 ns (~2 m)",
    "vconn_required": false,
    "epr": true,
    "pd_rev": 2,
    "vdos": ["0xFF00A041", "0x180005AC", "0x00001234", "0x12340101", "0x00004643"]
  }
}
```

| Field | Meaning |
| :--- | :--- |
| `format`, `version` | document type and its format version. A reader must ignore fields it does not know; a change that breaks existing fields bumps `version`. |
| `id` | the cable's number in the library, 1..9999, stable through renames; the sequence the K2 has (it has no clock, so there is no timestamp) |
| `revision` | 1 for a new cable, +1 for every rename |
| `name` | the user's name: 1..24 characters from `[A-Za-z0-9 -_.]`; defaults to `auto_name` (with `-2`, `-3` on a collision). It is also the file name. |
| `auto_name` | `<maker>-<USB version>-<watts>` computed from the record (`k2/pd/emname.hpp`): maker by VID (hex when unlisted), `USB2`, `USB3.2G1`, `USB3.2G2`, `USB4G3`, `USB4G4` or `TBT3`, `60W`/`100W`/`200W`/`240W` |
| `record` | the raw 66-byte record as upper-case hex: name[20], PD spec rev, five VDOs (VDM header, ID header, cert stat, product, cable VDO 1), reserved, checksum (`include/k2/pd/emrec.hpp`). This is the source of truth; every field below is decoded from it. |
| `cable.vid`, `vendor` | ID header bits 15:0 and the vendor name from the firmware's table (`Unknown` when not listed) |
| `cable.pid`, `xid` | product VDO bits 31:16, cert stat VDO |
| `cable.type` | `passive` or `active` (ID header product type 3 or 4, as the e-marker page) |
| `cable.speed` | cable VDO 1 bits 2:0 |
| `cable.current_a`, `max_v`, `max_w` | cable VDO 1 bits 6:5 and 10:9; watts are 60, 100, 200 or 240 |
| `cable.latency` | cable VDO 1 bits 16:13 |
| `cable.vconn_required`, `epr` | termination bits 12:11 nonzero; 5 A and 50 V |
| `cable.pd_rev` | the record's PD spec revision byte (1 = 2.0, 2 = 3.0) |
| `cable.vdos` | the five stored VDOs, `0x` + 8 hex digits |

Drive file names: the user name with every character outside `[A-Za-z0-9-_]` removed, upper case,
cut to 8 characters; when that collides with another cable or the name was cut, the short name
is `<first 6 or fewer>~N` (`DESKUSB4.JSN`, `DESKU~1.JSN`). The long name is `<name>.json`.
