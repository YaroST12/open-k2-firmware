# Math & FPU Selftest (`selftest`)

On-device correctness check and DWT cycle micro-benchmark for Cortex-M4F math, conversion, formatting, filtering, and RGB565 blending routines.

- Implementation: [`include/k2/core/math_selftest.hpp`](../include/k2/core/math_selftest.hpp), [`src/core/math_selftest.cpp`](../src/core/math_selftest.cpp)
- Shell wiring: [`include/k2/core/shell.hpp`](../include/k2/core/shell.hpp), [`src/core/shell.cpp`](../src/core/shell.cpp), [`src/core/shell_hw.cpp`](../src/core/shell_hw.cpp)
- Host tests: [`tests/test_math_selftest.cpp`](../tests/test_math_selftest.cpp)

## Design

`k2::selftest::run_math()` depends only on pre-existing pure headers (`fmt.hpp`, `adc_calc.hpp`, `power_calc.hpp`, `readout_fmt.hpp`, `smoothing.hpp`, `blend.hpp`) with zero dependency on `math_opt.hpp`, so the exact same selftest files build and run cleanly on both pre-optimization and post-optimization commits.

- **Timing source**: Enables CoreDebug `DEMCR.TRCENA` (`0xE000EDFC` bit 24) and DWT `CTRL.CYCCNTENA` (`0xE0001000` bit 0), reading `DWT->CYCCNT` (`0xE0001004`) around each kernel. On host test builds (`!__arm__` or `__unix__`), `read_cycles()` returns `0`.
- **Constant-folding barriers**: `keep()` uses `asm volatile("" : "+r"(v) :: "memory")` (for `<= 4` B values on GCC/Clang) or `volatile` reads so the compiler evaluates every loop iteration at runtime.
- **Safety**: Purely read-only compute kernels over synthetic inputs; no GPIO, CC, D+/D-, VBUS, or flash state is touched.

## Running on the K2

1. Ensure the K2 USB mode includes the CDC debug shell (**HID** or **HID+Drive** in **Settings > USB Link**).
2. Disconnect any non-essential power source / load setup as required by your A/B protocol, and make sure no other CDC stream reader (`picocom`, `sniff stream`, etc.) holds the serial port.
3. Send `selftest` over the USB CDC serial port (e.g. `/dev/ttyACM0`):

```text
selftest
```

### Example Output Format

```text
math selftest (arch=m4f opt=Os math_errno=off):
  [PASS] scale_div   sum=0xXXXXXXXX  cyc=...
  [PASS] adc_calc    sum=0xXXXXXXXX  cyc=...
  [PASS] power_calc  sum=0xXXXXXXXX  cyc=...
  [PASS] readout_fmt sum=0xXXXXXXXX  cyc=...
  [PASS] fpu_sqrt    sum=0xXXXXXXXX  cyc=...
  [PASS] one_euro    sum=0xXXXXXXXX  cyc=...
  [PASS] blend565    sum=0xXXXXXXXX  cyc=...
PASS 7/7 checks, total_cyc=...
```

The header prints `arch` (`m4f` / `host`), TU optimization macro (`Os` / `O2+` / `O0`), and `math_errno` (`off` when compiled with `-fno-math-errno`, `on` otherwise).

## Kernel Coverage

| Kernel | What it verifies and benchmarks | Primary optimization target |
|---|---|---|
| `scale_div` | `readout::power_half` exact strings (`PD 100W`, `PD 240W`, `5V 15W`) + 64 swept `(mv, ma)` iterations hashed with FNV-1a | `math::mw_of` / 32-bit `UDIV` fast-path vs `__aeabi_uldivmod` |
| `adc_calc` | `adc::to_mv`, `adc::vdd_mv`, `adc::ntc_temp` bounds + 32 iterations of `average_sweeps`, `make_reading`, and `sample_period_ns` | Header/conversion scaling and loop optimization |
| `power_calc` | `implied_shunt_uohm`, signed `power::mw` + 64 swept `Ina226`/`Ina228`/`Ina237` conversions (`raw_to_mv`, `raw_to_ma`, `mw`) | Reciprocal multiplies & signed scaling |
| `readout_fmt` | `readout::fmt3` fixed-digit formatting (`15.0V`, `5.08V`, `2.04A`, `0.62W`, `100W`) + 32 swept calls | 32-bit `< 99950` path + `div_u64_to_u32` above |
| `fpu_sqrt` | `std::sqrt` on four Pythagorean triples (`<= 1e-4f`) + 64-step `sqrt(dx^2 + dy^2)` loop checked against `512.71765f +/- 0.01f`; prints quantized sum `qsum` | `-fno-math-errno` hardware `vsqrt.f32` vs `libm` `sqrtf` |
| `one_euro` | `smoothing::Readout` (`Preset::Low`) over 32 alternating `+/-15` jitter ticks checking settled bounds (`4900..5100 mV`, `1900..2100 mA`) | Hard-float / `OneEuro` filtering throughput |
| `blend565` | `make_blend_lut` across 6 RGB565 pairs x 16 alpha levels compared bit-exact against `(f*a + b*(15-a) + 7) / 15` | Division-free `(x * 4369u + 4368u) >> 16` |

## A/B Comparison Workflow

1. Flash the pre-optimization branch (`k2/selftest-pre`) and run `selftest` -> record `math_errno=on`, per-kernel `cyc`, `total_cyc`, and `sum` CRCs.
2. Flash the post-optimization branch (`k2/ui-rework`) and run `selftest` -> confirm `PASS 7/7 checks`, verify matching `sum` CRCs, and compare per-kernel `cyc` and `total_cyc`.
3. Note that only `src/core/math_selftest.cpp` itself compiles with global flags (`-Os`), while speed-tiered translation units (`adc_calc.o`, `power_calc.o`, `fastcharge_classify.o` at `-O3`; `battery.o`, `dpsniff.o` at `-O2`; `power_monitor.o` stays at `-Os`: its -Ofast was removed because the One Euro filter is one shared out-of-line copy built at `-Os` whatever flag the caller object has) execute in `adc_calc`, `power_calc`, and `power_monitor` calls.
