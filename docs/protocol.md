# NES web server protocol

How the cartridge and the ESP32 gateway talk to each other over a single
NES controller port.

Two pieces of code implement this and must stay in step:

| | |
|---|---|
| [`src/nes/main.asm`](../src/nes/main.asm) | the cartridge: polls for requests, streams pages back |
| [`src/firmware/NES_router/NES_router.ino`](../src/firmware/NES_router/NES_router.ino) | the gateway: asks for pages, decompresses them, serves them |

The packet format is produced by [`scripts/build_rom.py`](../scripts/build_rom.py)
and also read by [`scripts/emulate_rom.py`](../scripts/emulate_rom.py), which
serves the site straight from a ROM file with no hardware involved.

## 1. Wiring

Everything goes through **controller port 1**. The ROM never reads `$4017`,
so port 2 is unused and should be left empty. Three signals plus ground:

> Moving the link to port 2 means changing the three reads of `$4016` to
> `$4017` *and* physically moving the gateway. The strobe writes stay on
> `$4016` either way, because OUT0 is one latch line shared by both ports.
> Beware: an empty port floats and reads back as ones, so a ROM pointed at
> a port with nothing driving it sees an endless stream of requests for
> page `$FF`.

| NES port pin | Signal | Direction | ESP32-C3 |
|---|---|---|---|
| 1 | GND | — | GND |
| 2 | CLK | NES → gateway | GPIO4, via level shifter |
| 3 | OUT0 / latch | NES → gateway | GPIO3, via level shifter |
| 4 | D0 | gateway → NES | GPIO6, 100 Ω series. **Inverted** — see below |
| 7 | +5V | — | leave disconnected, power the board over USB |

Pin numbering varies between diagrams — find pin 1 and pin 7 with a
multimeter (ground and +5V) before soldering.

**D0 is inverted by the console.** The data line reaches the CPU through an
inverting buffer, which is simply how a real pad works: a pressed button
pulls the wire low and the game reads a 1. So the gateway drives the wire
*high* to send a 0, *low* to send a 1, and idles high. Get this backwards
and an idle gateway looks like every button held down: a flash cart menu
goes haywire the moment it is plugged in, and the ROM sees a request on
every poll. The firmware keeps the inversion in one place, `d0Write()`;
everything else is written in terms of the value the NES reads.

Holding the wire high means driving a 5 V console input with 3.3 V. That is
comfortably inside typical CMOS switching thresholds but has no guaranteed
margin, so if an idle gateway still reads as a 1, suspect that next.

**The port is 5V and the ESP32-C3 is not 5V tolerant.** CLK and OUT0 are
driven by the console at 5V and need shifting down; a 10k/20k divider on
each is enough at these speeds. D0 is an input to the console and is happy
being driven at 3.3V.

Why these three signals: the NES can only *drive* one line to the port,
OUT0, which is set by writing bit 0 of `$4016`. The clock line is not
software controlled at all — it pulses once per read of `$4016`, for about
one CPU cycle. So OUT0 carries data from the NES, D0 carries data to it,
and the read strobe clocks both directions.

## 2. Bit timing

| | |
|---|---|
| Clock pulse width | ~0.56µs (one 6502 cycle), **cannot be widened** |
| Gap between clock pulses | ~37µs, set by `BIT_DELAY` in `main.asm` |
| Latch strobe, high and then low | ~103µs each, set by `STROBE_DELAY` |
| Throughput | ~27 kbit/s, so ~0.7s for the 2.3KB index page |

Because the pulse is so narrow, the gateway must capture clock edges with a
GPIO **interrupt**. A `while (digitalRead(...))` loop cannot see a pulse
that short and will silently drop bits.

`BIT_DELAY` is the knob to turn. It exists to give the gateway's interrupt
handler room, so it has to cover the worst case latency on the ESP32 side —
which gets worse once WiFi is running. Lower it once the link is proven; the
protocol itself has no minimum speed.

Direction of travel for the data lines:

- **NES → gateway:** the ROM puts the bit on OUT0, *then* reads `$4016` to
  pulse the clock. The data stays put for the whole bit period, so sampling
  it a couple of microseconds after the falling edge is safe.
- **gateway → NES:** the CPU samples D0 during the pulse it generates, so
  the gateway must present each bit *before* the pulse and only advance on
  the edge afterwards.

Both directions are **LSB first**.

## 3. Poll frame

The NES is the master: it asks, the gateway answers. There is no way for the
gateway to interrupt the console, so `ServerLoop` polls in a loop.

```
NES                                   gateway
 |  OUT0 high, ~103us                    |
 |  OUT0 low   ------------------------> | falling edge: arm the frame
 |  read $4016  ---- clock pulse 1 ----> | bit 0: ready flag
 |  read $4016  ---- clock pulse 2 ----> | bit 1: page id, bit 0
 |     ... 7 more ...                    |
 |  read $4016  ---- clock pulse 9 ----> | bit 8: page id, bit 7
```

Nine bits: a ready flag, then the 8 bit page id. The gateway holds D0 low
whenever it is idle, so an unanswered poll reads back as `0` and the NES
backs off for ~0.8ms and asks again.

Arming on the falling edge of the strobe is what keeps the two sides
synchronised. A request that turns up midway through a poll simply misses
that frame and goes out on the next one — worst case one extra millisecond.
While a response is streaming, OUT0 is carrying data, so the gateway ignores
strobes unless it is idle.

Page ids are indexes into `LookupTable`, in the order listed in
`build_rom.py`: `0` = index.html, `1` = style.css. Anything `>= PageCount`
gets the 404 page, so the gateway can ask for an unknown id safely.

## 4. Response frame

Immediately after the poll frame, with no further handshake:

```
[length low] [length high] [ ... length bytes ... ]
```

A 16 bit little endian byte count, then that many bytes of packet. The
gateway sizes its buffer from the length and refuses anything larger than
`MAX_PACKET` rather than overrunning it. If edges stop arriving mid transfer,
`LINK_TIMEOUT_US` puts the link back to idle and the request fails cleanly.

## 5. Packet format (`NHF2`)

One packet per page, stored in the ROM behind its length prefix. All
multi-byte integers are little endian. **All bit strings are packed MSB
first** and each section starts on a byte boundary.

| Field | Size | Notes |
|---|---|---|
| magic | 4 | `NHF2` |
| token count | 1 | 0–128 dictionary entries |
| per token: length | 1 | 1–30 characters |
| per token: text | ⌈len×7/8⌉ | 7 bits per character, padded to a byte |
| tree bit length | 2 | |
| tree | ⌈bits/8⌉ | see below |
| symbol count | 2 | Huffman symbols to decode |
| expanded length | 2 | bytes after tokens are substituted back |
| payload bit length | 4 | |
| payload | ⌈bits/8⌉ | Huffman codes, back to back |

The Huffman tree is serialised depth first: `1` followed by 8 bits for a
leaf's symbol, `0` followed by the left and then the right subtree.

Symbols 0–127 are literal bytes. Symbols 128+ are dictionary tokens: symbol
`128 + n` expands to token `n`. That is why the page content must be pure
ASCII — a byte ≥ 128 in the source would be indistinguishable from a token.
`build_rom.py` rejects non-ASCII input rather than producing a broken packet.

**Symbol count and expanded length are different numbers** and the decoder
needs both: it decodes `symbol count` symbols, and the result is `expanded
length` bytes. Sizing the output buffer from the symbol count overflows it
the moment a token expands.

## 6. Testing without hardware

```bash
python scripts/build_rom.py
python tests/run_tests.py
```

`tests/run_tests.py` runs three things:

1. `build_rom.py`'s encoder against `emulate_rom.py`'s decoder, byte for byte.
2. `tests/host/test_gateway.cpp` — compiles the real `.ino` for the PC behind
   a small Arduino shim and checks the decoder, its rejection of malformed
   packets, and the link layer's edge cases.
3. `tests/host/test_rom_link.cpp` — runs the **assembled ROM** on a small 6502
   core whose `$4016` is wired to the real firmware's interrupt handlers, and
   checks that every page arrives byte identical and decodes correctly.

Test 3 exercises the actual shipped bytes on both sides, so a change to the
timing constants, the frame layout or the packet format shows up immediately.
What it does *not* model is real time: pulse widths, interrupt latency and
5V/3.3V levels are the things that can still only be proven on hardware.

To serve the site from a ROM file with no NES at all:

```bash
python scripts/emulate_rom.py
```
