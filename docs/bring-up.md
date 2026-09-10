# Link layer bring-up

Two programs that test the controller port link one wire at a time,
with no protocol and no handshake between them. Use them when the web
server ROM misbehaves and you need to know *which* line is at fault.

| | |
|---|---|
| NES side | [`src/nes/debug.asm`](../src/nes/debug.asm) → `build/nes_link_test.nes` |
| smallest test | [`src/nes/d0test.asm`](../src/nes/d0test.asm) → `build/nes_d0_test.nes`, with [`src/firmware/NES_d0test/`](../src/firmware/NES_d0test/) |
| ESP32 side | [`src/firmware/NES_debug/NES_debug.ino`](../src/firmware/NES_debug/NES_debug.ino) |

```bash
python scripts/build_test_roms.py
```

```bash
arduino-cli compile --fqbn esp32:esp32:esp32c3:CDCOnBoot=cdc --upload -p COM3 src/firmware/NES_debug
```

**`CDCOnBoot=cdc` is not optional.** The bare `esp32:esp32:esp32c3` FQBN
defaults *USB CDC On Boot* to Disabled, which routes `Serial` to UART0 on
GPIO 20/21 instead of USB. The board still enumerates a COM port, because
that is the separate USB-Serial/JTAG peripheral esptool talks to, so a
monitor connects normally and shows nothing at all. The same flag is
needed when reflashing the gateway in `src/firmware/NES_router`.

Neither side waits for the other. The ROM cycles five phases forever;
the tester prints counters twice a second. You read the TV to see which
phase you are in and the serial log to see what actually arrived. Nothing
can go out of sync, which is the point — a handshake is exactly what we
are trying to test.

Counters are **per report window**, not cumulative, so a burst appears as
one large number and then falls back to zero.

## The five phases

| # | Screen | What the ROM does | Expect (PAL) |
|---|---|---|---|
| 1 | red / green | reads D0 once a frame, paints what it read | `clk` ~25/window, `out0` 0 |
| 2 | blue, flickering | toggles OUT0 every 4 frames | `clk` 0, `out0` ~3/window |
| 3 | yellow | exactly 1000 clock pulses, ~40 µs apart | `clk` totals 1000 |
| 4 | orange | exactly 1000 clock pulses, ~110 µs apart | `clk` totals 1000 |
| 5 | black | touches nothing | both 0 |

## Reading each one

**Phase 1 — gateway → NES.** This is the only direction the TV can show
you directly. With the tester idle (its default: the NES reads 0,
which is the wire held high, because the console inverts D0) the screen
should be solid red. Send `d0 1` and it should go solid green; `d0 sq`
and it should alternate about once a second. Clean response means the
D0 path works end to end. **A restless, random flicker means the line is
floating** — nothing is driving it. That is what an unpowered ESP32 looks
like, and also what an empty controller port looks like.

**Phase 2 — OUT0 → gateway.** OUT0 is software-timed and its pulses are
milliseconds wide, so it is immune to the pulse-width problem that
affects the clock. If `out0` stays at 0 through this phase, that line is
broken outright.

**Phases 3 and 4 — the clock.** The one that matters. The clock pulse is
about one CPU cycle (~0.56 µs) wide and cannot be widened in software, so
it is the fragile signal in the whole design. The ROM sends exactly 1000
pulses so the count is unambiguous:

- **1000** — the clock line is sound.
- **0** — nothing is getting through.
- **Between** — marginal. Edges are being rounded off or missed.

Running the same 1000 pulses fast and slow separates the two failure
modes. If **both** fail it is a signal problem: the pulse is not crossing
the input threshold, which is what a too-high-impedance divider does to a
0.56 µs edge. If **slow passes and fast does not**, it is a rate problem —
interrupt latency, not signal integrity.

**Phase 5 — the baseline.** Both counters must be 0. If they are not,
something is generating edges that the ROM is not, and every other number
in the log is suspect.

## Serial commands

| | |
|---|---|
| `d0 0` | NES reads 0, wire high — phase 1 should show solid red |
| `d0 1` | NES reads 1, wire low — phase 1 should show solid green |
| `d0 sq` | 1 Hz square wave — phase 1 should alternate |
| `zero` | reset the counters |
| `?` | command list, and the table above |

`arduino-cli monitor -p COM3 -c baudrate=115200` works, or use
`python scripts/link_monitor.py`, which does the same thing but timestamps
every line and tees the session to `link-test.log`.

## A trap worth knowing

**An empty controller port floats and reads back as ones.** The web server
ROM treats that as junk and pulses red rather than answering it, so red
with the cable out is expected. A flash cart menu reading a floating port
sees every button held down and launches immediately without being
touched. Neither symptom means a broken wire.
