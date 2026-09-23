# Arduino Mega gateway

The same gateway as the ESP32-C3's, on an Arduino Mega 2560: one sketch,
compiled for a different board. The ROM, the protocol and the bridge are
unchanged. The Mega is a 5 V board, so it wires straight onto the controller
port with no level shifting, which also makes it a useful second opinion
when the ESP32 link will not come up.

| | |
|---|---|
| gateway firmware | [`src/firmware/NES_router/`](../src/firmware/NES_router/NES_router.ino) |
| bring-up tester | [`src/firmware/NES_debug/`](../src/firmware/NES_debug/NES_debug.ino), with the test ROMs in [`bring-up.md`](bring-up.md) |
| board | Arduino Mega 2560, genuine or a clone. **Not** an Uno, Nano or Pro Mini: their 2 KB of RAM cannot hold a page |

## Wiring

Controller port 1. There are two ways to wire it, and the firmware is built
for one or the other.

| NES pin | Signal | Direction | header wiring (default) | RJ45 wiring |
|---|---|---|---|---|
| 1 | GND | — | GND | GND |
| 2 | CLK | NES → gateway | pin 2, via 1 kΩ | pin 12 |
| 3 | OUT0 / latch | NES → gateway | pin 3, via 1 kΩ | pin 9 |
| 4 | D0 | gateway → NES | pin 4, via 1 kΩ, **inverted** | pin 13, **inverted** |
| 7 | +5 V | — | not connected | not connected |

Looking into the console's socket:

```
        .-
 GND -- |O\
 CLK <- |OO\ -- +5V
 OUT <- |OO| <- D3
  D0 -> |OO| <- D4
        '--'
```

A controller's plug, seen from its face, is the mirror image: the lone hole
at the angled end is top right. **Identify the cable's wires with a
continuity meter, not by colour.** NESdev's table gives red for D0 and
yellow for CLK, plenty of tutorials say the opposite, and clones use
anything.

### Header wiring

- **Pins 2 and 3** are the Mega's INT4 and INT5, the only external
  interrupt pins free. 18 to 21 are Serial1 and I2C. INT4 has the higher
  priority, which is the one the clock wants.
- **The 1 kΩ resistors** are not level shifting; both ends are 5 V. They are
  there for when one side is powered and the other is not, which would
  otherwise feed a signal pin's current into the unpowered chip through its
  protection diodes. They slow the edges by tens of nanoseconds, nothing next
  to the clock pulse's ~0.5 µs.

### RJ45 wiring

For an RJ45 jack plugged straight into the header, with no wires.

- **CLK is on pin 12.** That is PB6. It has no external interrupt, but it
  does have a *pin change* interrupt, PCINT6. `attachInterrupt()` cannot use
  that, so the firmware sets it up itself. The clock needs an interrupt
  whatever the wiring, because its pulse is too short to poll.
- **OUT0 on pin 9 has no interrupt at all.** It doesn't need one. The
  gateway recognises a poll at its first clock rather than at its strobe:
  the first clock after a quiet line, with OUT0 low. The NES reads the
  frame's first bit during that pulse, before the gateway can present it,
  but that bit is always a zero, which an idle line already gives it.
- **D0 on pin 13 shares the board's L LED.** That makes the LED an
  activity light: it glows while the gateway idles and flickers during
  transfers. The bootloader also flashes it for a moment whenever the board
  resets, and resets happen whenever the serial port opens. The NES reads
  a few junk polls, shows red briefly, and carries on.
- **No resistors.** Nothing limits the current when one side is powered and
  the other is not. So **power both before the cable goes in, and pull the
  cable before either goes off.** Hot plugging with both powered is fine.
- **Find out where the cable's other wires land** (+5 V, and D3/D4 if the
  cable has them). On pins 9–11 they are harmless: the firmware leaves those
  as inputs. On AREF, +5 V would fight the Mega's own reference. Leave that
  wire disconnected.

### Both wirings

- **+5 V stays disconnected.** The Mega runs from USB; joining the two
  supplies back-feeds one from the other.
- **The inputs use the Mega's internal pull-ups**, so with the cable out they
  sit high and quiet rather than floating.

## Flashing

The AVR core, once:

```bash
arduino-cli core install arduino:avr
```

Then, with the Mega's port (`arduino-cli board list` shows it), for the
header wiring:

```bash
arduino-cli compile --fqbn arduino:avr:mega:cpu=atmega2560 --upload -p COM4 src/firmware/NES_router
```

or for the RJ45 wiring:

```bash
arduino-cli compile --fqbn arduino:avr:mega:cpu=atmega2560 --build-property "compiler.cpp.extra_flags=-DNES_WIRING_RJ45" --upload -p COM4 src/firmware/NES_router
```

From the Arduino IDE, uncomment `#define NES_WIRING_RJ45` near the top of
the sketch instead. The bring-up tester, `src/firmware/NES_debug`, takes the
same flag, and has a matching line to uncomment.

Clones with a CH340 USB chip may need its driver on older Windows.

## Running

Exactly as in [`running.md`](running.md), with the bridge on the Mega's port:

```bash
python scripts/serial_bridge.py --serial-port COM4
```

Opening the port resets the Mega, and it spends a moment in its bootloader.
The bridge waits for `# Arduino Mega gateway online` before it starts serving.
Only one program can hold the port, so close any serial monitor first.

## How it differs from the ESP32 gateway

| | ESP32-C3 | Arduino Mega |
|---|---|---|
| decompresses pages | on the gateway | in `serial_bridge.py` |
| page cache | in RAM, so a repeat hit is instant | none — every request goes to the NES |
| inputs with the cable out | pulled low by the dividers | held high by the pull-ups |
| resets when the serial port opens | no | yes |
| largest page packet | 8192 bytes | 4096 bytes |

**Raw packets.** The Mega's 8 KB of RAM holds a page's packet but not the
decompression tables as well, so it sends each page exactly as the
cartridge stores it, with an `X-NES-Packet: nhf2` header. The bridge
decompresses it with the decoder from `emulate_rom.py`, drops the header, and
serves the page. A packet that arrives short or does not decode cleanly gets
a 502 rather than a broken page. The link checksum has already been checked
on the Mega by then, so in practice that means the serial cable was pulled
mid-response.

**The 4096 byte limit.** Today's packets are 2290 (`index.html`), 1540
(`style.css`) and 373 (404) bytes. The firmware uses about 4.5 KB of RAM for
globals, most of it that buffer. If the site grows past the limit, the host
tests fail before the Mega does.

## Timing

The link is hard real time. After each clock pulse the gateway has until
the NES next reads `$4016` — about 33 µs on PAL — to put the next bit on D0,
and it has to read OUT0 before the NES moves it on. An ESP32 has time to
spare; a 16 MHz AVR, behind its interrupt dispatch, is worth checking.
[`tests/host/test_mega_avr.cpp`](../tests/host/test_mega_avr.cpp) checks it,
for each wiring. It runs the compiled firmware on an ATmega2560 simulator,
cycle for cycle, against the ROM on a 6502 whose every port access lands at
its real time, and fetches pages through it.

| Worst case over the page fetches, PAL | header | RJ45 |
|---|---|---|
| clock pulses, closest together | 33.1 µs | 33.1 µs |
| CLK edge to its interrupt handler starting | 18.5 µs | 14.4 µs |
| D0 settled before the NES reads it | 19.5 µs | 13.7 µs |
| OUT0 read before the NES writes the next bit | 8.6 µs | 10.4 µs |
| CLK handler, longest run | 22.8 µs | 29.1 µs |
| RAM left for the stack | 2.8 KB | 2.8 KB |

NTSC clocks come 30.7 µs apart, and the margins shrink by a couple of
microseconds. Across every run, each clock edge got exactly one handler run,
and no D0 change or OUT0 read landed after the NES's next port access.

The RJ45 wiring comes out ahead where it counts. With no OUT0 interrupt,
nothing competes with the clock's handler, so it starts sooner. On the header
wiring the tightest margin is reading OUT0 before the NES writes the next
bit: the clock handler can start late because the OUT0 edge's own handler,
or a timer or serial interrupt, may get in first. The RJ45 clock handler's
longest run is the first clock of a poll, which also starts the request
frame.

That leaves slack in the ROM as built. The test also lowers `BIT_DELAY`,
which takes 7 CPU cycles (~4 µs) off every bit per step. The RJ45 build still
serves every page first time at 1. The header build still serves first time
at 2, but already has OUT0 reads landing late, and at 1 it needs retries.
So leave `BIT_DELAY` at 4 for the Mega.

The test fails well before the link does. With 6 µs of busy-waiting added
to the clock handler, every page still arrives intact but the margin check
fails; at 10 µs, reads start landing late and pages need retries.

The simulation models the chips, not the wires: edges are instant, and CLK
is taken to be low for half a CPU cycle. A pin change interrupt's flag is
set a few cycles after the pin reads high, as on the real chip. The RJ45
handler waits that out before clearing the flag, or it would count each
pulse twice. Through a controller cable, with or without 1 kΩ, real edges
take well under a microsecond, which moves none of these numbers. It runs as
part of `python tests/run_tests.py` whenever `arduino-cli` and the AVR core
are installed.

## If it does not work

`GET /_link` reports the same edge counts as on the ESP32, but a wire with
nothing on it now reads high, not low:

| Reading | The wire is on |
|---|---|
| ~400 falling edges, high ~85% | OUT0 |
| thousands of falling edges, high ~100% | CLK |
| no edges, high 100% | nothing — the pull-up holds it — or +5 V |
| no edges, low 100% | GND |

On the RJ45 wiring OUT0 has no interrupt, so its edges are counted from
samples 0.1 ms apart. That misses nothing on the web server ROM, which holds
OUT0 high for ~2 ms before every poll.

For a quicker look with no bridge, flash `NES_debug` for the same wiring
and open a serial monitor at 250000 baud. With the web server ROM running,
a sound cable prints about `clk=1800 out0=200 | clkHigh=100% out0High=85%`
twice a second. If it doesn't, send `pins`: it watches every pin from 2 to
13 for a second and shows where CLK and OUT0 really landed.

Beyond that, the test ROMs in [`bring-up.md`](bring-up.md) isolate one wire
at a time.

## One sketch, every board

`NES_router.ino` picks the board from the one you compile for, and the Mega's
wiring from `NES_WIRING_RJ45`. The link layer is shared, and the host tests
hold every build to it. `run_tests.py` compiles the sketch for the PC as the
ESP32, the Mega and the Mega on RJ45. Each build runs through
[`tests/host/link_tests.h`](../tests/host/link_tests.h) and against the
assembled ROM ([`tests/host/nes_sim.h`](../tests/host/nes_sim.h)). The
Mega's end to end test also saves every HTTP response the firmware writes,
and `run_tests.py` replays those through the real `serial_bridge.py`.
