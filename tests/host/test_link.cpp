// End to end test: the real assembled ROM on the 6502 core (nes_sim.h),
// talking to the real gateway firmware through its interrupt handlers,
// down to the HTTP it writes to the host - in whichever build
// run_tests.py compiles (see arduino_shim.h).  Nothing here models
// main.asm: it runs the ROM bytes, so a change to either side that breaks
// the protocol shows up here.
//
// The cable between them is modelled too, so it can be pulled out and
// pushed back in (with contact bounce) at any point in a transfer.
//
// Every response the gateway writes can also be saved byte for byte, so
// run_tests.py can feed it through scripts/serial_bridge.py - the half of
// the gateway that runs on the host.
//
//   test_link [path/to/nes_web_server.nes] [directory to save responses in]
#include <string>

#include "arduino_shim.h"
#include "../../src/firmware/NES_router/NES_router.ino"
#include "harness.h"
#include "nes_sim.h"

// What the gateway serves for a page: the page itself where it decodes,
// else the ROM's packet for serial_bridge.py to decode.
static std::string servedBody(uint8_t page) {
#if DECODES
  char *text = nullptr;
  uint32_t len = 0;
  if (!decodePacket(&rom[offsets[page]], declaredLen(page), &text, &len)) return "";
  std::string s(text, len);
  free(text);
  return s;
#else
  return std::string((const char *)&rom[offsets[page]], declaredLen(page));
#endif
}

// What fetchPage() left behind is exactly what the ROM holds.
static bool fetchedMatchesRom(uint8_t page) {
#if DECODES
  std::string want = servedBody(page);
  return pageCache[page] && pageLen[page] == want.size() &&
         memcmp(pageCache[page], want.data(), want.size()) == 0;
#else
  return rxExpected == declaredLen(page) &&
         memcmp(packetBuf, &rom[offsets[page]], declaredLen(page)) == 0;
#endif
}

// Drop a cached page, so the next fetch goes to the NES.
static void forget(uint8_t page) {
#if DECODES
  free(pageCache[page]);
  pageCache[page] = nullptr;
  pageLen[page] = 0;
#else
  (void)page;
#endif
}

// --- HTTP, as the bridge will see it ---
struct Response {
  bool ok = false;                 // a status line and exactly Content-Length of body
  int status = 0;
  long contentLength = -1;
  std::string contentType, packet, body;
};

static std::string headerValue(const std::string &head, const char *name) {
  std::string key = std::string("\r\n") + name + ": ";
  size_t p = head.find(key);
  if (p == std::string::npos) return "";
  p += key.size();
  return head.substr(p, head.find("\r\n", p) - p);
}

// Log lines can come before the status line, as they do on the wire.
static Response parseResponse(const std::string &out) {
  Response r;
  size_t start = out.find("HTTP/1.1 ");
  if (start == std::string::npos) return r;
  size_t headEnd = out.find("\r\n\r\n", start);
  if (headEnd == std::string::npos) return r;
  std::string head = out.substr(start, headEnd + 2 - start);
  r.status = atoi(head.c_str() + 9);
  r.contentType = headerValue(head, "Content-Type");
  r.packet = headerValue(head, "X-NES-Packet");
  std::string len = headerValue(head, "Content-Length");
  r.contentLength = len.empty() ? -1 : atol(len.c_str());
  r.body = out.substr(headEnd + 4);
  r.ok = r.contentLength >= 0 && (long)r.body.size() == r.contentLength;
  return r;
}

static const char *saveDir = nullptr;

static void save(const char *name, const std::string &out) {
  if (!saveDir) return;
  std::string path = std::string(saveDir) + "/" + name;
  FILE *f = fopen(path.c_str(), "wb");
  if (!f) { printf("    cannot write %s\n", path.c_str()); failures++; return; }
  fwrite(out.data(), 1, out.size(), f);
  fclose(f);
}

// Send one request line the way serial_bridge.py does, and let the
// gateway's own loop() answer it.  Returns everything it wrote.
static std::string ask(const char *line) {
  g_serial_out.clear();
  g_serial_in = std::string(line) + "\n";
  loop();
  return g_serial_out;
}

int main(int argc, char **argv) {
  const char *romPath = argc > 1 ? argv[1] : "build/nes_web_server.nes";
  saveDir = argc > 2 ? argv[2] : nullptr;
  printf("%s gateway, CLK on %s %d, OUT0 on %s %d%s\n", BOARD_NAME, PIN_WORD, PIN_NES_CLOCK,
         PIN_WORD, PIN_NES_DATA_IN, OUT0_INTERRUPT ? "" : " (no interrupt)");
  if (!loadRom(romPath)) return 1;

  g_serial_echo = false;
  setup();                          // the real one: pins, pull-ups, banner
  g_serial_echo = true;
  simSetInput(PIN_NES_CLOCK, true);
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
    check(linkState == LINK_IDLE && !requestPending, "link returns to idle, request cleared");
    printf("    %s: %u byte packet, %llu instructions\n",
           names[cs.expect], declared, (unsigned long long)(cpu.cycles - t0));
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
    snprintf(label, sizeof label, "%s fetched, identical to the ROM", names[page]);
    check(fetchPage(page) && fetchedMatchesRom(page) && linkErrors == errors, label);
  }
#if DECODES
  uint32_t t0 = g_micros;
  check(fetchPage(0) && g_micros == t0, "a cached page is served without touching the link");
#endif

  printf("\nHTTP over serial, as serial_bridge.py sees it\n");
  struct Http {
    const char *line, *saveAs; int status; const char *mime; uint8_t page; const char *what;
  };
  const Http https[] = {
    {"GET / HTTP/1.1", "index.http", 200, "text/html", 0, "GET / serves index.html"},
    {"/index.html", "index_bare.http", 200, "text/html", 0, "a bare path works too"},
    {"GET /style.css?v=2 HTTP/1.1", "style.http", 200, "text/css", 1, "GET /style.css?v=2 ignores the query"},
    {"GET /missing HTTP/1.1", "missing.http", 404, "text/html", 2, "an unknown path gets the 404 page"},
  };
  for (const Http &h : https) {
    std::string out = ask(h.line);
    save(h.saveAs, out);
    Response r = parseResponse(out);
    std::string want = servedBody(h.page);
    check(r.ok && r.status == h.status && r.contentType == h.mime, h.what);
    check(r.packet == (DECODES ? "" : "nhf2") && r.contentLength == (long)want.size(),
          DECODES ? "    decoded here, with its length" : "    marked as a raw packet, with its length");
    check(r.body == want, "    the body is the page, byte for byte");
  }
  check(ask("hello").find("HTTP/") == std::string::npos, "a line with no path is ignored");

  Response serialTest = parseResponse(ask("GET /_serial HTTP/1.1"));
  bool patternOk = serialTest.ok && serialTest.body.size() == SERIAL_TEST_BYTES;
  for (uint16_t i = 0; patternOk && i < SERIAL_TEST_BYTES; i++)
    patternOk = (uint8_t)serialTest.body[i] == serialTestByte(i);
  check(patternOk, "GET /_serial sends scripts/serial_test.py's pattern, whole");

  // The numbers GET /_link tells people to expect have to be what the
  // ROM really produces, or the probe sends them hunting the wrong wire.
  printf("\nwiring probe (GET /_link)\n");
  std::string out = ask("GET /_link HTTP/1.1");
  save("link.http", out);
  Response r = parseResponse(out);
  check(r.ok && r.status == 200 && r.contentType == "text/plain" && r.packet.empty(),
        "answers with a plain text report, not a packet");
  unsigned long out0Edges = 0, out0High = 0, clkEdges = 0, clkHigh = 0, polls = 0;
  const char *b = r.body.c_str();
  const char *o = strstr(b, "OUT0:"), *c = strstr(b, "CLK:"), *p = strstr(b, "polls recognised");
  bool parsed = o && c && p &&
                sscanf(o, "OUT0: %lu falling edges, high %lu", &out0Edges, &out0High) == 2 &&
                sscanf(c, "CLK: %lu falling edges, high %lu", &clkEdges, &clkHigh) == 2 &&
                sscanf(p, "polls recognised %lu", &polls) == 1;
  check(parsed, "the report has both lines' figures");
  printf("    OUT0 %lu edges, high %lu%%   CLK %lu edges, high %lu%%   %lu polls\n",
         out0Edges, out0High, clkEdges, clkHigh, polls);
  // Without an interrupt on OUT0, a poll is counted at its first clock
  // and its edge from a sample, so one can fall either side of the window.
  check(out0Edges > 250 && out0Edges < 700 && polls + 1 >= out0Edges && out0Edges + 1 >= polls,
        "OUT0: a few hundred polls a second, every one recognised");
  check(out0High >= 70 && out0High <= 95, "    high most, but not all, of the time");
  check(clkEdges >= 8 * polls && clkHigh >= 99, "CLK: ~9 edges per poll, and high the rest of the time");

  printf("\nhot plug\n");
  unplug(true);
  uint64_t sends = sendsShown;
  runNesFor(1000000);
  check(sendsShown == sends, "port floating high: the ROM refuses to answer junk");
  check(cpu.ram[ZP_COLOUR] == COL_JUNK, "    and shows the junk colour");
  check(simInputLevel(PIN_NES_CLOCK) == SIM_INPUT_UNPLUGGED &&
        simInputLevel(PIN_NES_DATA_IN) == SIM_INPUT_UNPLUGGED,
        "    the gateway's inputs sit where its board holds them");

  forget(1);
  r = parseResponse(ask("GET /style.css HTTP/1.1"));
  check(r.ok && r.status == 502 && r.contentLength == 0, "a request with the cable out gets 502 Bad Gateway");

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
  check(fetchPage(0) && fetchedMatchesRom(0), "request waiting when the cable goes in");

  forget(0);
  uint32_t errors = linkErrors;
  unplugFloatsHigh = true;
  unplugAtRxByte = 1000;
  replugAfterUs = 250000;
  check(fetchPage(0) && fetchedMatchesRom(0) && linkErrors > errors,
        "cable pulled mid-response and pushed back: retried, intact");

  forget(1);
  unplugAtRxByte = 400;
  replugAfterUs = 0;
  uint32_t t1 = g_micros;
  ok = fetchPage(1);
  check(!ok && strstr(g_serial_last, "silent"), "cable pulled and left out: the fetch fails");
  check(g_micros - t1 < 2000000, "    promptly, not at the full deadline");
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
  check(fetchPage(0) && fetchedMatchesRom(0), "gateway restarts mid-response, next fetch is intact");

  printf("\nsoak: random unplug and replug during every fetch\n");
  const int rounds = 40;
  int good = 0;
  errors = linkErrors;
  for (int round = 0; round < rounds; round++) {
    uint8_t page = round % 3;
    forget(page);
    unplugFloatsHigh = rnd() & 1;
    unplugAt = g_micros + rnd() % 900000;
    plugAt = unplugAt + 1000 + rnd() % 700000;
    if (fetchPage(page) && fetchedMatchesRom(page)) good++;
    else printf("    round %d failed: %s\n", round, g_serial_last);
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
