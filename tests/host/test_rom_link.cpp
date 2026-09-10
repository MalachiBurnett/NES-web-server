// End to end test: the real assembled ROM, executed on a small 6502
// core (nes_sim.h), talking to the real ESP32 gateway firmware through
// its interrupt handlers. Nothing here models main.asm - it runs the ROM
// bytes, so a change to either side that breaks the protocol shows up
// here. test_mega_link.cpp does the same for the Arduino Mega port.
//
// The cable between them is modelled too, so it can be pulled out and
// pushed back in (with contact bounce) at any point in a transfer.
//
//   test_rom_link [path/to/nes_web_server.nes]
#include "arduino_shim.h"
#include "../../src/firmware/NES_router/NES_router.ino"
#include "harness.h"
#include "nes_sim.h"

// The gateway's cached copy of a page is exactly what the ROM holds.
static bool cacheMatchesRom(uint8_t page) {
  char *ref = nullptr;
  uint32_t refLen = 0;
  if (!decodePacket(&rom[offsets[page]], declaredLen(page), &ref, &refLen)) return false;
  bool same = pageCache[page] && pageLen[page] == refLen &&
              memcmp(pageCache[page], ref, refLen) == 0;
  free(ref);
  return same;
}

static void forget(uint8_t page) {
  free(pageCache[page]);
  pageCache[page] = nullptr;
  pageLen[page] = 0;
}

int main(int argc, char **argv) {
  const char *romPath = argc > 1 ? argv[1] : "build/nes_web_server.nes";
  if (!loadRom(romPath)) return 1;

  simSetInput(PIN_NES_CLOCK, true);
  resetLink();
  cpu.reset();

  printf("boot and idle polling\n");
  runUntil(never, 60000);
  check(!cpu.halted, "ROM boots and runs without hitting a bad opcode");
  check(pollCount > 5 && pollCount == strobes, "every NES strobe is recognised as a poll");
  check(linkState == LINK_IDLE && !rxComplete, "gateway stays idle while nothing is pending");
  check(sendsShown == 0 && cpu.ram[ZP_COLOUR] == COL_IDLE, "the ROM sits idle, sending nothing");
  printf("    %llu polls, %llu clock pulses in %llu instructions\n",
         (unsigned long long)pollCount, (unsigned long long)clockPulses,
         (unsigned long long)cpu.cycles);

  const char *names[] = {"index.html", "style.css", "404"};
  struct Case { uint8_t ask; uint8_t expect; const char *what; };
  Case cases[] = {{0, 0, "index.html"}, {1, 1, "style.css"},
                  {2, 2, "404"}, {9, 2, "unknown id falls back to 404"}};

  for (const Case &cs : cases) {
    printf("\nrequest id %u (%s)\n", cs.ask, cs.what);
    resetLink();
    uint32_t errors = linkErrors;
    uint64_t polls = pollCount;
    pendingId = cs.ask;
    requestPending = true;

    uint64_t t0 = cpu.cycles;
    uint64_t strobesBefore = strobes;
    bool ok = runUntil(rxDone, 40000000ULL);
    check(ok && linkErrors == errors, "response received first time, framing intact");
    if (!ok) continue;

    check(cpu.ram[ZP_ID] == cs.ask, "ROM latched the id we sent");
    uint16_t declared = declaredLen(cs.expect);
    check(rxExpected == declared, "length prefix matches the page in the ROM");
    check(memcmp(packetBuf, &rom[offsets[cs.expect]], declared) == 0, "packet bytes are identical");
    check(pollCount - polls <= 2 && strobes - strobesBefore > 300,
          "hundreds of data edges, none taken for a poll");

    char *out = nullptr;
    uint32_t outLen = 0;
    check(decodePacket(packetBuf, rxExpected, &out, &outLen), "gateway decodes the packet");
    if (out) {
      printf("    %s: %u byte packet -> %u bytes, %llu instructions\n",
             names[cs.expect], declared, outLen, (unsigned long long)(cpu.cycles - t0));
      printf("    starts: %.60s\n", out);
      free(out);
    }
    check(linkState == LINK_IDLE && !requestPending, "link returns to idle, request cleared");
  }

  printf("\nback to back requests without a reset\n");
  pendingId = 1;
  requestPending = true;
  rxComplete = false;
  bool ok = runUntil(rxDone, 40000000ULL);
  check(ok && rxExpected == declaredLen(1) &&
        memcmp(packetBuf, &rom[offsets[1]], declaredLen(1)) == 0,
        "second request works with no intervention");

  // From here on the firmware's own fetchPage() drives everything, and
  // the NES runs whenever it waits.
  g_delay_hook = runNesFor;
  g_serial_echo = false;

  printf("\nfetchPage, end to end\n");
  for (uint8_t page = 0; page < 3; page++) {
    forget(page);
    uint32_t errors = linkErrors;
    char label[96];
    snprintf(label, sizeof label, "%s fetched, cached copy identical to the ROM", names[page]);
    check(fetchPage(page) && cacheMatchesRom(page) && linkErrors == errors, label);
  }
  uint32_t t0 = g_micros;
  check(fetchPage(0) && g_micros == t0, "a cached page is served without touching the link");

  // The numbers GET /_link tells people to expect have to be what the
  // ROM really produces, or the probe sends them hunting the wrong wire.
  printf("\nwiring probe (GET /_link)\n");
  LinkProbe probe;
  measureLink(1000000, &probe);
  char report[1024];
  int reportLen = formatProbe(&probe, report, sizeof report);
  printf("    OUT0 %lu edges, high %lu%%   CLK %lu edges, high %lu%%   %lu polls\n",
         (unsigned long)probe.out0Edges, (unsigned long)probe.out0HighPct,
         (unsigned long)probe.clockEdges, (unsigned long)probe.clockHighPct,
         (unsigned long)probe.polls);
  check(probe.out0Edges > 250 && probe.out0Edges < 700 && probe.polls == probe.out0Edges,
        "OUT0: a few hundred polls a second, every one recognised");
  check(probe.out0HighPct >= 70 && probe.out0HighPct <= 95, "    high most, but not all, of the time");
  check(probe.clockEdges >= 8 * probe.polls && probe.clockHighPct >= 99,
        "CLK: ~9 edges per poll, and high the rest of the time");
  check(reportLen > 0 && reportLen < (int)sizeof report - 1, "the report fits its buffer");

  printf("\nhot plug\n");
  unplug(true);
  uint64_t sends = sendsShown;
  runNesFor(1000000);
  check(sendsShown == sends, "port floating high: the ROM refuses to answer junk");
  check(cpu.ram[ZP_COLOUR] == COL_JUNK, "    and shows the junk colour");

  plugIn();
  runNesFor(1000000);
  check(cpu.ram[ZP_COLOUR] == cpu.ram[ZP_RESULT] && linkState == LINK_IDLE,
        "plugged back in: the junk colour clears by itself");

  unplug(false);
  sends = sendsShown;
  runNesFor(200000);
  check(sendsShown == sends && cpu.ram[ZP_COLOUR] == cpu.ram[ZP_RESULT],
        "port floating low: reads as an idle gateway, nothing sent");

  forget(0);
  plugAt = g_micros + 300000;
  check(fetchPage(0) && cacheMatchesRom(0), "request waiting when the cable goes in");

  forget(0);
  uint32_t errors = linkErrors;
  unplugFloatsHigh = true;
  unplugAtRxByte = 1000;
  replugAfterUs = 250000;
  check(fetchPage(0) && cacheMatchesRom(0) && linkErrors > errors,
        "cable pulled mid-response and pushed back: retried, intact");

  forget(1);
  unplugAtRxByte = 400;
  replugAfterUs = 0;
  t0 = g_micros;
  ok = fetchPage(1);
  check(!ok && strstr(g_serial_last, "silent"), "cable pulled and left out: the fetch fails");
  check(g_micros - t0 < 2000000, "    promptly, not at the full deadline");
  check(!requestPending && linkState == LINK_IDLE && simGatewayOutput(PIN_NES_DATA_OUT),
        "    leaving the line idle");
  plugIn();
  runNesFor(200000);

  // The gateway reboots while the NES is streaming, so the NES carries
  // on sending into a gateway that has no idea a transfer is under way.
  resetLink();
  pendingId = 0;
  requestPending = true;
  runUntil([] { return linkState == LINK_RECEIVING && rxIndex >= 300; }, 40000000ULL);
  linkState = LINK_IDLE;
  requestPending = false;
  lastClockUs = lastActivityUs = 0;
  forget(0);
  check(fetchPage(0) && cacheMatchesRom(0), "gateway restarts mid-response, next fetch is intact");

  printf("\nsoak: random unplug and replug during every fetch\n");
  const int rounds = 40;
  int good = 0;
  errors = linkErrors;
  for (int r = 0; r < rounds; r++) {
    uint8_t page = r % 3;
    forget(page);
    unplugFloatsHigh = rnd() & 1;
    unplugAt = g_micros + rnd() % 900000;
    plugAt = unplugAt + 1000 + rnd() % 700000;
    if (fetchPage(page) && cacheMatchesRom(page)) good++;
    else printf("    round %d failed: %s", r, g_serial_last);
    runNesFor(20000);             // let any scheduled plug-in happen
    if (plugAt || unplugAt || cable != CONNECTED) {
      unplugAt = 0;
      if (cable == UNPLUGGED) plugIn();
      plugAt = 0;
      runNesFor(20000);
    }
  }
  char label[96];
  snprintf(label, sizeof label, "%d/%d fetches came through intact (%lu retries along the way)",
           good, rounds, (unsigned long)(linkErrors - errors));
  check(good == rounds, label);
  check(!cpu.halted, "the ROM never crashed");

  return finish();
}
