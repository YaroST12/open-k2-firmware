# Selftest (`selftest`)

On-device correctness check and DWT cycle benchmark of the hot paths: conversion math, formatting, filtering, RGB565 blending, text rasterisation, rectangle fills, `mem*`, `fmt::format`, the PD codec and request planner, the PD trace ring, the TinyUSB FIFO, and flash placement.

- Implementation: [`include/k2/core/math_selftest.hpp`](../include/k2/core/math_selftest.hpp), [`src/core/math_selftest.cpp`](../src/core/math_selftest.cpp)
- Shell wiring: [`include/k2/core/shell.hpp`](../include/k2/core/shell.hpp), [`src/core/shell.cpp`](../src/core/shell.cpp), [`src/core/shell_hw.cpp`](../src/core/shell_hw.cpp)
- Host tests: [`tests/test_math_selftest.cpp`](../tests/test_math_selftest.cpp)

## Design

Every check calls the production function (not a copy), feeds it deterministic input laundered through `keep()`, validates the result against an independent reference or invariants, and hashes everything it saw into one FNV-1a sum.

- **Timing source**: CoreDebug `DEMCR.TRCENA` (`0xE000EDFC` bit 24) and DWT `CTRL.CYCCNTENA` (`0xE0001000` bit 0); `DWT->CYCCNT` (`0xE0001004`) is read around each check. On host builds (`!__arm__` or `__unix__`) `read_cycles()` returns `0`. Each check runs with IRQs masked (`primask`), so a cycle count is the check alone.
- **Constant-folding barriers**: `keep()` uses `asm volatile("" : "+r"(v) :: "memory")` (values up to 4 B) or a `volatile` copy, so every loop iteration runs at run time.
- **Golden sums**: a deterministic check passes only when its sum equals the golden in the `kCases` table. The goldens were harvested from the host build of the same source (the host test prints the report with `K2_SELFTEST_REPORT=1`), and `tests/test_math_selftest.cpp` asserts the same constants. A device FAIL (the report adds a `want=0x...` line) therefore means a real host/target difference. `tu_fifo` exists only on the target; its golden is computed from the byte pattern.
- **Tolerance checks** (float state may round differently): `fpu_sqrt` is judged by the `512.71765 +/- 0.01` sum range (its printed `qsum` is not compared), `one_euro` by its final `vbus`/`current` ranges (host finals 5003 mV / 1997 mA, window +/-3).
- **No hardware side effects**: nothing touches the LCD, SPI, I2C, USB or FUSB302, and no flash is written. Drawing goes to a `PixelSink` over the scratch arena through `lcd::set_sink`; the previous sink is put back when the check ends (`SinkGuard`). The PD trace check works on a local ring, not the live black box. The arena checks (`text_aa`, `text_big`, `gfx_fill`, `mem_ops`, `pdtrace_ring`, `tu_fifo`) need the scratch arena free and the main-loop context; with the arena held they report FAIL instead of using it.
- **`zw=1` / `zw=0`**: the highest address among the production functions the check called is below `0x08040000` (zero-wait flash) or not. A `zw=0` check runs part of its path with flash wait states; see Known gaps.

## Running on the K2

1. Ensure the K2 USB mode includes the CDC debug shell (**HID** or **HID+Drive** in **Settings > USB Link**).
2. Disconnect any non-essential power source / load setup as required by your A/B protocol, and make sure no other CDC stream reader (`picocom`, `sniff stream`, etc.) holds the serial port.
3. Send `selftest` over the USB CDC serial port (e.g. `/dev/ttyACM0`), from the main loop context with no menu holding the scratch arena:

```text
selftest
```

### Example Output Format

```text
math selftest (arch=m4f opt=Os math_errno=off):
  [PASS] scale_div    sum=0xXXXXXXXX  cyc=... zw=1
  [PASS] adc_calc     sum=0xXXXXXXXX  cyc=... zw=1
  [PASS] power_calc   sum=0xXXXXXXXX  cyc=... zw=1
  [PASS] math_opt     sum=0xXXXXXXXX  cyc=... zw=1
  [PASS] readout_fmt  sum=0xXXXXXXXX  cyc=... zw=1
  [PASS] fpu_sqrt     sum=0xXXXXXXXX  cyc=... zw=1
  [PASS] one_euro     sum=0xXXXXXXXX  cyc=... zw=1
  [PASS] blend565     sum=0xXXXXXXXX  cyc=... zw=1
  [PASS] text_aa      sum=0xXXXXXXXX  cyc=... zw=1
  [PASS] text_big     sum=0xXXXXXXXX  cyc=... zw=1
  [PASS] gfx_fill     sum=0xXXXXXXXX  cyc=... zw=1
  [PASS] mem_ops      sum=0xXXXXXXXX  cyc=... zw=1
  [PASS] fmt_line     sum=0xXXXXXXXX  cyc=... zw=1
  [PASS] pd_codec     sum=0xXXXXXXXX  cyc=... zw=1
  [PASS] pd_sink      sum=0xXXXXXXXX  cyc=... zw=1
  [PASS] pdtrace_ring sum=0xXXXXXXXX  cyc=... zw=1
  [PASS] tu_fifo      sum=0xXXXXXXXX  cyc=... zw=1
  [PASS] ws_fast      sum=0xXXXXXXXX  cyc=... zw=1
  [PASS] ws_slow      sum=0xXXXXXXXX  cyc=... zw=0
PASS 19/19 checks, total_cyc=...
```

The `zw` values above are what the placement is expected to give, not measurements. The header prints `arch` (`m4f` / `host`), TU optimization macro (`Os` / `O2+` / `O0`), and `math_errno` (`off` when compiled with `-fno-math-errno`, `on` otherwise).

## Kernel Coverage

| Kernel | What it verifies and benchmarks | Hot path covered |
|---|---|---|
| `scale_div` | `readout::power_half` exact strings (`PD 100W`, `PD 240W`, `5V 15W`) + 64 swept `(mv, ma)` iterations | 32-bit `UDIV` fast-path vs `__aeabi_uldivmod` |
| `adc_calc` | `adc::to_mv`, `vdd_mv`, `ntc_temp` bounds + 32 iterations of `average_sweeps`, `make_reading`, `sample_period_ns` | ADC conversion scaling |
| `power_calc` | `implied_shunt_uohm`, signed `power::mw`, 64 swept `Ina226`/`Ina228`/`Ina237` conversions; mV x mA above 2^32 (`udiv64_by_u16` with `hi != 0`), the `raw_to_ma` clamp rows (`|raw| >= 20'000'000` and the rounding clamp), `Average` over 5 samples past 2^32, negative, and a full 128 ring | Reciprocal multiplies, signed scaling, 64-bit slow paths, clamps |
| `math_opt` | `div_u64_u32` and `div_u64_u32_slow` over eight dividends (multiply-back check, no 64-bit divide), den = 0 and num < den, `div_i64_to_i32`, `mul_div_i32`, `mul_div_u32`, `mw_of`, `div_u64_to_u32` | The 64-bit divide helpers the loops above stay clear of |
| `readout_fmt` | `readout::fmt3` strings + 32 swept calls | `div_u64_to_u32` above 99950 |
| `fpu_sqrt` | `std::sqrt` on four Pythagorean triples + a 64-step loop; tolerance | `vsqrt.f32` vs `sqrtf` |
| `one_euro` | `smoothing::Readout` (`Preset::Low`) over 32 alternating `+/-15` ticks; tolerance | Hard-float filtering |
| `blend565` | `make_blend_lut` across 6 colour pairs x 16 alpha levels, bit-exact against `(f*a + b*(15-a) + 7) / 15` | Blend ramp |
| `text_aa` | `gfx_text` with `font_16` ("9.0V 3A", degree and ohm signs), `font_12`, and `gfx_text_rot` Ccw and Cw, into a RAM sink: every pixel one of the 16 blend levels, no cell left unwritten, no missing glyph | Compressed-font decode, glyph blend, UTF-8 path |
| `text_big` | `font_24` "5.00V", `font_48` "-1", `font_64` "8", same checks | Big-digit fonts |
| `gfx_fill` | `kit::fill_except` with four overlapping and clipped holes, `gfx_fill` with three edge-clipped rectangles, against a per-pixel reference | Fill and clip |
| `mem_ops` | `memcpy` (7 to 1024 B, aligned and not), `memset`, forward and backward `memmove`, `strlen`, against a volatile byte loop | Pinned libc helpers |
| `fmt_line` | `fmt::format` literals (`%d %u %x %X %s %c %%`, widths, zero pad, `INT_MIN`, null `%s`, truncation) + 24 status lines against a 32-bit reference formatter | `fmt::format` |
| `pd_codec` | `decode_source_caps` (24 rounds of 1..5 PDOs, reuse), `decode_pdo` for Fixed, PPS, EPR AVS and SPR AVS, `current_limit_ma`, RDO round trips, `parse_vdm` (Identity, SVIDs, Modes), `parse_emarker` and `cable_limits` | PD message decode |
| `pd_sink` | `plan_request` over 17 rows (Fixed, PPS, SPR and EPR AVS, over-cap, bad position, hunt clamps, mismatch, no caps), `lookup_held`, `request_cap_mv`, `rdo_fixed` / `rdo_pps` / `rdo_avs` against the bit layout, `make_skedb` bytes | Request planning |
| `pdtrace_ring` | A wrapped `Ring<1024>` (150 records, 102 resident, order and payload), `Collapse` folding six Source_Capabilities rounds into one record plus a repeat count, `flush`, `cursor_for_last`, on a local ring | Black Box push and collapse |
| `tu_fifo` (device only) | TinyUSB `tu_fifo_config`, wrap-around `tu_fifo_write_n` / `tu_fifo_read_n`, overflow refused | HID log / CDC FIFO |
| `ws_fast` / `ws_slow` | The same integer loop, once in the hot block and once in `.cold_probe` at the end of `.text` (above `0x08040000`; `link.ld` asserts it). Sums are identical; the `cyc` ratio is the wait-state cost. The check fails if either function is not where it should be | Placement canary |

## Known gaps

- Not covered: the Sink state machine (it needs a live FUSB302 or a full fake wire; the `Fake`-env tests cover it on the host) and `pdtrace::Captures` (flash-backed).
- Pinned into the hot block (K2_HOT, or a section-name pattern in `link.ld` for header-inline code): `fmt::format` and its helpers, the codec in `pd.cpp` / `pd_vdm.cpp`, `plan_request` / `rdo_*` / `make_skedb` in `pd_sink.cpp`, and all of `k2::math` (`.text._ZN2k24math*`, including `div_u64_u32_slow`). `zw` is expected to read 1 for every check; a `zw=0` after a later change means a production function has slipped out of the block. `ws_slow` reads 0 on purpose. `systick::delay_*` stays unpinned: its timing is calibrated against flash speed.
- Goldens come from the host. A mismatch on the device is a finding about host versus target, not a flaw of the check.

## A/B Comparison Workflow

1. Flash the baseline build and run `selftest`: record `math_errno`, per-kernel `cyc`, `zw`, `total_cyc`.
2. Flash the build under test and run `selftest`: confirm `PASS n/n checks` (the goldens must match without edits), then compare per-kernel `cyc` and `total_cyc`. A check that appears only on one side is new coverage, not a regression.
3. Only `src/core/math_selftest.cpp` itself compiles with the global flags (`-Os`); speed-tiered translation units (`adc_calc.o`, `power_calc.o`, `fastcharge_classify.o` at `-O3`; `battery.o`, `dpsniff.o` at `-O2`; `power_monitor.o` stays at `-Os`: its -Ofast was removed because the One Euro filter is one shared out-of-line copy built at `-Os`) run inside the corresponding calls.
