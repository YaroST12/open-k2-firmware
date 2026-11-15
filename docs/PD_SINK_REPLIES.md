# PD sink replies: what the K2 answers a source, and how fast

The K2 is a sink (UFP) on its PD link. Every SOP message a source sends it that asks for an answer gets the spec's answer, from
the PD engine itself (`pd::Sink`), in the main-loop pass that reads the frame. The source's timer is tReceiverResponse
(15 ms from the GoodCRC of its request to the start of our answer); the K2's target is under 10 ms worst case.
This page is the table, one row per message, and the timing argument. `tools/pd_reply_audit.py` checks a recording against it.

## Who answers, and when the K2 stays silent

- One answerer: `fusb302::handle_frame` (src/drivers/fusb302.cpp:1059) hands every SOP frame to `Sink::on_message`
  (src/pd/pd_sink.cpp:912) and `active_tick` (src/drivers/fusb302.cpp:2140) transmits `Sink::take_tx` (src/pd/pd_sink.cpp:236)
  in the same `service()` call (src/drivers/fusb302.cpp:2802). VCONN_Swap alone is `pd::Vconn`'s (include/k2/pd/pd_vconn.hpp:110),
  sent from the same pass when the sink has nothing (src/drivers/fusb302.cpp:2197).
- Answer slots, in `take_tx` order: Sink_Capabilities, Sink_Capabilities_Extended, then one answer slot (`ans_`: a swap's Reject,
  Not_Supported, Revision, EPR_Sink_Capabilities, a VDM NAK, a chunk request), then the engine's own queue (`tx_`: Requests, the
  Xiaomi and identity VDMs, Soft_Reset's Accept). An answer never displaces a queued Request; a pending Hard Reset goes first.
- Gate (`answers_requests()`, include/k2/pd/pd_sink.hpp): `gives_sink_cap_ && answers_sop_`, set per frame at
  src/drivers/fusb302.cpp:1119-1120: a live Pd-mode sink (not the PD-silent legacy session), the line's only sink (not inline),
  on an attached CC. Passive, the TX choke point refuses every frame anyway (`g_tx_allowed`).
- The refusal rules are one pure function, `refusal_for` (include/k2/pd/pd_reply.hpp:19), and the NAK rule `vdm_nak_for`
  (include/k2/pd/pd_reply.hpp:62); the sink calls them after its own handlers.

Revision: PD 3 = the source's caps say PD 3.x (`rev_` 2). PD 2.0 rows are **inferred** from the revision (PD 2.0 has no
Not_Supported and no extended messages): a request the revision defines is refused with Reject, a type it does not define is
ignored.

## Table

Status: **ok** (was right before this page), **fixed** (branch k2/pd-replies), **exception** (deliberately different, see below).
Timing path: "loop" for every row, so the tables carry no column for it: every answer, VCONN_Swap's included, is queued and sent
by the `service()` pass that reads the frame; nothing replies from an IRQ (fake-chip proof:
`test_a_request_is_answered_in_the_service_pass_that_reads_it`, tests/test_fusb302_scan.cpp). Host tests for every row, SPR and
EPR mode: tests/test_pd_sink_replies.cpp.

### Control messages

| Message (type) | Rev | Spec reply | K2 does (path:line) | Status |
|---|---|---|---|---|
| GoodCRC (1), Accept (3), Reject (4), PS_RDY (6), Wait (12), Not_Supported (16) | all | none (replies) | engine state only, pd_sink.cpp:1003 ff. | ok |
| GotoMin (2), Ping (5), Data_Reset_Complete (15) | all | none | nothing, refusal_for 0 (pd_reply.hpp:19) | ok |
| Get_Source_Cap (7) | PD 3 | Not_Supported (sink-only port) | Not_Supported, default branch pd_sink.cpp:1116 | fixed |
| Get_Source_Cap (7) | PD 2.0 | Reject | Reject, same | fixed (inferred) |
| Get_Sink_Cap (8) | all | Sink_Capabilities | Sink_Capabilities (kSinkCapPdos, or a twin's override), pd_sink.cpp:1071 | ok |
| DR_Swap (9), PR_Swap (10) | all | Reject (UFP-only, sink-only) | Reject via the answer slot, pd_sink.cpp:1077 | ok |
| VCONN_Swap (11) | PD 3 | Not_Supported | pd_vconn.hpp:110 | ok |
| VCONN_Swap (11) | PD 2.0 | Reject | Reject, pd_vconn.hpp:140 (take_tx maps Not_Supported) | fixed (inferred) |
| Soft_Reset (13) | all | Accept, MessageID 0, then wait for Source_Capabilities | Accept (restart_id), state Idle, pd_sink.cpp:1088 | ok |
| Data_Reset (14) | PD 3 | Not_Supported (UFP without USB4 Data Reset) | Not_Supported, pd_sink.cpp:1116 | fixed |
| Get_Source_Cap_Extended (17), Get_Status (18), Get_PPS_Status (20), Get_Country_Codes (21), Get_Source_Info (23) | PD 3 | Not_Supported (Get_Status may be answered so) | Not_Supported, pd_sink.cpp:1116 | fixed |
| the same | PD 2.0 | none (not defined) | nothing | ok |
| FR_Swap (19) | PD 3 | Not_Supported | Not_Supported, pd_sink.cpp:1077 | ok |
| FR_Swap (19) | PD 2.0 | none | nothing | ok |
| Get_Sink_Cap_Extended (22) | PD 3 | Sink_Capabilities_Extended | SKEDB (Sink Modes 0x23), pd_sink.cpp:1074 | ok |
| Get_Revision (24) | PD 3 | Revision | Revision, RMDO 0x32100000 (3.2, v1.0; kSinkRmdo), pd_sink.cpp:1112, 885 | fixed |
| reserved (0, 25-31) | PD 3 | Not_Supported | Not_Supported, pd_sink.cpp:1116 | fixed |
| reserved | PD 2.0 | ignore | nothing | ok |

### Data messages

| Message (type) | Rev | Spec reply | K2 does (path:line) | Status |
|---|---|---|---|---|
| Source_Capabilities (1), SPR mode | all | Request | the engine's Request; a re-announce in a negotiation or contract: answer_reannounce, pd_sink.cpp:977, 798 | ok |
| Source_Capabilities, EPR mode, unsolicited | PD 3 | Hard Reset by the sink | Hard Reset, pd_sink.cpp:969 | ok |
| Request (2), Sink_Capabilities (4), EPR_Request (9) | all | none (a far sink's messages) | nothing | ok |
| BIST (3) | all | none (carrier mode not supported) | nothing | ok |
| Battery_Status (5), Alert (6), Source_Info (11), Revision (12) | PD 3 | none (notifications / replies) | nothing | ok |
| Get_Country_Info (7), Enter_USB (8) | PD 3 | Not_Supported | Not_Supported, pd_sink.cpp:995 | fixed |
| EPR_Mode (10) | PD 3 | none (Ack / Succeeded / Failed / Exit drive the entry) | engine state, pd_sink.cpp:978 ff. | ok |
| reserved (0, 13, 14, 16-31) | PD 3 | Not_Supported | Not_Supported, pd_sink.cpp:995 | fixed |
| reserved | PD 2.0 | ignore | nothing | ok |

### Vendor_Defined (15)

| Message | Rev | Spec reply | K2 does (path:line) | Status |
|---|---|---|---|---|
| Discover Identity REQ (SVID 0xFF00) | all | ACK | ACK as a Xiaomi sink, pd_sink.cpp:1344 | ok |
| Discover SVIDs REQ (0xFF00) | all | ACK | ACK [0x2717] (+0x05AC armed), pd_sink.cpp:1371 | ok |
| Discover Modes REQ, any SVID (0x05AC unarmed, 0xFF01, 0x8087, structured 0x2717, ...) | all | NAK (no modes) | NAK echoing SVID, version, position, command, pd_sink.cpp:1422 | fixed |
| Enter Mode / Exit Mode REQ, SVID-specific (16-31) and reserved (0, 7-15) commands, Discover Identity to a vendor SVID | all | NAK | NAK, pd_sink.cpp:1422 | fixed |
| Attention (6) | all | none | nothing (vdm_nak_for 0) | ok |
| ACK / NAK / BUSY from the source | all | none | Identify / Reread / Probe / host report read them | ok |
| SVID 0x05AC while the lab Apple readout is armed | all | the readout's script | apple_readout engine, pd_sink.cpp:1403 | exception |
| unstructured SVID 0x2717 | all | the Xiaomi script | the Xiaomi steps, pd_sink.cpp:1441 | exception |
| unstructured, any other SVID | PD 3 | Not_Supported | Not_Supported, pd_sink.cpp:1430 | fixed |
| unstructured, any other SVID | PD 2.0 | ignore | nothing | ok |

### Extended messages (PD 3 only; PD 2.0 has none, all ignored)

| Message (type) | Spec reply | K2 does (path:line) | Status |
|---|---|---|---|
| Source_Capabilities_Extended (1), Status (2), Battery_Capabilities (5), Manufacturer_Info (7), Security_Response (9), Firmware_Update_Response (11), Country_Info (13), Country_Codes (14), Sink_Capabilities_Extended (15), EPR_Sink_Capabilities (18) | none (replies) | nothing | ok |
| PPS_Status (12) | none (the answer to our Get_PPS_Status) | stored, pd_sink.cpp:927 | ok |
| EPR_Source_Capabilities (17), not the last chunk | Chunk Request | Chunk Request (padded 4-byte form), pd_sink.cpp:938 ff. | ok |
| EPR_Source_Capabilities, last chunk, entering EPR | EPR_Request | proceed_after_epr_caps, pd_sink.cpp:949 | ok |
| EPR_Source_Capabilities, last chunk, EPR mode re-announce | EPR_Request in time | EPR_Request for the held level (else the contract's, else PDO 1), answer_reannounce, pd_sink.cpp:954 | fixed |
| EPR_Source_Capabilities in SPR mode (our EPR_Get_Source_Cap's answer) | none (informational) | chunks fetched, nothing else | ok |
| Get_Battery_Cap (3), Get_Battery_Status (4), Get_Manufacturer_Info (6), Security_Request (8), Firmware_Update_Request (10), Vendor_Defined_Extended (30), reserved | Not_Supported (no battery, no such feature) | Not_Supported, on_unhandled_ext pd_sink.cpp:853 | fixed |
| the same, sent in several chunks | the rest asked for, then Not_Supported | Chunk Requests, then Not_Supported after the last chunk, pd_sink.cpp:853 | fixed |
| any extended message sent unchunked | Not_Supported (unchunked never negotiated) | Not_Supported, pd_sink.cpp:853 | fixed |
| a Chunk Request aimed at us | serve the chunk | nothing: every extended message of ours is one chunk | ok |
| Extended_Control (16) EPR_Get_Sink_Cap (2) | EPR_Sink_Capabilities | one chunk, the Get_Sink_Cap words (at most 6), answer_epr_sink_caps pd_sink.cpp:896 | fixed |
| Extended_Control EPR_Get_Source_Cap (1), EPR_KeepAlive (3), unknown ECDB | Not_Supported (sink-only port) | Not_Supported, pd_sink.cpp:853 | fixed |
| Extended_Control EPR_KeepAlive_Ack (4) | none (the answer to our keep-alive) | nothing | ok |

## Exceptions (deliberate)

| Mode | What differs | Where |
|---|---|---|
| Inline / sniff (another sink on the line) | zero TX: nothing is answered, nothing counted | gate at src/drivers/fusb302.cpp:1119-1120; TX choke point |
| Legacy (PD-silent) charger-test units, passive boot | no GoodCRC, no answers: the source hears nothing, so nothing is owed | same gate (`g_legacy_only`, Auto-CRC off) |
| Get_Sink_Cap_Extended mode Silent / Not_Supported (lab, `k2_remote pd-reread --sink-cap-ext`) | that one answer withheld or refused | pd_sink.cpp:1074, test_lab_modes_keep_their_own_answers |
| PD Spec Check keep-alive silence | our PPS / EPR keep-alive held back (not a reply) | `Sink::silence_keepalive` |
| PD Stress | deliberately aggressive requests; its own journal writes (src/pdstress/journal_hw.cpp) are not gated and stall replies (10.1 to 11.6 ms Source_Capabilities -> Request after its Soft_Resets, 7 on record in runs/2026-11-11-integ-hw/pd-stress) | the tool's own budget |
| Device twin | Get_Sink_Cap and EPR_Get_Sink_Cap carry the twin's PDOs | `set_sink_cap_override`, src/twin/twin_hw.cpp |
| Xiaomi chain (unstructured 0x2717), armed Apple readout (0x05AC) | the script owns the reply | pd_sink.cpp:1403, 1441 |

## Timing

**The path.** The chip GoodCRCs a frame on its own. `service()` runs once per millisecond from the main loop
(src/core/main_loop.cpp:368) and every millisecond inside long LCD draws (`idle_hook`, src/core/main_loop.cpp:294); it drains the
RX FIFO, the engine queues the answer, and `active_tick` loads and starts it in the same call. On hardware the reply leaves
1.5 to 1.7 ms after `service()` read the frame (TX record "1.6 ms after RX"), and 2.2 to 3.3 ms from the frame's INT_N edge to the
reply's TX_START when the loop is not held (`pd_reply_audit.py` over 183 trace files: minimum 2.19 ms, medians 2.5 to 3.2 ms
per message).

**Bound.** reply latency <= (time until `service()` next runs) + one pass (about 1.7 ms). The first term is 1 ms plus the longest
thing in the main loop that neither returns nor calls `service()`. With nothing else running that is under 2 ms (medians
below 3.3 ms edge to TX_START). It is NOT bounded by code alone:

- **Flash writes** stop everything: program and erase run from RAM with interrupts masked (src/drivers/flash.cpp): 17 to 38 ms
  per journal pump on record ("[CJRN] begin ok ... worst=21ms" to "replan ok ... worst=38ms"), more with an erase or compaction. The app spans both flash banks, so there
  is no read-while-write. Writers that can run during a live contract: the charger journal (now gated, below), PD Stress's
  journal (exception), the phase / attempt / finish / meta records of a charger-test run (written just before the run's own
  Request, by design). The Black Box persister (include/k2/core/bbpersist.hpp) and the Rd comeback record (src/core/rd_comeback.cpp) wait for
  300 ms of a quiet link; other savers were not audited here.
- **`delay_ms`** waits do not call `service()` (`wait_hook`, src/core/main_loop.cpp:148, samples buttons and the transition
  capture only).

**What was late, and the fixes.** `pd_reply_audit.py` over the 183 Black Box trace files under runs/ (all recorded before
this branch): 797 owed replies, 42 flagged (16 over 10 ms, 26 missing). Latency is the frame's INT_N edge to the reply's TX_START:

| Case | Worst | Cause | Fix |
|---|---|---|---|
| EPR_Source_Capabilities re-announced in EPR mode | 15.69 ms (2026-11-09-splitter/streamA6); 6 of 18 over 10 ms (11.39 to 12.4 in A2, A3, A4, A6, splitter-pps); median 8.37 ms | the engine did not answer the last chunk in Ready; the runner's next Request happened to cover it | answered by the engine in the same pass (answer_reannounce, pd_sink.cpp:954) |
| Get_Sink_Cap 226 ms after PS_RDY | 15.66 ms (ugreen-qa/r2-stream) | the charger journal's arm wrote flash 80 ms after the link went quiet ("[CJRN] begin ... busy=128ms", a 30 ms service() gap) | writes wait for 80 ms of quiet AND 600 ms since the last Accept / PS_RDY (charger_journal::write_ok, include/k2/chargertest/charger_journal.hpp:845; src/ui/ui_charger_test.cpp:401); forced only after 1000 ms (rows) / 5000 ms (arm) |
| Discover Identity at a run's start | 14.76 ms (spec-audit/ufcs-2) | the same journal arm ("[CJRN] begin ok ... worst=21ms") | the same gate |
| Source_Capabilities after a PD Stress Soft_Reset | 11.58 ms (7 over 10 ms, pd-stress owner-boot / owner-fail streams) | PD Stress's own journal write | exception (PD Stress) |
| Discover Modes REQ (0x05AC, structured 0x2717) | never (19 + 6 missing) | no handler: silence | NAK (pd_sink.cpp:1422) |
| Get_Sink_Cap_Extended (2026-11-11-integ-hw/fh22-5a/c2-silent) | never (1 missing) | the lab Silent mode, on purpose | exception (audit with `--ignore`) |

The source's own requests come a median 208 ms, 90 % within 304 ms and 95 % within 510 ms after PS_RDY (104 on record), which is
why the journal waits 600 ms after a contract. **Residual:** a request that comes later than that (5 % on record, one at 2.9 s)
or during a forced write still waits for the write in progress: 17 to 38 ms per journal pump on record, more with an erase or compaction. Only
read-while-write (the app in one bank) or a write split into sub-10 ms pieces would bound it.

## Checking a recording

```
python3 tools/k2_stream.py replies runs/NAME                  # one k2_stream session folder
python3 tools/pd_reply_audit.py runs/A runs/B/trace.jsonl     # several; --files LIST, --limit-ms N, --ignore TEXT, --json
```

Per message: replies owed, given, min / median / max latency (frame stamp to the reply's TX_START), and MISSING / WRONG / LATE
examples; exit 1 when anything is flagged. A frame is the K2's only when its chip sent the GoodCRC and no other sink spoke within
1.5 s. A session that silences a message on purpose (Get_Sink_Cap_Extended Silent) is checked with `--ignore`.

## Open decisions

- Structured Discover Modes for 0x2717 is now NAKed (the K2 lists 0x2717 in its SVIDs but has no structured modes); not yet seen
  answered on hardware by a Xiaomi-family source.
- EPR_Sink_Capabilities carries the Get_Sink_Cap words (two SPR PDOs, no EPR PDO); a sink that wants EPR power may need EPR PDOs.
- Revision claims USB PD 3.2, version 1.0.
- The PD 2.0 rows are inferred from the revision, not from a PD 2.0 source on the bench.
