# On-screen status codes

The ROM has no text output — CHR is blank, so the whole screen is one flat
colour. That colour is the status display. It is written to the backdrop at
`$3F00` by `SetBG` in `src/nes/main.asm`.

The resting colours **pulse** between a dark and a light shade, switching
about every 0.6s (every 256 polls). The pulse is the useful part: it only
advances if `ServerLoop` is still turning. **A frozen screen means the ROM
has hung, whatever colour it is stuck on.**

| Screen | Constant | Meaning |
|---|---|---|
| Black / no picture | — | Never reached the end of init. Died before rendering was enabled — bad ROM load, or hung in the vblank wait. |
| Dark blue, steady | `COL_BOOT` `$01` | Init finished, but `ServerLoop` never started pulsing. Hung between enabling rendering and the first poll. |
| Cyan, pulsing | `COL_IDLE` `$0C`/`$1C` | **Normal at rest.** Polling, the port reads clean, nothing served yet this power-on. |
| Green, pulsing | `COL_OK` `$0A`/`$1A` | Last request resolved to a real page. Rests here after a successful serve. |
| Amber, pulsing | `COL_404` `$07`/`$17` | Last request had an id the ROM has no page for, so the 404 was served. |
| Red, pulsing | `COL_JUNK` `$06`/`$16` | The port is reading junk: frames that fail the zero preamble or the id check. The ROM refuses to answer them. |
| Yellow, brief | `COL_SEND` `$28` | Streaming a response right now. A page takes well under a second, so this is a flash, not a resting state. |

Red is not sticky. The first clean idle poll puts the screen back to
whatever it showed before — cyan, green or amber — so red that stays means
junk is still arriving, and a single glitch as a plug goes in never shows.

## Reading it

- **Red, pulsing, without the cable in.** Expected: an empty port floats and
  reads as ones. Plug the gateway in and it should go back to its previous
  colour within a second.
- **Red, pulsing, with the cable in.** The NES is not reading what the
  gateway drives. Suspects, most likely first: the plug not fully seated, the
  gateway unpowered, D0 polarity backwards (idle must hold the wire *high*),
  or 3.3V not quite reaching the console's high threshold on pin 4.
- **Cyan pulsing, browser returns 502.** The NES half is healthy and sees an
  idle gateway. The request never got through, so look at what the bridge
  logged: `port silent` means no polls reach the ESP32 at all (OUT0 wiring);
  `N polls ... no good response` means polls arrive but the frame does not
  (CLK wiring, since the gateway can only advance a frame on clock edges).
- **Yellow flashes, then back to the old colour, retries in the bridge log.**
  The NES is serving, but the gateway refuses what arrives. The reason in
  `# link: ..., retrying` says which way: `cut short` or `checksum mismatch`
  means edges lost on CLK or OUT0; `response was for a different page`
  every time means the ROM and firmware are from different versions.
- **Green after a page load** is the end-to-end success state. Getting here
  once proves the whole path.
- **Any colour, not pulsing** — the ROM hung. The colour says where.

One caveat: dark blue is only reachable *after* the PPU is up, so a hang in
the earlier vblank wait shows as black rather than blue.
