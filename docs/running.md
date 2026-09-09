# Running it, in order

The order matters for one real reason: **the ESP32-C3 is not 5 V tolerant.**
If the NES is powered while the gateway is not, 5 V comes down the dividers
into unpowered pins and leaks through the chip's protection diodes. Power
the ESP32 first and turn it off last, and that never happens.

Everything below assumes the gateway is in **controller port 1**.

## Start up

1. **Everything off.** NES off at the switch, ESP32 unplugged from USB.

2. **Plug the controller cable into port 1** while it is all cold. Inserting
   a connector live can momentarily drag the 5 V pin across a signal pin.

3. **Controller into port 2**, if you have one, for the flash cart menu.

4. **ESP32 into the laptop's USB.** It boots, drives D0 low, and the port
   settles. Give it a second.

5. **Check the port letter** if this is the first run of the day:

   ```
   python -c "import serial.tools.list_ports as p; [print(x.device,'-',x.description) for x in p.comports()]"
   ```

6. **NES on.**

7. **Pick the ROM from the flash cart menu.** `!NES-Web-Server.nes` for the
   real thing, or one of the test ROMs if something is wrong.

8. **Wait for cyan, pulsing.** That is the healthy resting state: the ROM is
   polling, and D0 is reading low because the gateway is holding it there.
   Any other colour, check `docs/status-colours.md` before going further.

9. **Start the bridge.**

   ```
   python scripts/serial_bridge.py
   ```

10. **Open `http://127.0.0.1:8080/`.**

Watch the TV as the page loads: cyan → yellow while it streams → **green**.
Green is success.

## Shut down

Reverse of the above, and the order matters as much here:

1. Stop the bridge (Ctrl-C).
2. **NES off.**
3. **Then** unplug the ESP32 from USB.
4. Unplug the controller cable last, cold.

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

Then copy `build/nes_web_server.nes` onto the SD card.

## If it does not work

Don't debug the full protocol. Drop to the test ROMs, which isolate one wire
at a time — `docs/bring-up.md` explains both. Briefly:

| Symptom | Where to look |
|---|---|
| Screen never leaves cyan | gateway never asserted a request: bridge, USB, or the CDC flag |
| Yellow then amber, HTTP 502 | NES is serving, but the clock line is not reaching the gateway |
| Yellow/brown loop on its own | the ROM is reading a port nothing is driving — wrong port, or gateway unpowered |
| Random flicker, ignores commands | the data line is floating |
| Menu launches a game untouched | same floating port: it reads as every button held |

## Wiring, for reference

Controller port 1, seven pins:

| NES pin | Signal | Direction | ESP32-C3 |
|---|---|---|---|
| 1 | GND | — | GND |
| 2 | CLK | NES → gateway | GPIO 4, via 10k/20k divider |
| 3 | OUT0 / latch | NES → gateway | GPIO 3, via 10k/20k divider |
| 4 | D0 | gateway → NES | GPIO 6, via 100 Ω series |
| 7 | +5 V | — | not connected |

The dividers are there because the C3 is a 3.3 V part and the port is 5 V.
D0 needs no divider — it is an output into a high-impedance NES input, and
the 100 Ω is just series protection.
