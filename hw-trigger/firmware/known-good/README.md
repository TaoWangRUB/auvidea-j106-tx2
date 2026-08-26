# known-good — the pre-restructure rollback image

`camtrig-baseline.bin` is the **bare-metal** `camtrig` build, kept so the rig can be returned to a
known-working trigger generator at any point during `add-camtrig-rtos-usb-cdc` (the HAL / FreeRTOS /
USB CDC restructure). `build/` is git-ignored, so without this directory the image would be lost the
first time anyone ran `make clean`.

| | |
|---|---|
| Built from | `2b202b6` — *"hw-trigger: the module has its own 200R LED resistor — remove the external ones"* |
| Toolchain | `arm-none-eabi-gcc` 10.2.1 (GNU Arm Embedded 10-2020-q4-major) |
| `sha256` | `457aaef59c91da9f6d488bb1877a8784e9139a80051102e09eb4a7a90147a4df` |
| Size | 5036 bytes (`text 5032`, `data 4`, `bss 124`) |

Verified reproducible: `make clean && make` at that commit regenerates this file **bit-identically**.
`camtrig-baseline.elf` is kept alongside for symbol lookup if a fault ever needs decoding.

## Restore

No debugger needed. Hold **BOOT0**, tap **NRST**, release BOOT0 — that enters the STM32 ROM
bootloader, which is silicon and cannot be bricked by anything in the application image:

```bash
dfu-util -a 0 -s 0x08000000:leave -D known-good/camtrig-baseline.bin
```

`lsusb` should show `0483:df11` before you run it. `:leave` resets into the restored firmware.

## Reference behaviour the restructured build must reproduce

At the compiled-in power-on defaults (**30.000 fps, 5000 µs**, `pol 1`, `skew 0`), on **HSE 25 MHz**.
These are computed with the firmware's exact integer arithmetic (`pulse_for()` → `tim1_program()`),
not captured from a board — confirm them against a live `status` before relying on them as the
comparison baseline:

```
clock=hse25
timer_hz=25000000
running=1
period_us=33333            (period_ns 33333333, fps_milli 30000)
polarity=active_high (idle LED off)
opto_skew_ns=0
ch1..ch4_exposure_us=5000  pulse_ns=4985740  ccr=9587
psc=12
arr=64101
```

Derivation, for cross-checking after the clock tree changes:

- `period_ns` = 10¹²/30000 = **33 333 333** ns, above `tTGPD` (1126 H × 14815 ns = 16 681 690 ns) ✔
- `pulse_ns` = 5 000 000 − `tOFFSET` 14 260 − `skew` 0 = **4 985 740** ns; leaves 28.3 ms of readout
  margin against the 1 ms minimum ✔
- `pt` = 33 333 333 × 25 MHz / 10⁹ = 833 333 ticks → `div` = ⌈833333/65536⌉ = **13** → `psc` = 12,
  `arr` = 833333/13 − 1 = **64101**
- `ccr` = 4 985 740 × 25 MHz / 10⁹ / 13 = **9587** → duty 14.956 %, emitted pulse 4985.24 µs

**After the restructure the timer clock becomes 120 MHz** (design D3), so `timer_hz`, `psc`, `arr`
and `ccr` all change while `period_us`, `pulse_ns` and the exposures must not. That is the point of
recording both the values and the derivation: the arithmetic is unchanged, only the clock it reads
is.

Expected post-restructure values at the same defaults, computed with the same code:

```
clock=hse25-pll240
timer_hz=120000000
psc=61                     arr=64515        ch1..4 ccr=9649
```

| | emitted period | emitted pulse | step |
|---|---|---|---|
| 25 MHz (this image) | 33333.040 µs | 4985.240 µs | 520.0 ns |
| 120 MHz (restructured) | 33333.267 µs | 4985.317 µs | 516.7 ns |

Two things to expect at the §4.8 bench gate, so neither is mistaken for a regression:

- **Resolution barely improves.** `div = ceil(pt/65536)` means ARR's 16 bits, not the clock, set the
  step — it is ≈ `period/65536` either way. The 5× finer tick is absorbed by a 5× larger prescaler.
- **The emitted period shifts by ~227 ns**, because the prescaler/ARR pair rounds differently. All
  four cameras still share one counter, so inter-camera synchronisation is unaffected; only the
  absolute frame rate moves, by 7 ppm.
