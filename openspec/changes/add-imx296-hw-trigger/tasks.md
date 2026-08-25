## 1. Hardware documentation

- [x] 1.1 Create `hw-trigger/` with `WIRING.md`: signal-chain diagram, the STM32H7 ↔ TX2 ↔ 4-camera
      wiring tables (WeAct header pin numbers, J106/M110 connector pin numbers, camera pad names),
      and the datasheet constants table
- [x] 1.2 Write the level-domain measurement gate into `WIRING.md` — what to measure on `3V3 / MAS /
      XVS / XHS / XTR+ / XTR−` before connecting anything, the two outcomes, and the 820 Ω/1.0 kΩ
      divider for the 1.8 V case
- [x] 1.3 Document the optional serial link (WeAct `PA9`/`PA10` ↔ M110 `J22`), the
      `/dev/ttyTHS*` loopback identification procedure, and the USB-serial fallback
- [x] 1.4 Document powering the WeAct board (J22 pin 1 / 5 V vs USB-C — one or the other, never
      both) and the DFU flashing path via the BOOT0 button
- [x] 1.5 Add `hw-trigger/images/` with a README naming the two reference photos and referencing
      them from `WIRING.md`

## 2. STM32H7 trigger firmware

- [x] 2.1 Scaffold `hw-trigger/firmware/`: linker script for STM32H743VI, startup/vector table,
      `Makefile` for `arm-none-eabi-gcc`, and a `README.md` covering build and DFU flash
- [x] 2.2 Clock init — HSE 25 MHz direct to SYSCLK, no PLL, with an HSERDY timeout falling back to
      HSI 64 MHz; record the active source and the resulting timer clock in a runtime variable
- [x] 2.3 `TIM1_CH1..CH4` on `PE9/PE11/PE13/PE14`, one counter: PWM with runtime-selectable
      polarity, auto-prescaler (`div = ceil(period_ticks / 65536)`), start/stop that parks every
      channel unasserted, pull-downs so an unpowered board leaves the optos dark
- [x] 2.4 Datasheet-derived limits: reject periods shorter than `tTGPD` (1126 H = 16.6815 ms),
      subtract `tOFFSET` (14.26 µs) from the requested exposure, reject exposures that leave no
      readout margin — all computed from constants, not hard-coded tick counts
- [x] 2.5 `USART1` (`PA9`/`PA10`, 115200 8N1) line-based command interface: `fps`, `period`, `exp`
      (all or per-channel), `pol`, `skew`, `start`, `stop`, `burst`, `status`, `help`; `status`
      reports clock source, timer clock, period, polarity, skew, per-channel pulse widths and CCRs,
      running state and pulse count
- [x] 2.6 Boot defaults so the rig triggers with no host attached; heartbeat/activity on the `PE3`
      LED
- [x] 2.7 Build it — `make` must produce a `.bin`/`.elf` with `arm-none-eabi-gcc` and report size

## 3. Kernel driver support (patch 0003)

- [x] 3.1 Add the trigger registers to the driver: `TRIGEN` (0x300B), `LOWLAGTRG` (0x30AE),
      `SYNCSEL` (0x3036), reusing the existing `CTRL00`/`XMSTA` definitions
- [x] 3.2 Add the `trigger_mode` module parameter (0644) and read it at `start_streaming`
- [x] 3.3 Implement the mode transition on the stream-start standby cycle, matching the Raspberry Pi
      reference driver: clear `STANDBY` → settle → `TRIGEN`/`LOWLAGTRG`/`SYNCSEL` → release `XMSTA`.
      `SYNCSEL` is written as a **whole byte** (`0xc0`/`0xf0`) because bits [7:6] must stay `11b`
- [x] 3.4 Make `set_exposure` a no-op that stores the value when trigger mode is active, and log
      once per stream that exposure is owned by the trigger pulse
- [x] 3.5 Pin `VMAX` to the all-pixel value (1118) in trigger mode so a re-applied `frame_rate`
      cannot stall capture
- [x] 3.6 Log the active mode at stream start so a fallback to free-running is visible in `dmesg`
- [x] 3.7 Verify the driver never touches the shared reset GPIO on either path
- [x] 3.8 Generate `patches/0003-imx296-external-trigger-j106.patch` and confirm it applies cleanly
      on top of `0001`+`0002`

## 4. Host tools

- [x] 4.1 `tools/j106-trigctl.py` — `--port` (default `/dev/ttyUSB0`), subcommands mirroring the
      firmware protocol, sane errors when the port is absent
- [x] 4.2 `tools/j106-sync-check.py` — stream all four `/dev/video*` concurrently, report per-frame
      timestamp spread across cameras with min/median/max/worst-case, plus per-camera frame counts
- [x] 4.3 Make `j106-sync-check.py` print a free-running vs triggered verdict so the two runs are
      directly comparable, and have it refuse to run against a node it cannot open
- [x] 4.4 Add both to `tools/README.md` with the "where this runs" convention the other scripts use

## 5. Build and deploy

- [x] 5.1 Rebuild the kernel `Image` with patches `0001`+`0002`+`0003`; verify `4.9.337-tegra`
- [ ] 5.2 Deploy behind a new `extlinux` LABEL `j106trig`, keeping the current IMX296 label as
      fallback; DTB unchanged — *held until the wiring exists; needs a board reboot*
- [ ] 5.3 Confirm zero regression with `trigger_mode=0` — all four cameras still free-run and
      capture as before

## 6. Bring-up and verification (BLOCKED — requires the XTRIG wiring)

- [x] 6.1 Identify the camera pads by measurement and record the result in `WIRING.md` — **done**:
      the trigger input is an **optocoupler LED isolated from module ground** (1.2 V forward / OL
      reversed; both legs OL to a ground verified at 3.3 V powered, 1.9 kΩ unpowered). The
      level-translation gate is obsolete; the circuit was redesigned around current drive
- [ ] 6.1b Read the optocoupler part number off a module if legible (sets the usable exposure floor)
- [ ] 6.2 Wire 4 signal wires + common cathode return (no external components — module has R4=200 Ω)
- [ ] 6.2b Determine working polarity (`pol 0`/`1`) and calibrate `skew`
- [x] 6.3 Capture a free-running baseline with `j106-sync-check.py` — **done ahead of the wiring**: worst skew 2.43 ms, drift 8.33 µs/s over 20 s, phase re-randomised per stream start
- [ ] 6.4 Set `trigger_mode=1`, start the trigger, and confirm all four cameras deliver frames
- [ ] 6.5 Re-run `j106-sync-check.py` and record the triggered skew against the baseline
- [ ] 6.6 Confirm frame count equals trigger count over a `burst` run

## 7. Documentation

- [x] 7.1 Add a README stage section for the hardware trigger — what it does, the measured skew,
      the limitations (shared exposure, Argus, raw V4L2 only)
- [x] 7.2 Update README §7 status and the `CLAUDE.md` repo-layout table with `hw-trigger/`
- [x] 7.3 Update the memory notes for the 4×IMX296 population and the trigger design

## 8. Redesign after the pad measurement (opto-isolated input)

- [x] 8.1 Rewrite `WIRING.md` §2–§5, §7–§9 around current drive: 4 channels, no shared ground,
      revised bring-up order
- [x] 8.6 Vendor manual found (TLP281 + on-module `R4 = 200 Ω`): remove the external resistors the
      docs specified, add the 5 V variant, record why Ω-mode read OL and diode mode read 1.2 V
- [x] 8.2 Firmware: 4 channels on one counter, per-channel exposure, runtime `pol` and `skew`
- [x] 8.3 Spec: replace the level-translation requirement with "drive matched to the module's
      actual input type", and add "pulse sense and transport delay are correctable at runtime";
      make the waveform requirement transport-agnostic (assertion, not voltage level)
- [x] 8.4 Design: new D9 (the measurement and what it changed), D10 (runtime `pol`/`skew`), and
      D6 revised from one channel to four
- [x] 8.5 Update proposal, README Stage 8 + status, firmware README, `tools/README.md`, `j106-trigctl.py`
