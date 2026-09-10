# Arduino Mega gateway

The same gateway as the ESP32-C3's, on an Arduino Mega 2560. The ROM, the
protocol and the bridge are unchanged. The Mega is a 5 V board, so it wires
straight onto the controller port with no level shifting, which also makes
it a useful second opinion when the ESP32 link will not come up.

| | |
|---|---|
| gateway firmware | [`src/firmware/NES_router_mega/`](../src/firmware/NES_router_mega/NES_router_mega.ino) |
| bring-up tester | [`src/firmware/NES_debug_mega/`](../src/firmware/NES_debug_mega/NES_debug_mega.ino), with the test ROMs in [`bring-up.md`](bring-up.md) |
| board | Arduino Mega 2560, genuine or a clone. **Not** an Uno, Nano or Pro Mini: their 2 KB of RAM cannot hold a page |

## Wiring

Controller port 1. One 1 kΩ resistor in series with each of the three
signal wires, and nothing else — no dividers, no level shifter.

| NES pin | Signal | Direction | Mega |
|---|---|---|---|
| 1 | GND | — | GND |
| 2 | CLK | NES → gateway | pin 2, via 1 kΩ |
| 3 | OUT0 / latch | NES → gateway | pin 3, via 1 kΩ |
| 4 | D0 | gateway → NES | pin 4, via 1 kΩ, **inverted** |
| 7 | +5 V | — | not connected |

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
continuity meter, not by colour** — NESdev's table gives red for D0 and
yellow for CLK, plenty of tutorials say the opposite, and clones use
anything.

- **Pins 2 and 3** are the Mega's INT4 and INT5. They are the only external
  interrupt pins free — 18 to 21 are Serial1 and I2C — and INT4 has the
  higher priority, which is the one the clock wants.
- **The 1 kΩ resistors** are not level shifting; both ends are 5 V. They are
  there for when one side is powered and the other is not, which would
  otherwise feed a signal pin's current into the unpowered chip through its
  protection diodes. They slow the edges by tens of nanoseconds, nothing next
  to the clock pulse's ~0.5 µs.
- **+5 V stays disconnected.** The Mega runs from USB; joining the two
  supplies back-feeds one from the other.
- **The inputs use the Mega's internal pull-ups**, so with the cable out they
  sit high and quiet rather than floating.

Power order: with the resistors either order is safe, but keep the habit
from the ESP32 — gateway on first, off last.

## Flashing

The AVR core, once:

```bash
arduino-cli core install arduino:avr
```

Then, with the Mega's port (`arduino-cli board list` shows it):

```bash
arduino-cli compile --fqbn arduino:avr:mega:cpu=atmega2560 --upload -p COM4 src/firmware/NES_router_mega
```

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

## If it does not work

`GET /_link` reports the same edge counts as on the ESP32, but a wire with
nothing on it now reads high, not low:

| Reading | The wire is on |
|---|---|
| ~400 falling edges, high ~85% | OUT0 |
| thousands of falling edges, high ~100% | CLK |
| no edges, high 100% | nothing — the pull-up holds it — or +5 V |
| no edges, low 100% | GND |

For a quicker look with no bridge, flash `NES_debug_mega` and open a serial
monitor at 115200 baud. With the web server ROM running, a sound cable prints
about `clk=1800 out0=200 | clkHigh=100% out0High=85%` twice a second.

Beyond that, the test ROMs in [`bring-up.md`](bring-up.md) isolate one wire
at a time, and `NES_debug_mega` takes the same commands as the ESP32 tester.

## Keeping the two gateways in step

The link layer in `NES_router_mega.ino` is a port of `NES_router.ino`'s, and
a change to one belongs in the other. The host tests hold them to it: both
run [`tests/host/link_tests.h`](../tests/host/link_tests.h), and both run the
assembled ROM through [`tests/host/nes_sim.h`](../tests/host/nes_sim.h).
The Mega's end to end test also saves every HTTP response the firmware writes,
and `run_tests.py` replays those through the real `serial_bridge.py`.
