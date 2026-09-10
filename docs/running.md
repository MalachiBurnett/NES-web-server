# Running it, in order

There is one rule that matters: **the ESP32-C3 is not 5 V tolerant.** If the
gateway cable is in a powered NES while the ESP32 is not powered, 5 V comes
down the dividers into unpowered pins and leaks through the chip's
protection diodes. So: ESP32 on USB first, off last.

Everything else can happen in any order. With the ESP32 powered, the cable
can go in or out at any time — at the flash cart menu, while idle, even
halfway through a page — and both ends pick themselves back up.

The gateway lives in **controller port 1**.

Using the Arduino Mega gateway instead of the ESP32? The order below is the
same; its wiring, flashing command and `/_link` readings are in
[`mega.md`](mega.md).

## Start up

1. **ESP32 into the laptop's USB.** It boots holding D0 idle — the wire
   high, which the console reads as no buttons.

2. **Check the port letter** if this is the first run of the day:

   ```
   python -c "import serial.tools.list_ports as p; [print(x.device,'-',x.description) for x in p.comports()]"
   ```

3. **Start the bridge.** It can run the whole time.

   ```
   python scripts/serial_bridge.py
   ```

4. **NES on, controller in port 1, pick the ROM** from the flash cart menu:
   `!NES-Web-Server.nes` for the real thing, or a test ROM if something is
   wrong. The gateway can stay plugged in for this if you have a controller
   in port 2 — an idle gateway reads as no buttons — or swap it in afterwards.

5. **Gateway cable into port 1**, if it is not in already.

6. **Wait for cyan, pulsing.** That is the healthy resting state: the ROM is
   polling and reads a clean idle gateway. Red pulsing means the port is
   reading junk — usually the plug not seated. Anything else, see
   `docs/status-colours.md`.

7. **Open `http://127.0.0.1:8080/`.**

Watch the TV as the page loads: cyan → yellow while it streams → **green**.
Green is success.

## Plugging and unplugging while it runs

- **Pull the cable mid-page** and push it back within a second: the gateway
  retries on its own and the page still arrives. Leave it out and the
  browser gets a 502 after about a second; the next request starts clean.
- **With the cable out**, the screen pulses red (an empty port reads as
  junk). Plug it in and it returns to its previous colour within a second.
- **The bridge window** shows `# link: <reason>, retrying` for each retry
  during a request. A handful after a plug-in is normal; a steady stream
  with the cable seated is a wiring or timing problem.
- **Between requests**, the gateway also prints `# NES port active` and
  `# NES port quiet` as polling starts and stops. The bridge discards these;
  `scripts/link_monitor.py` shows them.

## Shut down

1. Stop the bridge (Ctrl-C).
2. **NES off**, or pull the gateway cable.
3. **Then** unplug the ESP32 from USB.

## Flashing

The gateway, after any firmware change:

```
arduino-cli compile --fqbn esp32:esp32:esp32c3:CDCOnBoot=cdc --upload -p COM3 src/firmware/NES_router
```

`CDCOnBoot=cdc` is **not optional**. Without it, `Serial` goes to UART0 on
GPIO 20/21 instead of USB. The board still enumerates a COM port — that is
the separate USB-Serial/JTAG peripheral — so a monitor connects normally and
shows nothing at all, and the bridge gets no reply.

Rebuilding the ROM after a change to `src/nes/main.asm` or the site:

```
python scripts/build_rom.py
```

Then copy `build/nes_web_server.nes` onto the SD card. **The ROM and the
firmware must come from the same version** — the frame format changed when
hot plugging was added, and an old ROM with new firmware (or the reverse)
never gets past `response was for a different page` or a red screen.

## If it does not work

**Probe the wiring first.** With the web server ROM running and the bridge
up, open `http://127.0.0.1:8080/_link`. The gateway watches both of its
inputs for a second and reports edges and time spent high on each:

| Reading | The wire is on |
|---|---|
| ~400 falling edges, high ~85% | OUT0 |
| thousands of falling edges, high ~100% | CLK |
| no edges, high 100% | +5V — or CLK, with its pulses not getting through |
| no edges, low 100% | D3, D4, or nothing: not making contact |

GPIO 3 should read as OUT0 and GPIO 4 as CLK. If they read as something
else, the wire is on the wrong NES pin — fix it at the RJ45 breakout, with
the NES off, and never move a cable wire onto the D0 position: that one has
no divider, and +5V there would reach the ESP32 directly.

Beyond wiring, don't debug the full protocol. Drop to the test ROMs, which
isolate one wire at a time — `docs/bring-up.md` explains both. Briefly:

| Symptom | Where to look |
|---|---|
| Red pulsing with the cable in | the NES reads junk on D0: plug not seated, gateway unpowered, or D0 polarity |
| Cyan, bridge says `port silent` | no polls reach the gateway: OUT0 wiring to GPIO 3, or the ROM not running |
| Cyan, bridge says `N polls ... no good response` | polls arrive but frames do not: CLK wiring to GPIO 4 |
| Yellow flashes, `cut short` / `checksum mismatch` retries | the NES is serving, but edges are being lost on CLK or OUT0 |
| `response was for a different page` every time | ROM and firmware are different versions: reflash and recopy |
| Menu launches a game untouched | the port is floating: it reads as every button held |
| Menu goes haywire with the gateway plugged in | D0 polarity: the console inverts it, so an idle gateway must hold the wire **high** |

## Wiring, for reference

Controller port 1, seven pins:

| NES pin | Signal | Direction | ESP32-C3 |
|---|---|---|---|
| 1 | GND | — | GND |
| 2 | CLK | NES → gateway | GPIO 4, via 10k/20k divider |
| 3 | OUT0 / latch | NES → gateway | GPIO 3, via 10k/20k divider |
| 4 | D0 | gateway → NES | GPIO 6, via 1 kΩ series, **inverted** |
| 7 | +5 V | — | not connected |

The dividers are there because the C3 is a 3.3 V part and the port is 5 V.
D0 needs no divider — it is an output into a high-impedance NES input. The
1 kΩ in series limits the current into that input's protection diodes when
the console is off and the gateway is not, which the start-up order above
makes routine: through 100 Ω it would be ~27 mA, past a logic chip's 20 mA
rating. At these speeds it costs nothing. The console inverts D0: wire low reads
as 1, so the gateway idles with the wire high.
