# Reference photos for the hardware trigger

Two photos were supplied when this work was scoped. **They are not yet in the repo** — pasted images
arrive in the chat as inline data, not as files on disk, so they have to be saved here by hand.

Drop them in with exactly these names and the links in [`../WIRING.md`](../WIRING.md) will resolve:

| Filename | What it shows | Used by |
|---|---|---|
| `imx296-module-trigger-pads.jpg` | The IMX296 module's rear pads: `3V3`, `MAS`, `XVS`, `XHS`, `XTR+`, `XTR−`, in a single vertical column down the left edge of the board, next to the M2 mounting hole | [`WIRING.md` §3.1](../WIRING.md#31-what-the-pads-are) — the pad identification table |
| `weact-ministm32h7.jpg` | The WeAct MiniSTM32H7xx board, DVP camera attached, both 2×22 headers legible (`E8 E9` … `3V3 5V` … `GND GND` on the right-hand header) | [`WIRING.md` §4.1](../WIRING.md#41-trigger--mcu--4-cameras-required-2-nets) — locating `PE9` by silkscreen |

Nothing depends on them: every pad and pin the photos show is also given by name, by header pin
number and by a "count the rows" description in `WIRING.md`, precisely so the wiring is followable
without them. They are useful confirmation, not the source of truth.

## Also worth capturing during bring-up

| Suggested filename | What to shoot |
|---|---|
| `trigger-wiring-installed.jpg` | The finished harness — the star ground and the fan-out point |
| `xtrig-scope.png` | `XTRIG` at the camera end: idle high, one low pulse per frame, pulse width = exposure |
