# Feature ideas

Ideas for features that are not built yet. Each needs the PD transmit path (done) and drives the
output, so each is marked **HV** in `ROADMAP.md`. Nothing here has run on a device.

## Actuators that can cut off or limit power

The VBUS path is the INA226/228/237 shunt (3.0 mOhm); there is no series switch in it. The firmware
can still stop or throttle a source through four things it controls:

| Actuator | How | Effect | Works with |
|---|---|---|---|
| Open the CC pull-downs | FUSB302 SWITCHES0 = 0 and the external CC switches (PC13/PC14/PC15) off | the source sees Rd removed and opens VBUS | any Type-C source when the K2 is the sink |
| PD Hard_Reset, or a request for PDO 1 | FUSB302 CONTROL3 bit 6 (send hard reset); Request for 5 V | the source returns to 5 V (hard reset: 0 V for about 1 s first) | any PD source |
| Clamp D+ / D- low | PA7, PB0, PB1, PB2 driven low for about 500 ms | QC, AFC, FCP and SCP sources fall back to 5 V | legacy fast-charge sources |
| Re-request a PPS / AVS contract | new RDO with lower current or voltage (PPS: 20 mV and 50 mA steps; request repeated well inside the 10 s PPS timeout) | throttles in place | PPS and AVS sources |

With a phone inline, the K2 cannot open the phone's own Rd, but a Hard_Reset or a low clamp on CC
and D+/D- still makes the source drop VBUS.

## Ideas

| # | Idea | Sketch |
|---|---|---|
| P1 | Smart charge limiter | stop the output after a set energy (Wh), charge (mAh) or time, or when the current stays below a tail threshold (for example under 120 mA for 60 s after a peak above 1 A); cut with one of the actuators above |
| P2 | Programmable eFuse | trip on over-voltage, over-current (with a debounce), over-power or connector over-temperature (ADC NTC) using the INA sample rate |
| P3 | CC/CV bench supply over PPS or AVS | closed loop on the measured voltage and current: trim the requested voltage in 20 mV steps to hold a setpoint at the connector, walk it down to hold a current below the source's own limit, keep the contract alive |
| P4 | Charger profiler | step through every advertised PDO, record settling time, steady-state voltage error and ripple (PA1 capture), return to 5 V, show a pass / warn / fail card per PDO (the Charger test covers most of this) |
| P5 | E-marker booster | answer SOP' Discover Identity with a selectable virtual cable (off, 100 W 20 V 5 A passive, 240 W EPR 50 V 5 A, or a cloned saved cable); built as Spoof, see `include/k2/pd/emk_spoof.hpp` |
| P6 | Cable resistance | two-point estimate from an idle sample and a loaded sample at the same PDO voltage: R = (V_idle - V_load) / (I_load - I_idle); built as the passive estimate in `include/k2/pd/cable_r.hpp` |

Order of work: the cut-off actuators first (shared by P1 and P2), then P3.
