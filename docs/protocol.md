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

> Moving the link to port 2 means changing the reads of `$4016` to `$4017`
> *and* physically moving the gateway. The strobe writes stay on `$4016`
> either way, because OUT0 is one latch line shared by both ports.
> Beware: an empty port floats and reads back as ones. The ROM rejects that
> as junk rather than answering it (the screen pulses red), but a ROM
> pointed at the wrong port will never see a request.

| NES port pin | Signal | Direction | ESP32-C3 |
|---|---|---|---|
| 1 | GND | — | GND |
| 2 | CLK | NES → gateway | GPIO4, via level shifter |
| 3 | OUT0 / latch | NES → gateway | GPIO3, via level shifter |
| 4 | D0 | gateway → NES | GPIO6, 1 kΩ series. **Inverted** — see below |
| 7 | +5V | — | leave disconnected, power the board over USB |

Pin numbering varies between diagrams — find pin 1 and pin 7 with a
multimeter (ground and +5V) before soldering.

**D0 is inverted by the console.** The data line reaches the CPU through an
inverting buffer, which is simply how a real pad works: a pressed button
pulls the wire low and the game reads a 1. So the gateway drives the wire
*high* to send a 0, *low* to send a 1, and idles high. Get this backwards
and an idle gateway looks like every button held down: a flash cart menu
goes haywire the moment it is plugged in, and the ROM sees junk on every
poll. The firmware keeps the inversion in one place, `d0Write()`;
everything else is written in terms of the value the NES reads.

Holding the wire high means driving a 5 V console input with 3.3 V. That is
comfortably inside typical CMOS switching thresholds but has no guaranteed
margin, so if an idle gateway still reads as a 1, suspect that next.

**The port is 5V and the ESP32-C3 is not 5V tolerant.** CLK and OUT0 are
driven by the console at 5V and need shifting down; a 10k/20k divider on
each is enough at these speeds. D0 is an input to the console and is happy
being driven at 3.3V. Keep the ESP32 powered whenever its cable is in a
powered console.

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
| Poll hold: OUT0 high, no clocks | ~2ms, `HoldDelay` |
| Strobe to first read | ~103µs, set by `STROBE_DELAY` |
| Throughput | ~27 kbit/s, so ~0.7s for the 2.3KB index page |

Because the pulse is so narrow, the gateway must capture clock edges with a
GPIO **interrupt**. A `while (digitalRead(...))` loop cannot see a pulse
that short and will silently drop bits.

`BIT_DELAY` is the knob to turn. It exists to give the gateway's interrupt
handler room, so it has to cover the worst case latency on the ESP32 side —
which gets worse once WiFi is running. Lower it once the link is proven; the
protocol itself has no minimum speed. Keep the gap between clocks well under
the gateway's `LINK_QUIET_US` (1ms) though, or data will be taken for polls.

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
gateway to interrupt the console, so `ServerLoop` polls in a loop, a few
hundred times a second.

```
NES                                         gateway
 |  OUT0 high, no clocks for ~2ms              |
 |  OUT0 low   ------------------------------> | edge after a quiet line: a poll
 |  read $4016 ---- clock pulses 1-8 --------> | bits 0-7:   zeros
 |  read $4016 ---- clock pulse 9 -----------> | bit 8:      ready flag
 |  read $4016 ---- clock pulses 10-17 ------> | bits 9-16:  page id
 |  read $4016 ---- clock pulses 18-25 ------> | bits 17-24: page id, inverted
```

The gateway holds D0 at 0 whenever it has nothing to ask, so an unanswered
poll reads nine zeros and the NES stops after the ready flag.

The NES only acts on a frame whose first eight bits really are zero and
whose id and complement differ in every bit. Anything else is junk — a
floating port reads as ones, and a gateway plugged in partway through a
frame hands over half of one — so the NES ignores it, shows red, and polls
again.

The leading zeros also protect other software on the same port. A flash
cart menu strobes and reads eight bits a frame; even with a request waiting,
all it ever sees from the gateway is zeros, which is no buttons pressed.

Page ids are indexes into `LookupTable`, in the order listed in
`build_rom.py`: `0` = index.html, `1` = style.css. Anything `>= PageCount`
gets the 404 page, so the gateway can ask for an unknown id safely.

## 4. Response frame

Immediately after an accepted poll frame, with no further handshake:

```
[id] [length low] [length high] [ ... length bytes ... ] [sum1] [sum2]
```

| Field | Notes |
|---|---|
| id | the page id the NES received, echoed back |
| length | 16 bit little endian packet size; the gateway refuses 0 or anything over `MAX_PACKET` |
| packet | the page, see below |
| sum1, sum2 | over every byte from the id to the end of the packet: `sum1 += byte`, `sum2 += sum1`, both mod 256 |

The echo tells the gateway the response answers *its* request. The checksum
catches bits flipped or lost in transit — a lost bit shifts everything after
it, which `sum2` notices even when `sum1` happens not to.

## 5. Staying in step

The cable can go in or come out at any moment, including halfway through a
frame. Neither side assumes the other was there a moment ago.

**Telling a poll from a data bit.** The NES drops OUT0 both to poll and to
send a 0, so the edge alone says nothing. Timing does: data edges come tens
of microseconds apart, each beside a clock pulse, while a poll follows ~2ms
of OUT0 held high with no clocks at all. The gateway counts an OUT0 falling
edge as a poll only if no clock arrived in the previous `LINK_QUIET_US`
(1ms), well clear of both.

**Every poll is a fresh start.** Whatever the gateway thought was going on,
a poll means the NES has moved on, so it drops it and begins a new frame.

**A request stays pending until a response checks out.** A frame the NES
refused, a response that stops, one for the wrong page, one with a bad
checksum: each puts the gateway back to idle with the request still pending,
so it goes out again on the very next poll. `fetchPage` logs each as
`# link: <reason>, retrying`.

**A line that goes quiet mid-transfer is dropped.** Clocks stopping for more
than `LINK_QUIET_US` partway through means the cable came out.

**Giving up.** `fetchPage` fails after `NES_SILENT_US` (1s) with no polls or
clocks at all, or `FETCH_DEADLINE_US` (5s) without a clean response. Both
leave the line idle and print `# NES link timeout` with the reason; the next
request starts from nothing.

What each kind of hot plug looks like:

| When | What happens |
|---|---|
| Plugged in, nothing pending | Contact bounce can look like a poll, but with nothing to ask the gateway only ever sends zeros. |
| Plugged in with a request waiting | Bounce may arm a frame nobody reads; the next real poll resets it and carries the request. |
| Plugged in partway through a poll | The gateway missed that strobe, so it stays idle and the NES reads zeros. The next poll carries the request. |
| Plugged in while the NES streams a response | The first data edge follows a quiet line and passes for a poll. The gateway takes the rest for a response, which fails the echo or checksum. The NES's next real poll puts both back in step. |
| Pulled mid-response | Clocks stop and the gateway drops the transfer after 1ms. Back in within a second, the retry just works; left out, the fetch fails after 1s. The NES finishes sending into nothing and goes back to polling. |
| Pulled, seen from the NES | A floating D0 reads as ones, which fail the preamble: red screen, nothing sent. If it floats low instead, it reads as an idle gateway. |

## 6. Packet format (`NHF2`)

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

## 7. Testing without hardware

```bash
python scripts/build_rom.py
python tests/run_tests.py
```

`tests/run_tests.py` runs, for both gateways:

1. `build_rom.py`'s encoder against `emulate_rom.py`'s decoder, byte for byte.
2. `tests/host/test_gateway.cpp` and `test_mega_gateway.cpp` — each
   gateway's real `.ino` compiled for the PC behind a small Arduino shim
   (`arduino_shim.h`, `avr_shim.h`), through the same link layer tests
   (`link_tests.h`): half frames, half responses, bad echoes and checksums,
   contact bounce, and a flash cart menu. The ESP32's decoder, and its
   rejection of malformed packets, are tested here too.
3. `tests/host/test_rom_link.cpp` and `test_mega_link.cpp` — the **assembled
   ROM** on a small 6502 core (`nes_sim.h`) whose `$4016` is wired through a
   model of the cable to each firmware's interrupt handlers. Every page must
   arrive byte identical, then the cable is pulled and replugged at chosen
   and then random moments in 40 fetches, all of which must come through
   intact.
4. `serial_bridge.py`, fed the HTTP the Mega firmware wrote in 3, which must
   decompress every page back to exactly what went into the ROM.
5. `tests/host/test_mega_avr.cpp`, when `arduino-cli` and the AVR core are
   installed — the Mega firmware compiled as it would be flashed, run cycle
   for cycle on an ATmega2560 simulator against the ROM on a 6502 with every
   port access at its real time. See [`mega.md`](mega.md#timing).

Tests 3 to 5 exercise the actual shipped bytes on both sides, so a change to
the timing constants, the frame layout or the packet format shows up
immediately. Test 3 models time only coarsely (instructions, not cycles),
which is enough for the logic because the gateway's thresholds sit an order
of magnitude from anything the ROM does; test 5 models it exactly, for the
Mega. Pulse widths, 5V/3.3V levels and the ESP32's real interrupt latency can
still only be proven on hardware.

To serve the site from a ROM file with no NES at all:

```bash
python scripts/emulate_rom.py
```
