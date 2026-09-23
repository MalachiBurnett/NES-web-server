# NES web server

A web server that runs on a stock NES. The cartridge stores a compressed
copy of a small site, a gateway board plugged into the controller port (an
ESP32-C3, or an Arduino Mega) asks it for pages, and a bridge on a host
machine turns that into HTTP you can open in a browser.

The NES has exactly one software-controlled output line to a controller
port, and the clock line pulses only as a side effect of reading `$4016`.
The whole link is built out of those two facts.

## Layout

| | |
|---|---|
| `src/nes/main.asm` | the cartridge: polls for requests, streams pages back |
| `src/nes/debug.asm` | the bring-up test ROMs, which test the link one wire at a time |
| `src/firmware/NES_router/` | the gateway: asks for pages and serves them. One sketch for the ESP32-C3 and the Arduino Mega 2560 |
| `src/firmware/NES_debug/` | the bring-up tester that pairs with the test ROMs, for either board |
| `scripts/build_rom.py` | compresses the site and assembles the ROM |
| `scripts/serial_bridge.py` | HTTP on the host, serial to the gateway |
| `scripts/emulate_rom.py` | serves the site from a ROM file, no hardware |
| `site-to-serve/` | the site itself |
| `tests/` | host tests: the real firmware against a 6502 core |

## Build and run

```bash
python scripts/build_rom.py
```

Copy `build/nes_web_server.nes` to the flash cart's SD card, then follow
[`docs/running.md`](docs/running.md) — the power-on order matters, because
the ESP32-C3 is not 5 V tolerant.

No hardware to hand? This serves the site straight out of the built ROM:

```bash
python scripts/emulate_rom.py
```

## Tests

```bash
python tests/run_tests.py
```

These are not mocks. `test_link.cpp` runs the **assembled ROM** on a
small 6502 core whose `$4016` is wired to the real firmware's interrupt
handlers, so the two halves of the protocol are checked against each other
exactly as they will run on hardware. The gateway is compiled once for each
board and wiring, and every build goes through the same tests. The Mega's
HTTP output is also fed through the real `serial_bridge.py`.

With `arduino-cli` installed, the tests also compile both sketches for every
board whose core is installed. `test_mega_avr.cpp` then goes further for
the Mega: it runs the compiled gateway and tester on a cycle-accurate
ATmega2560 simulator against the ROM, in both wirings. That measures
whether the interrupt handlers keep up with the NES, and by how much.

## Documentation

| | |
|---|---|
| [`docs/running.md`](docs/running.md) | start-up order, flashing, wiring, first thing to check when it breaks |
| [`docs/protocol.md`](docs/protocol.md) | the wire format |
| [`docs/status-colours.md`](docs/status-colours.md) | the ROM has no text output, so the screen colour is the status display |
| [`docs/bring-up.md`](docs/bring-up.md) | test ROMs that isolate one wire at a time |
| [`docs/mega.md`](docs/mega.md) | the Arduino Mega gateway: wiring, flashing, and how it differs |

## Hardware

- A NES, PAL or NTSC. The ROM is NROM, mapper 0, and enables no NMI or IRQ,
  so nothing in it depends on region timing.
- A flash cart. Developed against an EverDrive N8.
- A gateway on the controller port, either:
  - an ESP32-C3 SuperMini through a level shifter — pinout in
    [`docs/running.md`](docs/running.md), or
  - an Arduino Mega 2560, which is 5 V and wires straight in, or takes an
    RJ45 jack plugged straight into its header — see
    [`docs/mega.md`](docs/mega.md).
