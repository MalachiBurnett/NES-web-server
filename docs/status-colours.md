# On-screen status codes

The ROM has no text output — CHR is blank, so the whole screen is one flat
colour. That colour is the status display. It is written to the backdrop at
`$3F00` by `SetBG` in `src/nes/main.asm`.

The idle colours **pulse** between a dark and a light shade (`EOR #$10`)
roughly twice a second, once every 256 idle polls. The pulse is the useful
part: it only advances if `ServerLoop` is still turning. **A frozen screen
means the ROM has hung, whatever colour it is stuck on.**

| Screen | Constant | Meaning |
|---|---|---|
| Black / no picture | — | Never reached the end of init. Died before rendering was enabled — bad ROM load, or hung in the vblank wait. |
| Dark blue, steady | `COL_BOOT` `$01` | Init finished, but `ServerLoop` never started pulsing. Hung between enabling rendering and the first poll. |
| Cyan, pulsing | `COL_IDLE` `$0C`/`$1C` | **Normal at rest.** Polling, link alive on the NES side, nothing served yet this power-on. |
| Green, pulsing | `COL_OK` `$0A`/`$1A` | Last request resolved to a real page. Idles here after a successful serve. |
| Amber, pulsing | `COL_404` `$07`/`$17` | Last request had an id the ROM has no page for, so the 404 was served. |
| Yellow, brief | `COL_SEND` `$28` | Streaming a response right now. A page takes well under a second, so this is a flash, not a resting state. |

## Reading it

- **Cyan pulsing, browser returns 502.** The NES half is healthy and looping.
  The gateway never asserted its ready flag, so the fault is downstream:
  the CLK/DATA wiring, the ESP32, or the bridge. This is the case that
  `# NES link timeout` also reports.
- **Never leaves cyan, but the bridge shows no timeout.** Edges are arriving
  but the framing is wrong.
- **Sits on amber constantly**, without you requesting anything, means the
  NES reads D0 as 1 on every poll: a ready flag with id `$FF`, which is out
  of range and falls back to the 404. Suspects, most likely first: the
  gateway holding the wire low (D0 is inverted, so idle must be high), the
  gateway unpowered, or the ROM reading an empty port.
- **Green after a page load** is the end-to-end success state. Getting here
  once proves the whole path.
- **Any colour, not pulsing** — the ROM hung. The colour says where.

One caveat: dark blue is only reachable *after* the PPU is up, so a hang in
the earlier vblank wait shows as black rather than blue.
