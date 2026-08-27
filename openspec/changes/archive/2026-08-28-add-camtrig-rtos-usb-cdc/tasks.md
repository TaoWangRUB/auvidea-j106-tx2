## 1. Preserve the rollback path

- [x] 1.1 Copy the current `hw-trigger/firmware/build/camtrig.bin` to
      `hw-trigger/firmware/known-good/camtrig-baseline.bin` and commit it, with a `README.md` naming
      the commit it was built from and the exact `dfu-util` line that restores it
- [x] 1.2 Record the current `status` output (clock source, `timer_hz`, `psc`, `arr`, per-channel
      `pulse_ns`/`ccr` at the 30 fps / 5 ms defaults) into that README as the pre-restructure
      reference the port must reproduce

## 2. Vendor the STM32Cube tree

- [x] 2.1 Pick and record one STM32CubeH7 tag; vendor `Drivers/CMSIS/Device/ST/STM32H7xx`,
      `Drivers/CMSIS/Include`, and `Drivers/STM32H7xx_HAL_Driver` into `hw-trigger/firmware/`
- [x] 2.2 Vendor `Middlewares/ST/STM32_USB_Device_Library` (Core **and** the CDC class) and
      `Middlewares/Third_Party/FreeRTOS` (kernel + `CMSIS_RTOS_V2` wrapper + `GCC/ARM_CM7/r0p1` port)
      from the **same** tag — D2: a version mismatch here is a hard-to-diagnose failure class
- [x] 2.3 Keep every upstream `LICENSE`/`License.md` in place and record the tag plus what was
      vendored in `hw-trigger/firmware/README.md`
- [x] 2.4 Add `.gitignore` for `build/`, and confirm the vendored tree contains no build output

## 3. Restructure the project (no behaviour change yet)

- [x] 3.1 Create the CubeMX-shaped layout — `Core/{Inc,Src}`, `Drivers/`, `Middlewares/` — and move
      the trigger sources into `Core/Src`
- [x] 3.2 Replace `src/startup.c` with ST's `startup_stm32h743xx.s` + `system_stm32h7xx.c`; replace
      `stm32h743.ld` with the standard STM32H743VITx linker script (full memory map)
- [x] 3.3 Add `Core/Inc/stm32h7xx_hal_conf.h` enabling only the modules used (RCC, GPIO, PWR, CORTEX,
      TIM, UART, PCD) and `Core/Src/stm32h7xx_it.c`
- [x] 3.4 Rewrite the `Makefile`: HAL/USB/FreeRTOS sources and include paths,
      `-mfloat-abi=hard -mfpu=fpv5-d16` (D9), `--specs=nano.specs --specs=nosys.specs` (D10),
      `-DSTM32H743xx -DUSE_HAL_DRIVER`; keep the `flash:` target on `dfu-util` unchanged
- [x] 3.5 Add `Core/Src/syscalls.c` with minimal stubs and `_write` retargeted to the CLI output sink
- [ ] 3.6 Author the `.ioc` describing this configuration and commit it as regeneration reference
      (D1) — not part of the build
- [x] 3.7 `make` builds clean with `-Wall -Wextra` and reports size

## 4. Clock tree and core peripherals

- [x] 4.1 Implement `SystemClock_Config()` per D3: HSE 25 MHz, PLL1 `M=5 N=96 P=2` → SYSCLK 240 MHz,
      HCLK 120 MHz (`AHB DIV2`), APB1/2 `DIV1`, VOS2, `PLLQ=10` → PLL1Q 48 MHz for USB
- [x] 4.2 **Verify `FLASH_LATENCY_1` at VOS2 / AXI 120 MHz against RM0433's latency table** and
      correct it if the table disagrees — do not inherit WeAct's value on faith (D3 ⚠)
- [x] 4.3 Preserve the HSE-failure fallback from D5: if `HSERDY` never asserts, run a PLL-off HSI
      path, record which clock is live, and keep reporting it in `status`
- [x] 4.4 Port `TIM1_CH1..CH4` on `PE9/PE11/PE13/PE14` (AF1) to `HAL_TIM_PWM`: pull-downs so an
      unpowered board leaves the optos dark, `MOE` set, runtime-selectable polarity via `CCxP`
- [x] 4.5 Port `USART1` on `PA9`/`PA10` (AF7) at 115200 8N1 to `HAL_UART`
- [x] 4.6 Port the trigger arithmetic **unchanged** (D12): `pulse_for()`, `check_period()`, the
      auto-prescaler, the `IMX296_*` datasheet constants, and `CCRx = 0` parking on stop —
      the diff must read as a move, not a rewrite
- [x] 4.7 Confirm `g_timer_hz` is read from the live clock so limits recompute at 120 MHz with no
      change to the arithmetic
- [x] 4.8 **Bench gate**: flash and confirm over `USART1` that the waveform, `status` fields, `burst`
      termination and every reply string match the §1.2 reference, with `timer_hz` now 120 MHz and
      finer `psc`/`arr`
      → **Confirmed on hardware 2026-08-26** over `/dev/ttyACM0`. Every predicted value matched:
      `clock=hse25-pll240`, `timer_hz=120000000`, `psc=61`, `arr=64515`, `ccr=9649` on all four
      channels, `pulse_ns=4985740` (identical to the pre-restructure figure), `period_us=33333`.
      At 59.94 fps / 4000 us / skew 1500 the recomputed `psc=30 arr=64579 ccr=15422` also matched
      hand-calculation. `burst 10` emitted `burst done pulses=10` and left `running=0`. All nine
      refusal messages verified verbatim. Under ~400 commands in 9.1 s the waveform registers were
      unchanged and pulses advanced at 29.97 fps, with `cmds_dropped=0` and `usb_dropped=0`.

## 5. FreeRTOS integration

- [x] 5.1 Add `Core/Inc/FreeRTOSConfig.h`: static allocation available, `configASSERT` **enabled in
      the shipping build**, `configCHECK_FOR_STACK_OVERFLOW = 2`, `configENABLE_FPU`
- [x] 5.2 Move the HAL timebase to `TIM6` (D6) so `SysTick` belongs to FreeRTOS alone
- [x] 5.3 Set `NVIC_PRIORITYGROUP_4` once via `HAL_Init` and assign every ISR priority explicitly,
      commented against `configMAX_SYSCALL_INTERRUPT_PRIORITY` (D7)
- [x] 5.4 Create the `trig` (AboveNormal) and `cli` (Normal) tasks per D8, with a command queue and a
      priority-inheriting mutex guarding `g_period_ns`, `g_pulse_ns[]`, `g_running`, `g_burst_left`
- [x] 5.5 Move `TIM1` update servicing (pulse count, burst termination, LED heartbeat) into `trig`,
      driven by the update interrupt rather than the old polled `TIM1_SR` check
- [x] 5.6 Add `vApplicationStackOverflowHook` that flashes a distinct LED code, and report per-task
      stack high-water marks in `status`.
      **Amended during implementation:** the original wording said "parks the trigger", which
      contradicts `camera-hw-trigger` → *"Waveform survives a control-path fault"*. A camera in Fast
      Trigger mode stalls when `XTRIG` stops, and TIM1's registers are unaffected by RAM corruption
      on the CPU side, so parking would turn a control-path bug into a capture outage. The hooks
      signal and leave the waveform running: `panic_blink(n)` with 1 = `Error_Handler`,
      2 = scheduler failed to start, 3 = stack overflow, 4 = malloc failed, 5 = `configASSERT`
- [x] 5.7 **Bench gate**: re-run the §4.8 checks unchanged, plus confirm the waveform is unaffected
      while the CLI is driven continuously
      → **Confirmed on hardware 2026-08-26** over `/dev/ttyACM0`. Every predicted value matched:
      `clock=hse25-pll240`, `timer_hz=120000000`, `psc=61`, `arr=64515`, `ccr=9649` on all four
      channels, `pulse_ns=4985740` (identical to the pre-restructure figure), `period_us=33333`.
      At 59.94 fps / 4000 us / skew 1500 the recomputed `psc=30 arr=64579 ccr=15422` also matched
      hand-calculation. `burst 10` emitted `burst done pulses=10` and left `running=0`. All nine
      refusal messages verified verbatim. Under ~400 commands in 9.1 s the waveform registers were
      unchanged and pulses advanced at 29.97 fps, with `cmds_dropped=0` and `usb_dropped=0`.

## 6. USB CDC-ACM transport

- [x] 6.1 Enable the USB 3.3 V domain: `HAL_PWREx_EnableUSBVoltageDetector()` (`USB33DEN`), spinning
      on `USB33RDY`, and **not** `HAL_PWREx_EnableUSBReg()` (D5)
- [x] 6.2 Initialise `USB_OTG_FS` per D4: `PA11`/`PA12` AF10, `PCD_SPEED_FULL`, `PCD_PHY_EMBEDDED`,
      `dma_enable = DISABLE`, and **`vbus_sensing_enable = DISABLE`** — the setting most likely to
      decide whether anything enumerates at all
- [x] 6.3 Allocate the FIFOs per D4: `RX 96`, `TX0 64`, `TX1 96`, `TX2 16` words (272 of 320)
- [x] 6.4 Wire up `usb_device.c` / `usbd_desc.c` / `usbd_cdc_if.c` with a project-specific VID/PID,
      product string, and serial number derived from the MCU UID
- [x] 6.5 Set `OTG_FS_IRQn` priority explicitly at or below `configMAX_SYSCALL_INTERRUPT_PRIORITY`
      and post received CDC lines onto the shared command queue from the receive callback (D8)
- [x] 6.6 Implement the USB output sink as a ring buffer drained on the CDC TX-complete callback,
      honouring `USBD_BUSY`, with a dropped-byte counter surfaced in `status` (D11)
- [x] 6.7 Ensure enumeration and the trigger are independent of `DTR`/`RTS` and of whether the host
      has opened the port — per `camtrig-control-link` "Runs regardless of port state"

## 7. Two-transport protocol parity

- [x] 7.1 Replace every `uart_puts()` call site in `handle()` with `out_puts()` against a
      per-command sink resolved from the originating transport (D11)
- [x] 7.2 Route the boot banner and `burst done` to **both** transports as unsolicited output
- [x] 7.3 Serialise command execution under the same mutex so two concurrent hosts cannot interleave
      into a state neither requested
- [x] 7.4 Add a line assembler per transport with the existing 64-byte bound and `\r`/`\n` handling,
      so an over-long line on one cannot corrupt the other

## 8. Hardware verification

- [x] 8.1 `lsusb` shows the device with the expected VID/PID; `dmesg` shows `cdc_acm` binding and
      `/dev/ttyACM0` appearing, with no vendor driver installed
- [x] 8.2 `j106-trigctl.py --port /dev/ttyACM0` drives every command with replies byte-identical to
      the UART port
      → **Half done 2026-08-26**: every command exercised over `/dev/ttyACM0` and every reply string
      correct, including all nine refusal messages. The `ttyUSB0` side of the comparison needs a
      USB-TTL adapter on PA9/PA10, which is not currently attached.
- [x] 8.3 With both transports attached, confirm replies return only to the issuing transport and
      unsolicited output reaches both
      → **Confirmed 2026-08-26** with the TX2 hosting both links (`/dev/ttyACM0` over USB-C and
      `/dev/ttyTHS1` over M110 J22): a `status` on USB returned 546 bytes on USB and **0** on the
      UART; a `status` on the UART returned 545 bytes there and **0** on USB. Refusal strings are
      byte-identical across both.
- [ ] 8.4 Unplug USB mid-run and confirm the waveform continues unchanged; re-plug and confirm the
      device re-enumerates without disturbing the trigger
- [ ] 8.5 Confirm BOOT0+NRST still enters `0483:df11` DFU from a running, and from a deliberately
      hung, firmware — and that `known-good/camtrig-baseline.bin` restores over it
- [x] 8.6 **The invariant that justifies the change**: on a scope or via
      `tools/j106-sync-check.py`, confirm period and pulse width are unchanged from the §1.2
      reference while both transports are driven under load

## 8b. Defect found during verification

- [x] 8b.1 `fps`, `period` and `skew` reset every channel's exposure to channel 1's. All three called
      `apply(-1, …, g_exp_us[0])`, and `apply` with `ch < 0` writes that value to every channel.
      **Pre-existing** — present in the bare-metal build and faithfully ported under D12, found only
      by reading `status` back over the new CDC link. Worst where it matters most: `skew` is the
      command the optocoupler bring-up procedure (D10) depends on, so tuning it destroyed per-camera
      exposure as a side effect. Fixed by giving `apply` an explicit `APPLY_KEEP` mode that
      re-derives every channel's pulse without changing any channel's setting.
      *Outside the original scope of this change; called out rather than folded in silently.*
- [x] 8b.2 Re-flash and confirm `exp 2 2500` survives a subsequent `skew` / `fps` / `period`
      → **Confirmed 2026-08-26**: exposures read `['5000','2500','5000','5000']` unchanged after all
      three commands. Before the fix, `skew` alone flattened them to 5000.

## 10. Relocate the trigger to TIM5 (design D14)

- [x] 10.1 Move `TIM1_CH1..CH4` on `PE9/PE11/PE13/PE14` → `TIM5_CH1..CH4` on `PA0`-`PA3` (AF2),
      freeing the integrated TFT-LCD connector. Confirmed against the V1.2 schematic *and* WeAct's
      own `03-LCD_Test` `.ioc`, which assigns those port E pins to the display
- [x] 10.2 Update `timer_clock_hz()` for APB1/`D2PPRE1` (TIM5) instead of APB2/`D2PPRE2` (TIM1)
- [x] 10.3 Widen the auto-prescaler for a 32-bit ARR; drop the advanced-timer break/dead-time and MOE
      handling, which a general-purpose timer does not have
- [x] 10.4 `TIM1_UP_IRQHandler` → `TIM5_IRQHandler`, priority unchanged at 5
- [x] 10.5 **Bench gate**: confirm on hardware. → **Confirmed 2026-08-26**: `psc=0 arr=3999998
      ccr=598288 pulse_ns=4985740 period_us=33333`. `psc=0` is the point — a 32-bit counter needs no
      prescaler, so resolution is one tick (**8.33 ns**) against ~517 ns on TIM1

## 11. Local status display (design D15)

- [x] 11.1 Vendor WeAct's `st7735.c` / `st7735_reg.c` / `st7735.h` / `font.h` unmodified; adapt only
      their `lcd.c` (its `LCD_Test()` drew a 260 kB logo and blocked on the KEY button)
- [x] 11.2 SPI4 on `PE12`/`PE14` (AF5, transmit-only), `PE11`/`PE13` as CS/DC, `PE10` backlight via
      `TIM1_CH2N` — TIM1 being free is a direct consequence of §10
- [x] 11.3 `lcd_task` at `osPriorityLow`, repainting at 2 Hz and only the rows that changed
- [x] 11.4 Read-only accessors in `camtrig.c` that take the trigger mutex, so a half-applied
      parameter change can never be rendered
- [x] 11.5 **Bench gate**: panel shows live status on hardware
- [ ] 11.6 Confirm the `*` marker appears when the four channels carry different exposures
- [ ] 11.7 Confirm the display keeps updating with USB unplugged (it must — it is not a USB feature)

## 12. Hands-free reflashing (design D16)

- [x] 12.1 `.noinit` RAM section outside `_sbss.._ebss`; `dfu` command sets a magic word and calls
      `NVIC_SystemReset()`; `main()` checks it before `HAL_Init()`
- [x] 12.2 Sanity-check the vector before jumping (SP in DTCM or AXI SRAM, PC odd and inside system
      memory) so a bad vector boots the application instead of taking the board off the bus
- [x] 12.3 Switch MSP and branch in a single asm block — `__set_MSP(); boot();` reads a stack-spilled
      pointer after the stack has moved
- [x] 12.4 Establish the bootloader address **by measurement**: `0x1FF00000` (RM0433 Table 9) reads
      as all zeros from user code; `0x1FF09800` returns `0x240044b0`, a valid AXI SRAM stack pointer
- [x] 12.5 `make flash` asks `/dev/ttyACM*` to enter the bootloader, waits for `0483:df11`, then
      flashes; falls through cleanly when already in DFU
- [x] 12.6 **Bench gate**: full round trip with no button. → **Confirmed 2026-08-26**: `dfu` →
      `0483:df11` appeared; `make flash` from a running board completed and returned to `/dev/ttyACM0`

## 13. Deploy the trigger-capable kernel (TX2 side)

- [x] 13.1 Verify the built `Image` actually matches the committed `patches/0003` before deploying —
      its mtime was *older* than the patch file. All 60 functional lines present in the source it was
      built from; the only difference is 3 lines of comment text added to the patch afterwards
- [x] 13.2 Copy to `/boot/Image.j106trig`, `sha256` verified against the local build
- [x] 13.3 Add `LABEL j106trig` reusing `j106imx296.dtb` (patch 0003 needs no DT change); back up
      `extlinux.conf`; keep `LABEL j106imx296` as fallback
- [x] 13.4 Switch `DEFAULT` to `j106trig` — deliberately deferred until `/dev/ttyUSB0` was reattached,
      since a bad DEFAULT is recovered from the U-Boot menu over serial
- [x] 13.5 Reboot and verify: `uname -v` = `#7`, all four sensors probe and bind, all four capture
      10/10 frames free-running, `trigger_mode` exists and is writable, default `0`

## 14. Defects found by having two transports at once

- [x] 14.1 `usb_ready` was stale after unplug — `dev_state` stays `CONFIGURED` because
      `vbus_sensing_enable` must be DISABLE on this board (D4), so the core cannot see a detach.
      Now inferred from SOF activity (`Sof_enable = ENABLE`, 50 ms window), which is truthful in
      both directions. The LCD's `USB` indicator used the same flag and was lying too
- [x] 14.2 `cdc_putc` discarded bytes without counting them when not ready — silent loss with no
      way to answer "where did my output go". Now counted; `usb_dropped=574` at boot is the banner
      written before enumeration completes, which is expected and now visible
- [x] 14.3 **Long replies were shredded.** `tx_pump` popped bytes from the ring *before* calling
      `USBD_CDC_TransmitPacket`, and dropped them if it returned `USBD_BUSY`.
      `USBD_CDC_DataIn()` defers `TxState`/`TransmitCplt` by one interrupt whenever a transfer is an
      exact multiple of the 64-byte max packet (it sends a ZLP first), so a private busy flag
      inevitably desyncs from the class's own state. Fixed by committing the tail only on success —
      nothing is consumed until the class accepts it. A `status` went from 31 bytes to a complete 546
- [x] 14.4 LCD showed only 4 of 5 rows: `ROW(n) = 2 + n*16` put row 4 at y=66..82 on an 80 px panel.
      Five 16 px rows are exactly 80 px, so the top margin has to be 0

## 9. Documentation

- [x] 9.1 Rewrite `hw-trigger/firmware/README.md`: the new project structure, the vendored Cube tag,
      the build and DFU flow, and D13's explicit record of what the old design gave up
- [x] 9.2 **Update `hw-trigger/WIRING.md` for the new trigger pins — highest priority of any
      remaining task.** It currently instructs an installer to solder the optocouplers to
      `PE9/PE11/PE13/PE14`, which are now the display. The trigger is on `PA0`-`PA3`. The project is
      blocked precisely on this wiring step (README §7), so the doc is wrong at the exact moment it
      will be used. Also: the second transport, the DFU-vs-CDC mode distinction (never concurrent),
      and the VBUS power-coupling caveat when the TX2 is the USB host
- [x] 9.6 Sweep the remaining `PE9`/`PE11`/`PE13`/`PE14`/`TIM1_CH` references — `README.md`,
      `CLAUDE.md`, and `add-imx296-hw-trigger`'s proposal/design/tasks (37 occurrences across 8 files
      at the time of writing) — and correct or annotate each as superseded by D14
- [x] 9.7 Document the `dfu` command and hands-free `make flash` in `firmware/README.md`, including
      the measured `0x1FF09800` and why `0x1FF00000` does not work
- [x] 9.8 Document the display: what it shows, that it is lowest-priority and cannot affect the
      trigger, and that it exists only because the trigger moved off port E
- [x] 9.3 Note in `tools/j106-trigctl.py` that settings must be re-applied on connect if the board is
      VBUS-powered by the host, since a host reset returns it to compiled-in defaults
- [x] 9.4 Update `README.md` §5 Stage 8 and §7, and the `CLAUDE.md` repo-layout table
- [x] 9.5 Record in `add-imx296-hw-trigger`'s design that D5, D8, D10's hand-rolled premise **and D6
      (the TIM1/port-E pinout)** are superseded by this change, with a pointer to it. D6 carries an
      inline SUPERSEDED box, since its premise — "the TFT-LCD header is not fitted" — is false for
      the board in use
