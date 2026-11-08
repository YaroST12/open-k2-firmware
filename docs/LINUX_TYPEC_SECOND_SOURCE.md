# Linux Type-C sysfs as a second source of truth (2026-11-05)

Question: can a Linux laptop independently confirm what the K2 sees or sends (charger PDOs, cable
e-marker, the spoofed e-marker)? Triggered by the GNOME extension gnomeUsbMon
(https://github.com/Ska1006/gnomeUsbMon).

## What gnomeUsbMon reads

All of it comes from sysfs plus GUdev events. It needs no root, runs no subprocess and reads no
debugfs.

- Partner source PDOs: `/sys/class/typec/portN-partner/usb_power_delivery/source-capabilities`
  (legacy layout) or `…-partner/pdN/source-capabilities`. It decodes fixed, PPS (`programmable_supply`),
  variable and battery supplies.
- Live contract: `/sys/class/power_supply/ucsi-source-psy-USBC000:00N/{online,voltage_now,current_now}`.
  The active PDO is matched by voltage.
- It does **not** read cables: no `portN-cable`, no `identity/`, no cable VDO, no EPR cable data.
  It cannot show an e-marker.
- It polls every 2 s while a UCSI source is online, plus udev-triggered refreshes.

## This laptop (dev host)

- Lenovo 82WM, driver `ucsi_acpi` (UCSI over ACPI; the PD controller firmware decides what is
  discovered), kernel 7.2.
- 3 Type-C ports: `port0`, `port1` (sink role), `port2` (source/host role, `vconn_source = yes`).
  `usb_power_delivery_revision` reads 2.0 and `usb_typec_revision` reads 1.1. UCSI often under-reports
  these.
- The kernel `typec` class can expose `/sys/class/typec/portN-cable/` with
  `identity/{id_header, cert_stat, product, product_type_vdo1..3}`. The cable VDO carries the current,
  voltage and EPR rating. That happens only if the PD controller reports cable properties and PD
  messages over UCSI. **Unverified on this laptop**: it needs a connected cable; the UCSI capability
  flags are in debugfs, which needs root.
- The laptop is an SPR device. It never shows a charger's EPR PDOs, only the SPR list.

## Can it read the K2's "e-marker"?

- The K2 top plug has no e-marker chip, so there is nothing to read.
- Only while the spoof is armed **and** the laptop, as source, sends SOP' would
  `portN-cable/identity` show the spoofed VDOs. That would be an independent proof of the spoof.
  Laptop PD controllers probably gate cable discovery on Ra at attach, like the Baseus. See
  `docs/K2_PORTS_SPEC.md`.
- The K2 house rule says no active PD while a laptop is attached. Any test with the K2 as an active
  sink on a laptop port needs explicit user approval.

## Uses, ranked

| Use | Setup | Rule-safe |
|---|---|---|
| Second PDO decoder for charger work | Charger → K2 (passive sniff) → laptop port 0/1. Diff the K2's sniffed caps against `portN-partner/…/source-capabilities` | yes (passive only) |
| Cross-check the K2 e-marker reader and saved clones | Real e-marked cable: laptop port 2 → any sink device. Read `port2-cable/identity`, compare with the K2's read of the same cable | yes (K2 not involved) |
| Check the K2's SOP Discover Identity reply | K2 as sink on a laptop port. Read `portN-partner/identity` | needs approval |
| Prove the spoof end to end | Laptop port as source → K2 top plug, spoof armed | needs approval; likely Ra-gated |

## First check to run

Plug the e-marked EPR cable into laptop port 2 with a sink on the far end, then run
`ls /sys/class/typec/` and `grep . /sys/class/typec/port2-cable/{type,plug_type,identity/*}`.
If `port2-cable` exists with a non-zero `product_type_vdo1`, the laptop can act as the cable reference.
A small `tools/typec_snapshot.sh` that dumps the whole `/sys/class/typec` tree would beat the extension
for this job.
